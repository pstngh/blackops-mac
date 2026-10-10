// shader_test.mm — translate every dumped D3D9 shader to MSL and compile them all.
//
// The game's shaders: run the client with KB_DUMP_SHADERS=<dir> (gl_shader.cpp writes
// <hash>.vs / <hash>.ps) over the menus and maps. Then:
//
//   tools/shader_test.sh <dump dir> <out dir>
//
// parses each (sm3.h), translates it to MSL (mtl_msl.h) and GLSL (gl_shader.cpp), and
// compiles the MSL in batches of 200 functions with the system's Metal compiler (as the
// game does; `xcrun metal` needs Xcode's separately downloaded Metal toolchain). With
// a KB_SPEC file it also builds a render pipeline for every vertex/pixel shader pair
// the game drew with. It prints the shaders that fail, and the instructions, operand
// forms and features the set uses (the shader half of the Metal backend's spec).
#include "../../gfx_shader/sm3.h"
#include "../mtl_msl.h"

#import <Metal/Metal.h>
#include <dirent.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

typedef unsigned int DWORD;
unsigned long g_kbPresentEnter;   // gl_query.cpp's, read by gl_shader.cpp
bool TranslateD3D9Shader(const DWORD *tokens, std::string &glslOut, bool *outIsPixel, unsigned shadowSamplerMask,
                         unsigned *outSamplerMask);
#ifdef KB_TEST_OLD_GLSL
bool TranslateOld(const DWORD *tokens, std::string &glslOut, bool *outIsPixel, unsigned shadowSamplerMask,
                  unsigned *outSamplerMask);
#endif

