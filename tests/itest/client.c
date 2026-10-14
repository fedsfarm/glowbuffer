// gbclient — the Wayland client tests/itest/itest.sh drives. One xdg_toplevel, a data device,
// and one of two roles, every data-device event printed on stdout, one per line:
//
//   gbclient target NAME [--actions N] [--preferred N] [--accept MIME] [--out FILE] [--selection]
//       A drop target (or, with --selection, a paste target): on enter it answers set_actions
//       and accepts MIME; on drop (or on a selection carrying MIME) it reads MIME into FILE.
//
//   gbclient source NAME [--actions N] [--offer MIME=FILE]... [--portal-file PATH]
//       --portal-file offers PATH the way a sandboxed Qt app does: only as a FileTransfer
//       portal key (StartTransfer + AddFiles), no text/uri-list.
//       A drag source driving its own input through wlr-virtual-pointer and
//       zwp-virtual-keyboard (the nested compositor has no input devices of its own).
//       Prints READY once mapped, then reads commands from stdin:
//         drag SX SY TX TY W H HOLD_MS [shift]   press on (SX,SY), start_drag, move to
//                                                (TX,TY), hold, release; W,H = layout size
//         select SX SY W H                       click (SX,SY), set_selection
//         focus X Y W H                          move the pointer there (focus follows mouse)
//         quit
//
// Events, as printed: ENTER, LEAVE, ACTION n, OFFER_SOURCE_ACTIONS n, DROP action=n,
// DATA bytes=n, SELECTION mime,mime..., SRC_TARGET mime, SRC_SEND mime, SRC_ACTION n,
// SRC_DROP_PERFORMED, SRC_FINISHED, SRC_CANCELLED, DONE.

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <poll.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <systemd/sd-bus.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include "virtual-keyboard-unstable-v1-client-protocol.h"
#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#define MAX_OFFERS 8
#define MAX_MIMES  32

struct offer_info {
    struct wl_data_offer* offer;
    char*                 mimes[MAX_MIMES];
    int                   n_mimes;
    uint32_t              source_actions;
    uint32_t              action;
};

static struct {
    struct wl_display*                      dpy;
    struct wl_compositor*                   comp;
    struct wl_shm*                          shm;
    struct xdg_wm_base*                     wm;
    struct wl_seat*                         seat;
    struct wl_data_device_manager*          ddm;
    uint32_t                                ddm_version;
    struct zwlr_virtual_pointer_manager_v1* vpm;
    struct zwp_virtual_keyboard_manager_v1* vkm;

    struct wl_pointer*                   pointer;
    struct wl_keyboard*                  keyboard;
    struct wl_data_device*               dd;
    struct zwlr_virtual_pointer_v1*      vp;
    struct zwp_virtual_keyboard_v1*      vk;
    struct wl_surface*                   surf;
    struct xdg_surface*                  xs;
    struct xdg_toplevel*                 top;
    int                                  mapped;

    uint32_t button_serial;
    int      got_button;

    // configuration
    int         is_source;
    const char* name;
    uint32_t    actions, preferred;
    int         actions_set;
    const char* accept;
    const char* out;
    int         selection;
    int         selection_delay;   // ms to wait for the clipboard to settle before reading
    struct offer_info* pending_sel;
    uint32_t           sel_deadline;
    const char* offer_mime[MAX_OFFERS];
    char*       offer_data[MAX_OFFERS];
    size_t      offer_len[MAX_OFFERS];
    int         n_offers;

    // target state
    struct offer_info* dnd;
    uint32_t           enter_serial;
    int                done;
} app;

static sd_bus* g_portal_bus; // the transfer lives as long as this connection

static void say(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
    fflush(stdout);
}

static void die(const char* what) {
    fprintf(stderr, "gbclient %s: %s\n", app.name ? app.name : "?", what);
    exit(2);
}

static uint32_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

