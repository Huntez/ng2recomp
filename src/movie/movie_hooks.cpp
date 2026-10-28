// ng2 - Ninja Gaiden II static recompilation
//
// The game's movie player is the XDK's XMedia2 XMV player (library at
// 0x8386xxxx). Its WMV decoder runs part of its work on the GPU and reads
// the results back, which neither renderer provides: movies end at once or
// stall the game (the opening's billboard). So by default the player is
// replaced: creating one opens a HostMovie (FFmpeg) and returns a small
// guest object standing for it, and the method wrappers the game calls (it
// never calls the player's vtable itself) are answered on the host.
// NG2_MOVIES=xmedia keeps the original player; NG2_MOVIE_TRACE=1 logs calls.
//
// What the game uses (from its calls and a trace of the original):
//   create(device, params, &player): params +4 flags (0x8: loop, the
//     opening's billboard), +24 audio stream (-1 none), +32 path
//     ("game:\x.wmv")
//   +72 frame(player, desc): desc +12/+16/+20 Y/U/V addresses (linear L8
//     textures the game created), +24/+28/+32 sizes, +36/+40/+44 pitches;
//     S_OK, or 0x16660026 once the movie has ended
//   +80 render(player, 0, 0): draw the frame to the screen (full-screen
//     movies)
//   +112 status via 83862980(player, out): out[0] < 6 playing
//   +120 audio(player, &voice): the game sets voice+140 to the volume and
//     calls voice->vtbl[+92]
//   +8 release, +96 stop, +92 and 83862888 at the start of full-screen
//     movies
//
// The original player also allocates physical memory for its frames when
// it is created, before the game creates the textures the frames go to.
// The game depends on that layout: its title movie setup clears a texture
// that, without those allocations, lands on the archive's table of contents
// (0xEBEB0000), and the title's font and sprites then never load. So the
// stand-in makes the same physical allocations (ReservePhysical) and holds
// them until the game releases the player.

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/xmemory.h>

#include "../gpu/guest.h"
#include "host_movie.h"
#include "movie.h"

REX_EXTERN(__imp__sub_83810258);  // XMemAlloc(size, attributes)
REX_EXTERN(__imp__sub_838102F0);  // XMemFree(address, attributes)

