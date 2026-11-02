// ng2 - Ninja Gaiden II static recompilation
//
// Aspect Ratio (Display page of RECOMP SETTINGS): the game draws for 16:9.
//
// 3D: the game's only 16:9 float (0x8228EBDC) is the aspect its projections
// are built with (the camera update sub_822FA8B8 builds them from the field
// of view at camera +176). It is set to the chosen aspect: wider screens see
// more at the sides; narrower ones (16:10) keep the sides and get a taller
// field of view (Vert+), set around that update.
//
// The picture is still drawn into the game's 16:9 targets, and the
// presenter stretches it to the chosen aspect (src/gpu/video.cpp), so 2D is
// drawn squeezed by the inverse, around the centre of the game's 1280x720
// space, to look right once stretched: sprites and gauges (their four
// pixel-space vertices, 24 bytes apart, at the quad draw sub_8368EFE8),
// text (the 4x4 transform the text pass hands sub_8370DD20 by value in
// r4..r10, a row-vector matrix: column 0 the high words of r4/r6/r8/r10,
// column 1 their low words) and movies (sub_83660FC8 builds a movie's quad
// in clip space: four vertices 16 bytes apart).

#include "aspect.h"

#include <cmath>
#include <cstdint>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>

#include <fmt/format.h>
#include <rex/logging.h>

#include <rex/cvar.h>
#include <rex/hook.h>

#include "gpu/guest.h"

REXCVAR_DEFINE_INT32(aspect_ratio, 0, "Display", "0: 16:9, 1: 16:10, 2: 21:9, 3: 32:9");

