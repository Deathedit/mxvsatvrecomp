#include "hooks/texture_dump.h"

#include "gpu/d3d9_texture.h"
#include "gpu/hle_types.h"

#include <rex/cvar.h>
#include <rex/logging.h>

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <set>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

REXCVAR_DEFINE_BOOL(texture_dump, false, "Debug",
                    "Write every decoded guest texture to logs/texdump as a "
                    "PNG, with an index.txt naming each one. Diagnostic; "
                    "encodes on the draw thread");

namespace mx::diag {
namespace {

using Microsoft::WRL::ComPtr;

// Enough to cover a level's worth of distinct textures without being able to
// fill a disk by accident: a 1024x1024 dump is ~1 MB of PNG, so this bounds the
// directory at a few GB in the worst case.
constexpr size_t kMaxDumps = 4096;

std::mutex g_mutex;
// Keyed on the payload key AND its content version, so Scaleform's glyph atlas
// -- which rewrites itself in place under a stable fetch constant -- dumps each
// revision instead of only the one that happened to be sampled first.
std::set<std::pair<uint64_t, uint32_t>> g_seen;
size_t g_written = 0;
bool g_capped = false;
FILE* g_index = nullptr;

// ---------------------------------------------------------------------------
// Block-compressed decode.
//
// The payload keeps BC1/2/3/5 compressed because D3D12 consumes them directly,
// so the only place the blocks are ever expanded is here. Transcribed from the
// format definitions rather than adapted from a decoder, and the two easy ways
// to get it wrong are both handled: the 3-colour BC1 mode exists only in BC1
// (BC2 and BC3 always use the 4-colour interpolation regardless of c0 vs c1),
// and the alpha block's 6-value mode reserves indices 6 and 7 for 0 and 255.
// ---------------------------------------------------------------------------

void Rgb565(uint16_t v, uint8_t out[3]) {
  const uint32_t r = (v >> 11) & 0x1F, g = (v >> 5) & 0x3F, b = v & 0x1F;
  out[0] = uint8_t((r * 255 + 15) / 31);
  out[1] = uint8_t((g * 255 + 31) / 63);
  out[2] = uint8_t((b * 255 + 15) / 31);
}

// `four_colour` forces the BC2/BC3 rule; BC1 passes false and decides by c0>c1.
void DecodeColourBlock(const uint8_t* b, bool four_colour, uint8_t out[64]) {
  const uint16_t c0 = uint16_t(b[0] | (b[1] << 8));
  const uint16_t c1 = uint16_t(b[2] | (b[3] << 8));
  uint8_t c[4][4] = {};
  Rgb565(c0, c[0]);
  c[0][3] = 255;
  Rgb565(c1, c[1]);
  c[1][3] = 255;
  if (four_colour || c0 > c1) {
    for (int i = 0; i < 3; ++i) {
      c[2][i] = uint8_t((2 * c[0][i] + c[1][i] + 1) / 3);
      c[3][i] = uint8_t((c[0][i] + 2 * c[1][i] + 1) / 3);
    }
    c[2][3] = c[3][3] = 255;
  } else {
    for (int i = 0; i < 3; ++i) {
      c[2][i] = uint8_t((c[0][i] + c[1][i]) / 2);
      c[3][i] = 0;
    }
    c[2][3] = 255;
    c[3][3] = 0;  // the punch-through texel
  }
  const uint32_t bits = uint32_t(b[4]) | (uint32_t(b[5]) << 8) |
                        (uint32_t(b[6]) << 16) | (uint32_t(b[7]) << 24);
  for (int t = 0; t < 16; ++t)
    std::memcpy(out + t * 4, c[(bits >> (t * 2)) & 3], 4);
}

// The 8-byte interpolated-alpha block shared by BC3 and both halves of BC5.
void DecodeAlphaBlock(const uint8_t* b, uint8_t out[16]) {
  uint8_t a[8];
  a[0] = b[0];
  a[1] = b[1];
  if (a[0] > a[1]) {
    for (int i = 1; i < 7; ++i)
      a[i + 1] = uint8_t(((7 - i) * a[0] + i * a[1] + 3) / 7);
  } else {
    for (int i = 1; i < 5; ++i)
      a[i + 1] = uint8_t(((5 - i) * a[0] + i * a[1] + 2) / 5);
    a[6] = 0;
    a[7] = 255;
  }
  uint64_t bits = 0;
  for (int i = 0; i < 6; ++i) bits |= uint64_t(b[2 + i]) << (8 * i);
  for (int t = 0; t < 16; ++t) out[t] = a[(bits >> (t * 3)) & 7];
}

// BC2's explicit 4-bit alpha, one nibble per texel, low nibble first.
void DecodeBc2Alpha(const uint8_t* b, uint8_t out[16]) {
  for (int t = 0; t < 16; ++t) {
    const uint8_t byte = b[t / 2];
    out[t] = uint8_t(((t & 1) ? (byte >> 4) : (byte & 0xF)) * 17);
  }
}

float HalfToFloat(uint16_t h) {
  const uint32_t sign = uint32_t(h >> 15) << 31;
  uint32_t exponent = (h >> 10) & 0x1F;
  uint32_t mantissa = h & 0x3FF;
  if (exponent == 0) {
    if (mantissa == 0) {
      const uint32_t bits = sign;
      float f;
      std::memcpy(&f, &bits, 4);
      return f;
    }
    // Subnormal: normalise it into the float32 range.
    while (!(mantissa & 0x400)) {
      mantissa <<= 1;
      --exponent;
    }
    ++exponent;
    mantissa &= 0x3FF;
  } else if (exponent == 31) {
    const uint32_t bits = sign | 0x7F800000u | (mantissa << 13);
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
  }
  const uint32_t bits = sign | ((exponent + 112) << 23) | (mantissa << 13);
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}

// Float channels are clamped, not tone-mapped. A render target carrying values
// well outside 0..1 will clip to white here, which is worth knowing when
// reading one of these images -- but a curve would make every image a guess.
// NaN maps to 0 so it is visible as a hole rather than as whatever the cast
// happens to produce.
uint8_t FloatToUnorm(float v) {
  if (!(v == v)) return 0;
  if (v <= 0.0f) return 0;
  if (v >= 1.0f) return 255;
  return uint8_t(v * 255.0f + 0.5f);
}

// ---------------------------------------------------------------------------
// Level 0, slice 0 of any host format -> RGBA8, in the decoder's own component
// order. The swizzle is applied afterwards, by the caller, because it is a view
// property rather than part of the data.
// ---------------------------------------------------------------------------
bool ExpandLevelToRgba8(const mx::hle::HleTexturePayload& p,
                        std::vector<uint8_t>& rgba, uint32_t& out_w,
                        uint32_t& out_h) {
  const mx::hle::HleTextureLevelData& level = p.levels[0];
  const uint32_t w = level.width ? level.width : p.width;
  const uint32_t h = level.height ? level.height : p.height;
  if (!w || !h) return false;
  const uint32_t pitch = level.row_pitch ? level.row_pitch : p.row_pitch;
  if (!pitch) return false;
  const size_t base = level.offset;
  const size_t available = p.data.size() > base ? p.data.size() - base : 0;
  if (!available) return false;
  const uint8_t* src = p.data.data() + base;

  out_w = w;
  out_h = h;
  rgba.assign(size_t(w) * h * 4, 0);
  auto put = [&](uint32_t x, uint32_t y, uint8_t r, uint8_t g, uint8_t b,
                 uint8_t a) {
    if (x >= w || y >= h) return;
    uint8_t* d = rgba.data() + (size_t(y) * w + x) * 4;
    d[0] = r;
    d[1] = g;
    d[2] = b;
    d[3] = a;
  };

  using F = mx::hle::HostTextureFormat;
  const bool block = p.format == F::kBc1 || p.format == F::kBc2 ||
                     p.format == F::kBc3 || p.format == F::kBc5;
  if (block) {
    const uint32_t block_bytes = p.format == F::kBc1 ? 8u : 16u;
    const uint32_t blocks_x = (w + 3) / 4;
    const uint32_t blocks_y = (h + 3) / 4;
    for (uint32_t by = 0; by < blocks_y; ++by) {
      for (uint32_t bx = 0; bx < blocks_x; ++bx) {
        const size_t at = size_t(by) * pitch + size_t(bx) * block_bytes;
        if (at + block_bytes > available) continue;
        const uint8_t* blk = src + at;
        uint8_t texels[64] = {};
        uint8_t alpha[16];
        for (int i = 0; i < 16; ++i) alpha[i] = 255;
        switch (p.format) {
          case F::kBc1:
            DecodeColourBlock(blk, false, texels);
            for (int t = 0; t < 16; ++t) alpha[t] = texels[t * 4 + 3];
            break;
          case F::kBc2:
            DecodeBc2Alpha(blk, alpha);
            DecodeColourBlock(blk + 8, true, texels);
            break;
          case F::kBc3:
            DecodeAlphaBlock(blk, alpha);
            DecodeColourBlock(blk + 8, true, texels);
            break;
          case F::kBc5: {
            // Two independent channels, R then G. Blue is left at zero rather
            // than reconstructed as a normal's Z: this is a dump of what the
            // texture holds, not an interpretation of what it is for.
            uint8_t red[16], green[16];
            DecodeAlphaBlock(blk, red);
            DecodeAlphaBlock(blk + 8, green);
            for (int t = 0; t < 16; ++t) {
              texels[t * 4 + 0] = red[t];
              texels[t * 4 + 1] = green[t];
              texels[t * 4 + 2] = 0;
              texels[t * 4 + 3] = 255;
            }
            break;
          }
          default:
            break;
        }
        for (int t = 0; t < 16; ++t) {
          const uint32_t x = bx * 4 + (t % 4);
          const uint32_t y = by * 4 + (t / 4);
          put(x, y, texels[t * 4 + 0], texels[t * 4 + 1], texels[t * 4 + 2],
              alpha[t]);
        }
      }
    }
    return true;
  }

  // Uncompressed: bytes per texel, and a per-texel reader.
  uint32_t bpp = 0;
  switch (p.format) {
    case F::kR8:
      bpp = 1;
      break;
    case F::kRg8:
    case F::kR16:
    case F::kR16Float:
    case F::kBgra4:
      bpp = 2;
      break;
    case F::kRgba8:
    case F::kR32Float:
    case F::kRg16Float:
    case F::kRg16Unorm:
    case F::kRg16Snorm:
    case F::kRgb10A2Unorm:
      bpp = 4;
      break;
    case F::kRg32Float:
    case F::kRgba16Float:
    case F::kRgba16Unorm:
    case F::kRgba16Snorm:
      bpp = 8;
      break;
    default:
      return false;
  }
  auto u16 = [](const uint8_t* q) { return uint16_t(q[0] | (q[1] << 8)); };
  auto f32 = [](const uint8_t* q) {
    float f;
    std::memcpy(&f, q, 4);
    return f;
  };
  auto snorm16 = [](uint16_t v) {
    const int16_t s = int16_t(v);
    return float(s) / 32767.0f * 0.5f + 0.5f;
  };
  for (uint32_t y = 0; y < h; ++y) {
    for (uint32_t x = 0; x < w; ++x) {
      const size_t at = size_t(y) * pitch + size_t(x) * bpp;
      if (at + bpp > available) continue;
      const uint8_t* q = src + at;
      uint8_t r = 0, g = 0, b = 0, a = 255;
      switch (p.format) {
        case F::kRgba8:
          r = q[0];
          g = q[1];
          b = q[2];
          a = q[3];
          break;
        // Single-channel formats fill R only, leaving G and B at zero, because
        // that is what the SRV returns and the swizzle below is applied on top
        // of it. Replicating to grey would read better and be a lie: a
        // heightmap whose swizzle selects G really does sample zero.
        case F::kR8:
          r = q[0];
          break;
        case F::kRg8:
          r = q[0];
          g = q[1];
          break;
        case F::kR16:
          r = uint8_t(u16(q) >> 8);
          break;
        case F::kR16Float:
          r = FloatToUnorm(HalfToFloat(u16(q)));
          break;
        case F::kBgra4: {
          // B4G4R4A4, low nibble first within each byte.
          const uint16_t v = u16(q);
          b = uint8_t(((v >> 0) & 0xF) * 17);
          g = uint8_t(((v >> 4) & 0xF) * 17);
          r = uint8_t(((v >> 8) & 0xF) * 17);
          a = uint8_t(((v >> 12) & 0xF) * 17);
          break;
        }
        case F::kR32Float:
          r = FloatToUnorm(f32(q));
          break;
        case F::kRg16Float:
          r = FloatToUnorm(HalfToFloat(u16(q)));
          g = FloatToUnorm(HalfToFloat(u16(q + 2)));
          break;
        case F::kRg16Unorm:
          r = uint8_t(u16(q) >> 8);
          g = uint8_t(u16(q + 2) >> 8);
          break;
        case F::kRg16Snorm:
          r = FloatToUnorm(snorm16(u16(q)));
          g = FloatToUnorm(snorm16(u16(q + 2)));
          break;
        case F::kRgb10A2Unorm: {
          const uint32_t v = uint32_t(q[0]) | (uint32_t(q[1]) << 8) |
                             (uint32_t(q[2]) << 16) | (uint32_t(q[3]) << 24);
          r = uint8_t(((v >> 0) & 0x3FF) >> 2);
          g = uint8_t(((v >> 10) & 0x3FF) >> 2);
          b = uint8_t(((v >> 20) & 0x3FF) >> 2);
          a = uint8_t((((v >> 30) & 0x3) * 255) / 3);
          break;
        }
        case F::kRg32Float:
          r = FloatToUnorm(f32(q));
          g = FloatToUnorm(f32(q + 4));
          break;
        case F::kRgba16Float:
          r = FloatToUnorm(HalfToFloat(u16(q)));
          g = FloatToUnorm(HalfToFloat(u16(q + 2)));
          b = FloatToUnorm(HalfToFloat(u16(q + 4)));
          a = FloatToUnorm(HalfToFloat(u16(q + 6)));
          break;
        case F::kRgba16Unorm:
          r = uint8_t(u16(q) >> 8);
          g = uint8_t(u16(q + 2) >> 8);
          b = uint8_t(u16(q + 4) >> 8);
          a = uint8_t(u16(q + 6) >> 8);
          break;
        case F::kRgba16Snorm:
          r = FloatToUnorm(snorm16(u16(q)));
          g = FloatToUnorm(snorm16(u16(q + 2)));
          b = FloatToUnorm(snorm16(u16(q + 4)));
          a = FloatToUnorm(snorm16(u16(q + 6)));
          break;
        default:
          break;
      }
      put(x, y, r, g, b, a);
    }
  }
  return true;
}

// The SRV's component mapping, applied to the pixels instead. Same `& 5`
// sanitisation as the renderer's host_component: guest GPUSWIZZLE is 3 bits, so
// 6 and 7 are representable and mean the same as 4 and 5 -- a forced 0 and a
// forced 1. Without this the dump shows the decoder's channel order, which is
// not the order the shader reads.
void ApplySwizzle(std::vector<uint8_t>& rgba, uint32_t swizzle) {
  for (size_t i = 0; i + 3 < rgba.size(); i += 4) {
    const uint8_t src[4] = {rgba[i], rgba[i + 1], rgba[i + 2], rgba[i + 3]};
    for (int c = 0; c < 4; ++c) {
      const uint32_t sel = (swizzle >> (3 * c)) & 7u;
      const uint32_t s = sel >= 4u ? (sel & 5u) : sel;
      rgba[i + c] = s < 4u ? src[s] : (s == 4u ? uint8_t(0) : uint8_t(255));
    }
  }
}

// ---------------------------------------------------------------------------
// PNG, through WIC -- it ships with Windows, so this needs no third-party
// encoder and no deflate of our own. BGRA is fed in because that is the PNG
// encoder's native input format; asking for RGBA makes it negotiate.
// ---------------------------------------------------------------------------
bool WritePngBgra(const std::wstring& path, uint32_t w, uint32_t h,
                  const std::vector<uint8_t>& bgra) {
  static thread_local const bool s_com = [] {
    // RPC_E_CHANGED_MODE means the thread is already an STA, which is fine --
    // CoCreateInstance works either way. Only a hard failure matters.
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    return SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;
  }();
  if (!s_com) return false;

  ComPtr<IWICImagingFactory> factory;
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                              CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))))
    return false;
  ComPtr<IWICStream> stream;
  if (FAILED(factory->CreateStream(&stream))) return false;
  if (FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)))
    return false;
  ComPtr<IWICBitmapEncoder> encoder;
  if (FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)))
    return false;
  if (FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)))
    return false;
  ComPtr<IWICBitmapFrameEncode> frame;
  ComPtr<IPropertyBag2> props;
  if (FAILED(encoder->CreateNewFrame(&frame, &props))) return false;
  if (FAILED(frame->Initialize(props.Get()))) return false;
  if (FAILED(frame->SetSize(w, h))) return false;
  WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
  if (FAILED(frame->SetPixelFormat(&format))) return false;
  if (!IsEqualGUID(format, GUID_WICPixelFormat32bppBGRA)) return false;
  const UINT stride = w * 4;
  if (FAILED(frame->WritePixels(h, stride, UINT(bgra.size()),
                                const_cast<BYTE*>(bgra.data()))))
    return false;
  if (FAILED(frame->Commit())) return false;
  return SUCCEEDED(encoder->Commit());
}

