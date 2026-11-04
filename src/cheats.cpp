// ng2 - Ninja Gaiden II static recompilation
//
// The Cheats page of RECOMP SETTINGS (recomp_settings.cpp).
//
// Infinite Health: damage to a character goes through sub_82412508 (r3 a
// record whose first word is the character), and the damage that lowers its
// most health (the red end of the bar) through sub_82412EA8 and
// sub_824131E0. Each gives none when the byte at 0x83CB5992 is 1 (anyone),
// or 2 and the character is the player (two of the character's own flags,
// and in sub_82412508 the record's +10 being 0). It is set to 2 for those
// calls only, so enemies still take theirs.
//
// Infinite Ninpo: a character's ninpo is a u16 at +0x5942 (200 a charge).
// A ninpo's cost is its item's property 0x19 (sub_83730AA8), read only to
// check a cast can be made (cost <= ninpo, sub_82412A00) and to spend it
// (sub_8372D360, which spends nothing at a cost of 0): it costs 0. The HUD's
// gauge still showed a cast's drain, so the player's ninpo is also kept at
// its most (+0x5940) each frame in play (CheatsFrame, from the frame's
// start): the gauge stays full.
//
// Unlock Difficulties: sub_823E1098(clears, d) says difficulty d is open
// (some difficulty at or above d - 1 cleared). The New Game path list
// (sub_8366F4D0) asks it for Mentor and Master Ninja; the answer is yes for
// those two calls. Mission Mode doesn't ask: choosing a mission
// (sub_823E4758) sets how many difficulties it offers (1, 2, 3 or 4) from
// the mission's record in the profile (cleared flags 0x10..0x80 in byte 7
// of [0x84C559E0] + 1016 + 8 * mission), except for missions whose table
// entry (0x83B2E334 + 12 * mission) is 3, which offer one. A hook right
// after the count is set raises it to 4.
//
// Unlock Costumes: the costume list (sub_823E1448) adds a costume per
// difficulty the same check says is cleared; it is yes there too.
//
// Neither writes the unlocks to the profile (a game played on a difficulty
// or in a costume they open is saved as usual): switched off, the menus
// are as before.

#include <cstdint>
#include <cstring>

#include <rex/cvar.h>
#include <rex/hook.h>

#include "aspect.h"
#include "gpu/guest.h"

REXCVAR_DEFINE_BOOL(cheat_infinite_health, false, "Cheats", "The player takes no damage");
REXCVAR_DEFINE_BOOL(cheat_infinite_ninpo, false, "Cheats", "Casting ninpo doesn't spend it");
REXCVAR_DEFINE_BOOL(cheat_unlock_difficulties, false, "Cheats",
                    "Every difficulty in New Game and Mission Mode");
REXCVAR_DEFINE_BOOL(cheat_unlock_costumes, false, "Cheats", "Every costume");

namespace {

constexpr uint32_t kNoDamage = 0x83CB5992;
constexpr uint8_t kPlayerOnly = 2;

// A damage function with the player's no-damage flag set.
void WithoutPlayerDamage(PPCContext& ctx, uint8_t* base, void (*damage)(PPCContext&, uint8_t*)) {
  uint8_t* flag = ng2::guest::Virtual(kNoDamage);
  uint8_t game = *flag;
  bool ours = REXCVAR_GET(cheat_infinite_health) && game == 0;
  if (ours) {
    *flag = kPlayerOnly;
  }
  damage(ctx, base);
  if (ours && *flag == kPlayerOnly) {
    *flag = game;
  }
}

}  // namespace

REX_EXTERN(__imp__sub_82412508);
REX_HOOK_RAW(sub_82412508) {
  WithoutPlayerDamage(ctx, base, __imp__sub_82412508);
}
REX_EXTERN(__imp__sub_82412EA8);
REX_HOOK_RAW(sub_82412EA8) {
  WithoutPlayerDamage(ctx, base, __imp__sub_82412EA8);
}
REX_EXTERN(__imp__sub_824131E0);
REX_HOOK_RAW(sub_824131E0) {
  WithoutPlayerDamage(ctx, base, __imp__sub_824131E0);
}

// The player's record is the first of the characters' 24-byte records; the
// game reads its character straight from 0x84C3DC60. The game's own test that
// a record's character is the player (sub_82412508): the record's +10 is 0,
// the character's +0x59D3 isn't 0xFF and its +0x59BF is 0.
constexpr uint32_t kPlayerRecord = 0x84C3DC60;
constexpr uint32_t kNinpo = 0x5942, kNinpoMost = 0x5940;

REX_EXTERN(__imp__sub_83730AA8);
REX_HOOK_RAW(sub_83730AA8) {
  if (REXCVAR_GET(cheat_infinite_ninpo)) {
    ctx.r3.u64 = 0;
    return;
  }
  __imp__sub_83730AA8(ctx, base);
}

// The unlock check, by caller (its return address).
REX_EXTERN(__imp__sub_823E1098);
REX_HOOK_RAW(sub_823E1098) {
  uint32_t from = uint32_t(ctx.lr);
  bool paths = from == 0x8366F874 || from == 0x8366F8A8;
  bool costumes = from == 0x823E1714 || from == 0x823E174C || from == 0x823E1784 ||
                  from == 0x823E17BC;
  if ((paths && REXCVAR_GET(cheat_unlock_difficulties)) ||
      (costumes && REXCVAR_GET(cheat_unlock_costumes))) {
    ctx.r3.u64 = 1;
    return;
  }
  __imp__sub_823E1098(ctx, base);
}

// Mission Mode: r11 the difficulties offered, r31 the mission screen
// (+4: the mission). ng2_codegen.toml places it at 0x823E4A80.
void Ng2MissionDifficulties(PPCRegister& r11, PPCRegister& r31) {
  constexpr uint32_t kMissionTable = 0x83B2E334;  // 12 bytes a mission
  constexpr uint32_t kFixedDifficulty = 3;
  constexpr uint32_t kAll = 4;
  if (!REXCVAR_GET(cheat_unlock_difficulties)) {
    return;
  }
  uint32_t mission = ng2::guest::Load32(r31.u32 + 4);
  if (mission < 100 && ng2::guest::Load32(kMissionTable + 12 * mission) != kFixedDifficulty &&
      r11.u32 < kAll) {
    // The screen keeps the count at +48 (the choice, its cursor's wrap);
    // r11 places the cursor once.
    r11.u64 = kAll;
    uint8_t* count = ng2::guest::Virtual(r31.u32 + 48);
    count[0] = 0, count[1] = 0, count[2] = 0, count[3] = uint8_t(kAll);
  }
}

// Each frame (perf_overlay.cpp's frame start): Infinite Ninpo keeps the
// gauge full.
void CheatsFrame() {
  // In play only: before it the record holds something else.
  if (!REXCVAR_GET(cheat_infinite_ninpo) || !ng2::aspect::HudShown()) {
    return;
  }
  uint32_t player = ng2::guest::Load32(kPlayerRecord);
  if (player < 0x40000000u || player >= 0xA0000000u ||
      ng2::guest::Virtual(kPlayerRecord)[10] != 0 ||
      ng2::guest::Virtual(player)[0x59D3] == 0xFF || ng2::guest::Virtual(player)[0x59BF] != 0) {
    return;
  }
  uint16_t most = ng2::guest::Load16(player + kNinpoMost);
  if (most && most <= 0x2000 && ng2::guest::Load16(player + kNinpo) < most) {
    uint16_t value = rex::byte_swap(most);
    std::memcpy(ng2::guest::Virtual(player + kNinpo), &value, 2);
  }
}
