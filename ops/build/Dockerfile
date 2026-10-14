# Build environment for glowbuffer.so, used by scripts/build-remote.sh on the job VPS.
#
# A Hyprland plugin is ABI-bound to the exact Hyprland it loads into: same headers, same
# compiler family. openSUSE Tumbleweed ships both, so the image tracks Tumbleweed and
# build-remote.sh refuses a build whose hyprland-devel differs from the workstation's.
FROM registry.opensuse.org/opensuse/tumbleweed:latest

RUN zypper -n --gpg-auto-import-keys refresh \
 && zypper -n install --no-recommends \
        hyprland-devel gcc-c++ cmake pkgconf-pkg-config \
        'pkgconfig(libsystemd)' 'pkgconfig(pixman-1)' 'pkgconfig(libinput)' \
        'pkgconfig(xkbcommon)' 'pkgconfig(libdrm)' 'pkgconfig(wayland-server)' \
 && zypper clean -a

WORKDIR /work
