#!/usr/bin/env bash
# Builds build/xinput1_3.dll (the Resonance of Fate VR mod rofvr, an xinput1_3 proxy) for Windows x64.
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
source "$HERE/scripts/env.sh"
OUT="$HERE/build"; OBJ="$OUT/obj"
mkdir -p "$OBJ"

CF="$CFLAGS_WIN /O2 /Zi /W3 -Wno-unused-function -Wno-microsoft-cast -DWIN32_LEAN_AND_MEAN -DNOMINMAX -D_CRT_SECURE_NO_WARNINGS -DXR_NO_PROTOTYPES -I$HERE/third_party/renderdoc -I$HERE/vrkit -I$HERE/vrkit/third_party/imgui"

# MinHook (C)
for f in buffer.c hook.c trampoline.c hde/hde64.c; do
  $L/clang-cl $CF /c /Fo"$OBJ/mh_$(basename "$f" .c).obj" -- "$DEPS/minhook/src/$f"
done

# Mod sources (C++)
OBJS=()
for f in "$HERE"/src/*.cpp "$HERE"/vrkit/*.cpp "$HERE"/vrkit/third_party/imgui/imgui*.cpp; do
  case "$f" in *d3d9*|*dx9*) continue;; esac   # D3D9 menu backend: not used by this D3D11 game
  o="$OBJ/$(basename "$f" .cpp).obj"
  $L/clang-cl $CF /std:c++20 /EHsc /c /Fo"$o" -- "$f"
  OBJS+=("$o")
done

$L/lld-link $LDFLAGS_WIN /dll /debug /out:"$OUT/xinput1_3.dll" /pdb:"$OUT/xinput1_3.pdb" \
  /def:"$HERE/src/xinput1_3.def" "${OBJS[@]}" "$OBJ"/mh_*.obj \
  kernel32.lib user32.lib psapi.lib d3d11.lib dxgi.lib d3dcompiler.lib

cp "$DEPS/openxr_loader/x64/bin/openxr_loader.dll" "$OUT/"
echo "built $OUT/xinput1_3.dll"
