#!/usr/bin/env python3
"""Build and run every test in this repo.

THIS EXISTS BECAUSE CMakeLists.txt REFERENCES NO TEST FILE. Every test here is
ad hoc, built by hand from a comment in its own header, so nothing runs them
unless someone deliberately does -- and a test that stops compiling goes on
looking like coverage indefinitely.

That is not hypothetical. 4dd1790 (2026-08-06) moved the GPU code from
mx::pm4 to mx::hle and silently killed two of the six. They stayed dead for 25
days. When they were finally built, BOTH had wrong expectations rather than
wrong code -- each asserted a behaviour that was later reverted or corrected
and could not complain:

  - d3d9_layout_test asserted the endian narrowing reverted by 332eda4, one
    day after the test stopped compiling
  - ucode_test read the scalar constant operand X-relative, against an
    interpreter that correctly reads it W-relative (the Xenos AB = WX rule)

So: A COMPILE FAILURE IS A TEST FAILURE here, reported as loudly as a red
assertion. That is the case this script exists to catch.

Two rules taken from the guard census, for the same reasons:

  - nothing is skipped silently. A test that cannot run says so by name, with
    what was missing, and the summary prints "ran N of M" so the denominator
    is never lost.
  - a skip is not a pass, and is never folded into one.

Usage, from the repo root:

    py -3 tools/run_tests.py            # everything
    py -3 tools/run_tests.py ucode      # only names containing "ucode"

Exit status is 0 only if every test ran AND passed; 1 if any failed; 2 if
nothing failed but something could not be run.
"""

import glob
import os
import shutil
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SDK_INC = os.environ.get('REXGLUE_SDK_INCLUDE', 'C:/rexglue-sdk/include')
SDK_LIB = os.environ.get('REXGLUE_SDK_LIB', 'C:/rexglue-sdk/lib')
SDK_BIN = os.environ.get('REXGLUE_SDK_BIN', 'C:/rexglue-sdk/bin')


def find_clang():
    """clang++, which is NOT on PATH in this environment.

    It ships inside the Visual Studio tree. The glob is version-agnostic so a
    VS update does not quietly turn every C++ test into a skip.
    """
    override = os.environ.get('REXGLUE_CLANG')
    if override:
        return override if os.path.isfile(override) else None
    found = shutil.which('clang++')
    if found:
        return found
    pats = [
        'C:/Program Files/Microsoft Visual Studio/*/*/VC/Tools/Llvm/x64/bin/clang++.exe',
        'C:/Program Files (x86)/Microsoft Visual Studio/*/*/VC/Tools/Llvm/x64/bin/clang++.exe',
    ]
    for pat in pats:
        hits = sorted(glob.glob(pat))
        if hits:
            return hits[-1]
    return None


def find_vcvars():
    """vcvars64.bat, needed by the tests that shell out to cl.exe."""
    override = os.environ.get('REXGLUE_VCVARS')
    if override:
        return override if os.path.isfile(override) else None
    pats = [
        'C:/Program Files/Microsoft Visual Studio/*/*/VC/Auxiliary/Build/vcvars64.bat',
        'C:/Program Files (x86)/Microsoft Visual Studio/*/*/VC/Auxiliary/Build/vcvars64.bat',
    ]
    for pat in pats:
        hits = sorted(glob.glob(pat))
        if hits:
            return hits[-1]
    return None


# name -> (sources, extra compile flags, link args, PATH additions to RUN it)
#
# The source lists are the ones that actually link. Both header build lines
# were incomplete when this was written -- d3d9_layout_test needs
# shader_ucode.cpp and ucode_test needs shader_alu.cpp, neither of which its
# own header mentions. That is its own evidence that nobody had run them.
CPP_TESTS = {
    'd3d9_layout': (
        ['tools/d3d9_layout_test.cpp',
         'src/gpu/d3d9_layout.cpp',
         'src/gpu/shader_ucode.cpp'],
        [], [], [],
    ),
    'ucode': (
        ['tools/ucode_test.cpp',
         'src/gpu/shader_ucode.cpp',
         'src/gpu/shader_alu.cpp'],
        [], [], [],
    ),
    'd3d9_texture': (
        ['tools/d3d9_texture_test.cpp',
         'src/gpu/d3d9_texture.cpp'],
        [],
        [os.path.join(SDK_LIB, 'rexruntime.lib')],
        [SDK_BIN],   # the tiling helpers live in rexruntime.dll
    ),
    'health': (
        ['tools/health_test.cpp',
         'src/gpu/health.cpp'],
        # FMT_HEADER_ONLY because the fmt library itself is linked by
        # rexglue_setup_target, which a standalone build does not run. health
        # deliberately has no logging dependency, so those two flags are all it
        # needs -- see the note in health.cpp on why the WARN lives in the
        # caller.
        ['-DFMT_HEADER_ONLY', '-D_CRT_SECURE_NO_WARNINGS'],
        [], [],
    ),
}

# name -> (script, needs cl.exe)
PY_TESTS = {
    'resolve_coverage': ('tools/test_resolve_coverage.py', True),
    'unproject_mesh': ('tools/test_unproject_mesh.py', False),
}


class Result:
    def __init__(self, name, state, detail='', output=''):
        self.name = name
        self.state = state         # 'pass' | 'fail' | 'skip'
        self.detail = detail
        self.output = output


def last_line(text):
    for line in reversed(text.strip().splitlines()):
        if line.strip():
            return line.strip()
    return '(no output)'


