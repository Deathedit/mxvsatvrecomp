// Xenos microcode -> HLSL: emit, compile, cache, and the coverage census.
//
// Split verbatim out of hooks_d3d9.cpp. This is the translated shader path's
// back half -- everything between "we have the guest's microcode" and "we have
// a DXBC blob to bind" -- and it shares nothing with draw submission except the
// two lookups the rest of the layer does through the header.
//
// Chosen by measuring both directions across the seam, which is the only way to
// tell a cheap cut from an expensive one in a file with no anonymous namespaces:
//
//   914 lines out
//   5 names defined here are used elsewhere; 4 were already published
//   6 names used here are defined elsewhere; 3 were already published
//   -> 3 new symbols total, for 914 lines
//
// By comparison the reporting block would have needed 61 names published to
// move, which is the condition hooks_d3d9_internal.h exists to prevent.
//
// The whole compile-worker cluster moves as a unit -- queue, mutex, condvar,
// stop flag, thread and both counters -- so its shutdown behaviour is exactly
// what it was. That behaviour has a pre-existing defect: g_compileStop is never
// assigned true, so the worker never returns and the thread is never joined.
// Not touched here; a move must not change what it moves.

#include "hooks/hook_common.h"

#include <d3dcompiler.h>
#include <wrl/client.h>

#include <rex/graphics/format/ucode.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "gpu/hle_types.h"
#include "gpu/shader_hlsl.h"    // EmitShaderHlsl
#include "gpu/shader_ucode.h"
#include "hooks/hooks_d3d9_shared.h"
#include "hooks/hooks_d3d9_shader_compile.h"

namespace mx::hooks::d3d9 {

// Vertex fetch translation coverage, keyed by refusal reason. Moved here with
// the emitter: after the split these are read and written only by this file.
std::map<std::string, uint64_t> g_vfetchRefused;
uint64_t g_vfetchCompiled = 0;

//===========================================================================
// Emitter coverage, measured before anything renders through it, because the
// whole plan rests on an untested claim: that a straight-line HLSL emitter can
// carry this game's shaders. Per distinct shader handle, not per draw, or a hot
// shader translating 12,000 times drowns out a cold one that fails.
//===========================================================================
struct HlslCoverage {
  uint64_t ok = 0;              // emitted AND compiled
  uint64_t compile_failed = 0;  // emitted, but FXC rejected the source
  std::map<std::string, uint64_t> failures;      // status name -> shaders
  std::map<uint32_t, uint64_t> blocking_opcode;  // opcode -> shaders
  // Shaders (not blocks) carrying a conditional exec we run unconditionally,
  // split by mechanism because the two need different fixes: p0 is evaluable
  // today, a bool constant is not (no bank exists). Counted at run level as well
  // as per dump.
  uint64_t predicated = 0;    // p0-gated, seen
  uint64_t p0_honoured = 0;   // --¦of which fully obeyed (all blocks emitted)
  uint64_t bool_gated = 0;    // bool-constant-gated
};
HlslCoverage g_hlslVs, g_hlslPs;
// Handle -> a hash of the GUEST MICROCODE that handle carried when it was
// translated. Was `map<uint32_t, bool>`, i.e. "have we ever seen this handle",
// which is wrong because a handle is an ADDRESS: the guest frees shaders on a
// map unload and allocates the next map's at recycled addresses, so the second
// shader to land on an address was never translated and g_translatedVs[handle]
// went on serving the PREVIOUS shader's translation. That is why the bike's tyre
// read fog out of a UV -- the pixel stage wants fog at interpolator 4 (a 5-export
// vertex variant) and the stale vertex shader was the 6-export one.
std::map<uint32_t, uint64_t> g_hlslReportedVs, g_hlslReportedPs;

// logs/hlsldump, emptied once per process before the first file of the run.
// These dumps are named by guest shader HANDLE, an address that varies per run,
// so a stale file neither collides with nor is overwritten by the current run's
// -- a FAILED_ dump written by an earlier binary was once read as evidence about
// the current one. Cleared lazily at the first dump rather than at startup, so a
// run that translates nothing leaves the previous run's files to be read.
void EnsureHlslDumpDir() {
  static const bool s_cleared = [] {
    std::error_code ec;
    std::filesystem::remove_all("logs/hlsldump", ec);
    return true;
  }();
  (void)s_cleared;
  std::error_code ec;
  std::filesystem::create_directories("logs/hlsldump", ec);
}

// Persisted DXBC cache, keyed by the EMITTED HLSL rather than the shader object
// handle. Bink re-creates its shader objects for every video, so a handle-keyed
// cache misses at every video start and pays FXC again -- 18-145ms per shader at
// O0, which is the Bink-start hang.
//
// Keyed on the SOURCE, not on the guest microcode it was translated from: the
// cached bytes are the output of EmitShaderHlsl, so hashing the microcode leaves
// every already-cached shader loading stale DXBC after an emitter change while
// the log reports a healthy hit rate. Hashing the source makes the key change
// whenever the emitter does, with no version stamp to remember to bump.
//
// Lives under userdata/cache so nothing wipes it between runs.
namespace {
uint64_t g_dxbcCacheHits = 0;
uint64_t g_dxbcCacheMisses = 0;

uint64_t ShaderSourceKey(mx::hle::HlslStage stage, const std::string& source) {
  uint64_t h = 1469598103934665603ull;
  h ^= (stage == mx::hle::HlslStage::kPixel ? 0xA5A5ull : 0x3C3Cull);
  for (const char c : source) {
    h ^= uint64_t(uint8_t(c));
    h *= 1099511628211ull;
  }
  return h;
}

// The guest's own name for a shader, keyed on the same code_key
// ReportHlslCoverage already computes. Built offline by
// tools/shader_manifest.py, which joins microcode to the .shader assets by
// content; see that file for how and for what it cannot reach.
//
// Loaded once. Missing or empty is not an error -- every shader simply reports
// unnamed, and the census says so rather than the tree pretending otherwise.
const std::unordered_map<uint64_t, std::string>& ShaderNames() {
  static const std::unordered_map<uint64_t, std::string> names = [] {
    std::unordered_map<uint64_t, std::string> m;
    std::ifstream f("userdata/shader_names.txt");
    if (!f) {
      REXLOG_INFO("d3d9: userdata/shader_names.txt absent -- shaders will "
                  "report unnamed. Build it with tools/shader_manifest.py");
      return m;
    }
    std::string line;
    while (std::getline(f, line)) {
      const size_t tab = line.find('\t');
      if (tab == std::string::npos || tab != 16) continue;
      char* end = nullptr;
      const uint64_t key =
          std::strtoull(line.substr(0, tab).c_str(), &end, 16);
      if (!end || *end) continue;
      std::string name = line.substr(tab + 1);
      while (!name.empty() &&
             (name.back() == '\r' || name.back() == '\n'))
        name.pop_back();
      m.emplace(key, std::move(name));
    }
    REXLOG_INFO("d3d9: shader names loaded: {} entries", m.size());
    return m;
  }();
  return names;
}

const std::string* ShaderNameFor(uint64_t code_key) {
  const auto& m = ShaderNames();
  const auto it = m.find(code_key);
  return it == m.end() ? nullptr : &it->second;
}

std::string ShaderCachePath(mx::hle::HlslStage stage, uint64_t key) {
  return fmt::format("userdata/cache/shaders/{}_{:016X}.dxbc",
                     stage == mx::hle::HlslStage::kPixel ? "ps" : "vs", key);
}

// The DXBC container declares its own total size at byte offset 24. Validating
// THAT rather than the magic is the difference between catching a truncated file
// and waving it through: writing straight to the final path with `trunc` leaves
// a process killed mid-write with a file that is truncated BUT STILL STARTS WITH
// "DXBC", passes validation on every later run, and is handed to
// CreateGraphicsPipelineState as a corrupt blob -- self-perpetuating, since
// nothing rewrites an entry that already exists.
constexpr size_t kDxbcHeaderBytes = 32;
constexpr size_t kDxbcTotalSizeOffset = 24;

std::shared_ptr<const std::vector<uint8_t>> LoadShaderDxbc(
    mx::hle::HlslStage stage, uint64_t key) {
  std::ifstream f(ShaderCachePath(stage, key), std::ios::binary);
  if (!f) return nullptr;
  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)),
                             std::istreambuf_iterator<char>());
  if (bytes.size() < kDxbcHeaderBytes ||
      std::memcmp(bytes.data(), "DXBC", 4) != 0)
    return nullptr;
  uint32_t declared = 0;
  std::memcpy(&declared, bytes.data() + kDxbcTotalSizeOffset, sizeof(declared));
  if (declared != bytes.size()) {
    // Truncated or overlong. Remove it so the next run recompiles instead of
    // reading the same bad bytes forever, and say so -- a cache entry that
    // silently disappears is worse than one that explains itself.
    REXLOG_WARN("d3d9: DXBC cache entry {} declares {} bytes but is {}; "
                "discarding and recompiling",
                ShaderCachePath(stage, key), declared, bytes.size());
    std::error_code rm;
    std::filesystem::remove(ShaderCachePath(stage, key), rm);
    return nullptr;
  }
  return std::make_shared<const std::vector<uint8_t>>(std::move(bytes));
}

