# Menus (notes for RECOMP SETTINGS)

Goal: the title menu's "Xbox LIVE Marketplace" becomes "RECOMP SETTINGS",
opening pages of ours in the game's own Options menu (as the NGB recomp does).

## Title menu

- Selection handler: `sub_8364D0C0` (ng2_recomp.499.cpp). Menu state at
  0x83386F10: entry count +0x15F0, cursor +0x15F4, entries from +0x814,
  0x140 bytes each; an entry's first word is its action type, dispatched
  through the jump table at 0x8364D284.
- Action types: 0 NEW GAME (screen 2), 1 LOAD GAME (screen 1, or message 10
  when signed out), 2 probably MISSIONS (screen 6), 3 LEADERBOARDS (7 / 11),
  4 NINJA CINEMA (8 / 12), 5 OPTIONS (screen 9), 6 Xbox LIVE Marketplace
  (XamShowMarketplaceUI via thunk 0x8380DEF8 when signed in, else message 13).
- Screens and messages open through `sub_8364CFC8(*(menu + 4), id)`;
  `sub_8364CA08` is the signed-in check.
- Plan: when type 6 is chosen, open screen 9 (Options) in a "recomp
  settings" mode instead of message 13.

## Labels

- Title entries are pictures: one 520x990 8-bit atlas (EXIT DEMO, NEW GAME,
  LOAD GAME, LEADERBOARDS, NINJA CINEMA, OPTIONS, MISSIONS, the difficulty
  names) and a separate 520x90 texture for "Xbox LIVE Marketplace", drawn by
  the generic sprite renderer (`837DCCB0` <- `8366BED8` <- `8366CF54` <-
  `8366B1E8`). Every letter of RECOMP SETTINGS is in the atlas, so the new
  label can be composed at runtime from the game's own glyphs (no shipped
  art), e.g. in the renderer when the label texture's content hash is seen.

## Options screen

- Categories: Game Settings, Controller Settings, Camera Settings, Screen
  Settings, Audio Settings, Copyright Information, Restore Defaults, Return
  to Main Menu. Rows are two-choice toggles or "<- value ->" lists.
- Its text is a UTF-16BE string table loaded from other.ng2 (physical
  0xC454D70 onwards in one run): preceded by a table of byte lengths, so
  strings are referenced by index. Still to map: the screen's update/draw
  code, its page and row structures, and how a row names its strings.

## Options screen internals (mapped 10-29)

- Screens live in a manager (one at 0x852E1428 in a run) as 8-byte objects
  at mgr+8*k; `sub_8364C6F0(mgr, id)` maps an id to its object (jump table
  at 0x8364C708), `sub_8364CFC8` pushes it on a stack at mgr+136 (count at
  +48) and calls vtable[1] (enter); vtable[2] is the per-frame update.
  Title menu: vtable 0x82280014, update `sub_8364D0C0`. Options (id 9,
  mgr+0x48): vtable 0x822800C8, enter `sub_8364DDF8` (sets the UI mode word
  at 0x83386F10+12 to 11; leave sets 5), update `sub_8364DEF0`.
- The Options pages are an object-oriented widget framework: widgets are
  built by constructors such as `sub_836ACF70` (the controller diagram,
  storing the label table 0x83BB8D18 / 0x83BB8E20 in the object) and keep
  string pointers resolved at runtime.
- Option strings: table 2, id 0x00020000 + index; the strings are stored
  back to back, each padded to 16 bytes, from "Game Settings" (index 0).
  0x0002009B is "Quick Attack". A grid of row string ids (Aspect Ratio
  16:9/4:3, Color/Monochrome, Gamma, Brightness...) sits at 0x83A936D8 in
  the image but nothing reads it after boot (gdb hardware watchpoints never
  fired there, nor on a known-read address: they don't work on this binary).
- Cheapest native path found so far: reuse existing pages in "recomp mode",
  rewriting their strings in place (same padded slot sizes) and mapping our
  settings onto the rows' values, restoring both on leaving.
