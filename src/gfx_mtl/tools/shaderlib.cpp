// shaderlib.cpp — the sources of the shader library (kb_shaders.metallib): every shader the
// game has, translated to MSL as the Metal backend translates it (MSL_TranslateNamed), the
// pixel shaders also per sample (transparency AA), in files of up to 256 functions for
// `xcrun metal`. tools/make_shaderlib.sh builds and runs it (make_portable.sh calls that).
//
//   shaderlib <dump dir> <out dir>
//
// <dump dir>: the game's shaders as <hash>.vs / <hash>.ps (KB_DUMP_SHADERS, gl_shader.cpp).
// Writes <out>/lib_NNN.metal, <out>/names.txt (the functions, sorted) and prints the set's
// stamp (a hash of the names, so of every function's code), which make_shaderlib.sh compares
// with the stamp of the library it built last.
#include "../../gfx_shader/sm3.h"
#include "../mtl_msl.h"

#include <dirent.h>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <map>
#include <string>
#include <vector>

static std::vector<unsigned> ReadTokens(const std::string &path) {
    std::vector<unsigned> v;
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return v;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    v.resize((size_t)n / 4 + 1, 0);
    if (fread(v.data(), 1, (size_t)n, f) != (size_t)n) v.clear();
    fclose(f);
    return v;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: shaderlib <dump dir> <out dir>\n");
        return 2;
    }
    const std::string in = argv[1], out = argv[2];
    std::vector<std::string> files;
    if (DIR *d = opendir(in.c_str())) {
        while (dirent *e = readdir(d)) {
            const std::string n = e->d_name;
            if (n.size() > 3 && (n.compare(n.size() - 3, 3, ".vs") == 0 || n.compare(n.size() - 3, 3, ".ps") == 0))
                files.push_back(n);
        }
        closedir(d);
    }
    std::sort(files.begin(), files.end());
    if (files.empty()) {
        fprintf(stderr, "shaderlib: no shaders in %s\n", in.c_str());
        return 1;
    }
    std::map<std::string, std::string> functions;   // name -> source (two bytecodes may translate alike)
    int shaders = 0, failed = 0;
    for (const std::string &n : files) {
        std::vector<unsigned> t = ReadTokens(in + "/" + n);
        sm3::Shader sh;
        std::string err;
        if (t.empty() || !sm3::Parse(t.data(), sh, err)) {
            fprintf(stderr, "shaderlib: %s: %s\n", n.c_str(), t.empty() ? "unreadable" : err.c_str());
            ++failed;
            continue;
        }
        ++shaders;
        for (int perSample = 0; perSample <= (sh.pixel ? 1 : 0); ++perSample) {
            MSLShader m;
            if (!MSL_TranslateNamed(sh, m, perSample != 0)) {
                fprintf(stderr, "shaderlib: %s%s: %s\n", n.c_str(), perSample ? " (per sample)" : "", m.error.c_str());
                ++failed;
                continue;
            }
            functions[m.entry] = m.source;
        }
    }
    // The files: the prelude once, then up to 256 functions each.
    int fileNo = 0, inFile = 0;
    FILE *f = nullptr;
    for (const auto &fn : functions) {
        if (!f || inFile == 256) {
            if (f) fclose(f);
            char path[1024];
            snprintf(path, sizeof(path), "%s/lib_%03d.metal", out.c_str(), fileNo++);
            f = fopen(path, "w");
            if (!f) { fprintf(stderr, "shaderlib: can't write %s\n", path); return 1; }
            fputs(MSL_Prelude(), f);
            inFile = 0;
        }
        fputs(fn.second.c_str(), f);
        fputc('\n', f);
        ++inFile;
    }
    if (f) fclose(f);
    unsigned long long stamp = 1469598103934665603ull;
    FILE *names = fopen((out + "/names.txt").c_str(), "w");
    if (!names) { fprintf(stderr, "shaderlib: can't write %s/names.txt\n", out.c_str()); return 1; }
    for (const auto &fn : functions) {
        fprintf(names, "%s\n", fn.first.c_str());
        for (char c : fn.first + "\n") { stamp ^= (unsigned char)c; stamp *= 1099511628211ull; }
    }
    fclose(names);
    fprintf(stderr, "shaderlib: %d shaders, %zu functions in %d files, %d failed\n", shaders, functions.size(), fileNo,
            failed);
    printf("%016llx\n", stamp);
    return failed ? 1 : 0;
}