namespace {

using ng2::guest::Load32;
using ng2::guest::Virtual;

constexpr float kNative = 16.0f / 9.0f;
constexpr uint32_t kAspectConstant = ng2::aspect::kConstantAddress;
constexpr uint32_t kCameraFov = 176;

float LoadFloat(uint32_t address) {
  uint32_t bits = Load32(address);
  float value;
  std::memcpy(&value, &bits, 4);
  return value;
}

void StoreFloat(uint32_t address, float value) {
  uint32_t bits;
  std::memcpy(&bits, &value, 4);
  uint8_t* p = Virtual(address);
  p[0] = uint8_t(bits >> 24), p[1] = uint8_t(bits >> 16), p[2] = uint8_t(bits >> 8),
  p[3] = uint8_t(bits);
}

// How much 2D is squeezed along each axis.
struct Squeeze {
  float x = 1.0f, y = 1.0f;
  bool active() const { return x != 1.0f || y != 1.0f; }
};

Squeeze CurrentSqueeze() {
  float aspect = ng2::aspect::Aspect();
  Squeeze s;
  if (aspect > kNative) {
    s.x = kNative / aspect;
  } else if (aspect < kNative) {
    s.y = aspect / kNative;
  }
  return s;
}


// The 2D pass (sub_8368E2D8) draws 91 buckets in order; bucket i's items
// are a list (item +4: next) from [kBuckets + parity * 728 + 4 * i],
// parity = [kBuckets + 1456] ^ 1, each drawn by its vtable +4 method. Every
// bucket also holds a placeholder (draw method 0x83705C98). The bucket of
// the item being drawn says what it is (the HUD's pieces have their own).
constexpr uint32_t kBuckets = 0x85325ED8;
constexpr int kBucketCount = 91;
constexpr uint32_t kPlaceholderDraw = 0x83705C98;

std::unordered_map<uint32_t, int> g_itemBucket;  // render pass thread
int g_drawBucket = -1;
uint32_t g_drawItem = 0;

// Where a HUD item stays when the screen is wider (or taller): -1 at the
// left (top) edge, 1 at the right (bottom), 0 the centre. Decided by the
// first piece it draws.
struct Anchor {
  int x = 0, y = 0;
};
std::unordered_map<uint32_t, Anchor> g_itemAnchor;
bool g_hud = false;  // the gameplay HUD is up this frame (health bar buckets 57-59)
std::atomic<bool> g_hudShown{false};  // the same, for other threads

// Diagnostic (NG2_BUCKET_LOG=1): each bucket's 2D box, once a second.
struct Box {
  float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
  int draws = 0;
};
Box g_boxes[kBucketCount];

std::chrono::steady_clock::time_point g_boxesLogged;
bool BucketLog() {
  static const bool on = std::getenv("NG2_BUCKET_LOG") != nullptr;
  return on;
}

void ScanBuckets() {
  g_itemBucket.clear();
  uint32_t parity = Load32(kBuckets + 1456) ^ 1;
  uint32_t heads = kBuckets + parity * 728;
  bool hud = false;
  for (int i = 0; i < kBucketCount; i++) {
    int guard = 0;
    for (uint32_t item = Load32(heads + 4 * i); item && guard < 4096; item = Load32(item + 4), guard++) {
      g_itemBucket[item] = i;
      if (i >= 57 && i <= 59 && Load32(Load32(item) + 4) != kPlaceholderDraw) {
        hud = true;
      }
    }
  }
  g_hud = hud;
  g_hudShown.store(hud, std::memory_order_relaxed);
  // Anchors are kept while their item is drawn (one sliding across a third
  // doesn't jump), and dropped with it or with the HUD.
  if (!hud) {
    g_itemAnchor.clear();
  } else {
    std::erase_if(g_itemAnchor, [](const auto& entry) { return !g_itemBucket.contains(entry.first); });
  }
}


void NotePoint(float x, float y) {
  if (g_drawBucket < 0) {
    return;
  }
  if (!BucketLog()) {
    return;
  }
  Box& b = g_boxes[g_drawBucket];
  b.x0 = std::min(b.x0, x), b.x1 = std::max(b.x1, x);
  b.y0 = std::min(b.y0, y), b.y1 = std::max(b.y1, y);
}

void NoteQuad(uint32_t vertices, uint32_t stride) {
  if (g_drawBucket < 0 || !BucketLog()) {
    return;
  }
  g_boxes[g_drawBucket].draws++;
  for (uint32_t i = 0; i < 4; i++) {
    NotePoint(LoadFloat(vertices + i * stride), LoadFloat(vertices + i * stride + 4));
  }
}

void LogBoxes() {
  auto now = std::chrono::steady_clock::now();
  if (!BucketLog() || now - g_boxesLogged < std::chrono::seconds(1)) {
    return;
  }
  g_boxesLogged = now;
  std::string line;
  for (int i = 0; i < kBucketCount; i++) {
    Box& b = g_boxes[i];
    if (b.draws) {
      line += fmt::format(" {}:{}[{:.0f},{:.0f}-{:.0f},{:.0f}]", i, b.draws, b.x0, b.y0, b.x1, b.y1);
    }
    b = Box{};
  }
  REXLOG_INFO("2D buckets{}:{}", g_hud ? " (HUD)" : "", line);
}

// Draws an item with its bucket known.
void DrawTagged(PPCContext& ctx, uint8_t* base, void (*draw)(PPCContext&, uint8_t*)) {
  auto it = g_itemBucket.find(ctx.r3.u32);
  int saved = g_drawBucket;
  uint32_t savedItem = g_drawItem;
  g_drawBucket = it == g_itemBucket.end() ? -1 : it->second;
  g_drawItem = ctx.r3.u32;
  draw(ctx, base);
  g_drawBucket = saved;
  g_drawItem = savedItem;
}

// Buckets of the HUD's left column (karma panel 39-41, health bar and ki
// 57-61), and menus' (never moved: pause and inventory 49-50, in-game
// options 70-72, loading and title screens 20, 80-86).
constexpr int kKarmaBucket = 40;
bool LeftHud(int bucket) {
  return (bucket >= 39 && bucket <= 41) || (bucket >= 57 && bucket <= 61);
}
// The frame-rate counter's layer (src/perf_overlay.cpp): the top left corner.
constexpr uint32_t kCounterLayer = 0x15;
bool MenuBucket(int bucket) {
  return bucket == 20 || bucket == 49 || bucket == 50 || (bucket >= 70 && bucket <= 72) ||
         (bucket >= 80 && bucket <= 86);
}

// The anchor of the item being drawn, from a point of its first piece.
Anchor ItemAnchor(float x, float y) {
  // The counter's words (copies of its text item, which keep its layer at
  // +8; the game files them in another bucket) are at the top.
  if (g_drawItem && Load32(g_drawItem + 8) == kCounterLayer && y < 48.0f) {
    return {-1, -1};
  }
  if (!g_hud || g_drawBucket < 0 || MenuBucket(g_drawBucket)) {
    return {};
  }
  auto [it, added] = g_itemAnchor.try_emplace(g_drawItem);
  if (added) {
    Anchor& a = it->second;
    if (g_drawBucket == kKarmaBucket && y >= 480.0f) {
      a = {0, 1};  // the boss's health bar, drawn with the karma panel: bottom centre
    } else if (LeftHud(g_drawBucket)) {
      a = {-1, -1};
    } else {
      a.x = x < 1280.0f / 3.0f ? -1 : x > 2560.0f / 3.0f ? 1 : 0;
      a.y = y < 720.0f / 3.0f ? -1 : y > 1440.0f / 3.0f ? 1 : 0;
    }
  }
  return it->second;
}

// v squeezed by k (the presenter stretches it back), kept at its edge.
float Map(float v, float k, int anchor, float size) {
  if (anchor < 0) {
    return v * k;
  }
  if (anchor > 0) {
    return size - (size - v) * k;
  }
  return size * 0.5f + (v - size * 0.5f) * k;
}
// The same as an affine map v * k + offset.
float MapOffset(float k, int anchor, float size) {
  return anchor < 0 ? 0.0f : anchor > 0 ? size * (1.0f - k) : size * 0.5f * (1.0f - k);
}

// Squeezes a quad (4 vertices, x and y first) in place. A quad covering the
// whole screen (a background, a fade) is instead zoomed evenly, about the
// centre, until it covers the wider (or taller) screen: cropped, not
// stretched out of shape.
void SqueezeQuad(uint32_t vertices, uint32_t stride, Squeeze s) {
  float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
  for (uint32_t i = 0; i < 4; i++) {
    float x = LoadFloat(vertices + i * stride), y = LoadFloat(vertices + i * stride + 4);
    x0 = std::min(x0, x), x1 = std::max(x1, x), y0 = std::min(y0, y), y1 = std::max(y1, y);
  }
  float kx = s.x, ky = s.y;
  if (x0 <= 1.0f && x1 >= 1279.0f && y0 <= 1.0f && y1 >= 719.0f) {
    float cover = 1.0f / std::min(s.x, s.y);
    kx = cover * s.x, ky = cover * s.y;
    for (uint32_t i = 0; i < 4; i++) {
      uint32_t at = vertices + i * stride;
      StoreFloat(at, Map(LoadFloat(at), kx, 0, 1280.0f));
      StoreFloat(at + 4, Map(LoadFloat(at + 4), ky, 0, 720.0f));
    }
    return;
  }
  Anchor a = ItemAnchor((x0 + x1) * 0.5f, (y0 + y1) * 0.5f);
  for (uint32_t i = 0; i < 4; i++) {
    uint32_t at = vertices + i * stride;
    StoreFloat(at, Map(LoadFloat(at), kx, a.x, 1280.0f));
    StoreFloat(at + 4, Map(LoadFloat(at + 4), ky, a.y, 720.0f));
  }
}

// High and low 32-bit halves of a 64-bit register as floats.
float High(uint64_t value) {
  uint32_t bits = uint32_t(value >> 32);
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}
float Low(uint64_t value) {
  uint32_t bits = uint32_t(value);
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}
uint64_t Pair(float high, float low) {
  uint32_t h, l;
  std::memcpy(&h, &high, 4);
  std::memcpy(&l, &low, 4);
  return uint64_t(h) << 32 | l;
}

}  // namespace

