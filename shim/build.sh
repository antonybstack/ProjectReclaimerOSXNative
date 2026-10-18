#!/bin/bash
# Build the Wine-free PE loader. Needs the OpenGL framework for the wgl->CGL
# bridge (the engine's menu renders via OpenGL).
set -e
cd "$(dirname "$0")"
# build to a temp file and rename into place: a running game keeps its (old) binary intact
clang -arch x86_64 -O1 -framework OpenGL -framework Cocoa -framework CoreText -framework CoreGraphics -framework CoreFoundation -framework AudioToolbox -framework IOSurface -o build/peload.new src/peload.c
mv -f build/peload.new build/peload
# the native GL helper (arm64; PELOAD_GL_HELPER=1, shim/docs/gl-helper.md): the renderer's GL calls run here natively
# its pixel-format cache (glhelper_pf.c): a dylib because dyld only interposes from dylibs; linked weakly (optional)
clang -arch arm64 -O2 -dynamiclib -install_name @executable_path/peload-gl-pf.dylib -framework Metal -o build/peload-gl-pf.dylib.new src/glhelper_pf.c
mv -f build/peload-gl-pf.dylib.new build/peload-gl-pf.dylib
clang -arch arm64 -O2 -Wno-deprecated-declarations -framework OpenGL -framework IOSurface -framework CoreFoundation -Wl,-weak_library,build/peload-gl-pf.dylib -o build/peload-gl.new src/glhelper.c
mv -f build/peload-gl.new build/peload-gl
echo "built: build/peload  (run under: arch -x86_64 build/peload <pe> [args])"
