#include <cstdint>
#include <cstdio>
#include <algorithm>
// Struct body SLICED OUT OF src/hooks/hooks_d3d9_internal.h at test time, so
// this exercises the shipped text rather than a copy that can drift.
struct Cov {
  uint32_t width = 0, height = 0;
// @@COVERAGE_BODY@@ -- spliced in by tools/test_resolve_coverage.py
};
static int fails = 0;
static void chk(bool ok, const char* what, long long got, long long want) {
  if (!ok) { std::printf("  FAIL %s: got %lld want %lld\n", what, got, want); ++fails; }
  else std::printf("  ok   %s (%lld)\n", what, got);
}
int main() {
  { Cov c; c.width=2048; c.height=2048; c.MarkCoverage(0,0,2048,2048);
    chk(c.coverage_percent()==100, "full 2048 resolve = 100%", c.coverage_percent(), 100); }

  { Cov c; c.width=2048; c.height=2048;
    for (int i=0;i<4;++i) c.MarkCoverage(0,0,2048,2048);
    chk(c.coverage_percent()==100 && c.covered_cells==4096,
        "repeated full resolves saturate", c.covered_cells, 4096); }

  { Cov c; c.width=2048; c.height=2048;
    for (uint32_t y=0;y<2048;y+=256) c.MarkCoverage(0,y,2048,y+256);
    chk(c.coverage_percent()==100, "full-width bands = 100%", c.coverage_percent(), 100); }

  { Cov c; c.width=129; c.height=129; c.MarkCoverage(0,0,129,129);
    chk(c.coverage_percent()==100, "129x129 full (tail cell)", c.coverage_percent(), 100); }

  { Cov c; c.width=1280; c.height=720; c.MarkCoverage(0,0,1280,720);
    chk(c.coverage_percent()==100, "1280x720 full", c.coverage_percent(), 100); }

  { Cov c; c.width=2048; c.height=2048; c.MarkCoverage(768,224,768+128,224+32);
    chk(c.covered_cells==4, "one 128x32 blit marks its 4 cells", c.covered_cells, 4); }

  // Inward rounding, stated as a property rather than left to be discovered:
  // a blit SHORTER than one cell and not aligned to the cell grid marks
  // nothing. That is the safe direction for a rule that decides whether to
  // trust a snapshot, and real render targets resolve whole or in edge-aligned
  // bands, which the clamps above credit in full.
  { Cov c; c.width=2048; c.height=2048; c.MarkCoverage(768,225,768+128,225+32);
    chk(c.covered_cells==0, "unaligned sub-cell blit marks nothing (by design)",
        c.covered_cells, 0); }

  { Cov c; c.width=2048; c.height=2048;
    uint32_t bx=0,by=0; int n=0;
    // 39 blits of 128x32 laid out to reproduce the box measured on
    // phys 0x1102F000 in mx_1750: 1152 x 1024+.
    for (uint32_t y=224;y<=992 && n<39;y+=192)
      for (uint32_t x=0;x<=1024 && n<39;x+=128,++n) {
        c.MarkCoverage(x,y,x+128,y+32);
        bx=std::max(bx,x+128); by=std::max(by,y+32);
      }
    const unsigned long long box=(unsigned long long)bx*by, full=2048ull*2048;
    std::printf("  scatter: %d blits, box %ux%u = %.1f%%, real %u%%\n",
                n, bx, by, 100.0*box/full, c.coverage_percent());
    chk(box*4 >= full, "MUTATION: old box rule CLAIMS this scatter",
        (long long)(100*box/full), 25);
    chk(c.covered_cells*4 < c.total_cells(), "new area rule REFUSES it",
        c.coverage_percent(), 25); }
  if (fails) std::printf("\nFAILED (%d)\n", fails);
  else std::printf("\nall passed\n");
  return fails != 0;
}