// Written to a temporary file and RENAMED into place, so the final path only
// ever holds a complete blob. rename is atomic within a directory, so a crash
// mid-write leaves a stray .tmp that Load will not read. The temp name carries
// the thread id as well as the key, because two workers may race on one key.
void SaveShaderDxbc(mx::hle::HlslStage stage, uint64_t key, ID3DBlob* blob) {
  if (!blob || !blob->GetBufferPointer() || blob->GetBufferSize() == 0) return;
  std::error_code ec;
  std::filesystem::create_directories("userdata/cache/shaders", ec);
  const std::string final_path = ShaderCachePath(stage, key);
  const std::string tmp_path =
      fmt::format("{}.{:X}.tmp", final_path, GetCurrentThreadId());
  {
    std::ofstream f(tmp_path, std::ios::trunc | std::ios::binary);
    if (!f) return;
    f.write(static_cast<const char*>(blob->GetBufferPointer()),
            std::streamsize(blob->GetBufferSize()));
    f.flush();
    // Only a fully written file earns the rename. A failed stream here leaves
    // the previous entry -- or no entry -- untouched.
    if (!f) {
      std::error_code rm;
      std::filesystem::remove(tmp_path, rm);
      return;
    }
  }
  std::error_code mv;
  std::filesystem::rename(tmp_path, final_path, mv);
  if (mv) {
    std::error_code rm;
    std::filesystem::remove(tmp_path, rm);
  }
}
}  // namespace

std::string HlslCoverageSummary(const HlslCoverage& c) {
  std::string s = fmt::format("{} translated+compiled", c.ok);
  // Both printed unconditionally, zero included: zero here is the finding that
  // makes the setp_* value translation safe, and an absent line would read as
  // "not measured" rather than "none". P0 is fixable now; BOOL needs a constant
  // bank first.
  s += fmt::format(", P0-EXEC={} (honoured {}), BOOL-EXEC={}", c.predicated,
                   c.p0_honoured, c.bool_gated);
  if (c.compile_failed) s += fmt::format(", FXC-REJECTED={}", c.compile_failed);
  for (const auto& [why, n] : c.failures) s += fmt::format(", {}={}", why, n);
  if (!c.blocking_opcode.empty()) {
    s += "; blocking opcodes";
    for (const auto& [op, n] : c.blocking_opcode)
      s += fmt::format(" {}x{}", op, n);
  }
  return s;
}

std::map<uint32_t, TranslatedShader> g_translatedPs;
std::map<uint32_t, TranslatedShader> g_translatedVs;

