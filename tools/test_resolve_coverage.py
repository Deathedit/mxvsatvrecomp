"""Test the resolve-coverage bitmask by SLICING IT OUT OF THE HEADER.

`ResolvedTargetByAddress::MarkCoverage` decides whether a resolve destination
has been written enough to be worth binding as a snapshot. Getting its cell
arithmetic wrong is not loud: it yields a plausible percentage, and the only
symptom is a texture that samples black somewhere the guest had written real
data. That is exactly how the terrain deformation buffer sank the ground by
2.008 world units -- see [[floating-bike-is-two-units]].

There is no unit-test harness in this repo and adding one for nine assertions
is not worth it. Instead this EXTRACTS the member functions from
src/hooks/hooks_d3d9_internal.h at run time and splices them into
tools/resolve_coverage_test.cpp. The test therefore exercises the shipped text:
it cannot pass against a copy that has drifted from the header, because there
is no copy.

THE MUTATION CASE IS THE POINT. Seven of these assertions passed the first time
they were written, which by [[prove-the-test-can-fail]] makes them worth
nothing on their own. The last two carry the result -- they run the OLD
bounding-box rule and the NEW area rule over the same 39 scattered 128x32 blits
and require them to DISAGREE:

    box 1152x1024 = 28.1%  ->  old rule CLAIMS it
    area          =  3.0%  ->  new rule REFUSES it

That geometry and those numbers are the ones measured on phys 0x1102F000 in
mx_1750, not invented ones. If a future change makes the two rules agree again,
this goes red.

Usage, from the repo root, with cl.exe on PATH (a VS developer prompt):

    python tools/test_resolve_coverage.py
"""

import io
import os
import subprocess
import sys
import tempfile

HEADER = os.path.join('src', 'hooks', 'hooks_d3d9_internal.h')
HARNESS = os.path.join('tools', 'resolve_coverage_test.cpp')
MARKER = '// @@COVERAGE_BODY@@'

# The slice: from the grid constants through the end of coverage_percent().
# Anchored on declarations rather than line numbers, so ordinary edits above or
# below cannot silently change what gets tested.
BEGIN = '  static constexpr uint32_t kCoverageGrid'
END = '  uint32_t coverage_percent() const {'


def slice_header(path):
    src = io.open(path, encoding='utf-8', newline='').read()
    a = src.index(BEGIN)
    b = src.index(END)
    b = src.index('  }', b) + 4
    return src[a:b]


def main():
    if not (os.path.exists(HEADER) and os.path.exists(HARNESS)):
        print('run me from the repo root')
        return 2

    body = slice_header(HEADER)
    harness = io.open(HARNESS, encoding='utf-8', newline='').read()
    marker_line = [ln for ln in harness.split('\n') if MARKER in ln]
    if not marker_line:
        print('%s no longer contains %s' % (HARNESS, MARKER))
        return 2
    source = harness.replace(marker_line[0], body)

    out = tempfile.mkdtemp(prefix='covtest')
    cpp = os.path.join(out, 'cov_test.cpp')
    exe = os.path.join(out, 'cov_test.exe')
    io.open(cpp, 'w', encoding='utf-8', newline='').write(source)

    build = subprocess.run(
        ['cl', '/nologo', '/std:c++20', '/EHsc', cpp,
         '/Fe:' + exe, '/Fo:' + out + os.sep],
        capture_output=True, text=True)
    if build.returncode:
        print(build.stdout + build.stderr)
        print('-- compile failed; is cl.exe on PATH?')
        return build.returncode

    run = subprocess.run([exe], capture_output=True, text=True)
    print(run.stdout + run.stderr, end='')
    return run.returncode


if __name__ == '__main__':
    sys.exit(main())
