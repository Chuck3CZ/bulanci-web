# WGSL shader ABI (translator ↔ WebGPU renderer)

Status: **2026-10-18**, authoritative. Owner: track `gpu-shaders`. Consumers: track `gpu-renderer`
(`runtime/web/gpu_web.c`, `web/src/gpu/*`).

This is the contract between the WGSL emitted on the game thread (the D3D9 shader translator, `runtime/vsemu.c` +
`runtime/web/vsemu_wgsl.inc`) and the TypeScript WebGPU encoder. It follows `DESIGN.md` §3; every deviation is listed in
§10 with the reason. The semantics of the emitted code are the Mac port's (`vs_to_msl`, `ps_to_msl_ex2`, `metal.m`
`kPrelude`/`kShaderSrc`), including its known quirks (§8), so the web build looks like the Mac build.

## 1. Modules and entry points

Every module is **self-contained WGSL text** (structs, bindings, helpers and one entry point). The renderer passes it to
`createShaderModule` unchanged; nothing is prepended. Each module has exactly **one** entry point, so `entryPoint` may be
omitted in the pipeline descriptor.

| Module | Produced by | Entry point | Used when (MDraw) |
|---|---|---|---|
| Game VS, one per (VS object, declaration) pair | `vs_to_wgsl(prog, gid, decl, …)`, from `gpuvs_id` (`com.c`) | `@vertex fn vs_<gid>` | `vsid = gid > 0` |
| Game PS, one per pixel shader object | `ps_to_wgsl(prog, id, …)` at CreatePixelShader | `@fragment fn ps_<id>` (id 1..255) | `ps = id` |
| Game PS, stage-0 cube twin | `ps_to_wgsl_ex(prog, id + 256, …, cubemask = 1)` | `@fragment fn ps_<id+256>` | `ps = id + 256` (stage-0 texture is a cube) |
| TVert VS (CPU-transformed vertices) | `wgsl_tvert_module()` | `@vertex fn vs_0` | `vsid = 0` |
| Fixed-function PS (`col0 × tex0`) | `wgsl_ff_module(0, …, 0)` / `wgsl_ff_module(256, …, 1)` | `@fragment fn ps_0` / `ps_256` | `ps = 0` (cube twin: stage-0 texture is a cube) |

- Any VS module combines with any PS module: all of them use the same `VOut` (§3) and the same bind group layouts (§2).
- The **no-discard twin** of every PS is not a separate module: it is the same module with the override constant
  `ALPHA_FUNC = 8` (§5).
- **Registration.** On wasm, `com.c` calls `metal_register_vs(gid, wgsl)` and `metal_register_ps(id, wgsl)` (and
  `id + 256`) exactly where the Mac passes MSL. The text is NUL-terminated ASCII in a static buffer that is reused by the
  next translation: `gpu_web.c` must copy it into its resource queue during the call. `com.c` never registers the id-0
  modules; `gpu_web.c` gets them once at init from `wgsl_tvert_module()` / `wgsl_ff_module()` (declared in
  `runtime/web/wgsl.h`) and queues them like any other shader (suggested ids: VS 0, PS 0 and 256).
- **`metal_vs_ready(gid)`** cannot compile synchronously on the web. Answer **1** whenever the text was queued: the
  translator only emits constructs that the CI harness (`web/src/test/shadercheck.ts`, §9) compiles with zero errors for
  every shader the game creates. If `createShaderModule`/`getCompilationInfo` or pipeline creation still reports an error
  at run time, the renderer logs it loudly (once per module, with the module id) and skips draws that use that module.
  It must not block the game thread on a WebGPU promise.
- A translation that fails returns 0 to `com.c` before anything is registered, exactly as on the Mac: an unsupported VS
  opcode or vertex declaration type makes `gpuvs_id` return -1 and the draw takes the CPU (`vs_run` → TVert) path.

## 2. Bind groups (explicit layouts, shared by every pipeline)