// ── event pump ───────────────────────────────────────────────────────────────────────────
static void pump(int timeout_ms) {
    while (wl_display_prepare_read(app.dpy) != 0)
        wl_display_dispatch_pending(app.dpy);
    wl_display_flush(app.dpy);
    struct pollfd p = {.fd = wl_display_get_fd(app.dpy), .events = POLLIN};
    if (poll(&p, 1, timeout_ms) > 0)
        wl_display_read_events(app.dpy);
    else
        wl_display_cancel_read(app.dpy);
    if (wl_display_dispatch_pending(app.dpy) < 0)
        die("connection lost");
}

static void pump_for(int ms) {
    const uint32_t END = now_ms() + (uint32_t)ms;
    while (!app.done) {
        const int LEFT = (int)(END - now_ms());
        if (LEFT <= 0)
            break;
        pump(LEFT);
    }
}

static int has_mime(const struct offer_info* o, const char* mime) {
    for (int i = 0; o && mime && i < o->n_mimes; ++i)
        if (strcmp(o->mimes[i], mime) == 0)
            return 1;
    return 0;
}

// Read MIME from an offer into the --out file. Returns bytes read, -1 on failure.
static long receive_to_file(struct wl_data_offer* offer, const char* mime) {
    int p[2];
    if (pipe2(p, O_CLOEXEC) != 0)
        return -1;
    wl_data_offer_receive(offer, mime, p[1]);
    close(p[1]);
    wl_display_flush(app.dpy);

    FILE* f     = app.out ? fopen(app.out, "wb") : NULL;
    long  total = 0;
    for (;;) {
        // keep dispatching while waiting: the source may be this very compositor round trip
        struct pollfd pf = {.fd = p[0], .events = POLLIN};
        const int     R  = poll(&pf, 1, 3000);
        if (R <= 0) {
            total = -1;
            break;
        }
        char          buf[4096];
        const ssize_t N = read(p[0], buf, sizeof buf);
        if (N < 0 && errno == EINTR)
            continue;
        if (N <= 0)
            break;
        if (f)
            fwrite(buf, 1, (size_t)N, f);
        total += N;
    }
    if (f)
        fclose(f);
    close(p[0]);
    return total;
}

// ── data offers ──────────────────────────────────────────────────────────────────────────
static void offer_offer(void* data, struct wl_data_offer* o, const char* mime) {
    struct offer_info* info = data;
    if (info->n_mimes < MAX_MIMES)
        info->mimes[info->n_mimes++] = strdup(mime);
}
static void offer_source_actions(void* data, struct wl_data_offer* o, uint32_t a) {
    ((struct offer_info*)data)->source_actions = a;
    if (!app.is_source)
        say("OFFER_SOURCE_ACTIONS %u", a);
}
static void offer_action(void* data, struct wl_data_offer* o, uint32_t a) {
    ((struct offer_info*)data)->action = a;
    if (!app.is_source)
        say("ACTION %u", a);
}
static const struct wl_data_offer_listener offer_listener = {
    .offer          = offer_offer,
    .source_actions = offer_source_actions,
    .action         = offer_action,
};

static void free_offer(struct offer_info* info) {
    if (!info)
        return;
    wl_data_offer_destroy(info->offer);
    for (int i = 0; i < info->n_mimes; ++i)
        free(info->mimes[i]);
    free(info);
}

static void dd_data_offer(void* data, struct wl_data_device* d, struct wl_data_offer* o) {
    struct offer_info* info = calloc(1, sizeof *info);
    info->offer             = o;
    wl_data_offer_add_listener(o, &offer_listener, info);
}

static void dd_enter(void* data, struct wl_data_device* d, uint32_t serial, struct wl_surface* s, wl_fixed_t x, wl_fixed_t y, struct wl_data_offer* o) {
    struct offer_info* info = o ? wl_data_offer_get_user_data(o) : NULL;
    if (app.is_source) {
        // a drag passing over the source itself: decline, like any toolkit with no drop site
        if (o)
            wl_data_offer_accept(o, serial, NULL);
        return;
    }
    app.dnd          = info;
    app.enter_serial = serial;
    say("ENTER");
    if (!o)
        return;
    if (app.actions_set && wl_data_offer_get_version(o) >= 3)
        wl_data_offer_set_actions(o, app.actions, app.preferred);
    wl_data_offer_accept(o, serial, has_mime(info, app.accept) ? app.accept : NULL);
}

