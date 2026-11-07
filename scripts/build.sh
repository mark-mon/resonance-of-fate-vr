#!/usr/bin/env bash
# Builds the Resonance of Fate VR mod (rofvr) for Windows x64, two DLLs from one source tree:
#   build/xinput1_3.dll    4K/HD edition (Direct3D 11, the game imports xinput1_3)
#   build_ee/xinput9_1_0.dll  Enhanced Remaster (Direct3D 12, the game imports xinput9_1_0)  [-DROFVR_EE]
# Usage: scripts/build.sh [hd|ee|all]   (default all)
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
source "$HERE/scripts/env.sh"
WHAT="${1:-all}"
CF="$CFLAGS_WIN /O2 /Zi /W3 -Wno-unused-function -Wno-microsoft-cast -DWIN32_LEAN_AND_MEAN -DNOMINMAX -D_CRT_SECURE_NO_WARNINGS -DXR_NO_PROTOTYPES -I$HERE/third_party/renderdoc -I$HERE/vrkit -I$HERE/vrkit/third_party/imgui"

build_one() {   # $1 out dir, $2 dll name, $3 def, $4 extra defines
  local OUT="$1" DLL="$2" DEF="$3" DEFS="$4" OBJ="$1/obj"
  mkdir -p "$OBJ"
  for f in buffer.c hook.c trampoline.c hde/hde64.c; do
    $L/clang-cl $CF $DEFS /c /Fo"$OBJ/mh_$(basename "$f" .c).obj" -- "$DEPS/minhook/src/$f"
  done
  local OBJS=()
  for f in "$HERE"/src/*.cpp "$HERE"/vrkit/*.cpp "$HERE"/vrkit/third_party/imgui/imgui*.cpp; do
    case "$f" in *d3d9*|*dx9*) continue;; esac   # D3D9 menu backend: not used
    local o="$OBJ/$(basename "$f" .cpp).obj"
    $L/clang-cl $CF $DEFS /std:c++20 /EHsc /c /Fo"$o" -- "$f"
    OBJS+=("$o")
  done
  $L/lld-link $LDFLAGS_WIN /dll /debug /out:"$OUT/$DLL.dll" /pdb:"$OUT/$DLL.pdb" \
    /def:"$HERE/src/$DEF" "${OBJS[@]}" "$OBJ"/mh_*.obj \
    kernel32.lib user32.lib psapi.lib d3d11.lib dxgi.lib d3dcompiler.lib
  cp "$DEPS/openxr_loader/x64/bin/openxr_loader.dll" "$OUT/"
  echo "built $OUT/$DLL.dll"
}
if [ "$WHAT" = all ] || [ "$WHAT" = hd ]; then build_one "$HERE/build" xinput1_3 xinput1_3.def ""; fi
if [ "$WHAT" = all ] || [ "$WHAT" = ee ]; then build_one "$HERE/build_ee" xinput9_1_0 xinput9_1_0.def "-DROFVR_EE"; fi