```
group 0  (shared by runs of draws; K and P with dynamic offsets)
  binding 0  VERTEX    buffer read-only-storage   VB : array<u32>   hasDynamicOffset false
  binding 1  VERTEX    buffer uniform             K  : VSWin        hasDynamicOffset true   minBindingSize 65536
  binding 2  FRAGMENT  buffer uniform             P  : PSPar        hasDynamicOffset true   minBindingSize 560
  binding 3  FRAGMENT  buffer uniform             H  : HLPar        hasDynamicOffset false  minBindingSize 656
group 1  (textures and samplers of stages 0..3)
  binding 0  FRAGMENT  texture  sampleType float, viewDimension "2d"   (layout CUBE: "cube")   tex0
  binding 1  FRAGMENT  texture  sampleType float, viewDimension "2d"                             tex1
  binding 2  FRAGMENT  texture  sampleType float, viewDimension "2d"                             tex2
  binding 3  FRAGMENT  texture  sampleType float, viewDimension "2d"                             tex3
  binding 4..7  FRAGMENT  sampler type "filtering"                                               smp0..smp3
```

- Two pipeline layouts: **L2D** = `[G0, G1_2d]` and **LCUBE** = `[G0, G1_cube]`. A pipeline uses LCUBE iff its fragment
  module is a cube twin (`ps ≥ 256`: `ps_<id+256>` or `ps_256`); everything else uses L2D.
