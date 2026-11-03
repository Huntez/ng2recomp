// ng2 - Ninja Gaiden II static recompilation
//
// NG2_ROOM_JUMP=<from>:<to>: a test aid to reach a cutscene without playing
// up to it. The story scripts load "rooms" (a stage, plus the cutscene stages
// that come with it) and play their cutscenes with commands run by
// sub_824428A0: a command is a u16 opcode (bit 14 picks one of two handler
// tables; the story's are in the other, at 0x83AE8A70), a u16 argument count,
// then u32 arguments. B18 loads a room, B166 waits for it, B20 plays its
// cutscene, B19 unloads it. Rewriting all four from one room to another keeps
// the script consistent: the loader and the cutscene ask for the same room.
//
// Rooms (lookup sub_836559F0, extra stages sub_836557B0) with their cutscene
// stages (the stage table at 0x83B08F98: 174 RTM01_10, 175 RTM01_30, ...):
// 3 = the opening (RTM01_10), 5 = the end of Chapter 1 (RTM01_30), 11 has
// RTM03_10 (177), 22 RTM05_10 (180), 28 RTM06_10 (182), 30 RTM05_15 (181),
// 37 RTM07_50 (185). So NG2_ROOM_JUMP=3:5 and New Game play the cutscene
// before Chapter 2 about 25 s after Chapter 1's card, then go on to Chapter
// 1's first area.

#include <cstdio>
#include <cstdlib>

#include <rex/hook.h>
#include <rex/logging.h>

#include "gpu/guest.h"

namespace {

constexpr uint32_t kLoadRoom = 18, kPlayRoomCutscene = 20, kUnloadRoom = 19, kWaitRoom = 166;
constexpr uint32_t kTableBit = 1u << 14;

bool ParseJump(uint32_t& from, uint32_t& to) {
  static const char* spec = std::getenv("NG2_ROOM_JUMP");
  return spec && std::sscanf(spec, "%u:%u", &from, &to) == 2;
}

}  // namespace

REX_EXTERN(__imp__sub_824428A0);
REX_HOOK_RAW(sub_824428A0) {
  uint32_t from = 0, to = 0;
  if (ParseJump(from, to)) {
    uint32_t command = ctx.r4.u32;
    uint8_t* bytes = ng2::guest::Virtual(command);
    uint32_t op = uint32_t(bytes[0] << 8 | bytes[1]);
    uint32_t index = op & 0x3FFF;
    bool roomCommand = !(op & kTableBit) && (index == kLoadRoom || index == kWaitRoom ||
                                             index == kPlayRoomCutscene || index == kUnloadRoom);
    if (roomCommand && ng2::guest::Load32(command + 4) == from) {
      uint8_t* argument = ng2::guest::Virtual(command + 4);
      argument[0] = uint8_t(to >> 24), argument[1] = uint8_t(to >> 16);
      argument[2] = uint8_t(to >> 8), argument[3] = uint8_t(to);
      REXLOG_INFO("room jump: story command B{} for room {} now room {}", index, from, to);
    }
  }
  __imp__sub_824428A0(ctx, base);
}
