// ng2 - Ninja Gaiden II static recompilation
//
// The Cheats page of RECOMP SETTINGS (recomp_settings.cpp).
//
// Infinite Health: damage to a character goes through sub_82412488 (r3 a
// record whose first word is the character), and the damage that lowers its
// most health (the red end of the bar) through sub_82412E28 and
// sub_82413160. Each gives none when the byte at 0x83CB5992 is 1 (anyone),
// or 2 and the character is the player (two of the character's own flags,
// and in sub_82412488 the record's +10 being 0). It is set to 2 for those
// calls only, so enemies still take theirs.
//
// Infinite Ninpo: a character's ninpo is a u16 at +0x5942 (200 a charge).
// A ninpo's cost is its item's property 0x19 (sub_83730A58), read only to
// check a cast can be made (cost <= ninpo, sub_82412980) and to spend it
// (sub_8372D310, which spends nothing at a cost of 0): it costs 0.
//
// Unlock Difficulties: sub_823E1060(clears, d) says difficulty d is open
// (some difficulty at or above d - 1 cleared). The New Game path list
// (sub_8366F4D0) asks it for Mentor and Master Ninja; the answer is yes for
// those two calls. Mission Mode doesn't ask: choosing a mission
// (sub_823E4720) sets how many difficulties it offers (1, 2, 3 or 4) from
// the mission's record in the profile (cleared flags 0x10..0x80 in byte 7
// of [0x84C559E0] + 1016 + 8 * mission), except for missions whose table
// entry (0x83B2E334 + 12 * mission) is 3, which offer one. A hook right
// after the count is set raises it to 4.
//
// Unlock Costumes: the costume list (sub_823E1410) adds a costume per
// difficulty the same check says is cleared; it is yes there too.
//
// Neither writes the unlocks to the profile (a game played on a difficulty
// or in a costume they open is saved as usual): switched off, the menus
// are as before.

#include <cstdint>

#include <rex/cvar.h>
#include <rex/hook.h>

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

REX_EXTERN(__imp__sub_82412488);
REX_HOOK_RAW(sub_82412488) {
  WithoutPlayerDamage(ctx, base, __imp__sub_82412488);
}
REX_EXTERN(__imp__sub_82412E28);
REX_HOOK_RAW(sub_82412E28) {
  WithoutPlayerDamage(ctx, base, __imp__sub_82412E28);
}
REX_EXTERN(__imp__sub_82413160);
REX_HOOK_RAW(sub_82413160) {
  WithoutPlayerDamage(ctx, base, __imp__sub_82413160);
}

REX_EXTERN(__imp__sub_83730A58);
REX_HOOK_RAW(sub_83730A58) {
  if (REXCVAR_GET(cheat_infinite_ninpo)) {
    ctx.r3.u64 = 0;
    return;
  }
  __imp__sub_83730A58(ctx, base);
}

// The unlock check, by caller (its return address).
REX_EXTERN(__imp__sub_823E1060);
REX_HOOK_RAW(sub_823E1060) {
  uint32_t from = uint32_t(ctx.lr);
  bool paths = from == 0x8366F874 || from == 0x8366F8A8;
  bool costumes = from == 0x823E16DC || from == 0x823E1714 || from == 0x823E174C ||
                  from == 0x823E1784;
  if ((paths && REXCVAR_GET(cheat_unlock_difficulties)) ||
      (costumes && REXCVAR_GET(cheat_unlock_costumes))) {
    ctx.r3.u64 = 1;
    return;
  }
  __imp__sub_823E1060(ctx, base);
}

// Mission Mode: r11 the difficulties offered, r31 the mission screen
// (+4: the mission). ng2_codegen.toml places it at 0x823E4A48.
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