static void dd_leave(void* data, struct wl_data_device* d) {
    if (app.is_source)
        return;
    say("LEAVE");
    free_offer(app.dnd);
    app.dnd = NULL;
}

static void dd_motion(void* data, struct wl_data_device* d, uint32_t t, wl_fixed_t x, wl_fixed_t y) {
    if (!app.is_source && app.dnd)
        wl_data_offer_accept(app.dnd->offer, app.enter_serial, has_mime(app.dnd, app.accept) ? app.accept : NULL);
}

static void dd_drop(void* data, struct wl_data_device* d) {
    if (app.is_source || !app.dnd)
        return;
    struct offer_info* info = app.dnd;
    say("DROP action=%u", info->action);
    if (has_mime(info, app.accept) && (info->action || app.ddm_version < 3)) {
        const long N = receive_to_file(info->offer, app.accept);
        say("DATA bytes=%ld", N);
        if (info->action == WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY || info->action == WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE)
            wl_data_offer_finish(info->offer);
    }
    free_offer(info);
    app.dnd  = NULL;
    app.done = 1;
}

static void dd_selection(void* data, struct wl_data_device* d, struct wl_data_offer* o) {
    if (app.is_source || !o)
        return;
    struct offer_info* info = wl_data_offer_get_user_data(o);
    char               list[1024] = "";
    for (int i = 0; i < info->n_mimes; ++i) {
        if (i)
            strncat(list, ",", sizeof list - strlen(list) - 1);
        strncat(list, info->mimes[i], sizeof list - strlen(list) - 1);
    }
    say("SELECTION %s", list);
    if (app.selection && has_mime(info, app.accept)) {
        if (app.selection_delay) {
            // read the newest clipboard once it settles, like a paste a moment later would
            if (app.pending_sel && app.pending_sel != info)
                free_offer(app.pending_sel);
            app.pending_sel  = info;
            app.sel_deadline = now_ms() + (uint32_t)app.selection_delay;
            return;
        }
        const long N = receive_to_file(o, app.accept);
        say("DATA bytes=%ld", N);
        app.done = 1;
    }
}

static const struct wl_data_device_listener dd_listener = {
    .data_offer = dd_data_offer,
    .enter      = dd_enter,
    .leave      = dd_leave,
    .motion     = dd_motion,
    .drop       = dd_drop,
    .selection  = dd_selection,
};

// ── data source ──────────────────────────────────────────────────────────────────────────
static void src_target(void* data, struct wl_data_source* s, const char* mime) {
    say("SRC_TARGET %s", mime ? mime : "(null)");
}
static void src_send(void* data, struct wl_data_source* s, const char* mime, int32_t fd) {
    say("SRC_SEND %s", mime);
    for (int i = 0; i < app.n_offers; ++i) {
        if (strcmp(app.offer_mime[i], mime) != 0)
            continue;
        size_t off = 0;
        while (off < app.offer_len[i]) {
            const ssize_t N = write(fd, app.offer_data[i] + off, app.offer_len[i] - off);
            if (N <= 0)
                break;
            off += (size_t)N;
        }
    }
    close(fd);
}
static void src_cancelled(void* data, struct wl_data_source* s) {
    say("SRC_CANCELLED");
    if (!app.selection)
        app.done = 1;
}
static void src_drop_performed(void* data, struct wl_data_source* s) {
    say("SRC_DROP_PERFORMED");
}
static void src_finished(void* data, struct wl_data_source* s) {
    say("SRC_FINISHED");
    app.done = 1;
}
static void src_action(void* data, struct wl_data_source* s, uint32_t a) {
    say("SRC_ACTION %u", a);
}
static const struct wl_data_source_listener src_listener = {
    .target             = src_target,
    .send               = src_send,
    .cancelled          = src_cancelled,
    .dnd_drop_performed = src_drop_performed,
    .dnd_finished       = src_finished,
    .action             = src_action,
};