// ASYNC SHADER COMPILATION.
//
// First-use translation used to run entirely on the GUEST thread that submitted
// the draw: emit (~0ms) + FXC (~143ms) + dump/disassembly (~26ms). A cold cache
// means dozens back to back -- about 8 seconds of guest-thread time -- and that
// stall is not merely slow, it CORRUPTS GUEST STATE by changing which thread
// wins a race.
//
// The 0x8234CE20 crash is exactly that. The guest's script thread advances with
// frames, the database worker drains its own ring regardless, and the front end
// loads its packages from the script. Stall the renderer and the script arrives
// late, so the worker constructs a BinkVideoComponent before the script has
// asked for the package holding its movie; the asset lookup misses, the NULL is
// cached at component+0x94 and dereferenced later with no null check. Measured
// both ways on the same build: the script wins by 0.9s when the cache is warm.
//
// HOW: on a cache MISS the job is handed to the thread below and the function
// returns WITHOUT installing, so TranslatedPixelShader keeps returning nullptr
// -- the same state as a shader that failed to translate, which every consumer
// already handles. The worker RE-ENTERS ReportHlslCoverage with
// t_shaderCompileWorker set, so there is one translation path, not two that can
// drift.
//
// The maps are read by every draw. The lock is held only around find/insert --
// the returned pointer stays valid without it because std::map nodes are stable
// and nothing is ever erased.
std::mutex g_translatedMu;

// The microcode content id for a bound vertex shader handle, or 0 if that handle
// has not been translated yet. This is the identity anything cross-tabbing BY
// SHADER must use. The map is already maintained for exactly this reason, so the
// value is a lookup, not a hash on the draw path -- an FNV over the microcode
// per draw was measured at 4.6 ms a frame elsewhere in this tree.
uint64_t VertexShaderContentId(uint32_t handle) {
  if (!handle) return 0;
  std::lock_guard<std::mutex> lk(g_translatedMu);
  auto it = g_hlslReportedVs.find(handle);
  return it == g_hlslReportedVs.end() ? 0 : it->second;
}
uint64_t PixelShaderContentId(uint32_t handle) {
  if (!handle) return 0;
  std::lock_guard<std::mutex> lk(g_translatedMu);
  auto it = g_hlslReportedPs.find(handle);
  return it == g_hlslReportedPs.end() ? 0 : it->second;
}
thread_local bool t_shaderCompileWorker = false;

const TranslatedShader* TranslatedPixelShader(uint32_t handle) {
  std::lock_guard<std::mutex> lk(g_translatedMu);
  const auto it = g_translatedPs.find(handle);
  return it == g_translatedPs.end() ? nullptr : &it->second;
}

const TranslatedShader* TranslatedVertexShader(uint32_t handle) {
  std::lock_guard<std::mutex> lk(g_translatedMu);
  const auto it = g_translatedVs.find(handle);
  return it == g_translatedVs.end() ? nullptr : &it->second;
}

void ReportHlslCoverage(mx::hle::HlslStage stage, uint32_t handle,
                        const uint32_t* code, uint32_t count);

struct ShaderCompileJob {
  mx::hle::HlslStage stage = mx::hle::HlslStage::kPixel;
  uint32_t handle = 0;
  std::vector<uint32_t> code;
};

std::mutex g_compileMu;
std::condition_variable g_compileCv;
std::deque<ShaderCompileJob> g_compileQueue;
bool g_compileStop = false;
std::thread g_compileThread;
uint64_t g_compileQueued = 0;
uint64_t g_compileDone = 0;

void ShaderCompileWorker() {
  for (;;) {
    ShaderCompileJob job;
    {
      std::unique_lock<std::mutex> lk(g_compileMu);
      g_compileCv.wait(lk, [] { return g_compileStop || !g_compileQueue.empty(); });
      if (g_compileStop && g_compileQueue.empty()) return;
      job = std::move(g_compileQueue.front());
      g_compileQueue.pop_front();
    }
    t_shaderCompileWorker = true;
    ReportHlslCoverage(job.stage, job.handle, job.code.data(),
                       uint32_t(job.code.size()));
    t_shaderCompileWorker = false;
    std::lock_guard<std::mutex> lk(g_compileMu);
    ++g_compileDone;
  }
}

// Returns true when the job was taken, meaning the caller must NOT install and
// must return. False keeps the caller on the old inline path -- which is what
// the worker itself gets, and what happens if the thread could not start.
bool EnqueueShaderCompile(mx::hle::HlslStage stage, uint32_t handle,
                          const uint32_t* code, uint32_t count) {
  if (!code || !count) return false;
  std::lock_guard<std::mutex> lk(g_compileMu);
  if (g_compileStop) return false;
  if (!g_compileThread.joinable()) {
    try {
      g_compileThread = std::thread(ShaderCompileWorker);
    } catch (...) {
      return false;  // no thread: compile inline rather than never
    }
  }
  g_compileQueue.push_back({stage, handle, std::vector<uint32_t>(code, code + count)});
  ++g_compileQueued;
  g_compileCv.notify_one();
  return true;
}

