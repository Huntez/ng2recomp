# D3D map, part B (37 of the 74 public functions)

XDK 2.0.6683, NG2. Method: static reading of `tools/callgraph.py asm/callers/callees`
output plus scans of the generated code's `// <ppc asm>` comments. Nothing was run.
Guest args: r3, r4, ... and floats in f1..; a float argument also uses up a GPR slot
(BeginTiling/EndTiling show `f1` = ClearZ with r8 skipped and ClearStencil in r9).
"dev" = D3DDevice*. Device offsets are decimal, as they appear in the asm.

## Table

| Address | D3D name | Signature (guest regs) | Conf. | Evidence |
| --- | --- | --- | --- | --- |
| 8373E970 | D3DResource_AddRef | r3=res -> r3=new refcount | high | lwarx/stwcx ++ on res+4; a surface (type 4) with the parent bit 0x40000000 also AddRefs its parent at +24 |
| 8373E9E8 | D3DResource_Release | r3=res -> r3=new refcount (0 = destroyed) | high | atomic -- on +4; at 0 it releases the parent, then calls 8373E3B8, the only caller of the destroy switch. **8373E3B8 is DestructResource**, Unleashed's hook point (it switches on the type, frees memory, frees EDRAM tiles at size/5120) |
| 8373EA68 | D3DResource_GetDevice | r3=res (unused), r4=D3DDevice** out | medium | loads the device through the pointer at 0x820007B8, increments dev+60 (device refcount) and stores it to *r4 |
| 8373EA90 | D3DResource_GetType | r3=res -> D3DRESOURCETYPE | high | returns the header type (common & 0xF) or 16..20 for texture variants, chosen by the fetch dimension (+48 bits 9-10: cube 18, 3D 17, 1D 20), the stacked bit (+32 & 0x400: 19) and a volume surface (16). The enum values for 16..20 are from memory of the XDK headers (Volume/VolumeTexture/CubeTexture/ArrayTexture/LineTexture) |
| 8373EB20 | D3DDevice_CreateVertexBuffer (inner, = Unleashed `CreateVertexBuffer`) | r3=Length, r4=Usage -> r3=D3DVertexBuffer* | high | allocates a 32-byte header (XMemAlloc attr 0x64800000) plus physical data; common = 0x00100001 (type 1); +24 = fetch dword0 (addr\|3), +28 = size\|endian 2; refcount +4 = 1, +20 = 0xFFFF0000 |
| 8373EBE8 | D3DVertexBuffer_Lock (= Unleashed `LockVertexBuffer`) | r3=vb, r4=OffsetToLock, r5=SizeToLock, r6=Flags -> r3=data ptr | high | calls lock helper 8373E5B8 with base +24 and the whole size +28. r4/r5 are not used, so it always returns the start of the buffer. Caller 83863B40 passes (vb,0,120,0) |
| 8373EC38 | D3DVertexBuffer_Unlock | r3=vb | high | tail call 8373E848(res, base(+24), 0): lock count -256 in common, dcbf flush of the dirty range through 83745B20 |
| 8373EC48 | D3DDevice_CreateIndexBuffer (inner, = Unleashed `CreateIndexBuffer`) | r3=Length, r4=Usage, r5=Format -> r3=D3DIndexBuffer* | high | 32-byte header, type 2; r5 is rotated into common bits 29-31 (endian, plus INDEX32 at bit 31, so INDEX16=1 and INDEX32=6 fit); +24 = address, +28 = length. Caller 838639B0 makes a 12-byte INDEX16 IB with r5=1 |
| 8373ECF8 | D3DIndexBuffer_Lock (= Unleashed `LockIndexBuffer`) | r3=ib, r4=Offset, r5=Size, r6=Flags -> r3=data ptr | high | same 8373E5B8 call as the VB lock, flags \| 2, whole buffer; caller writes 16-bit indices into the result |
| 8373ED40 | D3DIndexBuffer_Unlock | r3=ib | high | tail call 8373E848(res, +24, 0) |
| 8373ED50 | D3DResource_IsSet | r3=res, r4=dev -> BOOL | high | a switch on type compares the device binding slots (see the layout notes): VB in streams 12452[16], IB 12428, texture in 12536[26], surface in RT0 12432 or DS 12448, vdecl 11812, VS 12688, PS 12684. Called from game 837C61C8 |
| 8373EE40 | internal: bitmap range set/clear for the EDRAM tile allocator | r3=alloc, r4=first, r5=end, r6=1 set / 0 clear | medium | bit-range fill on words at alloc+32; called by 8373EF78 (allocate) and DestructResource 8373E3B8 (free) |
| 8373EF78 | internal: EDRAM tile allocator (best-fit run) | r3=tile count, r4=out first tile -> BOOL | medium | lazily creates an 804-byte global at 0x84C250D8 (bitmap +32, free count +28 = 6144), critical section; caller CreateSurface 83733F50 rejects base+count > 2048 tiles. Its only caller is 83733F50, which lies below the doc's D3D range |
| 8373F180 | internal: allocate a new resource-tag (fence) block | r3=dev | medium | takes 502 dwords from 837397E0, links into the list at dev+13500/13504, cursor/limit at 13508/13512, dcbf flush. Used when a bound resource is replaced and must be fenced (SetTexture, SetStreamSource, Set*Shader). Game code inlines the same sequence (837C6680, 836552E0) |
| 8373FAF8 | D3DDevice_GetDeviceCaps | r3=dev (unused), r4=D3DCAPS9* | medium | memcpy of 304 bytes (= sizeof D3DCAPS9) from the static at 0x822832C0; one game caller, 8382D570 |
| 8373FB10 | D3DDevice_GetDisplayMode | r3=dev, r4=iSwapChain (unused), r5=D3DDISPLAYMODE* | high | writes Width = dev+13528 (pp.BackBufferWidth), Height = 13532, RefreshRate = 21536 (from VdQueryVideoMode in 83741B08), Format = 13536 |
| 83741B08 | internal: create the device's frame buffers (CreateDevice/Reset step) | r3=dev, r4=D3DPRESENT_PARAMETERS* -> BOOL | medium | VdQueryVideoMode; front-buffer texture (CreateTexture 83733E30) -> dev+14812 unless pp+60; back-buffer surface (CreateSurface 83733F50) -> 14816 and SetRenderTarget(0) unless pp+56; auto depth -> 14808 and SetDepthStencilSurface if pp+36; copies 124 bytes of pp to dev+13528; gamma ramp (837377C0/83737888) |
| 83741DC0 | internal: release the device's frame buffers | r3=dev | medium | unbinds DS and RT0-3 (837374E8 / 83737180 with NULL), 8373A7D0, then releases 14812/14816/14808. Game 822F2218 and 837C6680 inline a Reset sequence: 8373A918(dev, pp+72); on failure 8373A7D0, 83741DC0, 83741B08(dev, pp), 83741DC0 |
| 83744D90 | D3DDevice_SynchronizeToPresentationInterval (guess) | r3=dev, r4=flag (0 = also write the extra wait packets) | low | maps pp.PresentationInterval (dev+13580: 0/1->1, 2->2, 4->3, IMMEDIATE->0) into a packet via 8373A5A8 with callback 0x83744BF8, writes 0xC0043C00 (WAIT_REG_MEM); runs before the front-buffer resolve in Present and before Swap in game 837C6680 |
| 837452C0 | D3DDevice_Swap | r3=dev, r4=front-buffer D3DBaseTexture*, r5=swap params or NULL | medium | the only VdSwap caller (also VdSetDisplayMode, VdGetSystemCommandBuffer); copies r4's fetch constant (+28, 24 bytes) and its 52-byte header to dev+14828. Present passes (dev, 14812, dev+13624); **the game's own present 837C6680 calls it directly** with its own front-buffer texture and r5=0 |
| 837459D8 | D3DDevice_Present (= Unleashed `Video::Present`) | r3=dev (PC-style rect/hwnd args absent) | medium | SetRenderTarget(0, 14816), 83744D90, Resolve 8373D570 into front buffer 14812, kick 8373A3D0, restores viewport (83734810, f1..f6 from dev+12640) and scissor (83736DC8, dev+12668), Swap(dev, 14812, dev+13624). Game callers: 8377E620, 83873B80 |
| 83745B20 | internal: flush CPU data cache over [r3, r4) | r3=start, r4=end, r5 unused | high | unrolled dcbf by 128-byte lines and a trailing sync; returns early for addresses in one window (r3-0x7F100000 < 0x7F00000). Game callers 836552E0 and 837C6680 inline the tag-block code above |
| 8374C5B8 | D3DDevice_SetPredication (probably) | r3=dev, r4=PredicationMask (0 = off) | medium | writes PM4 0xC0006000 (type 3, opcode 0x60 SET_BIN_MASK_LO) followed by the mask (also stored to dev+12700); recomputes the tiling flag in dev+10940. EndTiling calls it with 3<<(2*tile) for each tile, 0x80000000 and 0. Game caller 837DF208 |
| 8374C728 | D3DDevice_BeginTiling | r3=dev, r4=Flags, r5=Count, r6=D3DRECT* pTileRects, r7=D3DVECTOR4* pClearColor, f1=ClearZ, r9=ClearStencil | high | rects -> dev+12748 (16 B each) and 12988, clear color -> 13184, Z -> 13200, stencil -> 13204; saves RT0-3/DS into 12716-12736; sets 10940 \|= 0x20; EVENT_WRITE 0xC0004600; same argument order as the XDK prototype. Game caller 837DF208 |
| 8374CA98 | D3DDevice_EndTiling | r3=dev, r4=ResolveFlags, r5=pResolveRects (NULL -> tile rects), r6=pDestTexture, r7=pClearColor, f1=ClearZ, r9=ClearStencil (pResolveParams on the stack, not checked) -> HRESULT | high | per tile: SetPredication(3<<2i) + Resolve 8373D570; then replays the captured buffer (8373A8C0 under predication 0x80000000), clears 10940 &= ~0x20; returns 0 or 0x8007000E. Game caller 837DF3B8 |
| 8374CF50 | D3DDevice_BeginZPass (guess) | r3=dev | medium | sets 10940 \|= 0x40 and 10941 \|= 0x40, writes a marker into the secondary buffer at dev+13352, sets pending bits (+16 \|= 8, +40), clears each tile when tiling (83739188 with predication 1/2). Game caller 837DF208, which also calls BeginTiling |
| 8374D1E0 | D3DDevice_EndZPass (guess) | r3=dev -> HRESULT (from 8374D490) | medium | clears 10940 &= ~0x40, closes the two secondary buffers at dev+13232/13332, replays under predication 0x80000000, restores scissor. Game caller 822F16D0. D3D code ends at 8374D490 (this function's helper) |
| 8374D558 | not D3D: XAudio thread-safe wrapper | r3=obj | medium (not D3D) | locks the global CS at 0x83BBC830 (the same one 8374D8F0 uses), calls obj->vtbl[+4], unlocks. Game caller 83864830 |
| 8374D5C0 | not D3D: XAudio wrapper, vtbl[+8] | r3=obj | medium | same lock; 7 game audio callers (837D3F10, 837E3528, 83800CA0, 83864700, ...) |
| 8374D628 | not D3D: XAudio wrapper, vtbl[+48] | r3=obj, r4 | medium | same lock |
| 8374D680 | not D3D: XAudio wrapper | r3=obj, r4=56-byte struct, r5, r6 | medium | copies r4 to obj+84, calls (obj+76)->vtbl[+112], then 837516F8 and obj->vtbl[+36](r5) |
| 8374D728 | not D3D: XAudio wrapper, (obj+76)->vtbl[+28] | r3=obj, r4, r5 | medium | same lock |
| 8374D788 | not D3D: XAudio wrapper -> 83751368 | r3=obj | medium | same lock |
| 8374D7E0 | not D3D: XAudio wrapper -> 83750D78 | r3=obj, r4 | medium | same lock |
| 8374D828 | not D3D: XAudio wrapper, vtbl[+60] | r3=obj, r4 | medium | same lock |
| 8374D880 | not D3D: XAudio wrapper, (obj+76)->vtbl[+56] | r3=obj | medium | same lock; callers 83801030, 838649E8 |
| 8374D8E8 | not D3D: XAudio stub | r3 -> tail call 8374D8F0(r3, 0) | high (not D3D) | `li r4,0; b 0x8374d8f0`; 8374D8F0 takes the same XAudio CS |

Counts: high 15 (one of them is 8374D8E8, certain only in that it is not D3D), medium 21 (9 of them XAudio), low 1. That gives 27 D3D entries (21 D3D API names and 6 internal helpers) and 10 that are not D3D (XAudio). The XAudio wrappers are identified only by vtable slot; no API names are claimed for them.

## Corrections and cross-checks for docs/renderer-plan.md and part A

- **The D3D range is wrong at both ends.** Library code starts below 0x83737000 (at least
  0x83732008; XGraphics-style size and tiling helpers go further down) and ends at about 0x8374D558
  (8374D558-8374D8E8 are XAudio). Functions in 0x83732000-0x83737000 called from the game
  that the 74-list therefore misses (the names come from reading the code; they are medium unless
  marked):
  - 837341F0 **SetTexture**(dev, r4=sampler, r5=texture, r6=u64 pending bit), 67 game callers, high.
    It copies the texture's 6-dword fetch constant (+28..+48) into dev+1152+24*sampler, stores the
    pointer at dev+12536+4*sampler, ORs the bit into pending +24 and fences the old texture.
  - 83736F48 **SetStreamSource**(dev, r4=stream, r5=vb, r6=offset, r7=stride, r8=pending bit),
    8 game callers, high. It writes vertex fetch constants at dev+8*(239-stream) (+1780), pointer at
    12452+4*stream, stride/4 byte at 12520+stream (compared with 11824+stream, then pending +16).
  - 83733E30 **CreateTexture** (52-byte header; 17 game callers) and 83733F50 **CreateSurface**
    (48 bytes, EDRAM tiles via 8373EF78; 11 callers), both high. 83741B08 shows the argument order:
    CreateTexture(w, h, levels=1, usage=1, 0, format, 0, pool=3), CreateSurface(w, h, format,
    multisample, 0).
  - 83736EC8 **SetViewport**(dev, D3DVIEWPORT9*), which converts X/Y/W/H to float and calls 83734810
    (dev, f1..f6 = X, Y, W, H, MinZ, MaxZ). The viewport is shadowed at dev+12640.
  - 83736DC8 **SetScissorRect**(dev, RECT*). The rect is stored at dev+12668 and clamped to the
    viewport into the register shadow at 10436/10440.
  - 83732DB8 **GetLevelDesc**(tex, level, D3DSURFACE_DESC*) (16 callers), 837340B8
    **D3DSurface_GetDesc** (= Unleashed GetSurfaceDesc), 837334F0/83733428 texture **LockRect**,
    83734150 surface LockRect, 83734170 surface UnlockRect. These are medium.
  - 837363D8 and 83736568: SetSamplerState setters for one state each (sampler r4, value r5). They
    write dword 3 of the fetch constant (filter fields) and the pending +24 bit. They have 55 direct
    game callers each, probably MinFilter/MagFilter (guess).
- 83737180 is **SetRenderTarget**(dev, index, surface) and 837374E8 is
  **SetDepthStencilSurface**(dev, surface): 83741B08 binds the back buffer with the first and the
  auto-depth surface with the second. renderer-plan says "837374E8 SetRenderTarget-like".
- 8373B4D8 (dev+12684) is **SetPixelShader**, not SetVertexShader. 8373B880 (dev+12688) is
  **SetVertexShader**. Evidence: IsSet maps type 6 (VS) to 12688 and type 7 (PS) to 12684. The
  12688 setter also reads a larger header (fixups at +872 against +40) and touches VS-side bits.
- Device layout differs from Unleashed's `GuestDevice` in the first 0x40 bytes. The pending masks
  are qwords +0 (VS consts), +8 (PS consts), +16 (misc: shaders, vdecl 0x80000, ...), +24 (fetch
  constants, 1 bit per fetch slot from the top), +32 and +40 (register blocks). **+48 is the ring
  write cursor, +56 the ring limit, +60 the device refcount**, so it is not "8 dirty qwords". The
  render-state function table is at +0x40 and the sampler-state table at +0x1D4 (as in Unleashed).
  Fetch constants are at +0x480 (1152), VS constants at +0x780 and PS constants at +0x1780.
  Bindings: IB 12428, RT0-3 12432-12444, DS 12448, streams 12452[16], stream strides (bytes)
  12520, textures 12536[26], PS 12684, VS 12688, vdecl 11812 (Unleashed: 0x2E2C = 11820, so 8
  bytes earlier here), viewport 12640 (Unleashed 0x3168 = 12648), scissor 12668.
  Present params copy: 13528. Frame buffers: 14808 auto depth, 14812 front texture, 14816 back
  buffer. GPU register shadows sit around 10112-10440 with dirty bits in +32/+40. The fence
  counter is at 10908 and the last inserted fence at 10928.

## D3D functionality reached outside the 74 public functions

Many XDK calls in NG2 are inlined into game code. The XDK's inline headers explain part of it,
and link-time code generation of the D3D library may explain the rest (a guess). The inlined
calls write straight into the device shadow and pending masks. A hook-only renderer will not see
these calls. Every one of them still ends in a draw or a resolve that reads the device, so the
practical approach is to hook the draws (8373BD50, 8373C1F0, 8373C720, 8373CB08, part A) and
Resolve (8373D570) and **read the state from the device at that moment**: bindings, fetch
constants, shader constant shadows and register shadows. Do not rely on Set* hooks. The patterns
found:

1. **SetVertexDeclaration, inlined, no refcount.** `stw decl,11812(dev); ld r,16(dev); oris r,r,8;
   std r,16(dev)` appears in 59 game functions, for example 8231B3B0, 82805030, 837E9CC8,
   8361B580 and 837C6680 (which sets NULL). No library SetVertexDeclaration is called. Hook
   creation instead (CreateVertexDeclaration is likely one of 8373B3C8/8373B790/8373BBE0, the
   creators in part A's share; guess) and read dev+11812 at draw time. The device embeds its own
   declaration at dev+12184 (837ECC30 compares against it), probably for the UP draws.
2. **Shader constants, inlined.** VS constants are written with `stfs` into dev+0x780+16*reg plus
   `ld/ori 8192/std` on pending +0 (for example 82805030 writes c232 at dev+5120..5132). PS
   constants are written into dev+0x1780+16*reg plus `or` of a bit into +8 (8231B3B0 writes
   dev+6500.. = c30). Pending +0 is written by 39 game functions and +8 by 48. The library
   SetVertexShaderConstantF 8373B1F8 is used only for the non-constant cases.
3. **Render states and sampler states through the device function tables.** The game's state cache
   flush 837C6F90 (14 callers) calls `*(dev + 0x40 + D3DRS)` with (dev, value) and
   `*(dev + 0x1D4 + 4*(type-1))` with (dev, sampler, value). D3DRS values are byte offsets
   (multiples of 4). The table entries are the many small functions at 0x83734A78-0x837368xx, which
   update the register shadow and the pending +32/+40 bits. Hooking those addresses catches the
   table calls. A few states are also written directly:
   - fetch-constant edits at dev+1152 with `oris 0x8000` on +24 (82331348, sampler 0 address and
     clamp bits; 70 game functions touch +24);
   - register-shadow edits at dev+10112/10120 with a bit in +32 (8282D300; 34 functions touch +32).
4. **Texture and stream unbinding plus resource fencing, inlined.** 837C6680 (game present) and
   836552E0 loop over textures 12536.. and streams 12452.., clear the slots, and tag each old
   resource with the current fence (res+8 = dev+10908). When fence tracking is off they append a
   tag to the block at dev+13508, growing it with 837397E0 and 83745B20, which inlines 8373F180.
   This amounts to SetTexture(NULL)/SetStreamSource(NULL) for everything.
5. **Fences and kickoff, inlined.** 822F16D0 and 837C6680 read dev+10908, store it to 10928 and
   call 8373A3D0. This looks like InsertFence (guess). 837C6680 also writes ring packets directly
   through 8373A5A8 (callback 0x837C59C8) and `stw r3,48(dev)`. This is the only game-side PM4
   writing found: no game function was found writing `stwu` packets into the ring at dev+48.
6. **Present is done by the game itself.** 837C6680 does Resolve (8373D570) into its own
   front-buffer textures (double-buffered at obj+1304/1308), then the unbind loops from item 4,
   then SynchronizeToPresentationInterval-like 83744D90, then Swap 837452C0(dev, tex, 0).
   837459D8 (Present) is used only by 8377E620/83873B80. Swap is therefore the hook to take for
   frame presentation.
7. **Device Reset, inlined** in 822F2218 and 837C6680 (see 83741DC0).
8. **State capture.** 837ECC30 reads streams, IB (AddRef 8373E970), VS 12688, PS 12684, vdecl
   11812 and viewport 12640 directly. This is an inline equivalent of Get* and StateBlock. If the
   renderer replaces the guest objects, these fields must still hold valid guest pointers.
9. **Predicated tiling and Z pass** come through 8374C728/8374CA98/8374CF50/8374D1E0 (above),
   called from the game's 837DF208 and 837DF3B8 (and 822F16D0). A PC-style renderer can make
   BeginTiling/EndTiling one untiled pass plus a resolve, and make the Z pass a no-op or a depth
   prepass.

Not found in the game: DrawPrimitiveUP-style inline writes. UP draws presumably go through the
library draw functions (part A).

## Open questions

- How large is the 0x83728000-0x83737000 library block exactly (XGraphics vs D3D), and which other
  functions there does the game call? The 39 game-called functions in 0x83732000-0x83737000 are
  listed by the scan; only those named above were read.
- Names and argument order of the creators 8373B3C8/8373B790/8373BBE0 (vertex declaration and
  shader creation; part A's share).
- 83744D90 (SynchronizeToPresentationInterval?) and 8374CF50/8374D1E0 (Z pass?) are named from
  structure only.
- Whether any game code inlines Release (an atomic decrement on res+4) instead of calling
  8373E9E8. If it does, DestructResource 8373E3B8 is still the right hook, since it is reached only
  through 8373E9E8.
- The exact meaning of pending-mask bits in +16/+32/+40. They are needed to know which register
  shadows to re-read at draw time.
