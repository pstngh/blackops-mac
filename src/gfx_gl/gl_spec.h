// gl_spec.h — KB_SPEC=<file>: list every D3D9 call, state value, format, lock flag,
// primitive type and shader pairing the renderer uses, with a count each.
//
// The Metal backend implements exactly this list (HANDOFF, next step 0); collect it
// from the main menu and several maps. Off unless KB_SPEC is set: each hook is one
// test of a flag. The file is rewritten every 600 presents and at exit, one
// "<count> <key>" line per key, sorted by key. Shader bytecode goes to
// KB_DUMP_SHADERS=<dir> (gl_shader.cpp), named by the hash the keys use.
#ifndef KISAK_GL_SPEC_H
#define KISAK_GL_SPEC_H

extern bool g_kbSpec;

void KB_SpecNote(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void KB_SpecFlush();
// "R" on the thread that created the device (the render thread), else "T".
const char *KB_SpecThread();
void KB_SpecSetRenderThread();
// FNV-1a of a shader's tokens up to its end token (the dump's file name).
unsigned KB_ShaderHash(const unsigned int *tokens);

#define KB_SPEC(...) do { if (g_kbSpec) KB_SpecNote(__VA_ARGS__); } while (0)

#endif // KISAK_GL_SPEC_H
