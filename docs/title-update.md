# Title updates

Since 2026-11-03 the port runs Ninja Gaiden II with Title Update 3 (0.0.3.4), the last update for
the World disc (media ID 69F555CB): the update's `default.xexp` sits next to `default.xex` and the
SDK applies it, both when generating the code and when the game starts. The installers ask for
the update package (`TU_1A461UL_000000G000000.00000000000O4`, 2,486,272 bytes) once, extract its
`default.xexp` (2,416,640 bytes, SHA-1 `e9df578781355d9a25e1c0adba8fbb859333456a`) and check it;
an earlier install's TU2 is replaced. The Japanese release's TU3
(`TU_1A461UL_000000C000000.00000000000O3`, media ID 0E6FB1CE) has the same size but patches that
disc's 1.0 (version 3), not this one's (version 4), so it is refused.

What the updates changed isn't documented. Reports from 2008 tell of a late-July update that came
with the Mission Mode DLC and froze cutscenes in chapters 10, 12 and 14 for players on Xbox Live,
and a patch at the end of August that fixed those freezes and sped up the cutscene transitions;
TU2 and TU3 are most likely those two (an inference: the files carry no dates).

## Addresses, TU2 to TU3

TU3 moves less than TU2 did (most addresses by under 0x900 bytes; the game's data, the 2D buckets,
the menus' models and views, the title menu's entries don't move). The table maps every address
the code used under TU2 (in `src/` and `ng2_codegen.toml`) that TU3 moves.
`ng2_pointer_funcs.toml` was regenerated (`tools/find_pointer_funcs.py`, 48 functions; TU2: 45).

Mapped with `tools/map_addresses.py <TU2 image> <TU3 image> <repo>` (both dumped with
`ng2_dump_image`), then checked by hand:
- read-only data by content (vtables by the functions their slots point to, the stage table by
  its strings); the title menu's picture table moved by 8 bytes (TU3 adds two entries before it);
- data reached only as base + offset by the bases the code builds near them (hundreds of sites
  agree on no move): the in-game menu's views, confirmed in TU3's own notifier (83691480..4D0),
  which calls 83692378 for 85326650, 853268A0 and 8532B800 as TU2's did (the map's 853268A8 for
  853268A0 rested on one site and was wrong);
- 83692368 (the menu font's scale) moved far, to 836CEDE8: found through its 19 callers;
- the small functions by their bytes and the slots pointing at them, where the mapper's windows
  landed on a neighbour: the movie player's `lwz; lwz; mtctr; bctr` thunks (+8 83888098, +0x48
  836A41A8, +0x50 836A41B8, +0x5C 83888FD8, +0x60 83888180, +0x78 838881F8) and `kReturnZero`
  (8242C148, `li r3,0; blr`, which TU3's vtables use where TU2's used 836A3860).

