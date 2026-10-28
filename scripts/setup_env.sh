#!/usr/bin/env bash
# Rebuilds the Linux -> Windows x64 cross-compile environment for the SO6 VR mod.
# Usage: setup_env.sh /path/to/toolchain.zip
# Result: LLVM 22 in /opt/llvm22, MSVC STL + Windows SDK in /opt/so6tc, deps in /opt/so6deps
set -euo pipefail
ZIP="${1:?path to toolchain.zip}"
LLVM_VER=22.1.3
OPENXR_VER=1.1.65

retry_get() { # url out
  for i in $(seq 1 15); do curl -sSL -C - -o "$2" "$1" && return 0; echo "retry $i"; sleep 3; done; return 1; }

# 1. LLVM (clang-cl / lld-link). The MSVC STL in toolchain.zip requires Clang >= 22.
if [ ! -x /opt/llvm22/bin/clang-cl ]; then
  tmp=$(mktemp -d)
  retry_get "https://github.com/llvm/llvm-project/releases/download/llvmorg-$LLVM_VER/LLVM-$LLVM_VER-Linux-X64.tar.xz" "$tmp/llvm.tar.xz"
  mkdir -p /opt/llvm22
  tar -xJf "$tmp/llvm.tar.xz" -C /opt/llvm22 --strip-components=1 --wildcards \
    '*/bin/clang' '*/bin/clang-22' '*/bin/clang-cl' '*/bin/lld' '*/bin/lld-link' '*/bin/llvm-rc' \
    '*/bin/llvm-lib' '*/bin/llvm-ar' '*/bin/llvm-objdump' '*/bin/llvm-readobj' '*/bin/llvm-pdbutil' \
    '*/bin/llvm-mt' '*/bin/llvm-cvtres' '*/lib/clang/*'
  rm -rf "$tmp"
fi

# 2. MSVC STL/CRT + Windows SDK, with case aliases (Linux is case-sensitive, Windows headers are not).
if [ ! -d /opt/so6tc/Include ]; then
  mkdir -p /opt/so6tc && unzip -q -o "$ZIP" -d /opt/so6tc
  python3 - <<'PY'
import os,re
root='/opt/so6tc'
dirs={}
for dp,_,fns in os.walk(root): dirs[dp]={f.lower():f for f in fns}
for dp,m in dirs.items():                      # lowercase aliases for every file
    for lo,f in m.items():
        if lo!=f and not os.path.lexists(os.path.join(dp,lo)): os.symlink(f,os.path.join(dp,lo))
inc=re.compile(rb'#\s*include\s*[<"]([^>"]+)[>"]'); names=set()
for dp,m in dirs.items():                      # aliases for every case spelling used by #include
    for f in m.values():
        if f.lower().endswith(('.h','.hpp','.inl')) or '.' not in f:
            try: names.update(x.decode(errors='ignore').replace('\\','/') for x in inc.findall(open(os.path.join(dp,f),'rb').read()))
            except OSError: pass
for n in names:
    b=os.path.basename(n)
    for dp,m in dirs.items():
        if b.lower() in m and m[b.lower()]!=b and not os.path.lexists(os.path.join(dp,b)):
            os.symlink(m[b.lower()],os.path.join(dp,b))
PY
fi

# 3. Third-party deps: MinHook (hooking), OpenXR headers + prebuilt Windows loader.
mkdir -p /opt/so6deps && cd /opt/so6deps
[ -d minhook ] || git clone -q --depth 1 https://github.com/TsudaKageyu/minhook.git
[ -d OpenXR-SDK ] || git clone -q --depth 1 --branch release-$OPENXR_VER https://github.com/KhronosGroup/OpenXR-SDK.git
if [ ! -d openxr_loader ]; then
  retry_get "https://github.com/KhronosGroup/OpenXR-SDK-Source/releases/download/release-$OPENXR_VER/openxr_loader_windows-$OPENXR_VER.zip" loader.zip
  unzip -q -o loader.zip -d openxr_loader && rm loader.zip
fi
echo "Environment ready: /opt/llvm22 /opt/so6tc /opt/so6deps"
