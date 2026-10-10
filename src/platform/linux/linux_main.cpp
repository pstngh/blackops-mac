// linux_main.cpp — the Linux entry point + the remaining platform stubs that pull
// the whole engine together into a binary. main() mirrors win32/win_main.cpp's WinMain
// init sequence (critical sections -> main thread/TLS -> Com_Init -> Com_Frame loop).
// The gamepad, worker-thread and streaming symbols are stubbed so
// the engine links and boots; these are progressively replaced as bring-up continues.
#include <qcommon/common.h>
#include <qcommon/threads.h>
#include <qcommon/tl_support.h>
#include <win32/win_main.h>
#include <win32/win_common.h>
#include <win32/win_gamepad.h>
#include <win32/win_workercmds.h>
#include <win32/win_stream.h>
#include <demo/demo_common.h>
#include <physics/phys_broad_phase.h>
#include <universal/dvar.h>
#include <universal/q_parse.h>
#include <universal/timing.h>
#include <client/client.h>
#include <qcommon/cmd.h>
#include <win32/win_shared.h>

#include <cstring>
#include <cstdio>
#include <csignal>
#include <execinfo.h>
#include <cstdlib>

// Crash backtrace — the engine's asserts trap via __debugbreak (SIGILL); print where.
static void CrashHandler(int sig) {
    void *bt[40]; int n = backtrace(bt, 40);
    fprintf(stderr, "\n*** caught signal %d — backtrace (%d frames) ***\n", sig, n);
    backtrace_symbols_fd(bt, n, 2);
    fflush(stderr); _exit(128 + sig);
}

// ---- Gamepad: no controller (SDL game-controller support is future work) ----
const dvar_t *gpad_enabled = nullptr;
bool   GPad_IsActive(int) { return false; }
double GPad_GetButton(int, GamePadButton) { return 0.0; }
double GPad_GetStick(int, GamePadStick) { return 0.0; }
bool   GPad_IsButtonPressed(int, GamePadButton) { return false; }
bool   GPad_IsStickPressed(int, GamePadStick, GamePadStickDir) { return false; }
bool   GPad_IsStickReleased(int, GamePadStick, GamePadStickDir) { return false; }


// ---- Streaming -------------------------------------------------------------------
// R_InitWorkerThreads, IW_task_manager_*, and the nuge_physics job module are the
// real job-queue bring-up, ported to src/platform/linux/linux_workercmds.cpp; the
// stream thread (Stream_Init) is src/win32/win_stream.cpp's.
bool PC_StartWithNoSounds() { return false; }

// ---- vtable/typeinfo anchor for a decompiled polymorphic struct -------------
// Defining the class's key virtual out-of-line makes the compiler emit its vtable
// and typeinfo here (otherwise nothing in the build emits them).
void broad_phase_terrain_query_callback::query(const broad_phase_environment_query_input *, broad_phase_environement_query_results *) {}

