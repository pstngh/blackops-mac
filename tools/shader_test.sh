#!/bin/sh
# Translate every shader the game dumped (KB_DUMP_SHADERS) to MSL and GLSL and compile
# the MSL with the system's Metal compiler: src/gfx_mtl/tests/shader_test.mm.
#   tools/shader_test.sh <dump dir> <out dir> [extra sources, e.g. the old GLSL translator]
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
mkdir -p "$2"
BREW=$(brew --prefix 2>/dev/null || echo /opt/homebrew)
c++ -std=gnu++17 -O1 -w -fobjc-arc \
    -I"$ROOT/src" -I"$ROOT/src/platform/winsdk" -I"$BREW/include" -DGL_SILENCE_DEPRECATION \
    "$ROOT/src/gfx_mtl/tests/shader_test.mm" "$ROOT/src/gfx_shader/sm3.cpp" "$ROOT/src/gfx_mtl/mtl_msl.cpp" \
    "$ROOT/src/gfx_gl/gl_shader.cpp" "$ROOT/src/gfx_gl/gl_spec.cpp" $3 \
    -L"$BREW/lib" -lGLEW -framework OpenGL -framework Metal -framework Foundation -o "$2/shader_test"
"$2/shader_test" "$1" "$2"
