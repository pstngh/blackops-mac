#!/usr/bin/env python3
"""Fixed-view benchmark and screenshot harness for the client (uncapped fps per view).

usage: bench_views.py [options] [-- extra game args]

  --exe PATH       the client (default build_macos/blackops)
  --game DIR       the game folder to run in (default /Users/pstn/Documents/Games/codbo)
  --map M          a map in views.txt (default mp_nuked); 'all' runs every map, one start each
  --views A,B      view names from views.txt (default: all of the map's)
  --seconds S      seconds per view (default 8; the first 2 are not counted)
  --bots           the launcher's managed bots: TDM, 3 allies with the player, 4 axis
  --shots DIR      the console `screenshot` at each view, saved as DIR/<map>_<view>.tga
  --sample S       macOS `sample` for S seconds at each view: <out>/<map>_<view>.sample
  --gpu-trace S    Metal System Trace for S seconds at each view: <out>/<map>_<view>.trace
  --out DIR        logs, samples and traces (default: a new temporary directory)
  --home DIR       the profile (fs_h) to use and keep, e.g. to keep the Metal pipeline cache
                   (players/metal_pipelines) between runs; default a fresh one per map in <out>
  --env K=V        extra environment for the game (repeatable), e.g. KB_NOPREPASS=1

The game starts with a fresh scratch profile (fs_h in <out>, or --home), r_vsync 0 and com_maxfps 0,
joins a team, takes `god` and `noclip`, and sits at each view (`setviewpos`) for S
seconds; KB_FPS=1 gives the fps. The schedule runs on the game's clock
(KB_CMDS_GAMETIME=1), so screenshots of two builds or backends show the same moment of
the map's animations (foliage, flags) even when one loads faster; the match clock, script
movers, particles (they step per frame), `autoassign`'s team and Nuketown's mannequin
still vary, so compare with the closer of two reference runs. Views are positions in tools/views.txt
("<map> <name> x y z yaw pitch"). Other settings are the game folder's config.

Run it with Homebrew's python3 for the ASan build: /usr/bin/python3 is a SIP binary that
drops DYLD_LIBRARY_PATH (--env DYLD_LIBRARY_PATH=build_asan/asanrt).
Compare screenshots with tools/cmp_screens.py, read samples with tools/sample_report.py
and traces with tools/gpu_trace.py report.
"""
import os
import re
import signal
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VIEWS = os.path.join(ROOT, 'tools', 'views.txt')


def read_views():
    views = {}
    for line in open(VIEWS):
        line = line.split('#')[0].split()
        if len(line) == 7:
            views.setdefault(line[0], []).append((line[1], ' '.join(line[2:])))
    return views


def parse_fps(log, names, skip_last):
    """Per view: (fps, avg ms, max ms, swap ms, draws/frame) over its KB_FPS lines."""
    seg, data = None, {n: [] for n in names}
    for line in open(log, errors='replace'):
        m = re.search(r'\[kbcmds\] [\d.]+ s: setviewpos (.*)', line)
        if m:
            seg = next((n for n, p in names.items() if p == m.group(1).strip()), None)
            continue
        if '[kbcmds]' in line and ': quit' in line:
            seg = None
        m = re.search(r'\[fps\] ([\d.]+) fps, frame ([\d.]+) ms avg ([\d.]+) max, swap ([\d.]+) ms avg'
                      r'(?:, (\d+) draws/frame)?', line)
        if m and seg:
            data[seg].append(tuple(float(x) if x else 0.0 for x in m.groups()))
    out = {}
    for n, d in data.items():
        d = d[2:len(d) - skip_last]
        if d:
            out[n] = (sum(x[0] for x in d) / len(d), sum(x[1] for x in d) / len(d), max(x[2] for x in d),
                      sum(x[3] for x in d) / len(d), sum(x[4] for x in d) / len(d))
    return out


def wait_for(log, pattern, proc, timeout):
    end = time.time() + timeout
    while time.time() < end and proc.poll() is None:
        if os.path.exists(log) and pattern in open(log, errors='replace').read():
            return True
        time.sleep(0.25)
    return False