- `setBindGroup(0, g0, [kwin, poff])`: the dynamic offsets are in binding order (K, then P) and must be multiples of
  `minUniformBufferOffsetAlignment` (256). K is a **64 KB window** of the frame's VS constant arena (4096 rows, the
  default `maxUniformBufferBindingSize`), so the VS constant buffer needs 64 KB of slack after the last block. A draw's
  block starts at row `kb` of the window, given as the draw's `firstInstance` (§3): consecutive draws share one bind
  group, and a new one is set only for another vertex source, other PS parameters, or a block past the window
  (`kb > 3839`, so that the block's 257 rows stay inside it). VB has no dynamic offset: the byte offset of a draw's
  vertex 0 is the block header's `base` (§4), because storage-buffer dynamic offsets must be 256-aligned and pool
  offsets are not.
- VB is the whole buffer the draw reads: the raw-vertex arena (`metal_draw_raw`), the mirror pool (`metal_draw_ix`) or
  the TVert arena (`metal_draw_tv`). A buffer bound as VB needs usage `STORAGE`; the pool also needs `INDEX`.
  `maxStorageBufferBindingSize` is 128 MiB by default; request more at `requestDevice` if the pool can grow beyond it.
- Missing or freed textures: bind a 1×1 white texture (cube: a white 1×1 cube) as `metal.m` does. Samplers follow the
  `MDraw.smp[k]` keys (`gfx-d3d9.md` §2.3) and must be `filtering` samplers.
- A module may use any subset of these bindings; WebGPU allows layout entries that a shader does not use.
- `createShaderAbi(device)` in `web/src/gpu/shader-abi.ts` builds exactly these layouts (and exports the sizes,
  `PSCUBE`, `alphaFunc()`); the shader check builds its pipelines with it, so use it in the renderer too.

## 3. Structs

```wgsl
struct VOut {                      // interstage, identical in every module (7 of 16 locations)
  @builtin(position) pos: vec4f,
  @location(0) col0: vec4f,  @location(1) col1: vec4f,          // oD0, oD1 (not clamped, as on the Mac)
  @location(2) uv0: vec4f,   @location(3) uv1: vec4f,           // stage k reads uvk (TEXCOORDINDEX applied in the VS)
  @location(4) uv2: vec4f,   @location(5) uv3: vec4f,
  @location(6) fog: f32,                                        // vertex fog factor, 1 = no fog
}
struct VSWin { c: array<vec4f, 4096> }   // 65536 bytes, uniform: the window
// a draw's block at row kb (= instance_index, the draw's firstInstance), 16-byte aligned in the arena:
//   row kb      the header, read as bitcast<vec4u>: x = stride (vertex stride in bytes), y = tsel (byte k =
//               TEXCOORDINDEX of stage k, 0..3), z = base (byte offset of this draw's vertex 0 in VB), w = 0
//   row kb+1+n  vertex shader constant cn, n = 0..255
struct PSPar {                     // 560 bytes, uniform (= the Mac's PSPar)
  aref: f32,                       //  0  MDraw.alpha_ref
  usec: f32, pad0: f32, pad1: f32, //  4  0 (HD fields on the Mac)
  proj: vec4f,                     // 16  MDraw.proj: stage k = coordIndex + 16 * projMode (as f32)
  c: array<vec4f, 32>,             // 32  pixel shader constants c0..c31 (MDraw.psci block)
  fog: vec4f,                      // 544 MDraw.fogv: FOGCOLOR rgb, a = FOGENABLE (0 or 1)
}
struct HLight { t: mat4x4f, col: vec4f }   // 80 bytes
struct HLPar {                     // 656 bytes, uniform (gpu_proto.h GwHL)
  n: u32,                          //  0  other cars' headlights this frame (0..8)
  s: f32, o: f32, pad: f32,        //  4  falloff: ramp u = s * w + o (the lit VS's own constants)
  l: array<HLight, 8>,             // 16  car k: its light coordinate is uv1 * t; col = its HEADLIGHTCOLOR
}
```

- **K header** is what `raw_rec` (`metal.m`) writes, plus `base`: `stride` = the draw's stride, `tsel` = `Σ ((int)proj[k] & 3) << 8k`,
  `base` = `rawoff` (raw arena offset for `metal_draw_raw`, the VB mirror's pool offset for `metal_draw_ix`). Only rows
  `c[0 .. vs_kmax)` need to be written; the shader reads no other row except through relative addressing, which is
  clamped to `[0, 255]` exactly like the Mac (rows past `vs_kmax` read whatever the arena holds there, as before).
  Constant blocks can stay deduplicated (the header is part of the key). The window offset is the uniform's dynamic
  offset, the row the draw's `firstInstance`: `instance_index` includes `firstInstance` on every backend (WGSL), and
  the draws are not instanced.
- **TVert draws** (`vsid = 0`): any K window; draw with `firstVertex = MDraw.first`. `vs_0` reads no K at all:
  `VB[vertex_index * 112]` from the start of the TVert arena (a TVert is always 112 bytes: pos, col0, col1, t[4], all
  `f32`, `gfx.h`).
- **P** is the Mac's per-draw `PSPar` (`metal.m` encoder): the renderer fills it from the MDraw record and the PS constant
  pool, 256-aligned (a 768-byte slot stride is the simplest).
- **H** is the frame's, not the draw's: bytes `[GW_K_HL, + 656)` of the frame's K arena (`gpu_web.c` keeps the draws'
  blocks after them), bound at that fixed offset of the K buffer. Only the worldheadlight pixel shaders declare it: the
  translator adds `hl_more()` (the other cars' headlights, `runtime/headlights.c`) after their `t1 * t2 * c0 + v0`
  (`vsemu_wgsl.inc` `W_HL`); a web-only addition, the Mac has no such term.

## 4. Vertex pulling (game VS modules)

- Vertex address: `a = base + vertex_index * stride` (the block header, §3; u32 arithmetic). `vertex_index` includes `baseVertex` of
  `drawIndexed`, so D3D9 `BaseVertexIndex` is `drawIndexed(count, 1, firstIndex, ix_base)`. `firstVertex` of `draw()`
  also adds to `vertex_index`.
- Every input register `vN` that the declaration maps is decoded at `a + offset` with the formula below, baked into the
  module (offset and type come from the declaration, as `decl_read_msl` does). Loads are little-endian byte loads from
  `array<u32>` and work at **any byte alignment** (a u32 load at a misaligned address combines two words).
- Unmapped input registers read `(0, 0, 0, 1)`.
- The normalising divisions are rounded like C's (reciprocal product plus one FMA residual step), so the decoded values
  are **bit-exact** with `decl_fetch`, the CPU path. A plain WGSL division is not correctly rounded on the GPU (measured:
  126 of the 256 byte values of b/255 off by one ulp); the Mac's MSL path has the same one-ulp drift under fast math.

| D3DDECLTYPE | Decoded `vec4f` (bytes b[i] at `a + offset + i`) | = `com.c decl_fetch` |
|---|---|---|
| 0 FLOAT1 | (f32 b0..3, 0, 0, 1) | ✓ |
| 1 FLOAT2 | (f32, f32, 0, 1) | ✓ |
| 2 FLOAT3 | (f32, f32, f32, 1) | ✓ |
| 3 FLOAT4 | (f32, f32, f32, f32) | ✓ |
| 4 D3DCOLOR | (b2, b1, b0, b3) / 255 | ✓ (memory is B,G,R,A) |
| 5 UBYTE4 | (b0, b1, b2, b3), not normalised | ✓ |
| 8 UBYTE4N | (b0, b1, b2, b3) / 255 | ✓ |
| 6 SHORT2 | (s16, s16, 0, 1) | ✓ |
| 7 SHORT4 | (s16, s16, s16, s16) | ✓ |
| 9 SHORT2N | (s16 / 32767, s16 / 32767, 0, 1) | ✓ (−32768 → −1.00003, unclamped) |
| 10 SHORT4N | s16 / 32767 ×4 | ✓ |
| 11 USHORT2N | (u16 / 65535, u16 / 65535, 0, 1) | ✓ |
| 12 USHORT4N | u16 / 65535 ×4 | ✓ |
| 13 UDEC3, 14 DEC3N, 15 FLOAT16_2, 16 FLOAT16_4, ≥ 17 | **rejected**: no module, `gpuvs_id` → -1, CPU path | as the Mac |

## 5. Override constants (every PS module declares exactly these)

| Name | Type, default | Meaning |
|---|---|---|
| `ALPHA_FUNC` | `u32`, `7` | Alpha test of the final colour against `P.aref`, D3DCMP numbering: 1 NEVER, 2 LESS, 3 EQUAL, 4 LESSEQUAL, 5 GREATER, 6 NOTEQUAL, 7 GREATEREQUAL, **8 ALWAYS = no test, no `discard` in the pipeline** |
| `PS20_TEXCOORD` | `bool`, `true` | ps_2_0 only: `true` = D3D behaviour (`t#` = `uv#`), the default; `false` = the Mac's look (the `t#` coordinate registers read 0, §8), for an A/B |

- **Mac parity:** `ALPHA_FUNC = 7` when `MDraw.alpha_ref > 0`, else `8`. The second is the Mac's no-discard twin
  (`g_psfn_nd`), chosen for the same reason: `discard` disables hidden-surface removal on Apple GPUs. The comparison for
  7 is written `!(a < aref)` so NaN behaves like the Mac's `if (a < aref) discard`. The other values give full D3D9
  ALPHAFUNC support if the recorder ever passes the real function (`com.c` today folds 5, 6, 7 into "≥ aref").
- Because unknown keys are a pipeline-creation error in WebGPU and declared-but-unused ones are not (both checked in
  Chrome 155), the renderer can pass the same `constants` record to every PS: `{ ALPHA_FUNC: n }` (and
  `PS20_TEXCOORD` when the A/B is wired). VS modules declare **no** override constants.
- Pipeline key: (VS module, PS module, ALPHA_FUNC == 8 ? 1 : 0, …render state). The cube twin is a different PS module.

## 6. Pixel shader semantics (summary; `vsemu.c` is the reference)

- Inputs: `v0 = col0`, `v1 = col1`. Constants: `def` → literal (emitted as a function-scope `let`, so constant folding can
  never hit a WGSL const-expression error), else `P.c[n & 31]`.
- ps_1_1–1_3 `tex tN`: `textureSample(texN, smpN, uvN.xy / d)`, `d` = 1, `uvN.z` or `uvN.w` for `P.proj[N] >> 4` = 0, 1,
  2. ps_1_4 `texld rN, tM`: sampler N, coordinate M with M's divide. ps_1_4 dependent `texld rN, rM`: `rM.xy` (no
  divide). ps_2_0 `texld d, tK, sN`: `tK.xy`. Cube stages sample `uvM.xyz` (no divide) or `src.xyz`.
- Result: `r0` (ps_1_x) or `oC0` (ps_2_0) → alpha test (§5) → fog
  `rgb = mix(P.fog.rgb, rgb, mix(1, saturate(fog), P.fog.a))` → `@location(0)`.
- `texkill` (no NFSU2 shader has one): its register is the instruction's destination token. ps_1_x: discard if any of
  the first three components is negative, of texture coordinate set N (`texkill tN` → `uvN.xyz`) or of a temp
  (ps_1_4 `texkill rN`); ps_2_0: all four components of the register (as Wine). This is D3D9's rule, not the Mac's:
  `ps_to_msl_ex2` reads the instruction's unset source operand (stale text from the previous instruction), a bug not
  carried over. The reference interpreter `ps_ref_run` does the same.
- `textureSample` is only ever called in uniform control flow (straight-line code; the projective divide uses `select`).
- Fixed-function PS (`ps_0`/`ps_256`): `col0 * textureSample(tex0, smp0, uv0.xy)` (cube: `uv0.xyz`), then the same alpha
  test and fog. The cube twin has no Mac equivalent (the Mac would bind a cube to a 2D slot); it exists so a cube at stage
  0 with no pixel shader cannot invalidate a bind group.

## 7. Vertex shader semantics (summary)

- Registers `r0..r31`, `a0` (`vec4i`), outputs `oPos` = (0,0,0,1), `oD0` = (1,1,1,1), `oD1` = 0, `oT0..oT3` = 0,
  `oFog` = (1,1,1,1) (no fog), all written as on the Mac (`oT4..7` alias `oT0..3` through `& 3`, as `vs_dst_msl` does).
- Epilogue: `pos = oPos` (no half-pixel offset), `col0/col1 = oD0/oD1` (no clamp), `uvK = oT[(tsel >> 8K) & 3]` (the header's `tsel`),
  `fog = oFog.x`.
- `vs_0` (TVert): pass-through of the 112-byte record, `fog = 1` (the Mac's `vmain`).

## 8. Mac-parity quirks kept on purpose (do not "fix" in the renderer)

| Quirk | Mac source | Note |
|---|---|---|
| No half-pixel offset; oD0/oD1 not clamped | `vs_to_msl` epilogue | |
| `mov a0.x, r` truncates toward zero; `mova` rounds half away from zero | MSL implicit conversion / `round()` | WGSL `round` is half-to-even, so the emitter writes the Mac's rounding explicitly |
| ps_1_x unsupported texture ops (`texreg2ar/gb`, `texbem`, `texm3x*`, `bem`) and `phase` are skipped | `default: continue` | Only the SCREENFILTER fallback technique has them (two ps_1_1 shaders the game creates but never selects, because the device reports PS 2.0) |
| VS `logp` = `log` (one component); PS `logp` is the 4-component form | `vs_to_msl` / `ps_to_msl_ex2` | |
| `m4x4`-family rows always read `K.c[n + row]` (ignore `def` and relative addressing) | `vs_to_msl` | NFSU2 does not use them |
| ps_1_4 `texcrd rN, tM` writes `t[N] = uvN`; co-issued pairs run sequentially | `ps_to_msl_ex2` | NFSU2 has no `texcrd`; its 15 co-issued instructions are safe sequentially (`gfx-d3d9.md` §3.5) |
| Alpha test is "≥ aref" only (from `com.c`) | `com.c:719` | §5 allows the full set later |

**Not kept: the ps_2_0 `t#` registers.** The Mac never loads them (`vsemu.c` ps_to_msl_ex2): they read 0, so depth of
field (`ps_30`) and rain drops (`ps_31`) sampled their textures at (0,0). Depth of field runs in some of the camera
scenes before and after a race. There every pixel took the top-left texel, the black letterbox bar, which the tint
passes after it turned into a flat colour, and only the light glows (added later) showed. The game's random choices
decide whether a race intro is one of those: 5 of 18 quick-race runs (2026-10-20). The rain drops on the lens did
not show at all. Since then `PS20_TEXCOORD` defaults to `true` (D3D: `t#` = `uv#`), `false` gives the Mac's look,
and `ps_ref_run` loads them too. `uvK` has TEXCOORDINDEX applied in the vertex epilogue; for both shaders' draws that
is the identity (`tsel` 0x03020100), as D3D requires with a vertex shader.

Deliberate differences from the Mac's **text** (not its semantics), both toward the Mac's CPU reference path (`vs_run`,
`decl_fetch`): `def` constants are printed with 9 significant digits (`%.9g`, exact for f32) instead of `%g` (6 digits),
and the vertex-pulling divisions are correctly rounded (§4). Both differences are below 1e-6 relative.

## 9. Validation

Run it all with one command (the server on port 8612, `web/dist` built, the wasm game built):

```
node web/tools/shader-check.mjs --game        # dump + pairs + check; exit 0 = all good
node web/tools/shader-check.mjs --from <dir>  # re-check a dump on disk (--census: also the game-run floor)
```

Exit 0 means all of: every check below passes; no `REJECTED` line in the dump's `index.txt` or in `pairs.txt`; and for
`--game`, the game ran the whole quick race (`EXIT 0`, the race reached, `quit at guest frame`, no fault marker) and
the dump has at least a full quick race's census: 16 distinct VS objects, 24 distinct PS objects, 11 distinct
declarations, 17 registered (VS, declaration) modules, 68 PS modules (with the cube twins). A run that crashes or stops
early gives a smaller dump and fails (`--script` and `--env` run another scene for a dump).

1. **Dump.** `NFSU2_SHADER_DUMP=<dir>` (wasm build, `runtime/web/shader_dump.c`) makes the game write every shader
   object it creates (`vsobj_<n>.bin`/`psobj_<id>.bin` + disassembly), every vertex declaration (`decl_<obj>.bin`),
   every module it registers (`vs_<gid>.wgsl`, `ps_<id>.wgsl`, `ps_<id+256>.wgsl`), the built-ins and
   `pulltest.wgsl`, with `index.txt`. `--game` runs `tests/scripts/quickrace.txt` headless (null renderer, DESIGN.md §5
   environment) and copies the dump from OPFS to `out/web/shaders/` (game-derived: never committed).
2. **Pairs.** A quick race draws only 10 of the 16 distinct vertex shaders, so `web/tools/wgsl-xl --pairs` (the same C
   translator built natively) translates every vertex shader object against every dumped declaration that supplies all
   of its inputs (ids from 1001).
3. **Check** (`/test/shadercheck.html`, `web/src/test/shadercheck.ts`):
   - every module: `createShaderModule` + `getCompilationInfo` (+ a validation error scope): **0 errors**;
   - render pipelines with the explicit layouts of §2 (`web/src/gpu/shader-abi.ts`): each game VS with `ps_0`; `vs_0`
     with every PS and cube twin at `ALPHA_FUNC` 7, 8 and with `PS20_TEXCOORD` off; `&cross` adds every VS × every PS;
   - vertex pulling: `pulltest.wgsl` decodes all 13 declaration types at byte offsets 0..7 from 8 bases and must equal
     the CPU decoder (`decl_fetch` semantics) **bit for bit**;
   - vertex shaders: each WGSL vertex function runs as a compute function on random vertices and constants and is
     compared with `vs_run` (the Mac's CPU interpreter) on the same bytes;
   - pixel shaders: each fragment function runs as a compute function (analytic texture, `discard` as a flag) and is
     compared with `ps_ref_run`, a reference interpreter of the Mac's pixel shader semantics (but the ps_2_0 `t#`
     registers, §8). Colours within 1e-3
     relative; kill flags equal, except an alpha-test decision that flips because both alphas are within that
     tolerance of `aref` (counted as `edgeFlips`; SwiftShader/Vulkan has one, Metal none).

Results on Chrome 155 / Apple M4 Pro (2026-10-18): 167 modules, 0 compile errors, 307 pipelines, 0 errors; vertex
pulling 3,328/3,328 exact; 96 vertex shader modules max relative error 2.0e-6; 68 pixel shader modules max relative
error 1.1e-6, all kill flags equal.

**Note for the page:** the game only takes the GPU vertex shader path when `GPUVS=1` is in its environment (the Mac's
`bundle.m` and `run.sh` set it). Without it every vertex shader runs in `vs_run` on the CPU (in wasm) and draws go
through `vs_0`.

## 10. Deviations from DESIGN.md §3

| DESIGN.md §3 | Here | Why |
|---|---|---|
| `K = {stride, tsel, base, pad; c[NK]}` | K is a 64 KB window, `array<vec4f, 4096>`; the draw's block `{stride, tsel, base, pad; c[256]}` starts at row `firstInstance` | One bind group for many draws: a per-draw dynamic offset cost ~1,300 `setBindGroup` calls a race frame (page thread and GPU process, ~0.45 ms of CPU per frame on a Ryzen 5900X); the layout still fixes `minBindingSize` (no per-draw size validation), relative addressing never indexes past the window, and the recorder uploads only `vs_kmax` rows. The price: the constant reads use a per-vertex index on the GPU (+3.7 % GPU time per frame, 0.89 → 0.92 ms at 4K) |
| group 0 "dynamic offsets" | K and P dynamic, VB not | WebGPU storage dynamic offsets must be 256-aligned; mirror-pool vertex offsets are arbitrary, so the offset is the header's `base` |
| Alpha test as an override "no-discard variant" | `ALPHA_FUNC: u32` (D3DCMP, 8 = no discard) | Same twin, plus room for the full ALPHAFUNC set |
| `t0..t3`, `s0..s3` | `tex0..tex3`, `smp0..smp3` | Names only (the D3D register names `t#`, `r#` are used for registers); bindings are as in DESIGN |
| (not in DESIGN) | `PS20_TEXCOORD` override, FF PS cube twin, TVert `firstVertex` convention | Documented above |