void ReportHlslCoverage(mx::hle::HlslStage stage, uint32_t handle,
                        const uint32_t* code, uint32_t count) {
  auto& seen = stage == mx::hle::HlslStage::kPixel ? g_hlslReportedPs
                                                   : g_hlslReportedVs;
  // FNV-1a over the microcode. Content, not identity -- see g_hlslReportedVs.
  uint64_t code_key = 1469598103934665603ull;
  for (uint32_t i = 0; i < count; ++i) {
    code_key ^= code[i];
    code_key *= 1099511628211ull;
  }
  // Skipped on the compile worker: the guest thread already recorded this
  // handle before handing the job over, so the worker would take the
  // already-seen early-out and never compile anything.
  if (!t_shaderCompileWorker) {
    std::lock_guard<std::mutex> lk(g_translatedMu);
    const auto [it, inserted] = seen.emplace(handle, code_key);
    if (!inserted) {
      // Same address, same code: already translated OR a compile is in flight
      // for it. Either way nothing to do -- and this is what dedupes the queue.
      if (it->second == code_key) return;
      // Same address, DIFFERENT code: the guest reused it for another shader.
      // Fall through and re-translate, overwriting g_translatedVs[handle].
      it->second = code_key;
      ++g_shaderHandleRecycled;
    }
  }
  auto& cov = stage == mx::hle::HlslStage::kPixel ? g_hlslPs : g_hlslVs;
  // First-use cost split: translation vs FXC vs the dump/disassembly tail. KEPT
  // rather than removed, because it is the only thing that can show the DXBC
  // cache regressing -- a run where `compile` goes back to tens of milliseconds
  // per shader means the cache is missing, and the hits/misses line alone cannot
  // distinguish "missing" from "nothing to hit yet". Three steady_clock reads
  // per NEW shader, not per draw.
  const auto t_first_use = std::chrono::steady_clock::now();
  auto t_emit = t_first_use, t_compile = t_first_use;

  mx::hle::HlslShader out;
  // MUST match the width the renderer's vertex stage offers, or the two
  // signatures cannot link and pipeline creation fails with no message. See
  // kHlslInterpolatorLinkage.
  mx::hle::EmitShaderHlsl(code, count, stage,
                          mx::hle::kHlslInterpolatorLinkage, out);
  t_emit = std::chrono::steady_clock::now();

  // ZERO-EXPORT census: pixel shaders that name a colour target and never assign
  // it, so the target compiles to `mov o0.xyzw, l(0, 0, 0, 0)`. At the ONE
  // translate site rather than in the dump path, which is capped and would
  // sample whichever shaders happen to be dumped.
  //
  // MEMORY EXPORT census, BOTH STAGES, because memexport is a vertex-stage idiom
  // -- a shader that writes guest memory instead of a render target, and one way
  // the guest could be writing the terrain's uniform virtual-texture PAGE TABLE.
  // It could not even be observed before: the drop was recorded under
  // `if (dest < 32)` and the memexport registers are 32..37.
  //
  // Both reported unconditionally, zero included: a zero RULES OUT memexport and
  // sends the page table back to the CPU-write path.
  {
    static std::atomic<uint64_t> s_translated{0};
    static std::atomic<uint64_t> s_withMemexport{0};
    static std::atomic<uint64_t> s_memexportOps{0};
    const uint64_t t = ++s_translated;
    if (out.memexport_count) {
      const uint64_t w = ++s_withMemexport;
      s_memexportOps += out.memexport_count;
      if (w <= 16)
        REXLOG_INFO("d3d9: MEMEXPORT {} shader 0x{:08X}: {} export(s) to "
                    "registers 32-37 -- guest writes memory from a shader and "
                    "we drop it",
                    stage == mx::hle::HlslStage::kPixel ? "pixel" : "vertex",
                    handle, out.memexport_count);
    }
    if ((t % 25) == 0)
      REXLOG_INFO("d3d9: MEMEXPORT census: {} shaders translated, {} use "
                  "memory export, {} exports total",
                  t, s_withMemexport.load(), s_memexportOps.load());
  }

  if (stage == mx::hle::HlslStage::kPixel) {
    static std::atomic<uint64_t> s_psTranslated{0};
    static std::atomic<uint64_t> s_psZeroExport{0};
    const uint64_t n = ++s_psTranslated;
    if (out.color_unassigned_mask) {
      const uint64_t z = ++s_psZeroExport;
      if (z <= 16) {
        REXLOG_INFO("d3d9: ZERO-EXPORT PS 0x{:08X}: export_mask 0x{:X} "
                    "unassigned 0x{:X} -- colour target emitted as the zero "
                    "initialiser (opaque black)",
                    handle, out.export_mask, out.color_unassigned_mask);
      }
    }
    // Every 25, not 100: a menu-only run translates ~66 pixel shaders, so a
    // 100 interval reports nothing at all and "no zero-exports" is then
    // indistinguishable from "the census never printed".
    if ((n % 25) == 0) {
      REXLOG_INFO("d3d9: ZERO-EXPORT census: {} pixel shaders translated, {} "
                  "with a colour target named but never assigned",
                  n, s_psZeroExport.load());
    }
  }

  // Emitting is only half the claim. Source FXC rejects is exactly as useless as
  // a shader the emitter refused, and the two failures have entirely different
  // causes -- so they are counted apart and the compiler's own message is
  // logged, since "it did not compile" without the reason is not a finding.
  std::string compile_error;
  bool compiled = false;
  std::shared_ptr<const std::vector<uint8_t>> dxbc_bytes;
  if (out.status == mx::hle::HlslStatus::kOk) {
    Microsoft::WRL::ComPtr<ID3DBlob> blob, errors;
    const char* target =
        stage == mx::hle::HlslStage::kPixel ? "ps_5_0" : "vs_5_0";
    const uint64_t content_key = ShaderSourceKey(stage, out.source);
    dxbc_bytes = LoadShaderDxbc(stage, content_key);
    if (dxbc_bytes) {
      // Cache hit: same microcode seen in an earlier run or an earlier
      // video. Skip FXC entirely -- the emitter still ran above (0ms) for
      // the metadata the draw path needs.
      ++g_dxbcCacheHits;
      D3DCreateBlob(dxbc_bytes->size(), &blob);
      if (blob)
        std::memcpy(blob->GetBufferPointer(), dxbc_bytes->data(),
                    dxbc_bytes->size());
      compiled = blob != nullptr;
    } else if (!t_shaderCompileWorker &&
               EnqueueShaderCompile(stage, handle, code, count)) {
      // OFF THE GUEST THREAD. Nothing is installed, so the draw that triggered
      // this keeps seeing nullptr and takes the stand-in path until the worker
      // finishes. Everything below runs on the worker when it re-enters, so no
      // accounting is lost, only deferred.
      return;
    } else {
      const HRESULT hr = D3DCompile(
          out.source.data(), out.source.size(), nullptr, nullptr, nullptr,
          "main", target, D3DCOMPILE_OPTIMIZATION_LEVEL0, 0, &blob, &errors);
      compiled = SUCCEEDED(hr) && blob;
      if (compiled) {
        ++g_dxbcCacheMisses;
        SaveShaderDxbc(stage, content_key, blob.Get());
        // Carry the fresh bytes too, so the renderer skips its own O3
        // recompile of a first-sight shader -- the O0 compile here already
        // produced everything the PSO needs.
        const auto* p = static_cast<const uint8_t*>(blob->GetBufferPointer());
        dxbc_bytes = std::make_shared<const std::vector<uint8_t>>(
            p, p + blob->GetBufferSize());
      }
    }
    t_compile = std::chrono::steady_clock::now();
    // DIAG: dump the generated HLSL beside the DXBC the compiler produced from
    // it, for every pixel shader that compiles. Both halves are needed because a
    // RenderDoc pixel trace numbers its steps by DXBC INSTRUCTION, not by line
    // of our HLSL, and renderdoc-mcp's get_shader refuses these shaders outright
    // while still serving reflection.
    //
    // Unconditional rather than cvar-gated -- two cvar-gated diagnostics added
    // in the same session NEITHER ever armed here -- and bounded to 96 files
    // instead, above the ~72 pipelines a menu run builds. Compiled at
    // OPTIMIZATION_LEVEL0 above, so the DXBC follows the emitted source closely.
    if (!compiled && errors) {
      compile_error.assign(
          static_cast<const char*>(errors->GetBufferPointer()),
          errors->GetBufferSize());
      if (compile_error.size() > 400) compile_error.resize(400);
    }
    // Dumped whether or not FXC accepted it. This was `if (compiled)`, which
    // made the ONE shader worth reading the one shader never written out.
    // Failures carry their own budget rather than sharing s_dumped: letting 160
    // successes arrive first would starve exactly the file anyone came for.
    {
      // BUDGETED AND NAMED BY CONTENT, NOT BY HANDLE. Both used to key on
      // `handle`, and a guest shader handle is an ADDRESS the guest reuses within
      // a run, so many DIFFERENT shaders wrote to one filename, each overwriting
      // the last while still spending budget: the intro and menu burned the whole
      // cap across ~57 distinct handles and every level shader was then skipped
      // silently. Keyed on content_key, with the key in the filename so two
      // shaders at one handle cannot collide.
      static std::mutex s_dumpMu;
      static std::set<uint64_t> s_dumpedKeys;
      static uint32_t s_dumped = 0;
      static uint32_t s_dumpedFailed = 0;
      bool fresh_dump;
      {
        std::lock_guard<std::mutex> dump_lk(s_dumpMu);
        fresh_dump = s_dumpedKeys.insert(content_key).second;
      }
      uint32_t& budget = compiled ? s_dumped : s_dumpedFailed;
      // 160 was a menu-sized budget: a freeroam session saturates it, and a
      // saturated cap silently truncates the corpus that
      // `xenos_shader_disasm.py --xenia` diffs against Xenia. Xenia's dump of
      // this title holds 269 distinct blobs, so 512 leaves headroom. ~15 KB each.
      const uint32_t cap = compiled ? 512u : 64u;
      if (fresh_dump && budget < cap) {
        ++budget;
        std::error_code ec;
        EnsureHlslDumpDir();
        char path[128];
        // Vertex shaders included: a light-prepass draw was found exporting a
        // correct SV_Position and then ZERO for every interpolator, a defect on
        // the VERTEX side the pixel-only dump could not show. A rejection gets
        // its own prefix so `ls FAILED_*` names a run's failures.
        std::snprintf(path, sizeof(path), "logs/hlsldump/%s%s_%08X_%016llX.txt",
                      compiled ? "" : "FAILED_",
                      stage == mx::hle::HlslStage::kPixel ? "ps" : "vs",
                      handle, (unsigned long long)content_key);
        std::ofstream f(path, std::ios::trunc | std::ios::binary);
        if (f) {
          f << "; guest "
            << (stage == mx::hle::HlslStage::kPixel ? "pixel" : "vertex")
            << " shader 0x" << std::hex << handle << std::dec
            << "\n; sampler_count " << out.sampler_count << " max_const_index "
            << out.max_const_index << " input_mask 0x" << std::hex
            << out.input_mask << " export_mask 0x" << out.export_mask
            << " dropped_export_mask 0x" << out.dropped_export_mask << std::dec
            << " writes_position " << (out.writes_position ? 1 : 0);
          // Only when non-zero, so its presence in a dump means the shader has
          // a colour target that compiles to `mov o0, l(0,0,0,0)`.
          if (out.color_unassigned_mask)
            f << "\n; COLOUR TARGET NAMED BUT NEVER ASSIGNED: 0x" << std::hex
              << out.color_unassigned_mask << std::dec
              << " (emitted as the zero initialiser -- opaque black)";
          // Only when non-zero, so its presence in a dump means something.
          if (out.unhonoured_predicate_ops)
            f << "\n; SETP OPS WRITING p0: " << out.unhonoured_predicate_ops;
          // The half that used to be missing entirely. Non-zero means p0 is not
          // merely computed but ACTED ON, per instruction.
          if (out.predicated_alu_ops)
            f << "\n; PREDICATED ALU INSTRUCTIONS: " << out.predicated_alu_ops
              << " (all HONOURED as `if (xe_p0 == ...)`)";
          if (out.predicated_fetches)
            f << "\n; PREDICATED TEXTURE FETCHES: " << out.predicated_fetches
              << " (all HONOURED -- sample unconditionally, gate the "
                 "destination write)";
          // NOT a correctness gap, and it used to be described as one here. The
          // exec-level predicate is a WAVEFRONT branch: lanes whose p0 is clear
          // enter the block anyway, so the block gate never provided per-lane
          // correctness -- which is why the compiler also predicates the
          // instructions inside it, and all 240 ALU and fetch instructions in
          // this title's cond_exec_pred blocks carry their own (p0). We honour
          // both. An `if` here would be a wavefront-level SKIP: an optimisation,
          // and in the pixel stage an illegal one.
          if (out.pred_exec_blocks)
            f << "\n; P0-GATED EXEC BLOCKS: " << out.pred_exec_blocks
              << ", skipped as `if (xe_p0 == ...)`: "
              << out.honoured_pred_exec_blocks
              << " (the rest are ENTERED and their instructions gated"
                 " individually, which is what the console does per lane)";
          if (out.bool_exec_blocks)
            f << "\n; BOOL-GATED EXEC BLOCKS: " << out.bool_exec_blocks
              << " (cond_exec / cond_exec_pred_clean walked as a plain exec -- "
                 "gated on a BOOL CONSTANT, and this translator has no bool "
                 "constant bank, so the condition cannot be evaluated yet)";
          if (out.unhonoured_fetch_ops)
            f << "\n; UNHONOURED FETCH OPS: " << out.unhonoured_fetch_ops
              << " (getCompTexLOD/getGradients/getWeights/getBCF/setGradients "
                 "skipped; their destination keeps its previous value)";
          if (!compiled) f << "\n; FXC REJECTED: " << compile_error;
          // The guest's own bits, so a translation can be checked against its
          // INPUT instead of against itself. The DXBC section below is this
          // file's HLSL compiled, so the two agree by construction.
          if (code && count) {
            f << "\n\n=== GUEST MICROCODE (" << count << " dwords) ===\n";
            char line[160];
            for (uint32_t i = 0; i < count; i += 8) {
              int n = std::snprintf(line, sizeof(line), "; %04X:", i);
              if (n > 0) f.write(line, n);
              for (uint32_t j = i; j < i + 8 && j < count; ++j) {
                n = std::snprintf(line, sizeof(line), " %08X", code[j]);
                if (n > 0) f.write(line, n);
              }
              f << "\n";
            }
          }
          f << "\n\n=== EMITTED HLSL ===\n" << out.source;
          // Only a shader that compiled has DXBC to disassemble. The section
          // header is inside the guard too, so a failure file does not end with
          // an empty heading that reads like the disassembler broke.
          if (compiled && blob) {
            f << "\n=== DXBC DISASSEMBLY ===\n";
            Microsoft::WRL::ComPtr<ID3DBlob> disasm;
            if (SUCCEEDED(D3DDisassemble(blob->GetBufferPointer(),
                                         blob->GetBufferSize(), 0, nullptr,
                                         &disasm)) &&
                disasm) {
              f.write(static_cast<const char*>(disasm->GetBufferPointer()),
                      std::streamsize(disasm->GetBufferSize()));
            } else {
              f << "; D3DDisassemble failed\n";
            }
          }
        }
      }
    }
  }

  {
    static uint32_t s_timed = 0;
    if (s_timed < 8) {
      ++s_timed;
      const auto now = std::chrono::steady_clock::now();
      const auto emit_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                               t_emit - t_first_use)
                               .count();
      const auto compile_ms =
          std::chrono::duration_cast<std::chrono::milliseconds>(t_compile -
                                                                t_emit)
              .count();
      const auto tail_ms =
          std::chrono::duration_cast<std::chrono::milliseconds>(now - t_compile)
              .count();
      REXLOG_INFO("d3d9: HLSL {} 0x{:08X} first-use: emit {}ms compile {}ms "
                  "dump/disasm/vfetch {}ms",
                  stage == mx::hle::HlslStage::kPixel ? "PS" : "VS", handle,
                  emit_ms, compile_ms, tail_ms);
    }
  }

  // Cache health, every 256 lookups. The number to watch is MISSES on a second
  // run of the same content: it should be ~0, and anything else means the key is
  // not stable. A HIGH hit rate proves less than it looks -- the key is the
  // emitted HLSL, so a hit only says "this exact source compiled before".
  {
    static uint64_t s_last_reported = 0;
    const uint64_t total = g_dxbcCacheHits + g_dxbcCacheMisses;
    if (total - s_last_reported >= 256) {
      s_last_reported = total;
      REXLOG_INFO("d3d9: HLSL dxbc cache: {} hits {} misses",
                  g_dxbcCacheHits, g_dxbcCacheMisses);
    }
  }

  // Read before the move below; the report prints it.
  const size_t source_size = out.source.size();

  // Counted for every shader that EMITTED, whether or not FXC then took it:
  // predication is a property of the guest microcode, not of our compile, and
  // scoping it to the compiled ones would under-report the population.
  if (out.pred_exec_blocks) {
    ++cov.predicated;
    // Only when EVERY block in the shader was obeyed. A partly-honoured shader
    // still runs some bodies unconditionally, and counting it as honoured
    // would report the job done while it is half done.
    if (out.honoured_pred_exec_blocks == out.pred_exec_blocks) ++cov.p0_honoured;
  }
  if (out.bool_exec_blocks) ++cov.bool_gated;

  if (out.status != mx::hle::HlslStatus::kOk) {
    ++cov.failures[mx::hle::HlslStatusName(out.status)];
    if (out.blocking_opcode) ++cov.blocking_opcode[out.blocking_opcode];
  } else if (compiled) {
    ++cov.ok;
    // Retained only for shaders that both emitted and compiled. A source the
    // compiler rejects must never reach the renderer, which would only discover
    // the same failure later and with less context.
    std::lock_guard<std::mutex> install_lk(g_translatedMu);
    TranslatedShader& kept = (stage == mx::hle::HlslStage::kPixel
                                  ? g_translatedPs
                                  : g_translatedVs)[handle];
    kept.source = std::make_shared<const std::string>(std::move(out.source));
    kept.input_mask = out.input_mask;
    kept.export_mask = out.export_mask;
    kept.sampler_mask = out.sampler_mask;
    kept.sampler_count = out.sampler_count;
    kept.sampler_array_mask = out.sampler_array_mask;
    for (uint32_t i = 0; i < out.sampler_count; ++i)
      kept.slot_guest[i] = out.sampler_slot_guest[i];
    kept.max_const_index = out.max_const_index;
    // The guest's own name for this shader, resolved from the code_key already
    // computed at the top of this function. Once per shader, never per draw.
    kept.name = ShaderNameFor(code_key);
    // FROM `out`, THE MAIN TRANSLATION -- not from the fetch variant. These were
    // originally set only in the vfetch block below, from `fetched`, while every
    // other field here comes from `out`. That made the mask describe a DIFFERENT
    // translation than the shader a draw actually runs, and left it all-zero for
    // any shader with no fetch variant -- which reads as "reads no constants at
    // all". The SPEEDTREE census built on it reported tens of thousands of
    // a0-relative draws in a run whose 41 dumped shaders contain no xe_a0.
    for (int ci = 0; ci < 4; ++ci) kept.const_mask[ci] = out.const_mask[ci];
    kept.const_relative = out.const_relative;
    kept.dxbc = dxbc_bytes;

    // The vertex fetch variant of the same blob, emitted and compiled here
    // beside the one that already works, so a shader whose fetch form is refused
    // shows up as a counter rather than as a draw that silently stayed slow.
    // Failure is not an error: the shader keeps the CPU vertex path.
    if (stage == mx::hle::HlslStage::kVertex) {
      mx::hle::HlslShader fetched;
      mx::hle::EmitShaderHlsl(code, count, stage,
                              mx::hle::kHlslInterpolatorLinkage, fetched,
                              /*emit_vertex_fetch=*/true);
      if (fetched.status != mx::hle::HlslStatus::kOk) {
        ++g_vfetchRefused[mx::hle::HlslStatusName(fetched.status)];
      } else {
        Microsoft::WRL::ComPtr<ID3DBlob> fblob, ferrors;
        std::shared_ptr<const std::vector<uint8_t>> fetch_dxbc;
        // No fetch-variant salt any more: the fetch form IS a different source
        // string, so keying on the source separates the two by construction.
        const uint64_t fetch_key = ShaderSourceKey(stage, fetched.source);
        fetch_dxbc = LoadShaderDxbc(stage, fetch_key);
        if (fetch_dxbc) {
          ++g_dxbcCacheHits;
          D3DCreateBlob(fetch_dxbc->size(), &fblob);
          if (fblob)
            std::memcpy(fblob->GetBufferPointer(), fetch_dxbc->data(),
                        fetch_dxbc->size());
        } else {
          const HRESULT fhr = D3DCompile(
              fetched.source.data(), fetched.source.size(), nullptr, nullptr,
              nullptr, "main", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL0, 0,
              &fblob, &ferrors);
          if (SUCCEEDED(fhr) && fblob) {
            ++g_dxbcCacheMisses;
            SaveShaderDxbc(stage, fetch_key, fblob.Get());
            const auto* p =
                static_cast<const uint8_t*>(fblob->GetBufferPointer());
            fetch_dxbc = std::make_shared<const std::vector<uint8_t>>(
                p, p + fblob->GetBufferSize());
          }
        }
        if (fblob) {
          // DIAG: dump the FETCH variant separately. This is the form that
          // actually runs for a gpuVertexFetch draw and it is NOT the blob
          // dumped at the main compile site above -- a light-prepass draw was
          // found exporting a correct SV_Position and then zero for every
          // interpolator, and only this variant can show why.
          {
            // CONTENT-KEYED, like the main dump above and for the same reason:
            // this had the identical defect, a handle-keyed filename and a budget
            // that counted WRITES. After fixing only the main dump, 55 vs handles
            // against 16 vsfetch handles with ZERO overlap -- every fetch variant
            // on file was still menu-era while the vs dumps had reached the level.
            static std::mutex s_vfDumpMu;
            static std::set<uint64_t> s_vfDumpedKeys;
            static uint32_t s_vf_dumped = 0;
            const uint64_t vf_key =
                ShaderSourceKey(mx::hle::HlslStage::kVertex, fetched.source);
            bool vf_fresh;
            {
              std::lock_guard<std::mutex> vf_lk(s_vfDumpMu);
              vf_fresh = s_vfDumpedKeys.insert(vf_key).second;
            }
            if (vf_fresh && s_vf_dumped < 256) {
              ++s_vf_dumped;
              EnsureHlslDumpDir();
              char vpath[128];
              std::snprintf(vpath, sizeof(vpath),
                            "logs/hlsldump/vsfetch_%08X_%016llX.txt", handle,
                            (unsigned long long)vf_key);
              std::ofstream vf(vpath, std::ios::trunc | std::ios::binary);
              if (vf) {
                vf << "; guest vertex shader 0x" << std::hex << handle
                   << std::dec << " FETCH VARIANT\n; input_mask 0x" << std::hex
                   << fetched.input_mask << " export_mask 0x"
                   << fetched.export_mask << " dropped_export_mask 0x"
                   << fetched.dropped_export_mask << std::dec
                   << " writes_position " << (fetched.writes_position ? 1 : 0)
                   << " vertex_fetch_count " << fetched.vertex_fetch_count;
                // The guest's own bits, in the same format and section name the
                // main dump uses, so xenos_shader_disasm.py and its --xenia diff
                // read this variant too. The section was simply absent, so 59 of
                // 247 dumps -- specifically the form that ACTUALLY RUNS for a
                // gpuVertexFetch draw -- could only be compared against
                // themselves, and reconstructing them from the paired vs_ file by
                // guest handle left 5 of the 59 unpairable.
                if (code && count) {
                  vf << "\n\n=== GUEST MICROCODE (" << count
                     << " dwords) ===\n";
                  char vline[160];
                  for (uint32_t i = 0; i < count; i += 8) {
                    int n = std::snprintf(vline, sizeof(vline), "; %04X:", i);
                    if (n > 0) vf.write(vline, n);
                    for (uint32_t j = i; j < i + 8 && j < count; ++j) {
                      n = std::snprintf(vline, sizeof(vline), " %08X", code[j]);
                      if (n > 0) vf.write(vline, n);
                    }
                    vf << "\n";
                  }
                }
                vf << "\n\n=== EMITTED HLSL ===\n"
                   << fetched.source << "\n=== DXBC DISASSEMBLY ===\n";
                Microsoft::WRL::ComPtr<ID3DBlob> fdis;
                if (SUCCEEDED(D3DDisassemble(fblob->GetBufferPointer(),
                                             fblob->GetBufferSize(), 0, nullptr,
                                             &fdis)) &&
                    fdis) {
                  vf.write(static_cast<const char*>(fdis->GetBufferPointer()),
                           std::streamsize(fdis->GetBufferSize()));
                } else {
                  vf << "; D3DDisassemble failed\n";
                }
              }
            }
          }
          kept.fetch_source =
              std::make_shared<const std::string>(std::move(fetched.source));
          kept.fetch_dxbc = fetch_dxbc;
          kept.vertex_fetch_count = fetched.vertex_fetch_count;
          for (uint32_t i = 0; i < fetched.vertex_fetch_count; ++i)
            kept.vertex_fetch_slot[i] = fetched.vertex_fetch_slot[i];
          kept.computed_index_streams = fetched.computed_index_streams;
          for (int ci = 0; ci < 4; ++ci)
            kept.const_mask[ci] |= fetched.const_mask[ci];
          // OR, not assign: the fetch variant is a second translation of the
          // same shader, and a constant either of them reads is read. Assigning
          // here would clobber the main translation's mask set above.
          kept.const_relative = kept.const_relative || fetched.const_relative;
          kept.computed_index_fetches = fetched.computed_index_fetches;
          ++g_vfetchCompiled;
        } else {
          ++g_vfetchRefused["FXC rejected"];
          static uint32_t s_vf_logged = 0;
          if (s_vf_logged++ < 6 && ferrors) {
            std::string msg(
                static_cast<const char*>(ferrors->GetBufferPointer()),
                ferrors->GetBufferSize());
            if (msg.size() > 400) msg.resize(400);
            REXLOG_INFO("d3d9: VFETCH VS 0x{:08X} FXC REJECTED: {}", handle,
                        msg);
          }
        }
      }
    }

    // One line per distinct VERTEX CENSUS. A vertex shader that samples is the
    // shape a bone-matrix palette takes when the engine binds it as
    // g_BoneMatrixVectors rather than a constant array, and it is currently
    // refused the GPU vertex path.
    //
    // Unconditional, this was 4200 lines in a 210-frame segment, every one the
    // SAME handle with byte-identical fields -- they reach this line because the
    // (handle, code_key) dedupe at the top of this function sees a DIFFERENT
    // code hash each time, so each is a full re-translation. That is a real
    // defect and not this line's to fix; the dedupe here is on the CENSUS ITSELF.
    if (stage == mx::hle::HlslStage::kVertex) {
      // INTERPOLATOR ZERO-FILL. Every slot in the linkage this vertex shader does
      // not export is emitted as its float4(0,0,0,0) initialiser. The census for
      // that MOVED to the draw path -- see NoteInterpolatorFill -- because
      // counting it here fired on every unexported slot, and most shaders do not
      // use all eight, so it read ~80% in both scenes. A slot nobody reads is not
      // invented output.
      static std::set<uint64_t> s_census;
      const uint64_t census_key = (uint64_t(handle) << 32) ^
                                  (uint64_t(out.max_const_index) << 24) ^
                                  (uint64_t(out.sampler_count) << 20) ^
                                  (uint64_t(out.sampler_mask) << 8) ^
                                  uint64_t(out.input_mask);
      // Capped as well as deduped: a run that really does recycle a handle onto
      // thousands of distinct shaders must not get the log back by another
      // route.
      if (s_census.size() < 256 && s_census.insert(census_key).second) {
        REXLOG_INFO(
            "d3d9: VS census 0x{:08X}: samplers {} (mask 0x{:X}) inputs "
            "0x{:08X} max const c{}",
            handle, out.sampler_count, out.sampler_mask, out.input_mask,
            out.max_const_index);
      }
    }
    if (out.sampler_array_mask) {
      REXLOG_INFO("d3d9: HLSL {} 0x{:08X} declares cube slots 0x{:X}{}",
                  stage == mx::hle::HlslStage::kPixel ? "PS" : "VS",
                  handle, out.sampler_array_mask,
                  out.cube_fetch_without_cube_op
                      ? " -- WITHOUT a cube ALU op, coordinate form unverified"
                      : "");
    }
  } else {
    ++cov.compile_failed;
  }

  const char* tag = stage == mx::hle::HlslStage::kPixel ? "PS" : "VS";
  // Every compile failure, not just the first few: this is the one outcome that
  // means the emitter produced something plausible-looking and wrong, and each
  // distinct message is a separate defect to fix.
  if (!compile_error.empty()) {
    static uint32_t s_logged = 0;
    if (s_logged++ < 12) {
      REXLOG_INFO("d3d9: HLSL {} 0x{:08X} FXC REJECTED: {}", tag, handle,
                  compile_error);
    }
  }
  // The first few in full, so a failure can be read rather than inferred from a
  // count, and the source itself can be eyeballed for obvious nonsense.
  if (seen.size() <= 6) {
    REXLOG_INFO("d3d9: HLSL {} 0x{:08X}: {} ({} dwords) inputs 0x{:X} "
                "exports 0x{:X} samplers 0x{:X} consts<={} {}",
                tag, handle, mx::hle::HlslStatusName(out.status), count,
                out.input_mask, out.export_mask, out.sampler_mask,
                out.max_const_index,
                out.status == mx::hle::HlslStatus::kOk
                    ? fmt::format("{} bytes", source_size)
                    : fmt::format("opcode {}", out.blocking_opcode));
  }
  // `seen.size() % 16` IS BIMODAL. This function runs on every pass, not once
  // per distinct shader, so the predicate is re-evaluated against a count that
  // has stopped moving -- and where it parks decides everything: off a multiple
  // of 16 the line never prints again, ON a multiple it prints on EVERY pass.
  // Both were observed a run apart without a line of code changing.
  //
  // Bound on the only thing here that actually changes -- a new distinct shader
  // -- plus a slow heartbeat.
  {
    static size_t s_lastCovSeen = ~size_t(0);
    static std::chrono::steady_clock::time_point s_lastCovReport{};
    const auto now = std::chrono::steady_clock::now();
    if (seen.size() != s_lastCovSeen ||
        now - s_lastCovReport >= std::chrono::seconds(10)) {
      s_lastCovSeen = seen.size();
      s_lastCovReport = now;
      REXLOG_INFO("d3d9: HLSL {} coverage over {} shaders: {}", tag,
                  seen.size(), HlslCoverageSummary(cov));
    }
  }
  // "Every new vertex shader" was never what this did: it ran on every pass
  // through this function, and g_vfetchCompiled advances on every RE-translation
  // rather than once per shader, which is why one run reported the nonsense
  // "30573 of 42".
  //
  // The recycle count is printed HERE rather than left to its own summary,
  // because it is the denominator that makes the first number readable: 37,080
  // recycles against 42 distinct shaders is what "30573 of 42" was trying to say.
  if (stage == mx::hle::HlslStage::kVertex) {
    static size_t s_lastSeen = 0;
    static std::chrono::steady_clock::time_point s_lastReport{};
    const auto now = std::chrono::steady_clock::now();
    const bool grew = seen.size() != s_lastSeen;
    if (grew || now - s_lastReport >= std::chrono::seconds(10)) {
      s_lastSeen = seen.size();
      s_lastReport = now;
      std::string refused;
      for (const auto& [why, n] : g_vfetchRefused)
        refused += fmt::format(" {}={}", why, n);
      REXLOG_INFO(
          "d3d9: VFETCH coverage: {} compiles over {} distinct vertex "
          "shaders;{} (handles recycled onto different microcode: {})",
          g_vfetchCompiled, seen.size(),
          refused.empty() ? " none refused" : refused,
          g_shaderHandleRecycled.load());
    }
  }
}

}  // namespace mx::hooks::d3d9