| TU2 | TU3 |
|---|---|
| 8206EF3C | 8206EE68 |
| 82286610 | 82286618 |
| 82286658 | 82286660 |
| 822869A0 | 822869B0 |
| 8228A1B8 | 8228A1C8 |
| 8228EBDC | 8228EBFC |
| 822FA8B8 | 822FA8C8 |
| 822FB368 | 822FB378 |
| 822FDDF8 | 822FDE08 |
| 823E1060 | 823E1098 |
| 823E1200 | 823E1238 |
| 823E1410 | 823E1448 |
| 823E16DC | 823E1714 |
| 823E1714 | 823E174C |
| 823E174C | 823E1784 |
| 823E1784 | 823E17BC |
| 823E37F0 | 823E3828 |
| 823E4120 | 823E4158 |
| 823E4278 | 823E42B0 |
| 823E4378 | 823E43B0 |
| 823E4720 | 823E4758 |
| 823E4A48 | 823E4A80 |
| 823E57A0 | 823E57D8 |
| 823E5968 | 823E59A0 |
| 823E5D50 | 823E5D88 |
| 823E5FA8 | 823E5FE0 |
| 823E6680 | 823E66B8 |
| 823E6CC8 | 823E6D00 |
| 823E7230 | 823E7268 |
| 823E76D8 | 823E7710 |
| 823E86E8 | 823E8720 |
| 82412488 | 82412508 |
| 82412980 | 82412A00 |
| 82412E28 | 82412EA8 |
| 82413160 | 824131E0 |
| 8242AF00 | 8242AFC0 |
| 8242BF80 | 8242C048 |
| 824427D0 | 824428A0 |
| 8368E2D8 | 8368E2E8 |
| 8368E4F0 | 8368E500 |
| 8368E878 | 8368E888 |
| 8368EA48 | 8368EA58 |
| 8368EFE8 | 8368EFF8 |
| 8368F568 | 8368F578 |
| 8368FDA0 | 8368FDB0 |
| 836912D8 | 83691298 |
| 83691498 | 83691458 |
| 83691B18 | 83691AD8 |
| 83692368 | 836CEDE8 |
| 83692538 | 83692378 |
| 83692988 | 836927A8 |
| 83693080 | 83692EC0 |
| 83694370 | 836941B0 |
| 83695478 | 836952B8 |
| 836957D0 | 83695610 |
| 83695828 | 83695668 |
| 83695908 | 83695748 |
| 836959C0 | 83695800 |
| 83695AB0 | 836958F0 |
| 8369FBA8 | 8369FB08 |
| 8369FC88 | 8369FBE8 |
| 836A3860 | 8242C148 |
| 836A4210 | 836A4B90 |
| 836A4830 | 836A5200 |
| 836A50D0 | 836A5AA0 |
| 836CF9C0 | 836CF0D0 |
| 836CFA88 | 836CF198 |
| 836CFA98 | 836CF1A8 |
| 836CFCA0 | 836CF3B0 |
| 836D0CD8 | 836D03E8 |
| 836D1780 | 836D0E90 |
| 836D1DB0 | 836D14C0 |
| 836D2968 | 836D2078 |
| 836D3400 | 836D2B10 |
| 836D4C78 | 836D43C8 |
| 837045E0 | 83704630 |
| 83704790 | 837047E0 |
| 83704928 | 83704978 |
| 83705C98 | 83705CE8 |
| 8370D570 | 8370D5C0 |
| 8370DD20 | 8370DD70 |
| 8372D310 | 8372D360 |
| 83730A58 | 83730AA8 |
| 8375C500 | 8375C550 |
| 8375E290 | 8375E2E0 |
| 8375E538 | 8375E588 |
| 83760B00 | 83760B50 |
| 83760EC8 | 83760F18 |
| 83761488 | 837614D8 |
| 83761928 | 83761978 |
| 83761E58 | 83761EA8 |
| 83762240 | 83762290 |
| 83762CA8 | 83762CF8 |
| 8376A9F8 | 8376AA48 |
| 83774818 | 83774800 |
| 837803F0 | 83780438 |
| 837817B8 | 83781800 |
| 83833360 | 83833380 |
| 83833928 | 83833948 |
| 83833B00 | 83833B20 |
| 83833B90 | 83833BB0 |
| 83833BE0 | 83833C00 |
| 83833CC0 | 83833CE0 |
| 83835940 | 83835960 |
| 838359D8 | 838359F8 |
| 8383E678 | 8383E698 |
| 8383E8D0 | 8383E8F0 |
| 8383E8E0 | 8383E900 |
| 83887F58 | 83888098 |
| 83887F68 | 836A41A8 |
| 83887F78 | 836A41B8 |
| 83887F88 | 838880A8 |
| 83888060 | 83888180 |
| 83888070 | 83888190 |
| 838885A8 | 838886D8 |
| 83888EE0 | 83888FD8 |
| 8388D3B8 | 838881F8 |
| 8397B180 | 8397B1C0 |
| 8397B4A0 | 8397B4E0 |
| 83AB3870 | 83AB3878 |

## Title Update 2 (2026-11-02 to 2026-11-03)

From 2026-11-02 the port ran Title Update 2 (0.0.2.4, `TU_1A461UL_000000G000000.00000000000G4`,
its `default.xexp` 2,318,336 bytes, SHA-1 `22d3be744cc43316e6f93d3d17834a814dd28062`), then taken
for the last update for this disc.

### Addresses, 1.0 to TU2

The update moves most of the game's code and data. Notes written before the move (the other files
in `docs/`, older commit messages) use 1.0 addresses; the table below maps every address the code
used then in `src/` and `ng2_codegen.toml` to its TU2 one (`ng2_pointer_funcs.toml` is
regenerated by `tools/find_pointer_funcs.py` instead).

