#!/usr/bin/env python3
"""wlproxy.py LISTEN UPSTREAM — a Wayland relay that lets a nested Hyprland start on weston.

Hyprland's Wayland backend (aquamarine) binds wl_compositor v6, xdg_wm_base v6 and wl_seat v9
and aborts on any host offering less. No compositor that runs headless here offers all three
(cage 0.3: xdg_wm_base v5; weston 16: wl_compositor v5). Every version step those three take
past what weston has adds server-to-client EVENTS only (preferred_buffer_scale/transform,
wm_capabilities, the suspended state, axis_value120, axis_relative_direction), events a server
of the lower version simply never sends, so a client bound at the higher version talks the
lower one byte for byte. This relay advertises the higher versions in wl_registry.global and
binds the real ones upstream; every other byte, and every fd, passes through untouched.

Weston's headless backend also has no input, so it offers no wl_seat at all, and aquamarine
refuses to start without one. The relay therefore advertises one more global, a wl_seat with
no capabilities (the nested Hyprland gets its input from virtual devices, not from the host).
Binding it binds a wl_subcompositor upstream under the same object id: the client's id
sequence and weston's stay in step, and a wl_subcompositor never sends an event, so nothing
from weston ever arrives on that id. The relay answers the seat's capabilities(0) and name
itself, and repeats capabilities(0) four times a second: aquamarine flushes its queued
requests only when it has something to read, and a weston with no input goes silent once
the globals are listed, so without a heartbeat the nested Hyprland's first commit would sit
in its buffer forever. A repeated capabilities(0) changes nothing for a seat that never had
any.

Test infrastructure only: it understands those few messages and relays the rest blind.
"""
import os
import selectors
import socket
import struct
import sys

# interface -> version to advertise
BUMP = {b"wl_compositor": 6, b"xdg_wm_base": 6, b"wl_seat": 9}

DEBUG = bool(os.environ.get("WLPROXY_DEBUG"))

FAKE_SEAT_NAME = 0xFFFF0001
FAKE_SEAT_VERSION = 9


def pack_string(s):
    raw = s + b"\0"
    return struct.pack("=I", len(raw)) + raw + b"\0" * ((4 - len(raw) % 4) % 4)


def message(obj, opcode, payload):
    return struct.pack("=II", obj, ((8 + len(payload)) << 16) | opcode) + payload


def read_string(buf, off):
    (length,) = struct.unpack_from("=I", buf, off)
    s = bytes(buf[off + 4 : off + 4 + length - 1])  # without the NUL
    return s, off + 4 + ((length + 3) & ~3)


