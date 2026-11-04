// ng2 - Ninja Gaiden II static recompilation
//
// The PC has one storage device, so the game's "Change Storage Device"
// prompts (Load Game's button line, the Options pages' hints, NINJA CINEMA's)
// are left out: each string the game's text lookup (sub_837578C8: language,
// id) hands out is checked, and a "@[pad:..]Change Storage Device" in it is
// cut out where it lies (the table's own copy, so once), the rest of the
// string kept. Untagged labels of the same words are left alone.

#include <cstdint>
#include <cstring>
#include <string_view>

#include <rex/hook.h>

#include "gpu/guest.h"

namespace {

// The prompt's text in the languages it has been seen in.
constexpr std::u16string_view kTexts[] = {u"Change Storage Device",
                                          u"Changer le périphérique de stockage"};
constexpr std::u16string_view kTag = u"@[pad:";
constexpr uint32_t kMaxChars = 1024;

char16_t At(const uint8_t* p) {
  return char16_t(p[0] << 8 | p[1]);
}

bool Matches(const uint8_t* p, std::u16string_view text) {
  for (size_t i = 0; i < text.size(); i++) {
    if (At(p + 2 * i) != text[i]) {
      return false;
    }
  }
  return true;
}

// A UTF-16 BE string at a guest address: the tagged prompt cut out.
void CutPrompt(uint32_t address) {
  // (heap only: the image's read-only data can't be written)
  if (address < 0x40000000u || address >= 0xF0000000u ||
      (address >= 0x82000000u && address < 0x90000000u)) {
    return;
  }
  uint8_t* s = ng2::guest::Virtual(address);
  uint32_t length = 0;
  while (length < kMaxChars && At(s + 2 * length) != 0) {
    length++;
  }
  if (length == kMaxChars) {
    return;
  }
  for (uint32_t tag = 0; tag + kTag.size() < length; tag++) {
    if (!Matches(s + 2 * tag, kTag)) {
      continue;
    }
    uint32_t close = tag + uint32_t(kTag.size());
    while (close < length && At(s + 2 * close) != u']') {
      close++;
    }
    uint32_t text = close + 1;
    while (text < length && At(s + 2 * text) == u' ') {
      text++;
    }
    for (std::u16string_view prompt : kTexts) {
      if (text + prompt.size() > length || !Matches(s + 2 * text, prompt)) {
        continue;
      }
      uint32_t end = text + uint32_t(prompt.size());
      while (end < length && At(s + 2 * end) == u' ') {
        end++;
      }
      std::memmove(s + 2 * tag, s + 2 * end, 2 * (length - end));
      std::memset(s + 2 * (tag + length - end), 0, 2 * (end - tag));
      return;
    }
  }
}

}  // namespace

REX_EXTERN(__imp__sub_837578C8);
REX_HOOK_RAW(sub_837578C8) {
  __imp__sub_837578C8(ctx, base);
  CutPrompt(ctx.r3.u32);
}