namespace {

using ng2::guest::Load32;
using ng2::guest::LoadFloat;
using ng2::guest::Virtual;

constexpr uint32_t kEndOfMovie = 0x16660026;
constexpr uint32_t kFail = 0x80004005;  // E_FAIL
// A guest function that returns 0 (li r3,0; blr): every slot of the
// stand-in audio voice's vtable.
constexpr uint32_t kReturnZero = 0x837C1A60;
constexpr uint32_t kVoiceVtableSlots = 64;
constexpr uint32_t kVoiceSize = 256;
constexpr uint32_t kObjectSize = 64;

bool TraceEnabled() {
  static const bool enabled = [] {
    const char* v = std::getenv("NG2_MOVIE_TRACE");
    return v && v[0] == '1';
  }();
  return enabled;
}

bool HostMovies() {
  static const bool enabled = [] {
    const char* v = std::getenv("NG2_MOVIES");
    return !(v && std::string_view(v) == "xmedia");
  }();
  return enabled;
}

void Store32(uint32_t address, uint32_t value) {
  *reinterpret_cast<uint32_t*>(Virtual(address)) = rex::byte_swap(value);
}

rex::memory::Memory* GuestMemory() {
  return rex::Runtime::instance()->memory();
}

// Calls a guest function with r3/r4 on the caller's guest stack, leaving
// the caller's registers as they were.
uint32_t CallGuest(PPCContext& ctx, uint8_t* base, void (*function)(PPCContext&, uint8_t*),
                   uint32_t r3, uint32_t r4) {
  PPCContext saved = ctx;
  ctx.r3.u64 = r3;
  ctx.r4.u64 = r4;
  function(ctx, base);
  uint32_t result = ctx.r3.u32;
  ctx = saved;
  return result;
}

struct Player {
  std::unique_ptr<ng2::movie::HostMovie> movie;  // null if the file didn't open
  uint32_t voice = 0;                             // guest stand-in audio voice
  std::vector<std::pair<uint32_t, uint32_t>> reserved;  // (address, attributes)
  bool reportedEnd = false;
};

std::mutex g_mutex;
std::unordered_map<uint32_t, Player> g_players;  // by guest object address
std::filesystem::path g_gameDataRoot;

// "game:\name.wmv" -> <game data root>/name.wmv
std::filesystem::path HostPath(const char* guestPath) {
  std::string_view path(guestPath);
  if (auto colon = path.find(":\\"); colon != std::string_view::npos) {
    path.remove_prefix(colon + 2);
  }
  std::string relative(path);
  std::replace(relative.begin(), relative.end(), '\\', '/');
  return g_gameDataRoot / relative;
}

// The physical allocations the original player makes when created (traced
// for the 320x580 title movies): control blocks, then three frames of Y, U
// and V with 256-byte aligned pitches.
void ReservePhysical(PPCContext& ctx, uint8_t* base, Player& player, int width, int height) {
  auto align = [](uint32_t value) { return (value + 255) & ~255u; };
  std::vector<std::pair<uint32_t, uint32_t>> blocks = {
      {0x0C, 0xB2800000}, {0x78, 0xB2800000}, {0x60, 0xB5800000},
      {0xB8, 0xB5800000}, {0xB8, 0xB5800000},
  };
  uint32_t lumaSize = align(uint32_t(width)) * uint32_t(height);
  uint32_t chromaSize = align(uint32_t(width + 1) / 2) * uint32_t((height + 1) / 2);
  for (int frame = 0; frame < 3; frame++) {
    blocks.push_back({lumaSize, 0xBC800000});
    blocks.push_back({chromaSize, 0xBC800000});
    blocks.push_back({chromaSize, 0xBC800000});
  }
  for (auto [size, attributes] : blocks) {
    uint32_t address = CallGuest(ctx, base, __imp__sub_83810258, size, attributes);
    if (address) {
      player.reserved.push_back({address, attributes});
    }
  }
}

void FreeReserved(PPCContext& ctx, uint8_t* base, Player& player) {
  for (auto it = player.reserved.rbegin(); it != player.reserved.rend(); ++it) {
    CallGuest(ctx, base, __imp__sub_838102F0, it->first, it->second);
  }
  player.reserved.clear();
}

uint32_t CreateVoice() {
  uint32_t vtable = GuestMemory()->SystemHeapAlloc(kVoiceVtableSlots * 4);
  uint32_t voice = GuestMemory()->SystemHeapAlloc(kVoiceSize);
  if (!vtable || !voice) {
    return 0;
  }
  for (uint32_t i = 0; i < kVoiceVtableSlots; i++) {
    Store32(vtable + i * 4, kReturnZero);
  }
  std::memset(Virtual(voice), 0, kVoiceSize);
  Store32(voice, vtable);
  return voice;
}

void FreeVoice(uint32_t voice) {
  if (voice) {
    GuestMemory()->SystemHeapFree(Load32(voice));
    GuestMemory()->SystemHeapFree(voice);
  }
}

// The host player for a guest object, or null if it isn't one.
Player* Find(uint32_t object) {
  auto it = g_players.find(object);
  return it != g_players.end() ? &it->second : nullptr;
}

}  // namespace

namespace ng2::movie {

void SetGameDataRoot(const std::filesystem::path& root) {
  g_gameDataRoot = root;
}

}  // namespace ng2::movie

// XMedia2CreateXmvPlayer(device, parameters, &player)
REX_EXTERN(__imp__sub_83862EB8);
REX_HOOK_RAW(sub_83862EB8) {
  uint32_t params = ctx.r4.u32, out = ctx.r5.u32;
  uint32_t pathAddress = Load32(params) == 1 ? Load32(params + 32) : 0;
  const char* path = pathAddress ? reinterpret_cast<const char*>(Virtual(pathAddress)) : "";
  if (!HostMovies()) {
    __imp__sub_83862EB8(ctx, base);
    if (TraceEnabled()) {
      REXLOG_INFO("movie: create '{}' -> {:#x} player {:#x}", path, ctx.r3.u32, Load32(out));
    }
    return;
  }
  std::lock_guard lock(g_mutex);
  uint32_t object = pathAddress ? GuestMemory()->SystemHeapAlloc(kObjectSize) : 0;
  if (!object) {
    ctx.r3.u64 = kFail;
    return;
  }
  std::memset(Virtual(object), 0, kObjectSize);
  Player player;
  player.movie = ng2::movie::HostMovie::Open(HostPath(path), Load32(params + 24) != 0xFFFFFFFF,
                                             (Load32(params + 4) & 0x8) != 0);
  if (player.movie) {
    ReservePhysical(ctx, base, player, player.movie->Width(), player.movie->Height());
  }
  player.voice = CreateVoice();
  if (TraceEnabled()) {
    REXLOG_INFO("movie: create '{}' -> host player {:#x} ({} reserved blocks)", path, object,
                player.reserved.size());
  }
  g_players[object] = std::move(player);
  Store32(out, object);
  ctx.r3.u64 = 0;
}