namespace ng2::aspect {

namespace {
bool g_constantWritable = false;
}  // namespace

bool HudShown() {
  return g_hudShown.load(std::memory_order_relaxed);
}

float SqueezeX() {
  return CurrentSqueeze().x;
}
float SqueezeY() {
  return CurrentSqueeze().y;
}

uint32_t AspectIndex() {
  int32_t index = REXCVAR_GET(aspect_ratio);
  return index >= 0 && index < 4 ? uint32_t(index) : 0;
}

void SetConstantWritable(bool writable) {
  g_constantWritable = writable;
}

float Aspect() {
  if (!g_constantWritable) {
    return kNative;  // the 3D would stay 16:9
  }
  switch (AspectIndex()) {
    case 1:
      return 16.0f / 10.0f;
    case 2:
      return 21.0f / 9.0f;
    case 3:
      return 32.0f / 9.0f;
    default:
      return kNative;
  }
}

}  // namespace ng2::aspect

// The camera's update (sub_822FA8B8, r3 the camera): its projection
// (sub_822FB368) and right after it its other frustums (camera +688, +752)
// are built from the field of view at +176, so a narrower aspect's taller
// one is set around the whole update; the aspect constant is set first.
REX_EXTERN(__imp__sub_822FA8B8);
REX_HOOK_RAW(sub_822FA8B8) {
  float aspect = ng2::aspect::Aspect();
  if (LoadFloat(kAspectConstant) != aspect && ng2::aspect::g_constantWritable) {
    StoreFloat(kAspectConstant, aspect);
  }
  uint32_t fovAddress = ctx.r3.u32 + kCameraFov;
  if (aspect >= kNative || !ctx.r3.u32) {
    __imp__sub_822FA8B8(ctx, base);
    return;
  }
  float fov = LoadFloat(fovAddress);
  float taller = 2.0f * std::atan(std::tan(fov * 0.5f) * kNative / aspect);
  StoreFloat(fovAddress, taller);
  __imp__sub_822FA8B8(ctx, base);
  // The update may add an effect's change to it (sub_822FDDF8): kept.
  StoreFloat(fovAddress, fov + (LoadFloat(fovAddress) - taller));
}