def run_cpp(name, outdir, clang):
    sources, defines, extra, path_add = CPP_TESTS[name]
    missing = [s for s in sources if not os.path.isfile(os.path.join(REPO, s))]
    if missing:
        return Result(name, 'skip', 'missing source: ' + ', '.join(missing))
    for lib in extra:
        if not os.path.isfile(lib):
            return Result(name, 'skip', 'missing library: ' + lib)

    exe = os.path.join(outdir, name + '_test.exe')
    cmd = ([clang, '-std=c++23'] + defines
           + ['-I', 'src', '-I', SDK_INC, '-o', exe] + sources + extra)
    build = subprocess.run(cmd, cwd=REPO, capture_output=True, text=True)
    if build.returncode != 0:
        # THE case this script exists for. A test that no longer compiles is a
        # failure, never a skip: it is indistinguishable from coverage
        # otherwise, which is exactly how two of these rotted for 25 days.
        return Result(name, 'fail', 'DOES NOT COMPILE',
                      build.stdout + build.stderr)

    env = dict(os.environ)
    if path_add:
        env['PATH'] = os.pathsep.join(path_add) + os.pathsep + env.get('PATH', '')
    run = subprocess.run([exe], cwd=REPO, capture_output=True, text=True,
                         env=env)
    out = run.stdout + run.stderr
    if run.returncode != 0:
        return Result(name, 'fail', 'exit %d' % run.returncode, out)
    # The verdict comes from the test's OWN stdout. Reading it off the combined
    # streams let unrelated stderr -- vcvars complaining it cannot find
    # vswhere, for one -- land in the summary as though it were the result.
    return Result(name, 'pass', last_line(run.stdout), out)


def run_py(name, vcvars):
    script, needs_cl = PY_TESTS[name]
    if not os.path.isfile(os.path.join(REPO, script)):
        return Result(name, 'skip', 'missing script: ' + script)

    if needs_cl and not shutil.which('cl'):
        if not vcvars:
            return Result(name, 'skip',
                          'needs cl.exe and no vcvars64.bat was found')
        # Re-enter through a developer shell rather than reporting a skip: the
        # tool exists, it is just not on this PATH.
        #
        # Via a temp .bat, not `cmd /c "<line>"`. Both vcvars64.bat and
        # python.exe sit under "Program Files", so the command line needs
        # embedded quotes, and subprocess quotes the whole argument again on
        # top of them -- cmd then sees \"C:/...\" and reports the batch file as
        # an unrecognised command. A file has no command line to re-quote.
        bat = os.path.join(tempfile.gettempdir(), 'mx_run_%s.bat' % name)
        with open(bat, 'w') as f:
            f.write('@echo off\r\n')
            f.write('call "%s" >nul\r\n' % vcvars)
            f.write('"%s" "%s"\r\n' % (sys.executable, script))
        try:
            run = subprocess.run(['cmd', '/c', bat], cwd=REPO,
                                 capture_output=True, text=True)
        finally:
            try:
                os.remove(bat)
            except OSError:
                pass
    else:
        run = subprocess.run([sys.executable, script], cwd=REPO,
                             capture_output=True, text=True)

    out = run.stdout + run.stderr
    if run.returncode != 0:
        return Result(name, 'fail', 'exit %d' % run.returncode, out)
    return Result(name, 'pass', last_line(run.stdout), out)


def main():
    wanted = [a.lower() for a in sys.argv[1:]]

    def selected(name):
        return not wanted or any(w in name.lower() for w in wanted)

    names = ([n for n in CPP_TESTS if selected(n)]
             + [n for n in PY_TESTS if selected(n)])
    if not names:
        print('no test matches: ' + ' '.join(wanted))
        print('known: ' + ', '.join(list(CPP_TESTS) + list(PY_TESTS)))
        return 1

    clang = find_clang()
    vcvars = find_vcvars()
    print('repo   ' + REPO)
    print('clang  ' + (clang or 'NOT FOUND'))
    print('vcvars ' + (vcvars or 'not found'))
    print('')

    results = []
    with tempfile.TemporaryDirectory(prefix='mx_tests_') as outdir:
        for name in names:
            print('--- ' + name, flush=True)
            if name in CPP_TESTS:
                if not clang:
                    results.append(Result(name, 'skip', 'clang++ not found'))
                else:
                    results.append(run_cpp(name, outdir, clang))
            else:
                results.append(run_py(name, vcvars))
            r = results[-1]
            print('    %-5s %s' % (r.state.upper(), r.detail), flush=True)

    failed = [r for r in results if r.state == 'fail']
    skipped = [r for r in results if r.state == 'skip']

    for r in failed:
        print('')
        print('=' * 72)
        print('FAILED: %s -- %s' % (r.name, r.detail))
        print('=' * 72)
        print(r.output.rstrip())

    print('')
    # The denominator, always. "3 passed" without it is the shape of report
    # this project has been burned by more than once.
    print('ran %d of %d selected: %d passed, %d failed, %d could not run'
          % (len(results) - len(skipped), len(results),
             len(results) - len(failed) - len(skipped), len(failed),
             len(skipped)))
    if skipped:
        print('')
        print('COULD NOT RUN -- this is not a pass:')
        for r in skipped:
            print('  %-16s %s' % (r.name, r.detail))

    if failed:
        return 1
    return 2 if skipped else 0


if __name__ == '__main__':
    sys.exit(main())
