# D3D map, part C (the 39 game-called functions in 0x83732000-0x83737000)

XDK 2.0.6683, NG2. Static reading only (`tools/callgraph.py generated/default asm|callers|callees`,
plus the NG2_DUMP_IMAGE image for the state tables at 0x83A84EE0 / 0x83A853A0). Nothing was run.
Conventions as in parts A and B: guest args r3, r4, ...; "s84" = 9th argument at caller sp+84,
"s92" the 10th, and so on. "dev" = D3DDevice*. "fc" = the 6-dword GPUTEXTURE_FETCH_CONSTANT. Device
offsets are decimal unless written 0x. Bit numbers are LSB = 0.

## Table

| Address | Name | Signature (guest regs) | Conf. | Evidence |
| --- | --- | --- | --- | --- |
| 83732008 | not D3D: field setter on a 0x7880-byte object | r3 obj, r4 u32* dst, r5 const u32* src | medium | validates obj via 83730538 (+0 == 0x7880, +4 == 6), copies one dword and on change sets obj+17 = 1 and obj+12 = 0x01234567. Same family as 83730408..83732168; callers are game code at 823F..8246. No device or resource fields |
| 83732090 | not D3D: same object, 512-byte table set | r3 obj, r4 dst table, r5 src table | medium | memcpy 512 bytes (839542E0), compares 128 4-byte entries {u16,u8,u8}, flags a change like 83732008 |
| 83732168 | not D3D: same object, 512-byte table set | r3 obj, r4 dst, r5 src | medium | as 83732090 with 64 8-byte entries (4 halfwords) |
| 83732250 | XG: block dimensions of a GPU texture format (XGGetBlockDimensions-like) | r3 GPUTEXTUREFORMAT, r4 UINT* blockW, r5 UINT* blockH | high | returns 4x4 for 18-20 (DXT1/2_3/4_5), 49, 51-53, 58-61 (DXN, DXT*_AS_16, DXT3A/5A/CTX1); 2x1 for 11, 12, 40 (YUV / 32_AS_8_8); 4x1 for 39, 44; else 1x1 |
| 83732370 | XG internal: align a level's dimensions and return its size | r3 UINT* w, r4 UINT* h, r5 UINT* d (in/out), r6 bits per pixel, r7 GPU format, r8 dimension (2 = 3D), r9 tiled flag; returns size (4 KB aligned) | medium | aligns w/h to 32 blocks (tiled) or to 256-byte rows (linear, r9 == 0), depth to 4 for 3D, then w*bpp/8*h(*d) rounded to 4096 |
| 83732800 | internal: base dimensions from a texture header | r3 tex, r4 UINT* w, r5 UINT* h, r6 UINT* d | high | decodes fc dword2 (tex+36) by dimension (fc dword5 bits 9-10, tex+48): 1D 24-bit width, 2D/cube 13/13 (d = 1), 3D 11/11/10; adds 1 + 2*border (fc dword3 bit 31, tex+40) |
| 83732898 | XG internal: offset of a level inside the packed mip tail (guess) | r3 level-in-tail, r4 w, r5 h, r6 d, r7 row pitch, r8 slice pitch, r9 GPU format, r10 UINT* x, s84 UINT* y, s92 UINT* z; returns byte offset | low | pow2-rounds w/h, places levels < 3 at 16>>level on the longer axis, returns x*bpp/8 + y*rowPitch + z*slicePitch; callers 83732FB8 (level address core) and 8385C060 (XG code in the game area) |
| 83732DB8 | D3DTexture_GetLevelDesc (D3DBaseTexture GetLevelDesc core) | r3 tex, r4 level, r5 D3DSURFACE_DESC* | high | 837329F0 for the level size, rebuilds the D3DFORMAT from fc dword0/1/3 bits, Type from GetType 8373EA90, Usage/Pool/MS = 0, +24 Width, +28 Height (same struct as Unleashed GuestSurfaceDesc); 18 callers |
| 83733428 | internal: texture lock core (lock one level/face) | r3 tex, r4 face/slice, r5 level, r6 flags, r7 void** pBits out, r8 UINT* rowPitch out, r9 UINT* slicePitch out, r10 UINT* level offset out | medium | 83732FB8 for the level offset and pitches, then 8373E5B8(tex, kind 14, base fc1&~0xFFF, mip fc5&~0xFFF, ...) and *r7 = CPU address + offset. Called directly by 7 game functions (8361A508, 8362A498, 8363C828, ...) as an inline LockRect |
| 837334F0 | D3DCubeTexture_LockRect (base-texture LockRect with face) | r3 tex, r4 face, r5 level, r6 D3DLOCKED_RECT* out {Pitch, pBits}, r7 const RECT* or 0, r8 flags | high | calls 83733428; pBits += rect.top*pitch + rect.left*bpp/8 (bpp from the byte table at 0x822830C1 indexed by fc format); 83733C70 and 83734150 forward to it |
| 83733628 | XG internal: fill a texture header (core of CreateTexture and XGSetTextureHeader) | r3 D3DRESOURCETYPE (3 tex, 17 volume, 18 cube, 19 array, 20 line), r4 w, r5 h, r6 depth, r7 levels (0 = full chain), r8 usage, r9 D3DFORMAT, r10 packed-mips mode, s84 border, s92 exp bias, s100 pitch, s108 D3DBaseTexture* out, s116 UINT* baseSize, s124 UINT* mipSize | medium | writes Common = 0x00100003 (\|0x00200000 for usage bit 2, \|0x00400000 for usage bit 9), RefCount 1, Fence 0, +20/+24 = 0xFFFF0000, and the fc words +28..+48 (format, endian, tiled, pitch, size, swizzle, exp bias, mip levels, dimension). It keeps the address bits already in fc dword1/dword5, so the caller adds addresses. The last 8 args' order is a reading of the stack slots (medium) |
| 83733B90 | D3DBaseTexture_GetLevelCount | r3 tex; returns count | high | ((tex+44 >> 6) & 0xF) + 1 = fc mip_max_level + 1 |
| 83733BA0 | D3DTexture_GetSurfaceLevel | r3 tex, r4 level; returns D3DSurface* (AddRef'd parent) | high | allocates a 48-byte surface: Common = 0x40100004 (type 4, bit 30 = has parent), +24 = parent, +28 bits 28-31 = level, RefCount 1, +20 = 0xFFFF0000; AddRef(parent) |
| 83733C70 | D3DTexture_LockRect | r3 tex, r4 level, r5 D3DLOCKED_RECT*, r6 RECT*, r7 flags | high | `li r4,0` then tail call 837334F0 (face 0). Unleashed's LockTextureRect |
| 83733C88 | D3DTexture_UnlockRect (also the cube one) | r3 tex, r4 level (unused) | high | tail call 8373E848(tex, fc1 & ~0xFFF, fc5 & ~0xFFF). Unleashed's UnlockTextureRect |
| 83733CA0 | D3DCubeTexture_GetCubeMapSurface | r3 tex, r4 face, r5 level; returns D3DSurface* | high | as 83733BA0, plus face in +28 bits 22-27 |
| 83733D58 | D3DVolumeTexture_GetLevelDesc | r3 tex, r4 level, r5 D3DVOLUME_DESC* | medium | `b 0x83732EB8`; 83734188 (volume GetDesc) calls the same helper. Only callers 8382DBE8 and 8385B8F8 (texture-loading code in the game area) |
| 83733D60 | D3DVolumeTexture_GetVolumeLevel | r3 tex, r4 level; returns D3DVolume* | medium | like 83733BA0 but a 32-byte object, level in +28 bits 28-31 |
| 83733E28 | D3DCubeTexture_GetLevelDesc (alias of 83732DB8) | r3 tex, r4 level, r5 D3DSURFACE_DESC* | medium | `b 0x83732DB8`; which alias is Texture and which is Cube is a guess (83732DB8 has the 18 general callers) |
| 83733E30 | D3DDevice_CreateTexture (= Unleashed `CreateTexture`) | r3 w, r4 h, r5 depth, r6 levels, r7 usage, r8 D3DFORMAT, r9 pool (ignored), r10 D3DRESOURCETYPE; returns D3DBaseTexture* | high | XMemAlloc 52-byte header, 83733628(type, w, h, d, levels, usage, format, 2, ...), then physical allocs for base and mip sizes, stored as virtual addresses in fc dword1 / dword5 high 20 bits. 83741B08 passes (w, h, 1, 1, 0, fmt, 0, 3), so part B's reading "levels=1, usage=1, 0, format, 0, pool=3" was shifted by one register. Same 8-arg order as Unleashed's hook |
| 83733F50 | D3DDevice_CreateSurface (render target / depth) | r3 w, r4 h, r5 D3DFORMAT, r6 D3DMULTISAMPLE_TYPE, r7 const D3DSURFACE_PARAMETERS* or 0 {Base, HierarchicalZBase, ColorExpBias}; returns D3DSurface* | high | 48-byte header from 83733938; without r7 it allocates EDRAM tiles with 8373EF78 (fails over 2048), sets Common bit 31 (owns tiles) and the tile base in +28 low 12 bits; formats 22/23 (D24S8 / D24FS8) are depth. Unleashed's CreateSurface has 4 args; r7 is the extra one |
| 837340B8 | D3DSurface_GetDesc (= Unleashed `GetSurfaceDesc`) | r3 surface, r4 D3DSURFACE_DESC* | high | if Common bit 30 (texture level) -> 83732DB8(parent +24, level +28>>28); else Format = +40, Width = (+36 >> 18 & 0x3FFF) + 1, Height = (+36 >> 3 & 0x7FFF) + 1, MultiSample = +24 halfword & 3; Type = 4 |
| 83734150 | D3DSurface_LockRect | r3 surface, r4 D3DLOCKED_RECT*, r5 RECT*, r6 flags | high | forwards to 837334F0(parent +24, face +28 bits 22-27, level +28 bits 28-31, ...); only works for texture-level surfaces |
| 83734170 | D3DSurface_UnlockRect | r3 surface | high | 8373E848(parent, parent fc1 base, parent fc5 mip) |
| 83734188 | D3DVolume_GetDesc | r3 volume, r4 D3DVOLUME_DESC* | medium | 83732EB8(parent, level, desc), Type = 16 |
| 837341D0 | D3DVolume_LockBox | r3 volume, r4 D3DLOCKED_BOX*, r5 const D3DBOX*, r6 flags | medium | forwards to 83733580(parent, level, ...), which fills {RowPitch, SlicePitch, pBits} and offsets by box Left/Top/Front (+0/+4/+16) |
| 837341F0 | D3DDevice_SetTexture | r3 dev, r4 sampler (0-25), r5 D3DBaseTexture* or 0, r6 u64 dirty bit | high | merges the texture's fc into dev+1152+24*s (details in section 2), stores the pointer at dev+12536+4*s, ORs r6 into qword dev+24, fences the old texture (dev+10908 or the tag block at dev+13508). 69 callers; they pass `lis r6,..` = 1 << (31-s) (8231B3B0: s=10, r6=0x200000) |
| 83734A78 | SetRenderState D3DRS_CULLMODE (56) | r3 dev, r4 value | high | table entry 0x83A84EE0[14] (default 6); value into PA_SU_SC_MODE_CNTL bits 0-2 (dev+10568), dirty 0x40 in qword dev+16. Direct caller 83865110 |
| 83734B10 | SetRenderState D3DRS_ALPHABLENDENABLE (60) | r3 dev, r4 value | high | table entry [15]; keeps the flag in dev+11844 bit 31, builds RB_BLENDCONTROL0-3 (dev+10552, 10584-10592) from dev+11840 or 0x00010001 (no blend); dirty 0x400\|0x4\|0x2\|0x1. Direct caller 8377F5B8 |
| 837351D0 | SetRenderState D3DRS_ZENABLE (40) | r3 dev, r4 value | high | table entry [10]; dev+11868 = value; Z_ENABLE bit of RB_DEPTHCONTROL (dev+10548) forced 0 if no depth surface (dev+12448); dirty 0x800\|0x20000 (agrees with part A) |
| 83735CF8 | SetRenderState D3DRS 304 (VIEWPORTENABLE; name from memory) | r3 dev, r4 value | medium | table entry [76]; PA_CL_VTE_CNTL (dev+10572) = 0x43F if on, 0x400 if off; PA_CL_CLIP_CNTL (dev+10564) bit 16 (clip disable) = !value; dirty 0x20\|0x80. Neighbouring entries fit the 360 order: 240-300 write SQ_WRAPPING_0/1 (WRAP0-15), 308-320 RB_COLORn_INFO, 332 VGT_MULTI_PRIM_IB_RESET_INDX |
| 837363D8 | SetSamplerState D3DSAMP_MINFILTER (20) | r3 dev, r4 sampler, r5 D3DTEXTUREFILTERTYPE | high | sampler table 0x83A853A0[5]; fc dword3 min_filter (bits 21-22) and aniso_filter (25-27, from the per-sampler MAXANISOTROPY byte dev+11916+s and table 0x82283168), dword4 min_aniso_walk (bit 11); dirty 1 << (31-s) in dev+24 |
| 83736568 | SetSamplerState D3DSAMP_MAGFILTER (16) | r3 dev, r4 sampler, r5 filter | high | table [4]; fc dword3 mag_filter (bits 19-20) and aniso, dword4 mag_aniso_walk (bit 10); same dirty bit |
| 83736908 | SetSamplerState D3DSAMP_MIPMAPLODBIAS (28) | r3 dev, r4 sampler, r5 float bits | high | table [7]; float * const (0x8223E6DC) to int into fc dword4 lod_bias (bits 12-21); dirty 1 << (31-s). Game caller 8282D3F8 |
| 837369A8 | SetSamplerState D3DSAMP_MAXMIPLEVEL (32) | r3 dev, r4 sampler, r5 level | high | table [8]; byte dev+11942+s = value; if a texture is bound, fc dword4 mip_min_level = max(texture's, value); dirty bit |
| 83736A28 | SetSamplerState D3DSAMP_MINMIPLEVEL (52) | r3 dev, r4 sampler, r5 level | high | table [13] (default 13); byte dev+11968+s; fc dword4 mip_max_level = min(texture's, value); dirty bit. Game caller 82333238 |
| 83736DC8 | D3DDevice_SetScissorRect | r3 dev, r4 const RECT* | high | stores the rect at dev+12668..12680; if D3DRS_SCISSORTESTENABLE (dev+11848) it is clipped to the viewport (dev+12640.. floats); writes PA_SC_WINDOW_SCISSOR_TL/BR shadows (dev+10436/10440, regs 0x2081/0x2082) and emits them right away through 83734530 (no dirty bit) |
| 83736EC8 | D3DDevice_SetViewport | r3 dev, r4 const D3DVIEWPORT9* {X, Y, Width, Height, MinZ, MaxZ} | high | converts the four DWORDs to float and calls 83734810(dev, f1 X, f2 Y, f3 W, f4 H, f5 MinZ, f6 MaxZ); shadow at dev+12640 |
| 83736F48 | D3DDevice_SetStreamSource (full version) | r3 dev, r4 stream, r5 D3DVertexBuffer* or 0, r6 offset, r7 stride, r8 u64 dirty bit | high | vertex fetch slot 95-s: dword0 at dev+1912-8s = physical(vb+24 + offset), dword1 = vb+28 - offset; dev+24 \|= r8 (game passes 1 for stream 0 = fetch const 31); pointer at dev+12452+4s; stride/4 byte at dev+12520+s, and if it differs from dev+11824+s then dev+16 \|= 0x80000. Unleashed's SetStreamSource plus r8 |

Tally: 26 high, 12 medium, 1 low. 30 are D3D API entry points, 6 are D3D/XG internals (83732250,
83732370, 83732800, 83732898, 83733428, 83733628), 3 are not D3D.

## 1. Where the library starts

The D3D/XGraphics code starts at **0x83732250**. Evidence:

- 83730408-83732248 is one other module. 83730408 builds a 0x7880-byte object, 83730538 checks
  `+0 == 0x7880 && +4 == 6` and a 0x01234567 change sentinel, and every function from 83731E70 to
  83732168 calls 83730538. None of them touches a device, a resource header or a fetch constant.
  83732168 ends at 0x83732248.
- 83732250 (format block size), 837322E8 (max mip count), 83732370/83732490 (level sizes),
  83732800/837329F0/83732EB8/83732FB8 (level dimensions, pitches, addresses) are the shared
  texture-layout helpers that CreateTexture, LockRect, GetLevelDesc and Resolve (8373D570 calls
  83732FB8) use. They are called only by library code and by the XG code at 0x8385xxxx.
- Library-internal callers of these helpers sit directly above: 83733428 onwards (resource
  methods), 83734370-837368C0 (viewport/scissor internals, render- and sampler-state table
  functions), 83736DC8-83736F48 (Set* entry points), then part A's range from 83737068.

The **public XGraphics API is elsewhere, in the game-side code range** (XG is a separate lib):
8385CF58 = XGSetTextureHeader (24 game callers), 8385CFC8 = XGSetTextureHeaderEx (1 caller,
82332238), 8385CD58 = their common body (calls 83732250 and 83733628), 8385D0C0 =
XGOffsetResourceAddress (32 callers), 8385D058 = XGOffsetBaseTextureAddress. The functions
8382C738-83836F28 and 8385B6A0-8385E620 that call 83732370/83732800/83732898/83733D58/83733E28
look like more XG/D3DX texture code (tiling, fill, load). This is a guess; they were not read.

## 2. Textures: who creates them, object layout, SetTexture

**Both paths are used.** CreateTexture 83733E30 has 18 call sites (render-target copies, front
buffers in 83741B08, dynamic textures). Asset textures are built on game-owned memory with
XGSetTextureHeader. Example: 8231B008 calls `XGSetTextureHeader(128, 128, 1, 0, fmt, 0, 0,
0xFFFFFFFF, 0, &hdr, 0, 0)` (returns the size), then `XGOffsetResourceAddress(hdr, dataPtr)`, then
8380F110 = XPhysicalProtect(data, size, 2) (MmSetAddressProtect). The headers live inside game
structures, so a hook on CreateTexture alone will miss most of the textures the game draws with.
A host renderer has to work from the header passed to SetTexture (or from the fetch constant in
the device) and look up or upload by address, or also hook 8385CD58 and 8385D0C0.

Texture object (D3DBaseTexture, 52 bytes):

| Offset | Field |
| --- | --- |
| +0 | Common: type in bits 0-3 (3 texture; 17-20 come from GetType, not stored), 0x00100000 always, 0x00200000 / 0x00400000 from usage bits, lock count in bits 8-11 |
| +4 | RefCount (1 at creation) |
| +8 / +12 | Fence / ReadFence |
| +16 | Identifier (not written by the creators) |
| +20 / +24 | BaseFlush / MipFlush dirty ranges, 0xFFFF0000 = clean |
| +28 | fc dword0: type (2 = texture), signs, clamp (bits 10-18, overwritten by the device at SetTexture), pitch>>5 (bits 22-30), tiled (bit 31) |
| +32 | fc dword1: format (bits 0-5), endian (6-7), stacked (10), **base address, CPU virtual, bits 12-31** |
| +36 | fc dword2: size-1 per axis (1D 24 bits; 2D 13/13 + stack depth; 3D 11/11/10) |
| +40 | fc dword3: num format, swizzle (1-12), exp adjust (13-18), filters (19-30, from the device), border (31) |
| +44 | fc dword4: mip_min_level (2-5), mip_max_level (6-9) = levels-1, lod bias etc. from the device |
| +48 | fc dword5: dimension (bits 9-10: 0 1D, 1 2D, 2 3D, 3 cube), packed mips (11), **mip address, CPU virtual, bits 12-31** |

Addresses in the header are CPU virtual addresses (XPhysicalAlloc or game pointers).
SetTexture translates them for the GPU: `phys = (va & 0x1FFFFFFF) + (va >= 0xE0000000 ? 0x1000 : 0)`.

**SetTexture 837341F0 (dev, s, tex, bit)**: fc = dev+1152+24*s.

- dword0 = tex dword0 with bits 10-21 (clamp_x/y/z) kept from the device, so address modes are sampler state.
- dword1 = physical(tex dword1) with bit 11 (nearest clamp policy) from the device.
- dword2 = tex dword2.
- dword3 = tex dword3 with bits 19-30 (mag/min/mip/aniso/arbitrary filter) from the device.
- dword4 = tex dword4 with bits 0-1 and 10-31 from the device. mip_min_level = max(tex, MAXMIPLEVEL byte dev+11942+s), mip_max_level = min(tex, MINMIPLEVEL byte dev+11968+s).
- dword5 = physical(tex dword5 bits 9-28) with bits 0-8 (border colour, tri clamp, aniso bias) from the device.

The dirty bit is supplied by the caller in r6: **qword dev+24 bit (31-s)**, the same bit the
sampler-state setters compute (`(1<<63) >> (s+32)`). SetTexture(NULL) leaves the fetch constant
and the dirty mask unchanged and only clears the slot.

Surface (48 bytes): +0 Common type 4 (bit 31 = owns EDRAM tiles, bit 30 = texture level with
parent), +24 hi16 = MSAA type (bits 0-1) and width in EDRAM tile units, lo16 = tile count (or parent
pointer for level surfaces), +28 = RB_COLOR_INFO / RB_DEPTH_INFO (EDRAM base in bits 0-11; for level
surfaces, level in bits 28-31 and face in 22-27), +32 = HiZ word (depth), +36 = (w-1)<<18 \| (h-1)<<3,
+40 = D3DFORMAT.

## 3. Pending masks +0x10, +0x18, +0x20, +0x28

Read from the flush at the top of DrawVertices 8373C720, which runs before every draw and zeroes
each mask it handles. 837431A8(dev, mask << n, first reg, shadow) writes one register per set bit,
MSB first. "bit b <-> reg R" below means bit b set = re-send register R from its shadow.

**qword dev+0x10 (16)**

| Bits | Meaning | Shadow |
| --- | --- | --- |
| 0-11 | bit 11-k <-> reg 0x2200+k: 0x800 RB_DEPTHCONTROL, 0x400 RB_BLENDCONTROL0, 0x200 RB_COLORCONTROL, 0x100 RB_HIZCONTROL, 0x80 PA_CL_CLIP_CNTL, 0x40 PA_SU_SC_MODE_CNTL, 0x20 PA_CL_VTE_CNTL, 0x10 VGT_CURRENT_BIN_ID_MIN, 0x8 RB_MODECONTROL, 0x4/0x2/0x1 RB_BLENDCONTROL1/2/3 | dev+10548.. |
| 12-16 | bit 16-k <-> reg 0x2180+k: 0x10000 SQ_PROGRAM_CNTL, 0x8000 SQ_CONTEXT_MISC, 0x4000 SQ_INTERPOLATOR_CNTL, 0x2000 SQ_WRAPPING_0, 0x1000 SQ_WRAPPING_1 | dev+10528.. |
| 17-20 | derived state, resolved by 837442E0 before the register writes: 0x20000 depth/stencil/HiZ recompute (83742C10; set by ZENABLE, ZFUNC, stencil states, SetDepthStencilSurface), 0x40000 alpha test (ALPHATESTENABLE), 0x80000 vertex shader / declaration / stream stride changed, 0x100000 pixel shader changed. 837442E0 rebuilds SQ_PROGRAM_CNTL/CONTEXT_MISC/INTERPOLATOR, RB_MODECONTROL and RB_COLOR_INFO from dev+12688 (VS), 12684 (PS), 11812 (vdecl) and sets those bits | - |
| 21-41 | bit 41-k <-> reg 0x2100+k (k 0-20): VGT_MAX/MIN_VTX_INDX, VGT_INDX_OFFSET, IB reset index (bit 38), RB_COLOR_MASK (bit 37), RB_BLEND_RGBA (36-33), fog colour, RB_STENCILREFMASK_BF (0x20000000, bit 29), RB_STENCILREFMASK (bit 28), RB_ALPHA_REF (bit 27), viewport scale/offset PA_CL_VPORT_* (bits 26-21) | dev+10444.. |
| 42-57 | bit 57-k <-> reg 0x2000+k: RB_SURFACE_INFO (57), RB_COLOR_INFO (56), RB_DEPTH_INFO (55), RB_COLOR1-3_INFO (54-52), COHER_DEST_BASE_0-7, PA_SC_SCREEN_SCISSOR_TL/BR (43-42) | dev+10368.. |
| 58-63 | not consumed by the flush | - |

**qword dev+0x18 (24)**

| Bits | Meaning |
| --- | --- |
| 0-31 | bit 31-k <-> fetch constant k (6 dwords at dev+1152+24k, written to regs 0x4800+6k by 837433E8). Texture sampler s = constant s, so bit 31-s. Vertex stream s uses fetch slot 95-s, i.e. constant (95-s)/3, so bit s/3 (streams 0-2 share bit 0) |
| 34-54 | bit 54-k <-> reg 0x2280+k (k 0-20): PA_SU_POINT_SIZE, POINT_MINMAX, LINE_CNTL, ..., VGT_HOS_CNTL (bit 49, D3DRS 228), tessellation levels, VGT_GROUP_*, PA_SC_VIZ_QUERY, VGT_ENHANCE; shadow dev+10596.. |
| 32-33, 55-63 | not consumed by the flush |

So part A's "high 32 bits = fetch constants" should read "low 32 bits". Part B's "1 bit per fetch slot from the top" was correct.

**qword dev+0x20 (32)**

| Bits | Meaning |
| --- | --- |
| 0-37 | bit 37-k <-> reg 0x2300+k: PA_SC_LINE_CNTL, PA_SC_AA_CONFIG, PA_SU_VTX_CNTL, guard band, SQ_VS_CONST/SQ_PS_CONST, PA_SC_AA_MASK (bit 19, D3DRS 196 = MULTISAMPLEMASK), RB_COPY_* and clear registers, RB_SAMPLE_COUNT_*; shadow dev+10680.. |
| 38-45 | bit 45-k <-> reg 0x2380+k: PA_SU_POLY_OFFSET_FRONT/BACK_SCALE/OFFSET (depth bias), point radius/size/cull; shadow dev+10832.. |
| 49-54 | bit 54-k <-> user clip plane k, PA_CL_UCP_k_X..W (regs 0x2388+4k), written by 83743268 from dev+10272+16k |
| 56 | all bool and loop constants, regs 0x4900-0x4927 (40 dwords) from dev+10112 (0x2780) |
| others | not consumed by the flush |

**qword dev+0x28 (40)**: not a dirty mask. It is a filter on the +0x10 bits. At draw time
`if (dev[40] & dev[16]) 83742E10(...)` handles those registers through a special path (predicated
per-tile writes and HiZ) instead of a plain block write. Only three bits are ever set. 0x100
(RB_HIZCONTROL) is set or cleared by D3DRS 388, a HiZ-related state whose name is unverified.
1<<37 (RB_COLOR_MASK) and 1<<57 (RB_SURFACE_INFO) are set by BeginZPass 8374CF50. BeginZPass and
EndZPass 8374D1E0 are the only other writers (besides the default state setup 8373BAF0/837423D0).

**What a draw-time snapshot has to re-read.** The flush clears +0x00 ... +0x20 inside the draw, so
a hook that runs before the original draw sees the bits for this draw only. Since the game writes
many shadows inline (part B), a renderer should not try to track bits. The simpler rule:

- At every draw, read the full shadows: register blocks dev+10368 (0x2000-0x200F), 10444
  (0x2100-0x2114), 10528 (0x2180-0x2184), 10548 (0x2200-0x220B), 10596 (0x2280-0x2294), 10680
  (0x2300-0x2325), 10832 (0x2380-0x2387).
- Also read: fetch constants dev+1152 (32 x 24 bytes); VS/PS constants dev+0x780/0x1780; bool and
  loop constants dev+10112; clip planes dev+10272; bindings (VS 12688, PS 12684, vdecl 11812,
  streams 12452/12520, IB 12428, RT 12432-12444, DS 12448, textures 12536).
- Use the masks only as a cache hint: bit 31-k of +0x18 tells which fetch constants changed;
  +0x00 and +0x08 tell which shader constants changed.
- The derived bits 17-20 of +0x10 are resolved inside the draw (837442E0). A pre-draw read of
  RB_MODECONTROL / SQ_* / RB_COLOR_INFO can therefore be one shader change stale. Read those
  after the original flush runs, or derive them from the bound shaders yourself.
- Window scissor (0x2080-0x2082) does not go through any mask: SetScissorRect / SetViewport emit
  it directly (83734530). Take it from dev+12668 (rect), 11848 (enable) and 12640 (viewport).

## 4. Shader and declaration creation

| Function | Args as the game calls it | Pointer source |
| --- | --- | --- |
| CreateVertexShader 8373B790 | r3 = const DWORD* shader container; returns D3DVertexShader* | .rdata, header 0x102A1101 |
| CreatePixelShader 8373B3C8 | r3 = const DWORD* shader container; returns D3DPixelShader* | .rdata, header 0x102A1100 |
| CreateVertexDeclaration 8373BBE0 | r3 = const D3DVERTEXELEMENT9* (ends with stream 0xFF); returns decl | .rdata or a stack array built at run time (837DBB18) |

None of the three take the device. The game reaches them two ways:

- **Directly** with an `lis/addi` constant in r3, for example 8361A508 (VS 0x821B33E8), 82806720,
  837DBB18 (VS 0x8206D2A8).
- **Through out-of-line HRESULT wrappers** `(dev r3, pFunction r4, ppOut r5)`. They do `mr r3,r4`,
  call the creator, store the result to *r5 and return E_OUTOFMEMORY on NULL:
  - 8231AD88 (VS, 23 callers), 82319CB0 (PS, 19), 8231ADE8 (decl, 8);
  - vtable-only copies 8377E648 / 8377E698 / 8377E6E8 (0 direct callers);
  - 82319D10 (PS, caller 822F3708).

Every pointer checked lands on a shader container in the .rdata range 0x8200AC88-0x82240698.

- 8231B208: base 0x8223FA80. VS at 0x8223E728 and 0x8223F1C0 (header `102A1101`), PS at 0x8223EBF0
  ... 0x8223FA80 (`102A1100`), decl at 0x8223FC70 (`{0,0,FLOAT3 0x2A23B9,...}{0,12,FLOAT2 0x2C23A5,...}`
  then `00FF0000` end).
- 83614B90: VS 0x821B5168, PS 0x821B5258.

So the creators can be hooked as in Unleashed (`CreateVertexShader(const be<uint32_t>* function)`).
The container address works as a stable key for precompiled host shaders, which fits XenosRecomp's
container scan (tools/extract_shaders.py). Not all 50 call sites were read. Shaders built from
loaded files, if any, would show up as heap pointers.

## Corrections to parts A and B

- B: CreateTexture's arguments are (w, h, depth, levels, usage, format, pool, type). 83741B08 passes
  (w, h, 1, 1, 0, fmt, 0, 3), and r10 = 3 is D3DRTYPE_TEXTURE, not a pool.
- A: fetch-constant dirty bits are the low 32 bits of dev+0x18 (bit 31-k), not the high 32.
- B: +0x28 is not a register-block dirty mask (section 3).
- 83734810 is the inner viewport setter (float args). 83736EC8 is the public SetViewport(D3DVIEWPORT9*).
- 83736F48 is the full SetStreamSource. Part A's 83737950 is a separate partial one (no stride or pointer).

## Open questions

- 83730408-83732248 (the 0x7880-byte object) is unidentified. It is outside D3D, but its callers
  (823F09C0, 82455D40, 8246A500, ...) were not traced.
- 83733628's stack arguments s84-s124 (border, exp bias, pitch) are read from slot order, not from
  a caller passing distinct values. 83732898's exact XG name is a guess.
- Texture vs cube alias for 83732DB8 / 83733E28 is unproven (they behave the same).
- D3DRS 304 = VIEWPORTENABLE and the state at 388 (HiZ-related) are named from the 360 enum order
  as remembered, not from a header.
- Bits not consumed by the DrawVertices flush (+0x10 58-63, +0x18 32-33/55-63, +0x20 46-48/55/57-63)
  may be used by other draw paths (BeginVertices, DrawIndexedVertices 8373CB08, Resolve). Not checked.
- Which of the 8382C738-8385E620 functions are XGraphics and which are D3DX was not established.