def run_map(args, mapname, views, out):
    seconds = args.seconds
    settle = 2.0
    if args.sample:
        seconds = max(seconds, settle + args.sample + 2)
    if args.gpu_trace:
        seconds = max(seconds, settle + args.gpu_trace + 12)   # xctrace takes a while to attach
    if args.home:
        home = args.home
        subprocess.run(['rm', '-rf', os.path.join(home, 'main', 'screenshots')])
    else:
        home = os.path.join(out, f'home_{mapname}')
        subprocess.run(['rm', '-rf', home])
    os.makedirs(home, exist_ok=True)
    team = 'allies' if args.bots else 'autoassign'
    cmds = [f'8:openscriptmenu team_marinesopfor {team}', '11:openscriptmenu changeclass assault_mp,0',
            '13:god', '13.5:noclip']
    t = 24.0 if args.bots else 15.0   # the managed bots arrive ~10 s in
    for name, pos in views:
        # Again 2 and 4 s later: on some maps the player spawns after the first one
        # (a random spawn point); repeating it in place changes nothing.
        cmds += [f'{t + dt:g}:setviewpos {pos}' for dt in (0, 2, 4) if dt < seconds - 1.5]
        if args.shots:
            cmds.append(f'{t + seconds - 1.2:g}:screenshot')
        t += seconds
    cmds.append(f'{t:g}:quit')
    env = dict(os.environ)
    env.update({'KB_FPS': '1', 'KB_CMDS': '|'.join(cmds), 'KB_CMDS_GAMETIME': '1'})
    for kv in args.env:
        k, v = kv.split('=', 1)
        env[k] = v
    game = ['+set', 'fs_h', home, '+set', 'r_vsync', '0', '+set', 'com_maxfps', '0']
    if args.bots:
        game += ['+set', 'sv_botsAllowMovement', '1', '+set', 'sv_botsPressAttackBtn', '1',
                 '+set', 'sv_botsPressMeleeBtn', '1', '+set', 'scr_bots_managed_spawn', '1',
                 '+set', 'scr_bots_managed_allies', '3', '+set', 'scr_bots_managed_axis', '4',
                 '+set', 'scr_bot_difficulty', 'normal', '+set', 'g_gametype', 'tdm']
    game += args.extra + ['+map', mapname]
    log = os.path.join(out, f'{mapname}.log')
    with open(log, 'w') as f:
        proc = subprocess.Popen([args.exe] + game, cwd=args.game, stdout=f, stderr=subprocess.STDOUT, env=env)
    try:
        for name, pos in views:
            if not (args.sample or args.gpu_trace):
                break
            if not wait_for(log, f'setviewpos {pos}', proc, 180):
                break
            time.sleep(settle)
            base = os.path.join(out, f'{mapname}_{name}')
            if args.sample:
                subprocess.run(['sample', str(proc.pid), str(args.sample), '-file', base + '.sample'],
                               capture_output=True)
            if args.gpu_trace:
                subprocess.run([sys.executable, os.path.join(ROOT, 'tools', 'gpu_trace.py'), 'record',
                                str(proc.pid), str(args.gpu_trace), base + '.trace'])
        proc.wait(timeout=t + 120)
    except subprocess.TimeoutExpired:
        print(f'{mapname}: no quit in time, killed', file=sys.stderr)
    finally:
        if proc.poll() is None:
            proc.send_signal(signal.SIGKILL)
            proc.wait()
    if args.shots:
        os.makedirs(args.shots, exist_ok=True)
        shots = sorted(os.listdir(os.path.join(home, 'main', 'screenshots'))) \
            if os.path.isdir(os.path.join(home, 'main', 'screenshots')) else []
        for (name, pos), shot in zip(views, shots):
            os.replace(os.path.join(home, 'main', 'screenshots', shot), os.path.join(args.shots, f'{mapname}_{name}.tga'))
        if len(shots) != len(views):
            print(f'{mapname}: {len(shots)} screenshots for {len(views)} views', file=sys.stderr)
    res = parse_fps(log, dict(views), 2 if args.shots else 0)
    if 'AddressSanitizer' in open(log, errors='replace').read():
        print(f'{mapname}: AddressSanitizer report in {log}', file=sys.stderr)
    return res


class Args:
    pass


def main():
    a = sys.argv[1:]
    args = Args()
    args.extra = []
    if '--' in a:
        i = a.index('--')
        a, args.extra = a[:i], a[i + 1:]
    def opt(name, default, conv=str):
        if name in a:
            i = a.index(name)
            v = a[i + 1]
            del a[i:i + 2]
            return conv(v)
        return default
    def flag(name):
        if name in a:
            a.remove(name)
            return True
        return False
    args.env = []
    while '--env' in a:
        i = a.index('--env')
        args.env.append(a[i + 1])
        del a[i:i + 2]
    args.exe = os.path.abspath(opt('--exe', os.path.join(ROOT, 'build_macos', 'blackops')))
    args.game = opt('--game', '/Users/pstn/Documents/Games/codbo')
    mapname = opt('--map', 'mp_nuked')
    want = opt('--views', '')
    args.seconds = opt('--seconds', 8.0, float)
    args.bots = flag('--bots')
    args.shots = opt('--shots', '')
    args.shots = os.path.abspath(args.shots) if args.shots else ''
    args.sample = opt('--sample', 0.0, float)
    args.gpu_trace = opt('--gpu-trace', 0.0, float)
    args.home = opt('--home', '')
    args.home = os.path.abspath(args.home) if args.home else ''
    out = opt('--out', '') or tempfile.mkdtemp(prefix='bench_views_')
    out = os.path.abspath(out)
    if a:
        sys.exit(f'unknown arguments: {a}\n{__doc__}')
    os.makedirs(out, exist_ok=True)
    views = read_views()
    maps = sorted(views) if mapname == 'all' else [mapname]
    print(f'output in {out}')
    print(f'{"map":16s} {"view":10s} {"fps":>7s} {"ms":>6s} {"max":>6s} {"swap":>5s} {"draws":>6s}')
    for m in maps:
        if m not in views:
            sys.exit(f'{m} has no views in {VIEWS}')
        vs = [(n, p) for n, p in views[m] if not want or n in want.split(',')]
        res = run_map(args, m, vs, out)
        for n, p in vs:
            if n in res:
                fps, ms, mx, swap, draws = res[n]
                print(f'{m:16s} {n:10s} {fps:7.1f} {ms:6.2f} {mx:6.2f} {swap:5.2f} {draws:6.0f}', flush=True)
            else:
                print(f'{m:16s} {n:10s}   (no fps lines)', flush=True)


if __name__ == '__main__':
    main()
