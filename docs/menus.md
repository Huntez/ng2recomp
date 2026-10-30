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

## Options state (10-29)

- 0x83CEB414: the selected category (0 Game Settings, 1 Controller, 2
  Camera, ...); 0x83CEB418 + 4 * category: that page's cursor row.
- Option values are not in the image's data: changing 1st/3rd-Person Camera
  Controls or Subtitles changed no word there; heap and physical diffs
  showed no clean candidate either (they may live in widget objects, or be
  committed on leaving the page).
- Page layouts: Camera Settings has two four-choice rows (Default /
  Vertical Inverted / Horizontal Inverted / All Inverted) and two sliders;
  Game Settings has a language list, a two-choice row and five On/Off-style
  rows; Screen Settings: Aspect Ratio, Video Adjustment, "???"; Audio: three
  sliders, Speaker Output (5.1 / Stereo), "???".

## Options pages (mapped 10-29, second pass)

- Page handlers (input, per frame): 0 category list `sub_823E5B60`, 1 Game
  `sub_823E6490`, 2 Controller `6AD8`, 3 Camera `7040`, 4 Screen `74E8`, 5
  Audio `5DB8`, 7 `80B8`, 11 `7A18`; 0xFF (the list's Back) ends the loop.
  0x83CEB3FC is the page before this frame, 0x83CEB400 the depth, and
  0x83CEB414 + 4 * page each page's cursor. The loop runs the handler only
  while the menu model's event is idle (`sub_836827B0`).
- Handler helpers: `sub_823E3448(*0x84F41264, pad byte 0x84B5030A, 1, 0,
  dir)` a direction (0 up, 1 down, 2 right, 3 left); `sub_823E3D78(&pad,
  cursor, enabled[], 0)` Confirm; `sub_823E3ED0(&pad, cursor, n, 0)` Back;
  `sub_823E3FD0(&pad)` Y (a row's default); `sub_82429DA8(id)` with f1 =
  *(float*)0x8206EE58 a sound (2 cursor, 3 choice).
- `sub_83682750(n, flag)`: transition table at 0x82181618 on the model
  0x85313E30 (5 stores the choice direction flag at model+0x380, 6 enter,
  7 back); `sub_83681830(model, lastPage, page, cursors, values, n)` keeps
  cursors and values as (current, previous) pairs (values of page p at
  0x85313EB0 + 0x40 * p) and raises page / cursor / value events.
- Page builders run once as Options opens; the category list's is
  `sub_836B10B8` (label ids as data at +0x34...), the Game page's
  `sub_836AC710` (vtable 0x822810F4). A choice row is a widget whose
  choices are added with `sub_836AA270(widget, string id)` (two per line,
  so any count works), then `sub_836AB540(page, label id, widget)` appends
  it as the next row. widget+0x14 is the selected choice (first set from
  the model at build), +0x600 = 1 marks the game's On/Off rows, whose value
  1 is the first choice.

## RECOMP SETTINGS (built 10-29, reworked twice the same day)

- src/recomp_settings.cpp: sub_8364CFC8 turns the Marketplace message (13)
  into Options (9) in recomp mode; sub_823E55B0 (the whole Options run)
  closes it. The category list's handler is ours (categories Graphics and
  Display, names written over category names 0x00 and 0x01, the rest
  blanked). Graphics is the Game page (1), Display the Controller page (2):
  their builders run with every row and choice call dropped, then our rows
  are appended (reset, place, choices or list texts, value, sub_836AB540,
  height). Arrow rows are list widgets (vtable 0x82281010, 0x270 bytes):
  the Game page's language list plus two made in place over its Voice
  Language choice widget (+0x450), as its constructor sub_83681EB0 makes
  one (sprites sub_836DFDE0 at +0x50/+0xE0/+0x170, part sub_823E41F0 at
  +0x200, +0x260 = 1). A list shows the pointer at +0x20 + 4 * value
  (+0x234 the current one) and is placed for the first row only: its value
  part's y (+0x244) and its arrow sprites' y (+0x34 in each) are moved down
  to its row. The overwritten widget is put back before that page is next
  built (the closing screen still draws ours). Texts go in the Music Test's
  track name slots (0x63-0x96); "Total Play Time" is blanked and the play
  time string (0x859D8DC4) cleared each frame. Changed settings are merged
  into the config file on closing.
- Title menu (sub_8364CBE8, entries of 0x140 bytes from 0x85387600: action
  +0x124, place +0x128, Xbox LIVE flag +0x130): LEADERBOARDS removed, our
  entry not greyed (and sub_8364CA08 reports it signed out), EXIT GAME
  added as action 7 (pictures by language * 8 + action from the table at
  0x83A93700; action 7 is the atlas's EXIT DEMO band).
- The Options header ("OPTIONS", a 172x58 8-bit picture, hash
  3e4f94c916758ea1) is a quad of inline vertices (positions 150..322 x
  41..99) over a full-screen one. While ours is open the renderer makes it
  a wider single-level picture (src/gpu/menu_label.cpp MakeHeader: the
  atlas letters at 0.92, faces and outlines brought to the header's levels,
  a box-blurred glow) and widens that quad from its left edge in
  FlushPendingDraw (inline data only reaches guest memory by then).
- Display page: Window Mode (Windowed / Borderless / Fullscreen: rex's
  fullscreen plus exclusive_fullscreen, the SDL display mode set first on
  the UI thread), Window Size (windowed_width/windowed_height, ours: rex reads
  window_width/height as the guest video mode; applied with
  SDL_SetWindowSize when windowed), V-Sync (present_mode: off, FIFO,
  mailbox, FIFO relaxed through plume's setPresentMode), Show FPS.
- Graphics also has Anti-Aliasing (antialiasing: 0, 2, 4, 8 samples for
  the targets the game multisamples, its 1120x584 main picture at 2x;
  stepped down to what the device supports; list made at Game page +0xA60).
- In gameplay, Back opens "IN-GAME MANUAL AND OPTIONS"; Start the
  inventory. The list's strings are table 4 (0x40000 + index: 0 View
  In-Game Manual ... 22 Quit Game, 23 NINJA CINEMA, 24 Return to Game).
  Builder sub_836722A8 (vtable slot 11 of 0x82280D04; object 0x8530BB80 in
  a run): 12 row pointers at +0x08..+0x34, row 0 the manual (with its
  topic list), choice rows set up by sub_8366FCA0(row, label, choice ids,
  count, flag), sliders at rows 3-4, Quit Game +0x4B40, Return to Game
  +0x4DA0. Handler sub_823E84F8: state 0x83CEB3B4 (6 leaves, 2 the
  manual, 1 Quit Game's confirmation), cursor 0x83CEB3C4 + 4 * state;
  values to the settings object, shown through sub_8366E238(row, value);
  model 0x8530B8B0. String tables: sub_823E0E60(*0x83BBC138, language,
  table << 16 | index); table 2 Options, 3 inventory, 4 this list, 8
  Xbox LIVE messages, 12/13 item texts.
- RECOMP SETTINGS there is shown after Save Method: rows are drawn at the
  place their index gives them (set at setup, vt slot 0), so ours and
  Quit Game / Return to Game get indices 10-12 while the list keeps the
  game's order (other code, like Quit Game's question, finds rows by the
  handler's index); the cursor mark's slot (+0x4EC0, set by the list's
  cursor event sub_83672BE8) is moved to the shown place. Quit Game's class
  keeps its label at +0xF4 and its question's texts at +0x20..+0x28.
- RECOMP SETTINGS there (src/recomp_settings.cpp, end): a 13th row (label
  row class 0x82280C80, made as the list's constructor sub_8366DD78 makes
  Return to Game) at list+0x38, added to the list's 12-row methods
  (sub_83672908 reset, sub_83672960 update, sub_83672A40 draw: x in r5's
  high word, y in the low; sub_83672AF8 event 1). The handler's moves onto
  and off it are ours (its own pad queries held back meanwhile). Confirm
  swaps the 12 row pointers for ours: 8 choice rows (class 0x82280C5C,
  constructor sub_8366E078, set up by sub_8366FCA0; 4 choices at most, so
  Render Scale and Window Size show their value alone), Back and 3 empty
  rows of no height. Rows only show a value when it changes (the model
  keeps the game's), so ours are pushed a dummy value first. Sending the
  list's event 1 (sub_83672AF8) closes the whole menu: ours get it row by
  row. The row the cursor last named must stay valid (the highlight reads
  it), so our entry stays at +0x38 throughout.
- show_fps (src/perf_overlay.cpp): a text item (sprite + text part, as the
  arrow lists' value text) in guest memory, given UI package 0's resource
  0xA (a text frame like package 0x78's 0x2A, which only Options loads),
  updated (sub_836E0128) and queued (sub_8366E8B8, layer 0x15) right after
  the game starts a frame (sub_822F0EF8 flips the 2D lists). Text sprites
  only draw through their frame's nodes (+0x74), so a resource is needed;
  the text part sits at +0x90 (string +0xC4, scale +0xC8, position +0xD0
  from the frame's anchor, flags +0x98, measured +0xF0). Asking the loader
  for package 0x78 outside Options broke the next level load.
- src/gpu/menu_label.cpp: the Marketplace label picture (520x90, hash
  f8fb387ec4883ffc) is rebuilt as RECOMP SETTINGS from the entries' atlas
  (520x990, hash 3737e258b350fa36), letters cut at bright-pixel valleys.