How they were mapped: `tools/map_addresses.py <1.0 image> <TU2 image> <repo>` (images dumped
with `NG2_DUMP_IMAGE`, the TU2 one with the update in place) matches code by instruction windows
with branch offsets and lis-built address immediates masked out, and data through the code that
builds each address (lis + addi/load/store). Checked by hand: small identical functions the
linker shares (vtable thunks: found through the vtables or by their exact bytes), struct fields
reached as base + offset (the in-game menu's views were wrong in the first pass), and `kReturnZero`,
which must be a function the code generator knows.

| 1.0 | TU2 |
|---|---|
| 8206EE58 | 8206EF3C |
| 821821B4 | 8218243C |
| 82280C80 | 82286610 |
| 82280CC8 | 82286658 |
| 82281010 | 822869A0 |
| 82284800 | 8228A1B8 |
| 822F0EF8 | 822F0EF8 |
| 822F2688 | 822F2688 |
| 822F2870 | 822F2870 |
| 823E0E60 | 823E1200 |
| 823E3448 | 823E37F0 |
| 823E3D78 | 823E4120 |
| 823E3ED0 | 823E4278 |
| 823E3FD0 | 823E4378 |
| 823E41F0 | 8242BF80 |
| 823E55B0 | 823E57A0 |
| 823E5778 | 823E5968 |
| 823E5B60 | 823E5D50 |
| 823E6490 | 823E6680 |
| 823E6AD8 | 823E6CC8 |
| 823E7040 | 823E7230 |
| 823E84F8 | 823E86E8 |
| 82411EF0 | 82412488 |
| 824123E8 | 82412980 |
| 82412890 | 82412E28 |
| 82412B68 | 82413160 |
| 82429DA8 | 8242AF00 |
| 82440ED8 | 824427D0 |
| 83632DC8 | 836557B0 |
| 83633008 | 836559F0 |
| 8364CA08 | 8366F2E8 |
| 8364CBE8 | 8366F4D0 |
| 8364CFC8 | 8366FAC8 |
| 8364D0C0 | 8366FBC0 |
| 8365CDF0 | 8367F948 |
| 8366E078 | 836912D8 |
| 8366E238 | 83691498 |
| 8366E8B8 | 83691B18 |
| 8366F158 | 83692538 |
| 8366F5A8 | 83692988 |
| 8366FCA0 | 83693080 |
| 836722A8 | 83695478 |
| 83672908 | 836957D0 |
| 83672960 | 83695828 |
| 83672A40 | 83695908 |
| 83672AF8 | 836959C0 |
| 83672BE8 | 83695AB0 |
| 83680E48 | 83887F68 |
| 83680E58 | 83887F78 |
| 83681830 | 836A4210 |
| 83681EB0 | 836A4830 |
| 83682750 | 836A50D0 |
| 836A9EB0 | 83692368 |
| 836AA198 | 836CF9C0 |
| 836AA260 | 836CFA88 |
| 836AA270 | 836CFA98 |
| 836AA488 | 836CFCA0 |
| 836AB540 | 836D0CD8 |
| 836AC0C8 | 836D1780 |
| 836AC710 | 836D1DB0 |
| 836AD330 | 836D2968 |
| 836ADE10 | 836D3400 |
| 836DFDE0 | 837045E0 |
| 836DFF90 | 83704790 |
| 836E0128 | 83704928 |
| 83707DA0 | 8372D310 |
| 8370B4E8 | 83730A58 |
| 83737AE0 | 8375E538 |
| 837390C8 | 8375E290 |
| 8373B3C8 | 83760B00 |
| 8373B790 | 83760EC8 |
| 8373BD50 | 83761488 |
| 8373C1F0 | 83761928 |
| 8373C720 | 83761E58 |
| 8373CB08 | 83762240 |
| 8373D570 | 83762CA8 |
| 837452C0 | 8376A9F8 |
| 8374F078 | 83774818 |
| 83759FF8 | 837803F0 |
| 8375A460 | 837817B8 |
| 837C1A60 | 836A3860 |
| 8380DC78 | 83833360 |
| 8380DFF0 | 83833928 |
| 8380E418 | 83833B00 |
| 8380E4A8 | 83833B90 |
| 8380E4F8 | 83833BE0 |
| 8380E5D8 | 83833CC0 |
| 83810258 | 83835940 |
| 838102F0 | 838359D8 |
| 83818F90 | 8383E678 |
| 838191F0 | 8383E8D0 |
| 83819200 | 8383E8E0 |
| 83862878 | 83887F58 |
| 83862888 | 83887F88 |
| 83862960 | 83888EE0 |
| 83862970 | 83888060 |
| 83862980 | 83888070 |
| 83862EB8 | 838885A8 |
| 83867CB8 | 8388D3B8 |
| 83955930 | 8397B180 |
| 83955C50 | 8397B4A0 |
| 83A93700 | 83AB3870 |
| 83AC5938 | 83AE8A70 |
| 83AF3338 | 83B08F98 |
| 83BBC138 | 83BD1DC0 |
| 83BF1C70 | 83C07CF0 |
| 83BF1F78 | 83C07FF8 |
| 83C786DE | 83CB5992 |
| 83CEB3B4 | 83D0176C |
| 83CEB3C4 | 83D0177C |
| 83CEB3F8 | 83D017B0 |
| 83CEB3FC | 83D017B4 |
| 83CEB400 | 83D017B8 |
| 83CEB414 | 83D017CC |
| 84B5030A | 84C3DA3E |
| 84F41264 | 84F5BBD8 |
| 85309E08 | 85324808 |
| 8530B8C0 | 853265E0 |
| 8530B930 | 85326650 |
| 8530BB80 | 853268A0 |
| 85310AE0 | 8532B800 |
| 85313E30 | 8532EDB0 |
| 85313EB0 | 8532EE30 |
| 853552C0 | 85371760 |
| 85387600 | 853A3B60 |
| 85388500 | 853A4A60 |
| 85388504 | 853A4A64 |
| 859D8DC4 | 859F5574 |

## DLC

Content packages under the game folder's `dlc/` (any depth, so the console's
`544307D5/00000002/` folders can be copied in as they are) are installed into the user data
folder once, at launch (`src/dlc.cpp`); only this game's DLC (content type 2) is taken.