// +72: the frame due now into the caller's Y/U/V planes
REX_EXTERN(__imp__sub_83680E48);
REX_HOOK_RAW(sub_83680E48) {
  uint32_t object = ctx.r3.u32, desc = ctx.r4.u32;
  {
    std::lock_guard lock(g_mutex);
    if (Player* player = Find(object)) {
      bool playing = false;
      if (player->movie) {
        ng2::movie::HostMovie::Planes planes;
        for (int p = 0; p < 3; p++) {
          uint32_t address = Load32(desc + 12 + p * 4);
          planes.data[p] = address ? Virtual(address) : nullptr;
          planes.size[p] = Load32(desc + 24 + p * 4);
          planes.pitch[p] = Load32(desc + 36 + p * 4);
        }
        playing = player->movie->CopyFrame(planes);
        if (player->voice) {
          player->movie->SetVolume(LoadFloat(player->voice + 140));
        }
      }
      if (!playing && TraceEnabled() && !player->reportedEnd) {
        player->reportedEnd = true;
        REXLOG_INFO("movie: {:#x} ended", object);
      }
      ctx.r3.u64 = playing ? 0 : kEndOfMovie;
      return;
    }
  }
  __imp__sub_83680E48(ctx, base);
}

// +80: draw the frame to the screen
REX_EXTERN(__imp__sub_83680E58);
REX_HOOK_RAW(sub_83680E58) {
  {
    std::lock_guard lock(g_mutex);
    if (Find(ctx.r3.u32)) {
      static bool logged = false;
      if (!logged) {
        logged = true;
        REXLOG_WARN("movie: full-screen movies are not drawn yet");
      }
      ctx.r3.u64 = 0;
      return;
    }
  }
  __imp__sub_83680E58(ctx, base);
}

// status(player, out): out[0] = state (6 and up: finished), out[1]
REX_EXTERN(__imp__sub_83862980);
REX_HOOK_RAW(sub_83862980) {
  uint32_t object = ctx.r3.u32, out = ctx.r4.u32;
  {
    std::lock_guard lock(g_mutex);
    if (Player* player = Find(object)) {
      bool finished = !player->movie || player->movie->Finished();
      Store32(out, finished ? 6 : 1);
      Store32(out + 4, 0);
      ctx.r3.u64 = 0;
      return;
    }
  }
  __imp__sub_83862980(ctx, base);
}

// +120: the movie's audio voice
REX_EXTERN(__imp__sub_83867CB8);
REX_HOOK_RAW(sub_83867CB8) {
  uint32_t object = ctx.r3.u32, out = ctx.r4.u32;
  {
    std::lock_guard lock(g_mutex);
    if (Player* player = Find(object)) {
      Store32(out, player->voice);
      ctx.r3.u64 = player->voice ? 0 : kFail;
      return;
    }
  }
  __imp__sub_83867CB8(ctx, base);
}

// +8: release
REX_EXTERN(__imp__sub_83862878);
REX_HOOK_RAW(sub_83862878) {
  uint32_t object = ctx.r3.u32;
  {
    std::lock_guard lock(g_mutex);
    if (auto it = g_players.find(object); it != g_players.end()) {
      if (TraceEnabled()) {
        REXLOG_INFO("movie: release {:#x}", object);
      }
      FreeReserved(ctx, base, it->second);
      FreeVoice(it->second.voice);
      g_players.erase(it);
      GuestMemory()->SystemHeapFree(object);
      ctx.r3.u64 = 0;
      return;
    }
  }
  __imp__sub_83862878(ctx, base);
}

// Methods with nothing to do on the host: +96 stop, +92 and 83862888 (at
// the start of full-screen movies).
#define NG2_PLAYER_NO_OP(addr)                     \
  REX_EXTERN(__imp__sub_##addr);                   \
  REX_HOOK_RAW(sub_##addr) {                       \
    {                                              \
      std::lock_guard lock(g_mutex);               \
      if (Find(ctx.r3.u32)) {                      \
        ctx.r3.u64 = 0;                            \
        return;                                    \
      }                                            \
    }                                              \
    __imp__sub_##addr(ctx, base);                  \
  }

NG2_PLAYER_NO_OP(83862970)
NG2_PLAYER_NO_OP(83862960)
NG2_PLAYER_NO_OP(83862888)
