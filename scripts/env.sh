# Source this: common compiler/linker flags for building Windows x64 DLLs.
export L=/opt/llvm22/bin TC=/opt/so6tc DEPS=/opt/so6deps
SDKV=10.0.28000.0
export CFLAGS_WIN="--target=x86_64-pc-windows-msvc /nologo /MT /imsvc $TC/include /imsvc $TC/Include/$SDKV/ucrt /imsvc $TC/Include/$SDKV/um /imsvc $TC/Include/$SDKV/shared /I$DEPS/minhook/include /I$DEPS/OpenXR-SDK/include"
export LDFLAGS_WIN="/nologo /libpath:$TC/lib/x64 /libpath:$TC/Lib/$SDKV/um/x64 /libpath:$TC/Lib/$SDKV/ucrt/x64 /libpath:$DEPS/openxr_loader/x64/lib"
