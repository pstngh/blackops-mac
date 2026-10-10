// gl_spec.cpp — the KB_SPEC recorder (gl_spec.h).
#include "gl_spec.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unistd.h>

bool g_kbSpec = getenv("KB_SPEC") != nullptr;

namespace {
std::mutex g_specMu;
std::unordered_map<std::string, unsigned long> *g_specKeys;
std::thread::id g_specRenderThread;

void KB_SpecAtExit() { KB_SpecFlush(); }
} // namespace

void KB_SpecNote(const char *fmt, ...) {
    char key[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(key, sizeof(key), fmt, ap);
    va_end(ap);
    std::lock_guard<std::mutex> g(g_specMu);
    if (!g_specKeys) {
        g_specKeys = new std::unordered_map<std::string, unsigned long>;
        atexit(KB_SpecAtExit);
    }
    ++(*g_specKeys)[key];
}

void KB_SpecFlush() {
    const char *spec = getenv("KB_SPEC");
    if (!spec) return;
    // "%p" in the name is the process id (one file per run of a script that starts several).
    std::string name = spec;
    const size_t pid = name.find("%p");
    if (pid != std::string::npos) name.replace(pid, 2, std::to_string((long)getpid()));
    const char *path = name.c_str();
    std::map<std::string, unsigned long> sorted;
    {
        std::lock_guard<std::mutex> g(g_specMu);
        if (!g_specKeys) return;
        sorted.insert(g_specKeys->begin(), g_specKeys->end());
    }
    const std::string tmp = std::string(path) + ".tmp";
    FILE *f = fopen(tmp.c_str(), "w");
    if (!f) return;
    for (const auto &kv : sorted)
        fprintf(f, "%lu %s\n", kv.second, kv.first.c_str());
    fclose(f);
    rename(tmp.c_str(), path);
}

void KB_SpecSetRenderThread() { g_specRenderThread = std::this_thread::get_id(); }

const char *KB_SpecThread() { return std::this_thread::get_id() == g_specRenderThread ? "R" : "T"; }

unsigned KB_ShaderHash(const unsigned int *t) {
    unsigned h = 2166136261u;
    for (size_t n = 0;; ++n) {
        for (int b = 0; b < 4; ++b) { h ^= (t[n] >> (8 * b)) & 0xFF; h *= 16777619u; }
        if (n && t[n] == 0x0000FFFFu) break;
        if (n && (t[n] & 0xFFFFu) == 0xFFFEu) {   // a comment block: hash its words too
            const unsigned len = (t[n] >> 16) & 0x7FFF;
            for (unsigned i = 1; i <= len; ++i)
                for (int b = 0; b < 4; ++b) { h ^= (t[n + i] >> (8 * b)) & 0xFF; h *= 16777619u; }
            n += len;
        }
    }
    return h;
}