// ---- Scripted console commands (testing without a player at the keyboard) ----
// KB_CMDS="20:openscriptmenu team_marinesopfor autoassign|25:+attack|26:-attack"
// runs each command that many seconds after the client becomes active in a map
// (again on every map). Entries after "loop@<start>/<period>" repeat every
// <period> seconds from <start>, their times relative to each repetition.
// KB_CMDS_GAMETIME=1 times them by the game's clock instead (the client's server
// time, from the server's start): a screenshot then shows the same moment of the
// map's animations whatever the load took (tools/bench_views.py).
static void KB_RunScriptedCommands() {
    struct Entry { int ms; const char *cmd; };
    static Entry entries[64];
    static int count = -1, loopFirst, next, loopNext;
    static int startMs, loopStartMs, loopPeriodMs, loopBaseMs;
    if (count < 0) {
        count = 0;
        const char *env = getenv("KB_CMDS");
        if (env) {
            static char buf[4096];
            snprintf(buf, sizeof(buf), "%s", env);
            loopFirst = 64;
            for (char *tok = strtok(buf, "|"); tok && count < 64; tok = strtok(nullptr, "|")) {
                if (!strncmp(tok, "loop@", 5)) {
                    loopFirst = count;
                    loopStartMs = (int)(atof(tok + 5) * 1000.0);
                    const char *slash = strchr(tok, '/');
                    loopPeriodMs = slash ? (int)(atof(slash + 1) * 1000.0) : 0;
                    continue;
                }
                char *colon = strchr(tok, ':');
                if (!colon) continue;
                *colon = 0;
                entries[count].ms = (int)(atof(tok) * 1000.0);
                entries[count].cmd = colon + 1;
                ++count;
            }
            loopNext = loopFirst;
        }
    }
    if (!count) return;
    if (CL_GetLocalClientConnectionState(0) != CA_ACTIVE) {
        startMs = 0;   // the next map starts the list over
        next = 0;
        loopNext = loopFirst;
        return;
    }
    static const bool gameTime = getenv("KB_CMDS_GAMETIME") && *getenv("KB_CMDS_GAMETIME") == '1';
    int now = gameTime ? CL_GetLocalClientGlobals(0)->serverTime : Sys_Milliseconds();
    if (!startMs) {
        startMs = gameTime ? 1 : now;
        loopBaseMs = startMs + loopStartMs;
        if (gameTime) fprintf(stderr, "[kbcmds] active at game time %.1f s\n", now / 1000.0);
    }
    auto run = [&](const Entry &e) {
        fprintf(stderr, "[kbcmds] %.1f s: %s\n", (now - startMs) / 1000.0, e.cmd);
        Cbuf_AddText(0, e.cmd);
        Cbuf_AddText(0, "\n");
    };
    while (next < loopFirst && next < count && now - startMs >= entries[next].ms)
        run(entries[next++]);
    if (loopFirst >= count || loopPeriodMs <= 0 || now < loopBaseMs) return;
    while (loopNext < count && now - loopBaseMs >= entries[loopNext].ms)
        run(entries[loopNext++]);
    if (now - loopBaseMs >= loopPeriodMs) {
        loopBaseMs += loopPeriodMs;
        loopNext = loopFirst;
    }
}

// KB_MENU_CMDS: the same "<seconds>:<command>|..." list, timed from the first frame
// after Com_Init and run once, connected or not (the main menu has no map).
static void KB_RunMenuCommands() {
    struct Entry { int ms; const char *cmd; };
    static Entry entries[64];
    static int count = -1, next, startMs;
    if (count < 0) {
        count = 0;
        const char *env = getenv("KB_MENU_CMDS");
        if (env) {
            static char buf[4096];
            snprintf(buf, sizeof(buf), "%s", env);
            for (char *tok = strtok(buf, "|"); tok && count < 64; tok = strtok(nullptr, "|")) {
                char *colon = strchr(tok, ':');
                if (!colon) continue;
                *colon = 0;
                entries[count].ms = (int)(atof(tok) * 1000.0);
                entries[count].cmd = colon + 1;
                ++count;
            }
        }
        startMs = Sys_Milliseconds();
    }
    int now = Sys_Milliseconds();
    while (next < count && now - startMs >= entries[next].ms) {
        fprintf(stderr, "[kbmenucmds] %.1f s: %s\n", (now - startMs) / 1000.0, entries[next].cmd);
        Cbuf_AddText(0, entries[next].cmd);
        Cbuf_AddText(0, "\n");
        ++next;
    }
}

// ---- Entry point -----------------------------------------------------------
int main(int argc, char **argv) {
    char cmdline[2048] = {0};
    for (int i = 1; i < argc; ++i) {
        size_t n = strlen(cmdline);
        snprintf(cmdline + n, sizeof(cmdline) - n, "%s%s", argv[i], i + 1 < argc ? " " : "");
    }
    fprintf(stderr, "[KisakBlack] boot: cmdline=\"%s\"\n", cmdline);
    signal(SIGILL, CrashHandler); signal(SIGSEGV, CrashHandler); signal(SIGABRT, CrashHandler);
    signal(SIGBUS, CrashHandler);   // macOS reports some bad accesses as SIGBUS
    signal(SIGTRAP, CrashHandler);  // arm64 brk: __debugbreak, and clang's trap on reaching unreachable code

    Sys_InitializeCriticalSections();
    Sys_InitMainThread();
    // As WinMain: Dvar_Init registers sv_cheats and the dvar commands (set, seta,
    // toggle, ...), InitTiming the raw timer scale, Sys_FindInfo the hardware info.
    Com_InitParse();
    Dvar_Init();
    InitTiming();
    Sys_FindInfo();
    Sys_SetupTLCallbacks(0x900000);
    Com_Init(cmdline);

    for (;;) {
        Com_Frame();
        KB_RunScriptedCommands();
        KB_RunMenuCommands();
    }
    return 0;
}