// A filename component that is safe on Windows and readable in a file listing.
std::string Sanitise(const char* text) {
  std::string out;
  for (const char* p = text; p && *p; ++p) {
    const char c = *p;
    out.push_back((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
                          (c >= 'a' && c <= 'z') || c == '_' || c == '-'
                      ? c
                      : '_');
  }
  return out.empty() ? std::string("unknown") : out;
}

std::wstring Widen(const std::string& s) {
  return std::wstring(s.begin(), s.end());
}

const char* HostFormatName(mx::hle::HostTextureFormat f) {
  using F = mx::hle::HostTextureFormat;
  switch (f) {
    case F::kRgba8: return "RGBA8";
    case F::kBc1: return "BC1";
    case F::kBc2: return "BC2";
    case F::kBc3: return "BC3";
    case F::kBc5: return "BC5";
    case F::kR16Float: return "R16F";
    case F::kRgba16Float: return "RGBA16F";
    case F::kBgra4: return "BGRA4";
    case F::kR8: return "R8";
    case F::kR16: return "R16";
    case F::kR32Float: return "R32F";
    case F::kRg8: return "RG8";
    case F::kRg16Float: return "RG16F";
    case F::kRg16Unorm: return "RG16U";
    case F::kRg16Snorm: return "RG16S";
    case F::kRgba16Unorm: return "RGBA16U";
    case F::kRgba16Snorm: return "RGBA16S";
    case F::kRg32Float: return "RG32F";
    case F::kRgb10A2Unorm: return "RGB10A2";
  }
  return "?";
}

}  // namespace

