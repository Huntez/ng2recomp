// ng2 - Ninja Gaiden II static recompilation
//
// The Cheats page of RECOMP SETTINGS (recomp_settings.cpp).
//
// Infinite Health: damage to a character goes through sub_82411EF0 (r3 a
// record whose first word is the character), and the damage that lowers its
// most health (the red end of the bar) through sub_82412890 and
// sub_82412B68. Each gives none when the byte at 0x83C786DE is 1 (anyone),
// or 2 and the character is the player (two of the character's own flags,
// and in sub_82411EF0 the record's +10 being 0). It is set to 2 for those
// calls only, so enemies still take theirs.
//
// Infinite Ninpo: a character's ninpo is a u16 at +0x5942 (200 a charge).
// A ninpo's cost is its item's property 0x19 (sub_8370B4E8), read only to
// check a cast can be made (cost <= ninpo, sub_824123E8) and to spend it
// (sub_83707DA0, which spends nothing at a cost of 0): it costs 0.

#include <cstdint>

#include <rex/cvar.h>
#include <rex/hook.h>

#include "gpu/guest.h"

REXCVAR_DEFINE_BOOL(cheat_infinite_health, false, "Cheats", "The player takes no damage");
REXCVAR_DEFINE_BOOL(cheat_infinite_ninpo, false, "Cheats", "Casting ninpo doesn't spend it");

namespace {

constexpr uint32_t kNoDamage = 0x83C786DE;
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

REX_EXTERN(__imp__sub_82411EF0);
REX_HOOK_RAW(sub_82411EF0) {
  WithoutPlayerDamage(ctx, base, __imp__sub_82411EF0);
}
REX_EXTERN(__imp__sub_82412890);
REX_HOOK_RAW(sub_82412890) {
  WithoutPlayerDamage(ctx, base, __imp__sub_82412890);
}
REX_EXTERN(__imp__sub_82412B68);
REX_HOOK_RAW(sub_82412B68) {
  WithoutPlayerDamage(ctx, base, __imp__sub_82412B68);
}

REX_EXTERN(__imp__sub_8370B4E8);
REX_HOOK_RAW(sub_8370B4E8) {
  if (REXCVAR_GET(cheat_infinite_ninpo)) {
    ctx.r3.u64 = 0;
    return;
  }
  __imp__sub_8370B4E8(ctx, base);
}
