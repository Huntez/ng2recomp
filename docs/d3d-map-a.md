# D3D map, part A (37 of the 74 public D3D functions)

XDK 2.0.6683, D3D range 0x83737000-0x8374D8F0. Found by reading the disassembly
(`tools/callgraph.py ... asm/callers`) and the image dump (`NG2_DUMP_IMAGE`) for
static tables. Guest args are r3, r4, ...; floats in f1 still use up a GPR slot
(PPC64 ABI), so args after a float skip one register. "dev" = D3DDevice*.
Names are XDK names (`D3DDevice_X` is shortened to `X`). "Guess" means the name
was not proved.

NG2 does not use the D3D9-compat draw calls Unleashed hooks (DrawPrimitive,
DrawIndexedPrimitive, DrawPrimitiveUP). It calls the native 360 ones:
DrawVertices, DrawIndexedVertices, BeginVertices, BeginIndexedVertices.

| Address | D3D name | Signature (guest regs) | Conf. | Evidence |
| --- | --- | --- | --- | --- |
| 83737068 | GetStreamSource | r3 dev, r4 stream, r5 UINT* pOffset, r6 UINT* pStride; returns D3DVertexBuffer* (AddRef'd) | medium | reads buffer at dev+0x30A4+4s and vertex fetch const slot 95-s (dev+0x778-8s); offset = fetch addr - buffer phys base; stride = byte dev+0x30E8+s <<2; calls AddRef 8373E970 |
| 837370F0 | SetIndices | r3 dev, r4 D3DIndexBuffer* | high | stores r4 at dev+0x308C; the previous one gets a fence (+8) or is queued on the pending list; DrawIndexedVertices 8373CB08 reads dev+0x308C (+0 bit31 = 32-bit indices, +24 = address); game calls it with 0 to unbind |
| 83737180 | SetRenderTarget | r3 dev, r4 index (0-3), r5 D3DSurface* | high | stores at dev+0x3090+4i; surface+0x1C goes to the shadow of RB_COLOR_INFO (0x2001) or RB_COLORn_INFO (0x2003+); updates the RB_COLOR_MASK nibble for the RT (dev+0x28DC = reg 0x2104) |
| 837374E8 | SetDepthStencilSurface | r3 dev, r4 D3DSurface* | high | stores at dev+0x30A0; surface+0x1C goes to RB_DEPTH_INFO (dev+0x2888 = reg 0x2002) and +0x20 to dev+0x2940; recomputes Z_ENABLE in RB_DEPTHCONTROL (dev+0x2934); ZENABLE set-state (837351D0) checks dev+0x30A0 |
| 837377C0 | SetGammaRamp | r3 dev, r4 flags (unused), r5 const D3DGAMMARAMP* | medium | 8373F750 reads 3x256 WORDs at +0/+512/+1024 (R/G/B) and writes a 1536-byte table; copied to dev+0x3A8C only if it changed, then sets dev+0x4134 bit 1 and dev+0x56F0 bit 31; also called from device init 83741B08 |
| 83737950 | SetStreamSource (out-of-line part) | r3 dev, r4 stream, r5 D3DVertexBuffer*, r6 offset in bytes | medium | writes vertex fetch slot 95-stream (fetch const (95-s)/3 at dev+0x480+24*k): addr = vb+0x18 + offset, size = vb+0x1C - offset, type 3; sets the fetch dirty bit in qword dev+0x18. Does not store the stride (byte dev+0x30E8+s) or the pointer (dev+0x30A4+4s); the game writes those inline |
| 83737BB8 | Direct3D_CreateDevice | r3 adapter (0), r4 devtype (1), r5 hwnd (0), r6 behavior flags (1), r7 D3DPRESENT_PARAMETERS*, r8 D3DDevice** out; returns HRESULT | high | only path to VdInitializeEngines (83741F00); game 837C62E8 passes these exact args; allocates the device (see below) |
| 83737C98 | AddRef (device) | r3 dev; returns new count | high | ++dev+0x3C |
| 83737CB0 | Release (device) | r3 dev; returns new count | high | --dev+0x3C; when it hits 1, runs teardown 83742128 and XMemFree of the raw block at dev-4; CreateDevice calls it on failure |
| 83737D08 | AcquireThreadOwnership | r3 dev | medium | dev+0x2A88 = current thread (8396C5B0 = r13->thread+0x14C); 2 callers (a movie player), paired with 83737D48 |
| 83737D48 | ReleaseThreadOwnership | r3 dev | medium | eieio; dev+0x2A88 = 0 |
| 83737D58 | Suspend (guess) | r3 dev | low | calls the global hook *(0x820007B4)->+4 with 2, BlockUntilIdle, under the dev+0x3A50 crit-sec sets dev+0x2ABE bit 1 and gives ownership (dev+0x2A88) to the D3D worker thread (dev+0x2C68+80*dev[0x2C30]); 1 game caller 822F0BF8 |
| 83737DF0 | Resume (guess) | r3 dev | low | reverses 83737D58: hook with 1, ownership back to the current thread, clears dev+0x2ABE bit 1; same caller |
| 837390C8 | Clear | r3 dev, r4 count, r5 const D3DRECT* (or 0), r6 flags, r7 D3DCOLOR, f1 Z, r9 stencil | high | unpacks r7 with vupkd3d (D3DCOLOR to float4); calls 83739188 once (no rects) or for each 16-byte rect; game fills a struct (rects/count/flags 31/color/Z/stencil at renderer+1272..1292) that it also passes to Resolve |
| 837397E0 | internal: allocate dwords from the ring tail | r3 dev, r4 dword count, r5 alignment; returns ptr or 0 | low | carves r4*4 bytes from the end of the current ring segment (adjusts dev+0x34/+0x38); when recording a command buffer (dev+0x34BC) it goes through that buffer's callback (+0xAC); used by BeginVertices/BeginIndexedVertices/Resolve |
| 8373A3D0 | KickOff (internal segment kick, guess) | r3 dev; returns new write ptr (dev+0x30) | medium | called when dev+0x30 (write ptr) > dev+0x38 (limit) in every packet writer; may block on fence via 8373A4D0; 4 game callers |
| 8373A4D0 | BlockOnFence | r3 dev, r4 fence, r5 flags | medium | loops while fence is ahead of *(dev+0x2A90) (GPU fence write-back), timeout from mftb, wait helper 83737AE0; kicks if fence == dev+0x2A9C (current fence) |
| 8373A5A8 | internal: write InsertCallback packets | r3 dev, r4 ring ptr, r5 type/flags, r6 callback, r7 context; returns new ptr | medium | type0 WAIT_UNTIL (0x5C8), type0 to regs 0x57C-0x57D = r6, r7, type3 0x3C (WAIT_REG_MEM) on dev+0x2A94, then type3 0x54 (INTERRUPT) |
| 8373A7D0 | BlockUntilIdle | r3 dev | medium | writes WAIT_UNTIL (type0 reg 0x5C8 = 0x20000), blocks on the current fence (dev+0x2A9C), then spins until dev+0x2AF8 == 0 |
| 8373A8C0 | InsertCallback | r3 dev, r4 type, r5 callback fn, r6 context | medium | reserves ring space, calls 8373A5A8, updates dev+0x30; game passes a function address and an object |
| 8373A918 | SetRingBufferParameters | r3 dev, r4 D3DRING_BUFFER_PARAMETERS* (= presentparams+72) | medium | BlockUntilIdle, frees the old ring/fence memory (dev+0x39E4/0x39E8/0x2A94/0x2A90), VdInitializeRingBuffer, VdEnableRingBufferRPtrWriteBack, VdSetSystemCommandBufferGpuIdentifierAddress; called from device init and game reset paths |
| 8373B1F8 | SetVertexShaderConstantF (out-of-line) | r3 dev, r4 start reg, r5 const float*, r6 vec4 count, r7 64-bit dirty mask | high | copies to dev+0x780+16*reg with lvlx/lvrx; ORs r7 into qword dev+0x00 (the inline wrapper builds the mask) |
| 8373B2E0 | SetPixelShaderConstantF (out-of-line) | r3 dev, r4 start reg, r5 const float*, r6 vec4 count, r7 64-bit dirty mask | high | same as above to dev+0x1780+16*reg; ORs into qword dev+0x08 |
| 8373B3C8 | CreatePixelShader | r3 const DWORD* function; returns D3DPixelShader* | high | object = 40-byte header + copy of the shader header at +40; microcode copied to physical memory at +24; Common = 0x00100007 (type 7), RefCount +4 = 1, +20 = 0xFFFF0000; optional hook *(0x83C250D0) |
| 8373B4D8 | SetPixelShader | r3 dev, r4 D3DPixelShader* | high | dev+0x318C; fence/pending handling for the old shader; dirty bits 0x100000/0x20000 in qword dev+0x10; reads the shader header at shader+40 (+20 = constant-usage block) |
| 8373B790 | CreateVertexShader | r3 const DWORD* function; returns D3DVertexShader* | high | object = 872 zeroed bytes + shader header copy at +872; microcode to physical memory; 8373B698 sets Common, then \|= 0x100000 |
| 8373B880 | SetVertexShader | r3 dev, r4 D3DVertexShader* | high | dev+0x3190; reads shader+872 (+20); draws compare stream-0 stride with dev+0x2E30 and re-dirty when dev+0x3190 != 0 |
| 8373BAD8 | SetVertexDeclaration | r3 dev, r4 D3DVertexDeclaration* | high | dev+0x2E24 = r4, dirty 0x80000 in qword dev+0x10; the game writes the same field and bit inline next to its draws (Unleashed's field is at 0x2E2C) |
| 8373BBE0 | CreateVertexDeclaration | r3 const D3DVERTEXELEMENT9*; returns decl | high | counts 12-byte elements up to stream == 0xFF (D3DDECL_END), XMemAlloc(56 + 12n), builds with 8373BAF0 |
| 8373BC68 | SetShaderGPRAllocation (guess) | r3 dev, r4 flags, r5 VS GPR count, r6 PS GPR count | medium | 0/0 becomes 64/64; packs (vs<<8\|ps)<<4 into dev+0x2AA8; emits three type3 0x3F (INDIRECT_BUFFER) packets: dev+0x35CC/0x35C8, per-count table dev+0x35D0+8*vs (built by 8373B060 for 16..111), dev+0x39D4/0x39D0 |
| 8373BD50 | BeginVertices | r3 dev, r4 prim type, r5 vertex count, r6 stride; returns void* vertex data in the ring | high | stride/4 into byte dev+0x30E8 (stream 0), flushes dirty state, DRAW_INDX type3 0x22 with auto index (0x80) and count r5<<16, returns the inline data pointer (also at dev+0x3478); game writes vertices to r3 right after (e.g. 8231B3B0: type 8, 3 verts, stride 20) |
| 8373C1F0 | BeginIndexedVertices | r3 dev, r4 prim type, r5 base vertex, r6 vertex count, r7 index count, r8 index format (bit 2 = 32-bit), r9 stride, r10 void** ppIndexData, stack(84) void** ppVertexData; returns HRESULT | high | caller 82832C60 passes (6,0,n,n,1,16,&idx,&vtx) and memcpy's vertices/indices into the returned pointers; same packet path as BeginVertices |
| 8373C720 | DrawVertices | r3 dev, r4 prim type, r5 start vertex, r6 vertex count | high | flushes dirty state (VS/PS consts, register blocks), type0 reg 0x2102 = r5, DRAW_INDX type3 0x22 auto-index, count split at 0xFFFF per primitive type (table 0x820512B8); game: (8,0,3n), (4,0,6) |
| 8373CB08 | DrawIndexedVertices (specialised) | r3 dev, r4 prim type (always 6), r5 base vertex (always 0), r6 start index, r7 index count | high | type3 0xC0032201 (DRAW_INDX with DMA) using dev+0x308C (+24 address, bit31 = 32-bit); prim type 6 (strip) and base 0 are hard-coded (LTCG clone; every caller passes 6, 0); splits >0xFFFE indices with 2-index overlap |
| 8373D570 | Resolve | r3 dev, r4 flags (bits 0-2 = RT index, 4 = depth; clear bits), r5 const D3DRECT* src, r6 D3DBaseTexture* dest, r7 const D3DPOINT* destPoint, r8 dest level, r9 slice/face, r10 const D3DVECTOR4* clear color, f1 clear Z, stack(92) clear stencil, stack(100) D3DRESOLVE_PARAMETERS* | high | low 3 bits of r4 pick dev+0x3090+4i or dev+0x30A0 (index 4); 42 game callers pass the Clear struct's Z/stencil (renderer+1288/1292); this is Unleashed's StretchRect |
| 8373E5B8 | internal: generic resource lock | r3 resource, r4 kind (10 = VB, 12 = IB), r5/r6 base, r7 mip, r8 phys address, r9 size, r10 flags, stack args; returns CPU address | medium | waits on Fence (+8) or ReadFence (+12) via BlockOnFence, flushes/invalidates the range, lock count += 0x100 in Common, widens the dirty range in BaseFlush (+20) / MipFlush (+24); wrapped by 8373EBE8 (VB lock) and 8373ECF8 (IB lock); also called directly by game (83627318, 837E7078, 83733428) |
| 8373E848 | internal: generic resource unlock | r3 resource, r4 base address, r5 mip address (0 = none) | medium | lock count -= 0x100 in Common; on the last unlock flushes the BaseFlush/MipFlush ranges (16:16 in 128-byte units) with 83745B20 and resets them to 0xFFFF0000; wrapped by 8373EC38 (VB) and 8373ED40 (IB); 15 callers, many game-side inline texture UnlockRects |

Tally: 21 high, 13 medium, 3 low (two of the low ones are name guesses:
Suspend/Resume).

## Device creation (83737BB8 and game 837C62E8)

- 837C62E8 fills a D3DPRESENT_PARAMETERS at 0x839D3440 (EnableAutoDepthStencil
  +0x24 = 0, DisableAutoBackBuffer +0x38 = 1, DisableAutoFrontBuffer +0x3C = 1),
  calls `Direct3D_CreateDevice(0, 1, 0, 1, &pp, &pp+128)` and keeps the device
  at renderer+92 (`lwz r3,92(rX)` before every D3D call). It then calls
  SetRingBufferParameters again on resets (837C6680, 822F2218) with pp+72.
- 83737BB8: XMemAlloc(0x5800, 0x24800000), align the result up to 128 bytes
  and store the raw pointer at dev-4, memset **0x5780 bytes = device size**
  (Unleashed's static_assert is 0x5E00, so their device is bigger). Sets
  refcount dev+0x3C = 1, qword dev+0x2A80 from 0x83BCAAC0, owner thread
  dev+0x2A88, flags dev+0x5708 = 0x0C000001, second XMemAlloc(4800) at
  dev+0x4148, then 83741F00 (engine init). Failure: Release, return
  0x8007000E.
- 83741F00 (init) calls 8373A918 (ring), 83748DC8 (worker threads),
  8373B060 (constant-reload indirect buffers), 83748F18 (default register
  shadows, all dirty), 83741B08 (video mode, gamma), **83741898 (state
  tables)**, then KickOff.

### SetRenderState / SetSamplerState tables (filled by 83741898)

Same layout as Unleashed's GuestDevice:

| Offset | Content |
| --- | --- |
| dev+0x040 | SetRenderState fn table, 0x65 entries, index = D3DRS value / 4; called as `fn(dev, value)` |
| dev+0x1D4 | SetSamplerState fn table, 0x14 entries, index = D3DSAMP value / 4; called as `fn(dev, sampler, value)` |
| dev+0x224 | GetRenderState fn table, 0x65 entries, `fn(dev)` returns the value |
| dev+0x3B8 | GetSamplerState fn table, 0x14 entries |
| dev+0x480 | 32 fetch constants x 24 bytes (texture fetch for samplers 0-25; vertex fetch slots 95-s at the top) |

The static source tables are 12-byte entries {get, set, default}:
render states at **0x83A84EE0** (101 entries), sampler states at
**0x83A853A0** (20 entries). 83741898 copies set to +0x40/+0x1D4 and get to
+0x224/+0x3B8, then calls each setter with the default (all 26 samplers).
Examples: D3DRS_ZENABLE (40, index 10): set 0x837351D0 (writes dev+0x2E5C and
the Z_ENABLE bit of RB_DEPTHCONTROL at dev+0x2934, forced 0 when there is no
depth surface), get 0x83735208, default 1. Indices 0-9 are stubs (set
0x836D87A8 = blr, get 0x837C1A60 = return 0). Sampler setters write the
fetch constant at dev+0x480+24*s and set bit (63-(32+s)) in qword dev+0x18.

**Game code calls render states through the table.** Its wrapper 837C7080
(this=renderer, r4 = D3DRS value, r5 = value; 74 callers) does
`lwz r11,64(dev+state); mtctr; bctr` with r3 = dev, r4 = value. So
Unleashed's approach (overwrite dev+0x40[...] with host thunks after
CreateDevice) works unchanged. Sampler states seem to go through dev+0x1D4
the same way (indirect loads of 468+ near mtctr in game code). Not every
call site was checked.

## Device offsets confirmed

| Offset | Meaning | Evidence |
| --- | --- | --- |
| 0x00 / 0x08 | qword dirty masks: VS / PS float constants | 8373B1F8 / 8373B2E0 OR into them; draws flush to regs 0x4000 / 0x4400 |
| 0x10 | qword dirty: register blocks + shaders/decl (bit 0x80000 = decl/VS bind, 0x800 + 0x20000 = depth control) | draws: blocks 0x2000 (dev+0x2880), 0x2100 (0x28CC), 0x2180 (0x2920), 0x2200 (0x2934) |
| 0x18 | qword dirty: fetch constants (high 32 bits, one per constant) + block 0x2280 (dev+0x2964) | samplers, SetStreamSource, 8373CB08 |
| 0x20 | qword dirty: blocks 0x2300 (dev+0x29B8), 0x2380 (dev+0x2A50), bool consts (dev+0x2780 = reg 0x4900) | 8373CB08 |
| 0x30 / 0x38 | ring write ptr / segment limit | every packet writer; 8373A3D0 when exceeded |
| 0x3C | device refcount | 83737C98/83737CB0 |
| 0x40-0x47F | state fn tables (above) | 83741898 |
| 0x480 | fetch constants 32 x 24 | sampler setters, 83737950 |
| 0x780 / 0x1780 | VS / PS float constants (256 x vec4 each) | 8373B1F8 / 8373B2E0 |
| 0x2780 | bool constants (reg 0x4900) | 8373CB08 |
| 0x2880.. | GPU register shadows by block: reg 0x2000+k at 0x2880+4k (RB_SURFACE_INFO, RB_COLOR_INFO 0x2001 = +0x2884, RB_DEPTH_INFO 0x2002 = +0x2888); RB_COLOR_MASK 0x2104 = +0x28DC; RB_DEPTHCONTROL 0x2200 = +0x2934 | SetRenderTarget, SetDepthStencilSurface, ZENABLE setter |
| 0x2A88 | owner thread | 83737D08/D48 |
| 0x2A90 / 0x2A94 | GPU fence write-back ptr / callback scratch memory | 8373A4D0, 8373A5A8 |
| 0x2A9C | current (next) fence | resources get this as Fence (+8) when unbound |
| 0x2E24 | current vertex declaration | 8373BAD8 |
| 0x2E30 (byte) | stride last bound with the VS | draws, 8373C1F0 |
| 0x2E5C | D3DRS_ZENABLE value | 837351D0 / 837374E8 |
| 0x308C | index buffer | 837370F0, 8373CB08 |
| 0x3090 + 4i | render targets 0-3 | 83737180 |
| 0x30A0 | depth-stencil surface | 837374E8, Resolve |
| 0x30A4 + 4s | stream source buffers | 83737068 |
| 0x30E8 + s (byte) | stream stride / 4 | 83737068, BeginVertices |
| 0x318C / 0x3190 | pixel shader / vertex shader | 8373B4D8 / 8373B880 |
| 0x3A50 | critical section | 83737D58/DF0, 83741F00 |
| 0x3A8C | 1536-byte hardware gamma table | 837377C0 |
| 0x5708 | creation flags (0x0C000001) | 83737BB8 |
| size | 0x5780 | memset in 83737BB8 |

## Resource offsets confirmed

- Header: +0 Common (low 4 bits type: 4 = surface, 6 = VS, 7 = PS; bits
  8-11 = lock count; bit 30 on surfaces = has parent at +24), +4 RefCount
  (atomic, AddRef 8373E970), +8 Fence, +12 ReadFence, +20 BaseFlush, +24
  MipFlush or data (16:16 range in 128-byte units, 0xFFFF0000 = clean).
- Vertex buffer: +24/+28 = GPUVERTEX_FETCH_CONSTANT dwords (address \| type,
  size \| endian).
- Index buffer: +0 bit 31 = 32-bit indices, +24 = address, +28 = size.
- Surface: +24 low 2 bits = (MSAA?) kind used by Resolve, +28 =
  RB_COLOR_INFO / RB_DEPTH_INFO value, +32 = extra depth word (to dev+0x2940).
- Pixel shader: 40-byte object, shader header copy at +40, microcode phys ptr
  at +24. Vertex shader: 872-byte object, header copy at +872.
- Vertex declaration: 56-byte header + 12 bytes per element.

## Open questions

- Who calls EndVertices? BeginVertices records dev+0x3474/0x3478/0x3480. No
  public function reads them, so it is probably inline in game code; check
  before hooking BeginVertices.
- The game writes stream pointers/strides and the vertex declaration inline,
  so a renderer has to read dev+0x30A4/0x30E8/0x2E24 at draw time rather than
  rely on hooked setters alone. Same for SetVertexShaderConstantF: the hook
  must take the dirty mask in r7.
- 8373E5B8/8373E848 are called directly by game code (inline Lock/Unlock), so
  the lock/unlock hooks must go on these helpers too, not only on the
  8373EBE8/ECF8 wrappers.
- Suspend/Resume and SetShaderGPRAllocation are not proven. The meaning of the
  r4 "kind" argument of 8373E5B8 isn't known either.