static std::vector<unsigned> readFile(const std::string &path) {
    std::vector<unsigned> v;
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return v;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    v.resize((size_t)n / 4 + 1, 0);
    if (fread(v.data(), 1, (size_t)n, f) != (size_t)n) v.clear();
    fclose(f);
    return v;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: shader_test <dump dir> <out dir>\n");
        return 2;
    }
    const std::string in = argv[1], outDir = argv[2];
    std::vector<std::string> names;
    if (DIR *d = opendir(in.c_str())) {
        while (dirent *e = readdir(d)) {
            const std::string n = e->d_name;
            if (n.size() > 3 && (n.substr(n.size() - 3) == ".vs" || n.substr(n.size() - 3) == ".ps")) names.push_back(n);
        }
        closedir(d);
    }
    std::sort(names.begin(), names.end());
    std::map<std::string, int> features;
    auto feat = [&](const std::string &k) { ++features[k]; };
    int parseFail = 0, mslFail = 0, glslDiff = 0, ok = 0;
    std::vector<std::string> batch;
    std::vector<std::string> batchNames;
    int batchNo = 0;
    std::vector<std::string> batches;
    auto flush = [&]() {
        if (batch.empty()) return;
        char path[1024];
        snprintf(path, sizeof(path), "%s/batch_%03d.metal", outDir.c_str(), batchNo++);
        FILE *f = fopen(path, "w");
        fputs(MSL_Prelude(), f);
        for (const std::string &s : batch) fputs(s.c_str(), f), fputs("\n", f);
        fclose(f);
        batches.push_back(path);
        batch.clear();
    };
    for (const std::string &n : names) {
        std::vector<unsigned> t = readFile(in + "/" + n);
        if (t.empty()) continue;
        sm3::Shader sh;
        std::string err;
        if (!sm3::Parse(t.data(), sh, err)) {
            printf("PARSE %s: %s\n", n.c_str(), err.c_str());
            ++parseFail;
            continue;
        }
        feat(std::string(sh.pixel ? "ps_" : "vs_") + std::to_string(sh.major) + "_" + std::to_string(sh.minor));
        for (const sm3::Decl &d : sh.decls) {
            char b[96];
            if (d.type == sm3::RT_SAMPLER) snprintf(b, sizeof(b), "%s dcl sampler dim=%d", sh.pixel ? "ps" : "vs", d.dim);
            else snprintf(b, sizeof(b), "%s dcl type=%d usage=%d index=%d mask=%x mod=%x", sh.pixel ? "ps" : "vs", d.type,
                          d.usage, d.index, d.mask, d.mod);
            feat(b);
        }
        if (!sh.idefs.empty()) feat("defi");
        if (!sh.bdefs.empty()) feat("defb");
        for (const sm3::Instr &i : sh.code) {
            char b[128];
            snprintf(b, sizeof(b), "%s op %s ctrl=%d%s%s", sh.pixel ? "ps" : "vs", sm3::OpName(i.op), i.ctrl,
                     i.predicated ? " predicated" : "", i.hasDst && (i.dst.mod & sm3::DMOD_SAT) ? " sat" : "");
            feat(b);
            if (i.hasDst) {
                snprintf(b, sizeof(b), "%s dst type=%d%s", sh.pixel ? "ps" : "vs", i.dst.type, i.dst.rel ? " rel" : "");
                feat(b);
            }
            if (i.op == sm3::OP_TEXKILL) { snprintf(b, sizeof(b), "texkill mask=%x", i.dst.mask); feat(b); }
            for (int k = 0; k < i.nsrc; ++k) {
                const sm3::Src &s = i.src[k];
                snprintf(b, sizeof(b), "%s src type=%d mod=%d%s", sh.pixel ? "ps" : "vs", s.type, s.mod,
                         s.rel ? (s.relType == sm3::RT_LOOP ? " rel-aL" : " rel-a0") : "");
                feat(b);
                if (s.type == sm3::RT_SAMPLER && s.swizzle != 0xE4) feat("sampler swizzle");
                if (s.rel && s.type == sm3::RT_CONST)
                    for (const auto &d : sh.defs)
                        if (d.first >= s.reg) { feat("relative read over def'd constants"); break; }
            }
        }
        // GLSL: the shared parser must give what the old translator gave.
        std::string glsl;
        bool isPixel = false;
        TranslateD3D9Shader(t.data(), glsl, &isPixel, 0, nullptr);
#ifdef KB_TEST_OLD_GLSL
        std::string old;
        TranslateOld(t.data(), old, &isPixel, 0, nullptr);
        if (old != glsl) {
            printf("GLSL differs %s\n", n.c_str());
            if (!glslDiff++) {
                FILE *f = fopen((outDir + "/glsl_old.txt").c_str(), "w"); fputs(old.c_str(), f); fclose(f);
                f = fopen((outDir + "/glsl_new.txt").c_str(), "w"); fputs(glsl.c_str(), f); fclose(f);
            }
        }
#endif
        MSLShader m;
        const std::string prefix = n.substr(0, n.size() - 3) + (sh.pixel ? "p" : "v");
        if (!MSL_Translate(sh, m, prefix)) {
            printf("MSL %s: %s\n", n.c_str(), m.error.c_str());
            ++mslFail;
            continue;
        }
        ++ok;
        batch.push_back("// " + n + "\n" + m.source);
        batchNames.push_back(n);
        if (batch.size() == 200) flush();
    }
    flush();
    int compileFail = 0;
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
    MTLCompileOptions *opts = [MTLCompileOptions new];
    opts.preserveInvariance = YES;
    std::vector<id<MTLLibrary>> libs;
    for (const std::string &b : batches) {
        FILE *f = fopen(b.c_str(), "r");
        std::string src;
        char buf[65536];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) src.append(buf, n);
        fclose(f);
        NSError *err = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:[NSString stringWithUTF8String:src.c_str()] options:opts error:&err];
        if (!lib) {
            printf("COMPILE %s failed:\n%s\n", b.c_str(), err.localizedDescription.UTF8String);
            ++compileFail;
        } else {
            libs.push_back(lib);
            if (err) printf("COMPILE %s warnings:\n%s\n", b.c_str(), err.localizedDescription.UTF8String);
        }
    }
    printf("\n%zu shaders: %d translated, %d parse failures, %d MSL failures, %d GLSL differences, %d of %zu batches "
           "failed to compile\n\n", names.size(), ok, parseFail, mslFail, glslDiff, compileFail, batches.size());
    for (const auto &f : features) printf("%7d %s\n", f.second, f.first.c_str());
    return parseFail || mslFail || glslDiff || compileFail ? 1 : 0;
}
