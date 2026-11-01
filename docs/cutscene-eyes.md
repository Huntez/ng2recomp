# Cutscene eyes (fixed 2026-11-01, investigation notes)

Fixed: the eyelids are bones, and the renderer gave those draws palettes three frames old ("Cause
and fix" below); Sonia's shoulder hole was the Xenos multiply rule (XenosRecomp patch 0009). The
rest is the hunt, kept for what it found. The `NG2_TEST_*` switches and F8 to F12 keys it mentions
were temporary and are gone.

In the cutscene after Chapter 1's boss (Elizébet takes Sonia into a helicopter), Ryu's eyeballs sit
in front of his eyelids and Elizébet's eyes and teeth float beside her head. Xenia Edge shows the
scene correctly, so the cause is ours (recompiled CPU code or the renderer). Faces in other
cutscenes (Chapter 2's opening) are right.

## Reaching the scene in a test run

From the user's slot 2 save (Chapter 1, the statue before the last fight; `NG2_SAVE_FROM`), a test
autopilot (temporary code in `src/perf_overlay.cpp`, `NG2_TEST_AUTOPILOT`, not committed) walks Ryu
up the stairs, waits out the fight casting ninpo (Infinite Health and Ninpo on), opens the door
(RB) and casts at the boss; the cutscene follows at about 300-350 s. Steering needs the camera:
its position is at 0x84C3C3E0 (x, y, z), and stick up moves away from it with left and right
mirrored. Ryu's character object is at 0x8563EF70 in every run (record 0x84C23C70), position at
+0x270 (x, y, z).

## What draws the eyes

- Ryu's eyeballs are rigid meshes (vertex buffers 0x19DDA000 and 0x19DD9000 in the cutscene) drawn
  with vertex shader 18161E8508C7FB4D: positions from fetch slot 95, one WorldViewProj in c21-c24,
  no bones. Skinned draws (7D7B9634653FDB27 and others) read 3x4 bone matrices (12 floats each)
  from fetch slot 94 and use c21-c24 as the camera's view-projection.
- The eye's world matrix is a 4x4 (rows: basis, then translation) in a per-frame render entry pool
  (0x84E79xxx; Ryu's eyes at 0x84E79240 and 0x84E792D0 during the cutscene; the same slots hold
  other objects at other times). An entry: matrix at +0, previous entry at +0x44, a type pointer at
  +0x48, an index at +0x4C.
- In the cutscene that entry is written by sub_824805B0, called from sub_8247B728 <- sub_8247B2A8
  <- sub_8247A730 <- the frame update sub_822F16D0. (sub_8233D418 writes attached parts' world
  matrices, character+216+0x70*k, local at -88, offset at -24, times the character's root at +0,
  but not the cutscene eyes.)
- 0x859D3C60/0x859D4E30 also hold the eye matrix: the draw-time matrix state, written by
  sub_837CCC28 for every draw.

## The eye matrices are right (2026-11-01)

- sub_824805B0 (object, entry, ...) computes an attached entry's world matrix by its type,
  `*(*(entry + 0x58) + 4)`. The eyes are type 2 (info words 5, 0 or 1 for left or right), which
  calls sub_824810D0(out, object, entry, base matrix by value in r6-r10 plus the stack, v1, v2). It
  fetches a look-at target with sub_823F8448(id, slot) (Ryu looks at 62, probably Sonia) and turns
  the base matrix a few degrees toward it, keeping the translation (the eye's pivot). In the
  cutscene the two pivots are 5.7 cm apart and the turns small: the eyes are where they should be.
- So the face around them is wrong: the skinned face (bones from fetch slot 94, the facial bones
  driven by the `.fac` facial animation) sits too far back on Ryu, and Elizébet's face skin is off
  while her eyes and teeth (rigid) are in place. Next: what writes the facial bones' skinning
  matrices.
- The vector instructions checked in sub_824810D0 (vmaddfp128, vnmsubfp128, vrsqrtefp with its
  Newton step, vsel128, vmsum3fp128, vcsxwfp128, vrlimi128 with rotations 2 and 3) translate
  correctly.

## Bone palettes and the head (2026-11-01, later)

- Skinning palettes rotate through three buffers, one per frame (Ryu in the helicopter scene:
  0x1D55F000, 0x1D5BF000, 0x1D61F000; Sonia's 0x6000 lower). Physical memory is seen at
  0xA0000000 + p, 0xC0000000 + p and 0xE0000000 + p - 0x1000.
- Bones 4-71 of Ryu's palette hold one identical matrix in both the helicopter scene and the
  Chapter 2 opening (a rigid head group, or unused slots): not a symptom.
- The eye's pivot, taken into the bind space of those head bones, is the same in both scenes,
  (-0.029, 0.664, 0.078) against (-0.028, 0.660, 0.083): the eyes sit correctly on the head in the
  buggy scene too.
- The look-at in sub_824810D0 turns Ryu's eyes about 5 degrees toward character 62, close to the
  straight direction to it. If the eye mesh isn't centred on its pivot, a turn like that moves the
  eyeball forward; whether Xenia turns them by as much is the open question.
- The write trap can't watch physical memory: the SDK's handler runs before ours and takes those
  faults (it unprotects the page). Watching the palette needs our handler first.

## Closed eyelids (2026-11-01, evening)

- Ryu's eyeball mesh is static (the same 178 vertices for both eyes, centred on the pivot), and the
  eye look-at isn't the cause (copying the input matrix to the output leaves the bulge).
- The bulge shows in the medium shot of Ryu and Sonia, not in Ryu's close-ups. With the eye meshes
  hidden (`NG2_TEST_HIDEMESH`), the medium shot shows his face skin with the eyelids shut over the
  sockets: the open eyeballs then sit in front of closed lids. So the facial animation (eyelids)
  is not applied there, or shuts them. Ryu's skinned body mesh (0x19CDD000) is the same in both
  shots (no level-of-detail switch); its bind-space positions end below the eyes, so the head and
  face skin are other meshes.

- Ryu's head is rigid meshes drawn with vertex shader D7A4920F (not skinned), vertex buffers
  0x1A0F3000, 0x1A09D000, 0x1A0BB000, 0x19DDC000, 0x1A0ED000 in the helicopter scene. All are static
  except 0x19DDC000 (154 indices), whose contents change nearly every frame: the CPU morphs it,
  which makes it the eyelid or face part the facial animation drives. The game doesn't lock it
  through D3D (sub_8373E5B8 never sees it), so its writer has to be found by a write trap on
  physical memory (all four host views: 0xA0000000 + p, 0xC0000000 + p, 0xE0000000 + p - 0x1000,
  and the SDK's own physical view), which hasn't worked reliably yet.

## The changing head buffer is cloth (2026-11-01, night)

- The trap finally caught 0x19DDC000's writer once the SDK was kept from making the page writable
  again (it does that through mprotect when the game changes a physical range's protection, from
  sub_8233B420/sub_8233B638 via sub_836F1618; a temporary mprotect override keeps watched pages
  read-only). The writes come through the 0xE0000000 view from sub_836ED380, called by
  sub_836ED768 after 15 passes of sub_836ECF20 (sub_836ED858 <- sub_836EB2E0 <- sub_822F2870 <-
  sub_822F27B8): a cloth solver, writing positions (12 bytes in each 32-byte vertex). So that
  buffer is cloth (likely his scarf), not the eyelids, and the head has no CPU morph.
- So the shut eyelids in the medium shot come from which head meshes are drawn there, not from a
  morph.

## Against Xenia Edge's GPU traces (2026-11-01, night)

- Xenia Edge needs `-DXENIA_ENABLE_GPU_TRACE=ON` for F4 to write traces (the Release build only
  logs that it can't); `--trace_gpu_prefix=<dir>/` picks the folder. `tools/xtr_draws.py` replays
  a trace's register writes and memory reads and lists each draw's index count, fetch 95, chosen
  constants (`--consts`), registers (`--regs`) and index range, and dumps buffers (`--dump`).
- At matching moments (frames whose head matrices agree), the eyes' WorldViewProj relative to the
  head piece 1A0F3000 matches Xenia's within 1-2 mm and a degree. The hood (1A09D000), face
  (1A0BB000, vertex shader 9373E6B1) and 1A0F3000 keep the same matrices relative to each other.
- The eye, hood and face vertex buffers and the face's index buffer (0x1A0C1000, 0x3F4 bytes) are
  byte-identical to Xenia's; depth, blend, polygon offset and colour-control registers are the
  same; both shaders only multiply a float3 position by WorldViewProj. Texture slots 1-2 of the
  eye draws hold leftovers in both.
- So the guest side is the same as Xenia's: the difference is in our renderer. Suspect: its own
  copy of guest memory (g_guestMemory, kept in step by comparing with a shadow) holding something
  else at those addresses, such as memory-export output that the shadow comparison never sees.

- With the user's settings (render_scale 5, antialiasing 2) a test run of the current build
  (2026-11-01, run74) draws the scene right: Ryu's eyes inside his lids and Elizébet's face as in
  Xenia. The test runs' pictures before that were 320x180, too small to show the bug, and the
  user's traces (10-31 18:15) predate the vertex rounding change (patch 0008), the XMA fixes and
  scissor support. Forcing every vertex buffer to upload at every draw (`NG2_TEST_ALWAYSSYNC`)
  changed nothing visible at low resolution. Open: whether the user still sees it with the
  current build.

## The user's session (2026-11-01, afternoon)

- With the rebuilt AppImage the user still gets it (traces 7-15 in ~/ng2-trace-*, with memory and
  the GPU-copy check). In their session the eyes are at 0x19DD8000/0x19DD9000 and the hood at
  0x1A09C000 (a page lower than in Xenia and the test runs): a different allocation history. Their
  eye matrices still match Xenia's within a millimetre, their eye, face and hood buffers are
  byte-identical to Xenia's, and the renderer's GPU copies of them match guest memory (the F7
  check, `gpucheck.txt`; only cloth and hair ranges differ, written after their draws).
- The eye pixel shader (container 0x820B9550) clips by the eye texture's alpha (slot 0, DXT5
  0x19CC8000) only when boolean word 4 bit 1 is set; it is clear in Xenia, the user's run and
  the test runs alike, so the eyeball's visible part comes from depth against the face patch
  (1A0BB000, 506 indices, 128x64 skin textures: the eyelids). That patch is drawn after the eyes
  in both. Same depth control, cull key, booleans, texture bindings and draw order everywhere.
- So the difference is GPU-side state a trace can't show (depth buffer contents, a cached
  pipeline, surface or texture made earlier in their session). The trace line now also prints
  the cull key, depth format and texture, the scissor registers and the pipeline key hash.
- Reproduction attempts: the user skips the cutscene before the boss and fights with the sword;
  the autopilot gained `NG2_TEST_SKIPCUT` (Start for 12 s in the boss area) and
  `NG2_TEST_SWORD` (X instead of ninpo) to try the same.

- Reproduction attempts with the user's render settings, none showing the bug: skipping the
  cutscene before the boss (`NG2_TEST_SKIPCUT`), the boss by sword (`NG2_TEST_SWORD`), sound on
  (`NG2_TEST_AUDIO=disk`), the FPS counter on (`show_fps`, which does move the buffers to the
  user's addresses: the counter's text goes through the game's 2D lists). Next: their ng2.toml
  verbatim (fullscreen, vsync, present mode: also fine). The test runs' pictures are of the front
  buffer, so presentation can't be the difference. Next: the live test in their session: F8
  throws the render target surfaces away, F10 the pipelines and textures (both temporary, in the
  AppImage of 17:07; exercised headlessly with `NG2_TEST_FLUSHONTRACE=3`, the game goes on fine).
  If the eyes fix themselves after one of them, that cache holds the bad object.
- Neither F8 nor F10 changed anything in the user's session (17:31): surfaces (a fresh depth
  buffer), pipelines and textures are all out. The eye index buffers (Xenia 0x19E88000 and
  0x19E89000, 0x334 bytes each, a page lower in the user's session) and the hood's and 1A0F3000's
  are byte-identical to Xenia's too. The one environmental difference found so far: the user's
  game runs in a Wayland window, the test runs in an X11 one under gamescope (`NG2_TEST_WAYLAND=1`
  tries the former: also fine). The game folders are the same files. The user's process environment
  holds nothing graphics-related beyond DXVK_HUD=0 and MESA_SHADER_CACHE_MAX_SIZE=16G.

## F11 fixes the faces (2026-11-01, evening)

- In the user's session, F11 (temporary: `SyncGuestMemory` uploads every range whole at every
  draw) fixes Ryu's eyes and the faces, live. So the renderer's GPU copy of some range those draws
  read is wrong at draw time, though right at the end of the frame (the earlier F7 check). The
  check now runs right after the traced frame's first eye draw instead, to catch the bad bytes.
- Sonia's hole (her right shoulder, the crouching shot) stays with F11: a separate bug.
- Xenia Edge fixes in the same area (memexport output vs guest memory in its shared memory
  buffer): d1541a57 (invalidate physical pages on allocation: reused pages kept stale GPU bytes),
  b8372465 and 16df2598 (memexport pages vs resolve output), 31a696a9 (vertex shader memexport
  output landing after the guest rewrote that memory). Ours has no record of the pages memexport
  wrote, so a skinning pass writing over a static mesh's range (the eyes sit right after Ryu's
  skinned body buffer, 0x19CDD000 + 0xE6950) would never be undone: the shadow says unchanged.

- The check at the eye draw itself (fixed to read back only after its frame is submitted;
  ~/ng2-trace-30): in the user's session the GPU copies of the eye, face, hood, 1A0F2000/1A0EC000
  pieces, Ryu's body and his palettes all match guest memory at that moment. So F11 doesn't fix
  the data; its side effect does: each of its uploads puts a barrier before the draw, which also
  ends the render pass. F12 (temporary) now does only that (a barrier before every draw, no
  upload): if it fixes the faces too, the bug is missing synchronization or a render pass
  problem between draws, not memory contents.

## Cause and fix (2026-11-01, evening)

- F12 then (temporary: only fetch 94, the bone palettes, uploaded at every draw) fixed the faces
  live: the eyelids are bones. The palettes rotate through three buffers; each comes round
  "stable" (unchanged for over a frame), so SyncGuestMemory checked it at its first draw only,
  while the game's jobs were still writing it during the frame's draws. FinishGuestMemory then
  saw it changed and checked it at every draw for one frame, but the ring moved on and three
  frames later the same buffer was stable again. CPU timing decides how far the jobs are at the
  first draw, so it showed in the user's sessions and not in the headless runs.
- First fix, `GuestMemoryRange::dynamic` (a range checked once in a frame and found written by
  its end is checked at every draw from then on): not enough, the user still saw it. F12's
  every-draw upload also kept the range's changedFrame current, so a palette written after the
  frame's draws became late; without it, a buffer the jobs fill only after the draws looks
  unchanged at them, takes FinishGuestMemory's other branch and never becomes late, and its
  draws use the bones of three frames before.
- Fix: palettes (ranges read through fetch slot 94) are late from their first draw: every draw
  of the frame gets the palette as it is just before submission, as the console's GPU would.
  Confirmed by the user (2026-11-01, 19:34 AppImage): faces right. Sonia's shoulder hole stays.

## Sonia's shoulder hole

- Where: the rooftop shot of Ryu facing the camera with Sonia's back to it, under the strap on
  her right shoulder blade: Ryu's chest (grey weave) shows through her skin. Deterministic: the
  headless runs show it too (out/run90, frame 16140); Xenia's trace of that shot (16121) doesn't.
- Her back is drawn twice, blended (SRC_ALPHA, INV_SRC_ALPHA): skinned VS 7D7B9634, 4453
  indices from VB 0x16A22000 (Xenia 0x16A23000), texture DXT5 512x512 at 0x165E0000. First pass
  without depth write or alpha test, second with depth write and alpha test (ref 0.251). Same
  state and blend in Xenia. Its pixel shader (0x82238150) takes alpha from the texture times an
  interpolant, and can clip parts listed in PS c30 (`SupDel`, 65535.1 in Xenia: none).
- Flat colour (NG2_DEBUG_HIGHLIGHT, now also <hash>,<hash>... with a colour per shader): the
  hole is geometry. 7D7B9634 is Ryu's body and Sonia's hair; her skin is VS 8282819E (container
  0x8221A3C8, VB 0x1697A000, 11 dwords a vertex), and the hole is a gap in it.
- That shader blends four position streams by constant weights (PosWeight, PosWeight2): fetch
  slots 90, 91 and 93 (0x169F6000, 0x16A0C000, 0x169E0000, 3 floats a vertex) and slot 92, which
  points at the skin's own vertex buffer read 3 dwords a vertex: garbage, with 92 NaNs, weighted
  0. On Xenos a multiply by 0 is 0 even for NaN (Shader Model 3; Xenia's ZeroIfAnyOperandIsZero
  for mul, mad, dp and the scalar muls); ours gave NaN positions and those triangles vanished.
- Fix (XenosRecomp, patch 0009): mul, mad, dp2add/dp3/dp4, muls, mulsprev and mulsc go through
  `mulz`/`dotz` in shader_common.h, which give 0 when either operand is 0 or denormal; so does
  dst's y. The helpers are `precise` (DXC doesn't carry precise into called functions; without
  it the dot products lost the no-fusion guarantee of patch 0008). Headless: the hole is gone.

## Ruled out

- Shader rounding: vertex shaders now round toward zero and don't fuse multiply-adds (as Xenos);
  the eyes were unchanged.
- XMA timing: the decoders match Xenia Edge's but for about one frame.
- In sub_8233D418: vupkd3d128 (mode 4), vpermwi128, vrlimi128 and vmsum4fp128 translate correctly.

## Tools

- F7 (or `NG2_TRACE_FILE`) traces: draws (with cull, depth target, scissor, viewport, clip and
  vertex control registers, the pipeline key), clears, pictures, the frame's memory and vertex
  shader constants.
- `tools/xtr_draws.py`: the same draws, constants and registers from a Xenia Edge GPU trace (F4 in
  a build with `-DXENIA_ENABLE_GPU_TRACE=ON`), and its buffers.
- `NG2_DEBUG_HIGHLIGHT=<hash>,<hash>,...`: those shaders flat, a colour each (logged): a hole that
  stays is geometry.
