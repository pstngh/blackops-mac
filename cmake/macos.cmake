# Native macOS (Apple Silicon) build. Included from portable.cmake after the
# shared source list and include directories are set up.

execute_process(COMMAND brew --prefix OUTPUT_VARIABLE KISAK_BREW_PREFIX
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if (NOT KISAK_BREW_PREFIX)
    set(KISAK_BREW_PREFIX /opt/homebrew)
endif()
# openal-soft is keg-only (macOS ships a deprecated OpenAL.framework).
set(ENV{PKG_CONFIG_PATH} "${KISAK_BREW_PREFIX}/opt/openal-soft/lib/pkgconfig:$ENV{PKG_CONFIG_PATH}")

find_package(PkgConfig REQUIRED)
pkg_check_modules(KISAK_DEPS REQUIRED IMPORTED_TARGET sdl2 glew openal speex vpx)
find_library(KISAK_OPENGL_FRAMEWORK OpenGL REQUIRED)

# <malloc.h> stand-in (macOS has none).
target_include_directories(${BIN_NAME} BEFORE PRIVATE "${SRC_DIR}/platform/compat/darwin")

# x86 SSE headers resolve to sse2neon wrappers on arm64.
if (CMAKE_OSX_ARCHITECTURES MATCHES "arm64")
    target_include_directories(${BIN_NAME} BEFORE PRIVATE "${SRC_DIR}/platform/compat/arm64")
    target_include_directories(${BIN_NAME} PRIVATE "${KISAK_BREW_PREFIX}/include/sse2neon")
endif()

target_compile_definitions(${BIN_NAME} PRIVATE KISAK_MP GL_SILENCE_DEPRECATION)
target_compile_options(${BIN_NAME} PRIVATE
    # The decompiled code type-puns through pointer casts (`*(float *)&i64`) and
    # relies on wrapping signed arithmetic, as MSVC compiled it.
    -fno-strict-aliasing
    -fwrapv
    $<$<COMPILE_LANGUAGE:CXX>:-fms-extensions;-Wno-c++11-narrowing;-include;${SRC_DIR}/platform/compat/msvc_compat.h>
    $<$<COMPILE_LANGUAGE:C>:-D__cdecl=;-D__stdcall=;-D__fastcall=;-D__int8=char;-D__int16=short;-D__int32=int>
    $<$<COMPILE_LANGUAGE:C>:-w>
    # Warnings stay off (decompiled code), except the ones that are 64-bit bugs:
    # a Ptr32<T> field passed through varargs (printf etc.) as an object, and a
    # pointer cast to or from a 32-bit int (-fms-extensions makes the truncating
    # direction a mere warning).
    $<$<COMPILE_LANGUAGE:CXX>:-Wno-everything;-Werror=class-varargs;-Werror=int-to-pointer-cast;-Werror=pointer-to-int-cast;-Werror=pointer-to-enum-cast;-ferror-limit=0>
)

# The Metal backend (src/gfx_mtl; KB_RENDERER=gl picks GL): Objective-C++ with ARC, built with
# its own flags (the engine's MSVC compatibility header and warning policy are for the
# decompiled code).
enable_language(OBJCXX)
file(GLOB KISAK_MTL_SOURCES CONFIGURE_DEPENDS "${SRC_DIR}/gfx_mtl/*.mm" "${SRC_DIR}/gfx_mtl/*.cpp")
add_library(kisak_mtl OBJECT ${KISAK_MTL_SOURCES})
target_include_directories(kisak_mtl PRIVATE "${SRC_DIR}" "${SRC_DIR}/platform/winsdk")
target_link_libraries(kisak_mtl PRIVATE PkgConfig::KISAK_DEPS)
target_compile_options(kisak_mtl PRIVATE -fobjc-arc -Wall -Wno-unused-function)
set_target_properties(kisak_mtl PROPERTIES CXX_STANDARD 20 OBJCXX_STANDARD 20)

target_link_libraries(${BIN_NAME} PRIVATE
    kisak_mtl
    PkgConfig::KISAK_DEPS
    ${KISAK_OPENGL_FRAMEWORK}
    "-framework Metal"
    "-framework QuartzCore"
    "-framework Foundation"
    pthread
    m
)

set_target_properties(${BIN_NAME} PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS ON
    OUTPUT_NAME blackops
)
