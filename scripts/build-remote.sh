#!/bin/sh
# Build build/glowbuffer.so on a remote job runner instead of this machine.
#
# The plugin compile pulls in Hyprland's whole header tree and holds a core for a while;
# on a workstation that is also the desktop, that belongs elsewhere. VPS_JOB points at the
# runner (a `vps-job`-style CLI: run [-b DIR] [-f PATH] -- CMD). The image is
# ops/build/Dockerfile: Tumbleweed with hyprland-devel.
#
# The plugin must match the Hyprland it loads into, so the build refuses to run when the
# container's hyprland-devel is not the workstation's: a mismatched .so is rejected at load
# time anyway (hash check in PLUGIN_INIT), this only fails earlier and says why.
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

VPS_JOB=${VPS_JOB:-vps-job}
WANT=$(rpm -q --qf '%{VERSION}-%{RELEASE}' hyprland-devel 2>/dev/null || true)
[ -n "$WANT" ] || { echo "build-remote: hyprland-devel is not installed here; nothing to match" >&2; exit 1; }

"$VPS_JOB" run -n glowbuffer -b ops/build -x build -x tests/itest/out -f build/glowbuffer.so -e WANT="$WANT" -- sh -euc '
    HAVE=$(rpm -q --qf "%{VERSION}-%{RELEASE}" hyprland-devel)
    if [ "$HAVE" != "$WANT" ]; then
        echo "build-remote: container has hyprland-devel $HAVE, workstation has $WANT" >&2
        echo "build-remote: update one side (zypper dup here, or rebuild the image there)" >&2
        exit 1
    fi
    g++ --version | head -1
    # Fresh every time: header mtimes come from the package build, so make would keep objects
    # compiled against a different hyprland-devel, and a mixed .so crashes on load.
    rm -rf build
    cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_CXX_COMPILER=g++ >/dev/null
    cmake --build build -j"$(nproc)"
'
ls -l build/glowbuffer.so