static struct wl_data_source* make_source(void) {
    struct wl_data_source* s = wl_data_device_manager_create_data_source(app.ddm);
    wl_data_source_add_listener(s, &src_listener, NULL);
    for (int i = 0; i < app.n_offers; ++i)
        wl_data_source_offer(s, app.offer_mime[i]);
    return s;
}

// ── input ────────────────────────────────────────────────────────────────────────────────
static int g_pointer_inside = 0;
static void ptr_enter(void* d, struct wl_pointer* p, uint32_t serial, struct wl_surface* s, wl_fixed_t x, wl_fixed_t y) {
    g_pointer_inside = s == app.surf;
}
static void ptr_leave(void* d, struct wl_pointer* p, uint32_t serial, struct wl_surface* s) {
    g_pointer_inside = 0;
}
static void ptr_motion(void* d, struct wl_pointer* p, uint32_t t, wl_fixed_t x, wl_fixed_t y) {}
static void ptr_button(void* d, struct wl_pointer* p, uint32_t serial, uint32_t t, uint32_t button, uint32_t state) {
    if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
        app.button_serial = serial;
        app.got_button    = 1;
    }
}
static void ptr_axis(void* d, struct wl_pointer* p, uint32_t t, uint32_t axis, wl_fixed_t v) {}
static void ptr_frame(void* d, struct wl_pointer* p) {}
static void ptr_axis_source(void* d, struct wl_pointer* p, uint32_t s) {}
static void ptr_axis_stop(void* d, struct wl_pointer* p, uint32_t t, uint32_t a) {}
static void ptr_axis_discrete(void* d, struct wl_pointer* p, uint32_t a, int32_t v) {}
static const struct wl_pointer_listener ptr_listener = {
    .enter         = ptr_enter,
    .leave         = ptr_leave,
    .motion        = ptr_motion,
    .button        = ptr_button,
    .axis          = ptr_axis,
    .frame         = ptr_frame,
    .axis_source   = ptr_axis_source,
    .axis_stop     = ptr_axis_stop,
    .axis_discrete = ptr_axis_discrete,
};

static void kb_keymap(void* d, struct wl_keyboard* k, uint32_t f, int32_t fd, uint32_t size) {
    close(fd);
}
static void kb_enter(void* d, struct wl_keyboard* k, uint32_t serial, struct wl_surface* s, struct wl_array* keys) {
    if (!app.is_source)
        say("KEYBOARD_FOCUS");
}
static void kb_leave(void* d, struct wl_keyboard* k, uint32_t serial, struct wl_surface* s) {}
static void kb_key(void* d, struct wl_keyboard* k, uint32_t serial, uint32_t t, uint32_t key, uint32_t state) {}
static void kb_mods(void* d, struct wl_keyboard* k, uint32_t serial, uint32_t a, uint32_t b, uint32_t c, uint32_t g) {}
static void kb_repeat(void* d, struct wl_keyboard* k, int32_t rate, int32_t delay) {}
static const struct wl_keyboard_listener kb_listener = {
    .keymap      = kb_keymap,
    .enter       = kb_enter,
    .leave       = kb_leave,
    .key         = kb_key,
    .modifiers   = kb_mods,
    .repeat_info = kb_repeat,
};

static void seat_caps(void* d, struct wl_seat* s, uint32_t caps) {
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !app.pointer) {
        app.pointer = wl_seat_get_pointer(s);
        wl_pointer_add_listener(app.pointer, &ptr_listener, NULL);
    }
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !app.keyboard) {
        app.keyboard = wl_seat_get_keyboard(s);
        wl_keyboard_add_listener(app.keyboard, &kb_listener, NULL);
    }
}
static void seat_name(void* d, struct wl_seat* s, const char* n) {}
static const struct wl_seat_listener seat_listener = {.capabilities = seat_caps, .name = seat_name};