class Conn:
    def __init__(self, client, upstream):
        self.client, self.upstream = client, upstream
        self.registries = set()  # registry object ids the client created
        self.real = {}  # global name -> (interface, real version)
        self.injected = set()  # registries already told about the fake seat
        self.upstream_seat = False
        self.subcompositor = None  # (name, version) of weston's wl_subcompositor
        self.fake_seats = []  # object ids the client bound the fake seat as
        self.buf = {client: bytearray(), upstream: bytearray()}
        self.fds = {client: [], upstream: []}

    def other(self, sock):
        return self.upstream if sock is self.client else self.client

    def rewrite(self, sock, msg):
        obj, word = struct.unpack_from("=II", msg, 0)
        opcode = word & 0xFFFF
        if sock is self.client:
            if obj == 1 and opcode == 1:  # wl_display.get_registry(new_id)
                self.registries.add(struct.unpack_from("=I", msg, 8)[0])
            elif obj in self.registries and opcode == 0:  # wl_registry.bind(name, iface, version, id)
                (name,) = struct.unpack_from("=I", msg, 8)
                iface, off = read_string(msg, 12)
                if name == FAKE_SEAT_NAME:
                    (new_id,) = struct.unpack_from("=I", msg, off + 4)
                    if self.subcompositor is None:
                        raise RuntimeError("upstream has no wl_subcompositor to stand in for the seat")
                    sub_name, _ = self.subcompositor
                    self.fake_seats.append(new_id)
                    seat_events = message(new_id, 0, struct.pack("=I", 0)) + message(new_id, 1, pack_string(b"seat0"))
                    self.client.sendall(seat_events)
                    return bytearray(message(obj, 0, struct.pack("=I", sub_name) + pack_string(b"wl_subcompositor") + struct.pack("=II", 1, new_id)))
                if name in self.real:
                    real_iface, real_version = self.real[name]
                    (asked,) = struct.unpack_from("=I", msg, off)
                    if asked > real_version:
                        struct.pack_into("=I", msg, off, real_version)
        else:
            if obj in self.registries and opcode == 0:  # wl_registry.global(name, iface, version)
                (name,) = struct.unpack_from("=I", msg, 8)
                iface, off = read_string(msg, 12)
                (version,) = struct.unpack_from("=I", msg, off)
                self.real[name] = (iface, version)
                if iface == b"wl_seat":
                    self.upstream_seat = True
                if iface == b"wl_subcompositor":
                    self.subcompositor = (name, version)
                if iface in BUMP and version < BUMP[iface]:
                    struct.pack_into("=I", msg, off, BUMP[iface])
                if obj not in self.injected and not self.upstream_seat:
                    self.injected.add(obj)
                    msg += message(obj, 0, struct.pack("=I", FAKE_SEAT_NAME) + pack_string(b"wl_seat") + struct.pack("=I", FAKE_SEAT_VERSION))
        return msg

    def pump(self, sock):
        data, ancdata, _flags, _addr = sock.recvmsg(65536, socket.CMSG_SPACE(28 * 4))
        if not data:
            return False
        for level, kind, cdata in ancdata:
            if level == socket.SOL_SOCKET and kind == socket.SCM_RIGHTS:
                n = len(cdata) // 4
                self.fds[sock].extend(struct.unpack("=%di" % n, cdata[: n * 4]))
        buf = self.buf[sock]
        buf.extend(data)
        out = bytearray()
        while len(buf) >= 8:
            size = struct.unpack_from("=I", buf, 4)[0] >> 16
            if size < 8 or len(buf) < size:
                break
            msg = self.rewrite(sock, bytearray(buf[:size]))
            if DEBUG:
                obj, word = struct.unpack_from("=II", msg, 0)
                print("%s obj=%d op=%d size=%d" % ("C>S" if sock is self.client else "S>C", obj, word & 0xFFFF, word >> 16), file=sys.stderr, flush=True)
            out.extend(msg)
            del buf[:size]
        if DEBUG and buf:
            print("%s partial %d bytes pending" % ("C>S" if sock is self.client else "S>C", len(buf)), file=sys.stderr, flush=True)
        if out or self.fds[sock]:
            # fds travel with the first complete messages after them; libwayland queues fds
            # per connection and consumes them in message order, so earlier is fine, later is not
            fds = self.fds[sock]
            anc = [(socket.SOL_SOCKET, socket.SCM_RIGHTS, struct.pack("=%di" % len(fds), *fds))] if fds else []
            dest = self.other(sock)
            sent = dest.sendmsg([bytes(out)], anc)
            for fd in fds:
                os.close(fd)
            self.fds[sock] = []
            if sent < len(out):
                dest.sendall(bytes(out[sent:]))
        return True


def main():
    if DEBUG:
        import faulthandler, signal
        faulthandler.register(signal.SIGUSR1)
    listen_path, upstream_path = sys.argv[1], sys.argv[2]
    try:
        os.unlink(listen_path)
    except FileNotFoundError:
        pass
    server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    server.bind(listen_path)
    server.listen(8)
    sel = selectors.DefaultSelector()
    sel.register(server, selectors.EVENT_READ)
    conns = {}
    while True:
        ready = sel.select(timeout=0.25)
        if not ready:
            for c in set(conns.values()):
                for seat in c.fake_seats:
                    try:
                        c.client.sendall(message(seat, 0, struct.pack("=I", 0)))
                    except OSError:
                        pass
        for key, _ in ready:
            sock = key.fileobj
            if sock is server:
                client, _ = server.accept()
                upstream = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                upstream.connect(upstream_path)
                c = Conn(client, upstream)
                for s in (client, upstream):
                    conns[s] = c
                    sel.register(s, selectors.EVENT_READ)
                continue
            c = conns.get(sock)
            if c is None:
                continue
            try:
                alive = c.pump(sock)
            except OSError:
                alive = False
            if not alive:
                for s in (c.client, c.upstream):
                    sel.unregister(s)
                    conns.pop(s, None)
                    s.close()


if __name__ == "__main__":
    main()
