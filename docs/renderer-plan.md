# Native renderer plan

Goal: replace GPU emulation (ReXGlue's `xenos` plugin, Xenia-derived) with a
renderer that hooks NG2's statically linked D3D9 (XDK) calls and draws through
Vulkan, modelled on Unleashed Recompiled's `gpu/video.cpp` (plume RHI, Vulkan
backend only). The xenos plugin stays available as the reference picture.

## What NG2 links

- XDK 2.0.6683 (xboxkrnl/xam import versions). Unleashed's game is a later
  XDK, so struct offsets must be checked, not copied.
- D3D library: 0x83737000-0x8374D8F0, 210 functions, 74 called from game code.
  XAudio follows from 0x8374D8F0.
- `GuestDevice` layout matches Unleashed's where checked so far:
  VS float constants at device+0x780 (1920), PS float constants at +0x1780
  (6016), dirty flags in the first 0x40 bytes (+16/+24 used by draws).

## Identified so far

| Address | Role | Evidence |
| --- | --- | --- |
| 83737BB8 | Direct3D_CreateDevice (inner) | only path to VdInitializeEngines (83741F00) |
| 837C62E8 | game: device creation | calls 83737BB8 |
| 837452C0 | swap (VdSwap, display mode) | only VdSwap caller |
| 837C6680 | game: present | calls 837452C0 |
| 8373BD50 | draw (49 callers) | flushes VS/PS constants from +0x780/+0x1780 |
| 8373B1F8 | SetVertexShaderConstantF | copies to device+0x780+16*reg |
| 8373B4D8 | SetVertexShader-like | device+12684, dirty bits |
| 837374E8 | SetRenderTarget-like | device+12448 |
| 8373E9E8 | resource Release | refcount, destroy |

`tools/callgraph.py generated/default callers|callees|asm <addr>` answers the
rest; the 74 public functions are the hook list to complete.

## Steps

1. Map the 74 public D3D functions to their D3D9 names (signatures, device
   fields touched, PM4 packets written) and fill `GuestDevice` offsets.
2. Shaders. Unleashed translates its game's shader archive offline with
   XenosRecomp (HLSL, then DXC). NG2's shaders are created at runtime from
   microcode (CreateVertexShader/CreatePixelShader); find where the microcode
   lives (.ng2 archives or the XEX). Options: dump all microcode once and
   run XenosRecomp offline like Unleashed, or translate at load time with the
   SPIR-V translator already in ReXGlue's xenos plugin. Decide after step 1.
3. Port video.cpp: plume Vulkan backend, render command queue, resource
   types, texture upload (tiling, endian swap, formats), render targets and
   resolves (EDRAM semantics), present. Drop D3D12 and Metal paths.
4. Hook in `ng2_codegen.toml`/`REX_HOOK`, boot with `gpu_plugin` empty
   ("native rendering mode"), compare against `--gpu_plugin=xenos` shots.

Audio: ReXGlue's SDL3 output with its XMA decoder (libavcodec) is in use;
Unleashed's SDL driver only if this turns out worse.

License: copying Unleashed code makes this project GPL-3.0-or-later
(Unleashed is GPL-3.0; ReXGlue is BSD-3-Clause).