bool TextureDumpEnabled() {
  static const bool s_on = REXCVAR_GET(texture_dump);
  return s_on;
}

void DumpDecodedTexture(const mx::hle::HleTextureSource& source,
                        const mx::hle::HleTexturePayload& payload,
                        const char* site, uint32_t sampler) {
  if (!TextureDumpEnabled()) return;

  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_capped) return;
  if (!g_seen.emplace(payload.key, payload.content_version).second) return;
  if (g_written >= kMaxDumps) {
    g_capped = true;
    REXLOG_INFO("texdump: cap of {} files reached; stopping", kMaxDumps);
    return;
  }

  if (g_written == 0) {
    std::error_code ec;
    std::filesystem::remove_all("logs/texdump", ec);
    std::filesystem::create_directories("logs/texdump", ec);
    if (fopen_s(&g_index, "logs/texdump/index.txt", "w") != 0) g_index = nullptr;
    if (g_index) {
      std::fprintf(g_index,
                   "seq\tfile\tw\th\tguest_format\thost_format\taddress\tkey"
                   "\tversion\tslices\tlevels\tsampler\tsite\talpha\n");
    }
  }

  std::vector<uint8_t> rgba;
  uint32_t w = 0, h = 0;
  if (!ExpandLevelToRgba8(payload, rgba, w, h)) {
    REXLOG_INFO("texdump: cannot expand host format {} ({}x{} at 0x{:08X})",
                HostFormatName(payload.format), payload.width, payload.height,
                source.address);
    return;
  }
  ApplySwizzle(rgba, payload.swizzle);

  // Is the alpha channel carrying anything? A UI atlas is white RGB with the
  // glyph shape entirely in alpha, so the colour image alone would show a blank
  // square -- and an image written WITH alpha reads as white where alpha is 0,
  // which has already cost this project a wrong reading once. So the colour
  // image is forced opaque and the alpha, when it varies, is written beside it
  // as its own greyscale PNG. Nothing is hidden and nothing is composited.
  bool alpha_varies = false;
  const uint8_t first_alpha = rgba.size() >= 4 ? rgba[3] : 255;
  for (size_t i = 3; i < rgba.size(); i += 4) {
    if (rgba[i] != first_alpha) {
      alpha_varies = true;
      break;
    }
  }

  const size_t seq = g_written;
  char stem[192];
  std::snprintf(stem, sizeof(stem), "t%04zu_%ux%u_%s_%s_a%08X_k%016llX_v%u",
                seq, w, h,
                Sanitise(mx::hle::GuestTextureFormatName(source.guest_format))
                    .c_str(),
                HostFormatName(payload.format), source.address,
                static_cast<unsigned long long>(payload.key),
                payload.content_version);

  std::vector<uint8_t> bgra(rgba.size());
  for (size_t i = 0; i + 3 < rgba.size(); i += 4) {
    bgra[i + 0] = rgba[i + 2];
    bgra[i + 1] = rgba[i + 1];
    bgra[i + 2] = rgba[i + 0];
    bgra[i + 3] = 255;  // opaque: see the note above
  }
  const std::string colour_path = std::string("logs/texdump/") + stem + ".png";
  if (!WritePngBgra(Widen(colour_path), w, h, bgra)) {
    REXLOG_INFO("texdump: PNG encode failed for {}", colour_path);
    return;
  }
  if (alpha_varies) {
    for (size_t i = 0; i + 3 < rgba.size(); i += 4) {
      const uint8_t a = rgba[i + 3];
      bgra[i + 0] = bgra[i + 1] = bgra[i + 2] = a;
      bgra[i + 3] = 255;
    }
    WritePngBgra(Widen(std::string("logs/texdump/") + stem + "_alpha.png"), w, h,
                 bgra);
  }

  ++g_written;
  if (g_index) {
    std::fprintf(g_index,
                 "%zu\t%s.png\t%u\t%u\t%s\t%s\t0x%08X\t0x%016llX\t%u\t%u\t%u"
                 "\t%u\t%s\t%s\n",
                 seq, stem, w, h,
                 mx::hle::GuestTextureFormatName(source.guest_format),
                 HostFormatName(payload.format), source.address,
                 static_cast<unsigned long long>(payload.key),
                 payload.content_version, payload.array_size,
                 payload.level_count, sampler, site,
                 alpha_varies ? "varies" : "constant");
    std::fflush(g_index);
  }
  if (g_written <= 8 || (g_written % 100) == 0)
    REXLOG_INFO("texdump: wrote {} textures, latest {}", g_written, stem);
}

}  // namespace mx::diag
