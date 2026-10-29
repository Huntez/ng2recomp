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