// A virtual keyboard gives the seat its keyboard capability, which is what lets the paste
// target receive keyboard focus, and so the selection, at all.
static void make_virtual_keyboard(void) {
    if (!app.vkm)
        die("no zwp_virtual_keyboard_manager_v1");
    struct xkb_context*    ctx   = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    struct xkb_rule_names  names = {0};
    struct xkb_keymap*     km    = xkb_keymap_new_from_names(ctx, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
    char*                  str   = km ? xkb_keymap_get_as_string(km, XKB_KEYMAP_FORMAT_TEXT_V1) : NULL;
    if (!str)
        die("cannot compile a keymap");
    const size_t SIZE = strlen(str) + 1;
    const int    FD   = memfd_create("gbclient-keymap", MFD_CLOEXEC);
    if (FD < 0 || write(FD, str, SIZE) != (ssize_t)SIZE)
        die("keymap memfd");
    app.vk = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(app.vkm, app.seat);
    zwp_virtual_keyboard_v1_keymap(app.vk, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, FD, (uint32_t)SIZE);
    close(FD);
    free(str);
    xkb_keymap_unref(km);
    xkb_context_unref(ctx);
}

static void shift(int down) {
    zwp_virtual_keyboard_v1_key(app.vk, now_ms(), KEY_LEFTSHIFT, down ? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED);
    zwp_virtual_keyboard_v1_modifiers(app.vk, down ? 1 : 0, 0, 0, 0);
    wl_display_flush(app.dpy);
}

static void move_to(double x, double y, uint32_t w, uint32_t h) {
    zwlr_virtual_pointer_v1_motion_absolute(app.vp, now_ms(), (uint32_t)x, (uint32_t)y, w, h);
    zwlr_virtual_pointer_v1_frame(app.vp);
    wl_display_flush(app.dpy);
}

static void button(int down) {
    zwlr_virtual_pointer_v1_button(app.vp, now_ms(), BTN_LEFT, down ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED);
    zwlr_virtual_pointer_v1_frame(app.vp);
    wl_display_flush(app.dpy);
}

// Move onto our own surface and make sure the compositor agrees we are under the pointer
// before pressing: a press that lands on the other window is a test that tests nothing.
static int point_at_self(double x, double y, uint32_t w, uint32_t h) {
    for (int i = 0; i < 20 && !g_pointer_inside; ++i) {
        move_to(x + (i % 2), y, w, h);
        pump_for(50);
    }
    return g_pointer_inside;
}

static int press_and_wait(void) {
    app.got_button = 0;
    button(1);
    for (int i = 0; i < 100 && !app.got_button; ++i)
        pump(20);
    return app.got_button;
}

// ── commands (source) ────────────────────────────────────────────────────────────────────
static void cmd_drag(double sx, double sy, double tx, double ty, uint32_t w, uint32_t h, int hold, int with_shift) {
    if (!point_at_self(sx, sy, w, h)) {
        say("NOT_ON_SELF");
        return;
    }
    if (!press_and_wait()) {
        say("NO_BUTTON");
        return;
    }
    struct wl_data_source* s = make_source();
    if (app.actions_set && app.ddm_version >= 3)
        wl_data_source_set_actions(s, app.actions);
    wl_data_device_start_drag(app.dd, s, app.surf, NULL, app.button_serial);
    pump_for(100);
    for (int i = 1; i <= 10; ++i) {
        move_to(sx + (tx - sx) * i / 10, sy + (ty - sy) * i / 10, w, h);
        pump_for(30);
    }
    pump_for(hold / 2);
    if (with_shift) {
        shift(1);
        move_to(tx + 1, ty + 1, w, h); // a motion after the key, as a real hand makes
    }
    pump_for(hold / 2);
    button(0);
    pump_for(3000);
    if (with_shift)
        shift(0);
    say("DONE");
}

static void cmd_select(double x, double y, uint32_t w, uint32_t h) {
    if (!point_at_self(x, y, w, h)) {
        say("NOT_ON_SELF");
        return;
    }
    if (!press_and_wait()) {
        say("NO_BUTTON");
        return;
    }
    button(0);
    struct wl_data_source* s = make_source();
    wl_data_device_set_selection(app.dd, s, app.button_serial);
    pump_for(100);
    say("SELECTION_SET");
}

static void run_source(void) {
    if (!app.vpm)
        die("no zwlr_virtual_pointer_manager_v1");
    app.vp = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(app.vpm, app.seat);
    make_virtual_keyboard();
    for (int i = 0; i < 5; ++i)
        wl_display_roundtrip(app.dpy);
    say("READY");

    char   line[512];
    FILE*  in = stdin;
    int    stdin_fd = fileno(in);
    for (;;) {
        // serve the compositor while waiting for the next command: a pasted selection is
        // read from this process whenever the target asks
        struct pollfd p[2] = {{.fd = stdin_fd, .events = POLLIN}, {.fd = wl_display_get_fd(app.dpy), .events = POLLIN}};
        wl_display_flush(app.dpy);
        if (poll(p, 2, 100) < 0 && errno != EINTR)
            break;
        if (p[1].revents & POLLIN)
            pump(0);
        if (!(p[0].revents & (POLLIN | POLLHUP)))
            continue;
        if (!fgets(line, sizeof line, in))
            break;

        double   a, b, c, e;
        unsigned w, h;
        int      hold;
        char     extra[32] = "";
        if (sscanf(line, "drag %lf %lf %lf %lf %u %u %d %31s", &a, &b, &c, &e, &w, &h, &hold, extra) >= 7) {
            app.done = 0;
            cmd_drag(a, b, c, e, w, h, hold, strcmp(extra, "shift") == 0);
        } else if (sscanf(line, "select %lf %lf %u %u", &a, &b, &w, &h) == 4)
            cmd_select(a, b, w, h);
        else if (sscanf(line, "focus %lf %lf %u %u", &a, &b, &w, &h) == 4) {
            move_to(a, b, w, h);
            pump_for(200);
            say("FOCUSED");
        } else if (strncmp(line, "quit", 4) == 0)
            break;
        else
            say("UNKNOWN %s", line);
    }
    say("BYE");
}

// ── surface ──────────────────────────────────────────────────────────────────────────────
static void wm_ping(void* d, struct xdg_wm_base* wm, uint32_t serial) {
    xdg_wm_base_pong(wm, serial);
}
static const struct xdg_wm_base_listener wm_listener = {.ping = wm_ping};

static struct wl_buffer* white_buffer(int w, int h) {
    const int    STRIDE = w * 4;
    const size_t SIZE   = (size_t)STRIDE * (size_t)h;
    const int    FD     = memfd_create("gbclient-buffer", MFD_CLOEXEC);
    if (FD < 0 || ftruncate(FD, (off_t)SIZE) != 0)
        die("buffer memfd");
    uint32_t* px = mmap(NULL, SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, FD, 0);
    if (px == MAP_FAILED)
        die("mmap");
    for (size_t i = 0; i < SIZE / 4; ++i)
        px[i] = app.is_source ? 0xFF4C8BF5u : 0xFFFFFFFFu;
    munmap(px, SIZE);
    struct wl_shm_pool* pool = wl_shm_create_pool(app.shm, FD, (int32_t)SIZE);
    struct wl_buffer*   buf  = wl_shm_pool_create_buffer(pool, 0, w, h, STRIDE, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(FD);
    return buf;
}

static void xs_configure(void* d, struct xdg_surface* xs, uint32_t serial) {
    xdg_surface_ack_configure(xs, serial);
    if (!app.mapped) {
        wl_surface_attach(app.surf, white_buffer(400, 300), 0, 0);
        wl_surface_damage_buffer(app.surf, 0, 0, INT32_MAX, INT32_MAX);
        app.mapped = 1;
    }
    wl_surface_commit(app.surf);
}
static const struct xdg_surface_listener xs_listener = {.configure = xs_configure};

static void top_configure(void* d, struct xdg_toplevel* t, int32_t w, int32_t h, struct wl_array* states) {}
static void top_close(void* d, struct xdg_toplevel* t) {
    exit(0);
}
static void top_bounds(void* d, struct xdg_toplevel* t, int32_t w, int32_t h) {}
static void top_caps(void* d, struct xdg_toplevel* t, struct wl_array* caps) {}
static const struct xdg_toplevel_listener top_listener = {
    .configure        = top_configure,
    .close            = top_close,
    .configure_bounds = top_bounds,
    .wm_capabilities  = top_caps,
};

// ── registry ─────────────────────────────────────────────────────────────────────────────
static uint32_t min_u(uint32_t a, uint32_t b) {
    return a < b ? a : b;
}

static void reg_global(void* d, struct wl_registry* r, uint32_t id, const char* iface, uint32_t v) {
    if (strcmp(iface, wl_compositor_interface.name) == 0)
        app.comp = wl_registry_bind(r, id, &wl_compositor_interface, min_u(v, 4));
    else if (strcmp(iface, wl_shm_interface.name) == 0)
        app.shm = wl_registry_bind(r, id, &wl_shm_interface, 1);
    else if (strcmp(iface, xdg_wm_base_interface.name) == 0) {
        app.wm = wl_registry_bind(r, id, &xdg_wm_base_interface, 1);
        xdg_wm_base_add_listener(app.wm, &wm_listener, NULL);
    } else if (strcmp(iface, wl_seat_interface.name) == 0 && !app.seat) {
        app.seat = wl_registry_bind(r, id, &wl_seat_interface, min_u(v, 5));
        wl_seat_add_listener(app.seat, &seat_listener, NULL);
    } else if (strcmp(iface, wl_data_device_manager_interface.name) == 0) {
        app.ddm_version = min_u(v, 3);
        app.ddm         = wl_registry_bind(r, id, &wl_data_device_manager_interface, app.ddm_version);
    } else if (strcmp(iface, zwlr_virtual_pointer_manager_v1_interface.name) == 0)
        app.vpm = wl_registry_bind(r, id, &zwlr_virtual_pointer_manager_v1_interface, 1);
    else if (strcmp(iface, zwp_virtual_keyboard_manager_v1_interface.name) == 0)
        app.vkm = wl_registry_bind(r, id, &zwp_virtual_keyboard_manager_v1_interface, 1);
}
static void reg_remove(void* d, struct wl_registry* r, uint32_t id) {}
static const struct wl_registry_listener reg_listener = {.global = reg_global, .global_remove = reg_remove};

// Start a FileTransfer for PATH and return its key, as QXdgDesktopPortalFileTransfer does.
static char* portal_key_for(const char* path) {
    const char*     DEST = "org.freedesktop.portal.Documents";
    const char*     OBJ  = "/org/freedesktop/portal/documents";
    const char*     IF   = "org.freedesktop.portal.FileTransfer";
    sd_bus_error    e    = SD_BUS_ERROR_NULL;
    sd_bus_message* reply = NULL;
    if (sd_bus_open_user(&g_portal_bus) < 0)
        die("session bus");
    if (sd_bus_call_method(g_portal_bus, DEST, OBJ, IF, "StartTransfer", &e, &reply, "a{sv}", 2, "writable", "b", 0, "autostop", "b", 1) < 0)
        die(e.message ? e.message : "StartTransfer");
    const char* key = NULL;
    if (sd_bus_message_read(reply, "s", &key) < 0 || !key)
        die("StartTransfer reply");
    char* out = strdup(key);
    sd_bus_message_unref(reply);

    const int FD = open(path, O_PATH | O_CLOEXEC);
    if (FD < 0)
        die("--portal-file: cannot open");
    sd_bus_message* m = NULL;
    sd_bus_message_new_method_call(g_portal_bus, &m, DEST, OBJ, IF, "AddFiles");
    sd_bus_message_append(m, "s", out);
    sd_bus_message_open_container(m, 'a', "h");
    sd_bus_message_append(m, "h", FD);
    sd_bus_message_close_container(m);
    sd_bus_message_append(m, "a{sv}", 0);
    if (sd_bus_call(g_portal_bus, m, 0, &e, NULL) < 0)
        die(e.message ? e.message : "AddFiles");
    sd_bus_message_unref(m);
    close(FD);
    return out;
}

static char* slurp(const char* path, size_t* len) {
    FILE* f = fopen(path, "rb");
    if (!f)
        die("cannot read an --offer file");
    char*  buf = NULL;
    size_t cap = 0, n = 0;
    for (;;) {
        if (n == cap) {
            cap = cap ? cap * 2 : 4096;
            buf = realloc(buf, cap);
        }
        const size_t R = fread(buf + n, 1, cap - n, f);
        if (R == 0)
            break;
        n += R;
    }
    fclose(f);
    *len = n;
    return buf;
}

int main(int argc, char** argv) {
    if (argc < 3)
        die("usage: gbclient source|target NAME [options]");
    app.is_source = strcmp(argv[1], "source") == 0;
    app.name      = argv[2];
    for (int i = 3; i < argc; ++i) {
        const char* A = argv[i];
        if (strcmp(A, "--actions") == 0 && i + 1 < argc) {
            app.actions     = (uint32_t)atoi(argv[++i]);
            app.actions_set = 1;
        } else if (strcmp(A, "--preferred") == 0 && i + 1 < argc)
            app.preferred = (uint32_t)atoi(argv[++i]);
        else if (strcmp(A, "--accept") == 0 && i + 1 < argc)
            app.accept = argv[++i];
        else if (strcmp(A, "--out") == 0 && i + 1 < argc)
            app.out = argv[++i];
        else if (strcmp(A, "--selection") == 0)
            app.selection = 1;
        else if (strcmp(A, "--selection-delay") == 0 && i + 1 < argc) {
            app.selection       = 1;
            app.selection_delay = atoi(argv[++i]);
        }
        else if (strcmp(A, "--offer") == 0 && i + 1 < argc && app.n_offers < MAX_OFFERS) {
            char* spec = strdup(argv[++i]);
            char* eq   = strrchr(spec, '='); // mime types may contain "=" themselves
            if (!eq)
                die("--offer wants MIME=FILE");
            *eq                          = '\0';
            app.offer_mime[app.n_offers] = spec;
            app.offer_data[app.n_offers] = slurp(eq + 1, &app.offer_len[app.n_offers]);
            ++app.n_offers;
        } else if (strcmp(A, "--portal-file") == 0 && i + 1 < argc && app.n_offers < MAX_OFFERS) {
            app.offer_mime[app.n_offers] = "application/vnd.portal.filetransfer";
            app.offer_data[app.n_offers] = portal_key_for(argv[++i]);
            app.offer_len[app.n_offers]  = strlen(app.offer_data[app.n_offers]);
            ++app.n_offers;
        } else
            die("unknown option");
    }

    app.dpy = wl_display_connect(NULL);
    if (!app.dpy)
        die("cannot connect to WAYLAND_DISPLAY");
    wl_registry_add_listener(wl_display_get_registry(app.dpy), &reg_listener, NULL);
    wl_display_roundtrip(app.dpy);
    if (!app.comp || !app.shm || !app.wm || !app.seat || !app.ddm)
        die("compositor lacks a required global");

    app.dd = wl_data_device_manager_get_data_device(app.ddm, app.seat);
    wl_data_device_add_listener(app.dd, &dd_listener, NULL);

    app.surf = wl_compositor_create_surface(app.comp);
    app.xs   = xdg_wm_base_get_xdg_surface(app.wm, app.surf);
    xdg_surface_add_listener(app.xs, &xs_listener, NULL);
    app.top = xdg_surface_get_toplevel(app.xs);
    xdg_toplevel_add_listener(app.top, &top_listener, NULL);
    xdg_toplevel_set_app_id(app.top, app.name);
    xdg_toplevel_set_title(app.top, app.name);
    wl_surface_commit(app.surf);
    while (!app.mapped)
        if (wl_display_dispatch(app.dpy) < 0)
            die("connection lost before mapping");
    wl_display_roundtrip(app.dpy);

    if (app.is_source)
        run_source();
    else {
        say("READY");
        while (!app.done) {
            pump(100);
            if (app.pending_sel && (int32_t)(now_ms() - app.sel_deadline) >= 0) {
                const long N = receive_to_file(app.pending_sel->offer, app.accept);
                say("DATA bytes=%ld", N);
                app.done = 1;
            }
        }
        wl_display_roundtrip(app.dpy);
        say("DONE");
    }
    wl_display_disconnect(app.dpy);
    return 0;
}