// A sprite's or gauge's quad, in 1280x720 pixels.
REX_EXTERN(__imp__sub_8368EFE8);
REX_HOOK_RAW(sub_8368EFE8) {
  Squeeze s = CurrentSqueeze();
  uint32_t vertices = ctx.r4.u32;
  if (!s.active() || !vertices) {
    if (vertices) {
      NoteQuad(vertices, 24);
    }
    __imp__sub_8368EFE8(ctx, base);
    return;
  }
  constexpr uint32_t kStride = 24, kCount = 4;
  NoteQuad(vertices, kStride);
  uint8_t saved[kStride * kCount];
  std::memcpy(saved, Virtual(vertices), sizeof(saved));
  SqueezeQuad(vertices, kStride, s);
  __imp__sub_8368EFE8(ctx, base);
  std::memcpy(Virtual(vertices), saved, sizeof(saved));
}

// A string's transform.
REX_EXTERN(__imp__sub_8370DD20);
REX_HOOK_RAW(sub_8370DD20) {
  Squeeze s = CurrentSqueeze();
  NotePoint(High(ctx.r10.u64), Low(ctx.r10.u64));
  if (s.active()) {
    Anchor a = ItemAnchor(High(ctx.r10.u64), Low(ctx.r10.u64));
    for (PPCRegister* row : {&ctx.r4, &ctx.r6, &ctx.r8}) {
      row->u64 = Pair(High(row->u64) * s.x, Low(row->u64) * s.y);
    }
    // The translation row.
    ctx.r10.u64 = Pair(High(ctx.r10.u64) * s.x + MapOffset(s.x, a.x, 1280.0f),
                       Low(ctx.r10.u64) * s.y + MapOffset(s.y, a.y, 720.0f));
  }
  __imp__sub_8370DD20(ctx, base);
}

// A movie's quad, in clip space.
REX_EXTERN(__imp__sub_83660FC8);
REX_HOOK_RAW(sub_83660FC8) {
  uint32_t out = ctx.r3.u32;
  __imp__sub_83660FC8(ctx, base);
  Squeeze s = CurrentSqueeze();
  if (!s.active() || !out) {
    return;
  }
  constexpr uint32_t kStride = 16, kCount = 4;
  for (uint32_t i = 0; i < kCount; i++) {
    uint32_t at = out + i * kStride;
    StoreFloat(at, LoadFloat(at) * s.x);
    StoreFloat(at + 4, LoadFloat(at + 4) * s.y);
  }
}

// The 2D pass: its items' buckets first.
REX_EXTERN(__imp__sub_8368E2D8);
REX_HOOK_RAW(sub_8368E2D8) {
  ScanBuckets();
  __imp__sub_8368E2D8(ctx, base);
  g_drawBucket = -1;
  LogBoxes();
}

// The items' draw methods: sprite, gauge, text, rectangle, and the other.
REX_EXTERN(__imp__sub_8368FDA0);
REX_HOOK_RAW(sub_8368FDA0) {
  DrawTagged(ctx, base, __imp__sub_8368FDA0);
}
REX_EXTERN(__imp__sub_8368F568);
REX_HOOK_RAW(sub_8368F568) {
  DrawTagged(ctx, base, __imp__sub_8368F568);
}
REX_EXTERN(__imp__sub_8370D570);
REX_HOOK_RAW(sub_8370D570) {
  DrawTagged(ctx, base, __imp__sub_8370D570);
}
REX_EXTERN(__imp__sub_8368E878);
REX_HOOK_RAW(sub_8368E878) {
  DrawTagged(ctx, base, __imp__sub_8368E878);
}
REX_EXTERN(__imp__sub_8368EA48);
REX_HOOK_RAW(sub_8368EA48) {
  DrawTagged(ctx, base, __imp__sub_8368EA48);
}

// An untextured rectangle (sub_8368E878 builds it: 4 vertices 16 bytes
// apart, x, y, z, colour), e.g. the health bar's red damage piece.
REX_EXTERN(__imp__sub_8368E4F0);
REX_HOOK_RAW(sub_8368E4F0) {
  Squeeze s = CurrentSqueeze();
  uint32_t vertices = ctx.r4.u32;
  constexpr uint32_t kStride = 16, kCount = 4;
  if (vertices) {
    NoteQuad(vertices, kStride);
  }
  if (!s.active() || !vertices) {
    __imp__sub_8368E4F0(ctx, base);
    return;
  }
  uint8_t saved[kStride * kCount];
  std::memcpy(saved, Virtual(vertices), sizeof(saved));
  SqueezeQuad(vertices, kStride, s);
  __imp__sub_8368E4F0(ctx, base);
  std::memcpy(Virtual(vertices), saved, sizeof(saved));
}
