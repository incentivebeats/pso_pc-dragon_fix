# PSO PC V2 Dragon Corruption Bug — Technical Reference
Disclaimer: Work done here was done together with LLMs. The below report is an analysis provided by Opus 4.7.

If anyone with experience wants to poke around or see if anything discovered below is helpful to finding a solution before me, please feel free.

**Status:** Active investigation. Not patch-server ready. ~40 hours of investigation invested.
**Target:** Phantasy Star Online PC V2 client (NOT Blue Burst, NOT GameCube/Xbox V3, NOT Dreamcast V2).
**Last updated:** Post Blue Burst cross-version verification (added Section 2.5).
**Authors of investigation:** Bruce (DevOps engineer, primary), Claude Sonnet 4.6 (implementation), GPT (review), Claude Opus 4.7 (architecture review).

> **Note for the impatient reader (added in this revision):** The PSOBB client was compared against V2's binary at the byte level. The dragon-relevant bone matrix code is **identical** in both binaries — same fallback, same guard logic, same constants. Sonic Team shipped this code twice without changing it. The bugs documented in this report are real and unfixed upstream. See **Section 2.5** for details and what it means for this investigation. The V2 community's lack of progress on this bug for 20 years is now answerable: there was no upstream fix to backport.

To apply patches:
Build d3d8:
```
i686-w64-mingw32-gcc -shared -O2 -o D3D8.dll d3d8_widescreen.c d3d8.def -lkernel32 -Wl,--enable-stdcall-fixup
```

Apply current patches:
```
python3 apply_dragon_fix.py pso.exe           # apply
python3 apply_dragon_fix.py pso.exe --verify  # check state
python3 apply_dragon_fix.py pso.exe --revert  # restore backup
```
---

## 1. TL;DR

A 20-year-old visual corruption bug specific to the PC V2 release of PSO. Dragon (Forest 2 boss) renders with severe texture/geometry corruption in three distinct ways. After ~200 test iterations and three binary patches, the catastrophic forms are largely contained but residual artifacts persist during clear-screen / post-death.

**Bug decomposes into three families:**
- **Bug A — XYZRHW Particle Near-Z Explosion.** Largely contained.
- **Bug B — Matrix B Degenerate Bone Transform.** Partially mitigated; verification pending.
- **Bug C — Clear-Screen Body Bleed.** Unresolved.

**Three binary patches applied to pso.exe** at addresses `0x0061EA20`, `0x0061EFD0`, `0x00634A4A`. Patches verified by cmp(1) diff against `pso_exe.original`.

**Cross-version verification (Section 2.5):** Static binary comparison against PSOBB confirms the bone matrix bug is present and unfixed in both releases. The investigation is on the correct code path.

**Remaining real fix work** lives in pso.exe, not the proxy. The D3D8 proxy is a diagnostic harness + safety net.

---

## 2. User-visible symptoms

The bug can be triggered manually mid-fight or it manifests during dragon's death sequence.

**Pre-mitigation (original behaviour):**
- Dragon turns "pink" or visually corrupt
- Textures / particles / geometry bleed across the entire map
- Loot boxes near dragon contaminate visually
- Post-death / clear-screen phase strobes / flashes violently
- Objects intersecting dragon body show texture bleed

**Post-mitigation (current state, with all three patches + proxy):**
- Catastrophic strobing largely gone
- Bruce can still trigger the bug manually mid-fight by rubbing the player against dragon body and adjusting camera
- Clear-screen / post-death texture bleed still occurs
- Dragon dies "pink" but less chaotic
- Texture bleed still appears when dragon body intersects player or loot boxes during clear-screen
- **Critical observation:** *"When a part of the dragon's body goes out of frame the bug stops."* This is a load-bearing clue — see Section 7.

---

## 2.5 Cross-version verification (Blue Burst comparison)

PSOBB (Phantasy Star Online Blue Burst) was built on top of V2 — not a rewrite, an evolution. Episode 1 content, including the Forest 2 dragon, was carried forward. A static comparison between V2 and BB binaries provides important validation, with mixed implications.

### What's identical between V2 and BB

The bone-matrix degenerate-case code from Bug B is structurally unchanged across releases:

| Item | V2 location | BB location | Status |
|------|-------------|-------------|--------|
| FLT_MAX fallback (16 writes of `0x7F7FC99E`) | `0x0061EFD2` onwards | `0x0083CCDD` onwards | identical structure (16 consecutive writes, same constant, same offsets `0x00–0x3C`) |
| Guard `fcom; fnstsw; sahf; je →fallback` | `0x0061EA1D` | `0x0083C698` | identical opcode sequence |
| Epsilon constant for the comparison | `0.0f` at `0x007C1674` | `0.0f` at `0x0098AF70` | same value |
| Divisor constant for `1/x` | `1.0f` at `0x007C1670` | `1.0f` at `0x0098AF68` | same value |
| Null-input early-out | `test esi,esi; je null_handler` | `test esi,esi; je null_handler` | identical |
| Adjacent fallback constant `0x7F7FC99E` in `.data` | one occurrence | one occurrence | identical |

Same literal-zero epsilon. Same number of writes. Same FLT_MAX-pattern fallback. Same null-input handling. Sonic Team shipped this code twice without modifying it.

### What's different between V2 and BB

The per-mesh culling system was rewritten:

- V2's per-mesh cone-setup pattern `push 0x3e99999a (0.3); push 0x3f75c28f (0.96); call <cone_setup>` at `0x00441EDE` **does not exist anywhere in BB.** Byte-pattern search returns zero hits.
- The V2 cone-setup function at `0x00619100` (signature: `fld [esp+4]; fchs; fstp ds:GLOBAL`) **has no signature equivalent in BB.** Search returns zero hits.
- BB has a graduated table of float values at `0x0090332C–0x00903364` containing `0.30, 0.35, 0.42, 0.45, 0.50, 0.60` — values matching what we have empirically calibrated for various aspect ratios. Closer inspection shows this table is read by `fdiv` instructions (integer-conversion arithmetic), not cone tests, so the value match is likely coincidental rather than purposive.

The culling rewrite isn't directly dragon-related, but it does prove Sonic Team touched the rendering pipeline between releases. They had the opportunity to fix the bone matrix bug and elected not to.

### What this means for this investigation

1. **Validation.** The bone matrix patches at `0x0061EA20` and `0x0061EFD0` address a real, unfixed bug. There is no Sonic-Team-blessed correct version of this code we missed. The investigation is on the right code path.
2. **No free reference fix.** Porting BB's solution to V2 isn't an option because BB doesn't have a solution.
3. **The bug exists in BB too** — the same code is there, with the same constants, on the same control flow path. But BB doesn't visibly show dragon corruption in normal play. Three plausible explanations, each testable:

   - **Different inputs.** BB's gameplay timing, model loading order, or update sequence may never drive the bone matrix into the degenerate state. If true, the bug's root cause is upstream of the bone matrix function — in whatever feeds it — not in the function itself.
   - **Downstream NaN/infinity guards.** BB's rendering pipeline may have added finite-value gates between bone matrix output and the actual draw call, filtering corrupt matrices before they affect pixels. If true, those guards could potentially be ported back to V2 as a complementary fix.
   - **Different model data.** PC V2 was a port of DC V2 with various tweaks; BB had further model updates. If BB's dragon has a different skeleton or different bone weights, the degenerate state may simply not be reachable. If true, the actual fix is an asset swap, not a code change.

### Cross-version patch addresses

For anyone interested in applying equivalent patches to BB (e.g., to verify the bug *can* be triggered there), the analogous addresses are:

| V2 patch | V2 address | BB equivalent address |
|----------|-----------|----------------------|
| Matrix guard `je → jbe` | `0x0061EA26` | `0x0083C6A1` |
| FLT_MAX fallback (zeroscale replacement) | `0x0061EFD0`+ | `0x0083CCDD`+ |
| Perspective divide near-Z fallback (Bug A) | `0x00634A4A` | not yet located in BB |

The first two are direct functional analogs and would receive the same patch logic. The third needs a separate hunt in BB if anyone pursues this.

### Why the V2 community hasn't progressed on this in 20 years

Now answerable: **there was no upstream fix to backport.** Sega left the bug in V2, the bug stayed in BB, and there was no canonical "correct version" anyone could reference. Investigators looking at later versions for hints would have found the same broken code and concluded the bug must be elsewhere.

---

## 3. Bug Decomposition

### 3.1 Bug A — XYZRHW Particle Near-Z Explosion

**Mechanism:** PSO's software particle/billboard projector produces XYZRHW vertices. When view-space Z approaches zero (camera close to particle emitter), the perspective divide produces astronomically large screen-space coordinates. The downstream rasterizer then attempts to draw screen-spanning quads.

**Vertex signature observed:**
- `FVF = 0x0144` (XYZRHW | DIFFUSE | TEX1)
- `stride = 28` bytes
- `vc = 4` (quad)
- `prim = 5` (triangle strip) → `count = 2`
- `z = 0`, `rhw ≈ 1.0`
- Alpha blend on, `srcblend = 5` (SRCALPHA), `destblend = 2` (one minus, written as 2 here — verify D3D8 enum)
- `zwriteenable = 0`, `lighting = 0`
- Caller around `0x006405E0`

**Observed bad coordinate samples** (PSO normal screen space is x:0–640, y:0–480):
```
x = 5037
x = -4397
x = 13597
x = -27700
x = -101847    (older run, pre-mitigation)
```

**Source cause in pso.exe:** Two functions return `65535.0f` as a fallback when view-space Z is too small:
- `0x00634A30` — patched (see Section 4.3)
- `0x00638360` — NOT patched. Shared with screen-fade rendering. Patching globally broke fades and Forest 2 environmental lighting. Return-address discriminator alone (around `0x006382FE` / `0x00638303`) cannot distinguish fades from particles.

**Mitigation in proxy:** `hook_VBUnlock` scans all XYZRHW vertex buffers and collapses outlier quads to zero area, alpha = 0. Two tiers:
- `[SUPPRESS]`: hard collapse when `|x|` or `|y|` > 5000.
- `[SUPPRESS-NEAR]`: collapse when bbox extends beyond `x<-1024`, `x>1664`, `y<-1024`, `y>1504`, or width/height > 1500. Originally log-only (`[NEAR_OUTLIER]`); promoted to suppression in v15.

**Status:** Catastrophic strobing eliminated. Sub-threshold cases still possible.

---

### 3.2 Bug B — Matrix B Degenerate Bone Transform

**Mechanism:** Each frame, the dragon's primary bone matrix function (called from `0x00617455` in pso.exe, returning to `0x0061745A`) fires *twice* on the same matrix pointer (`0x01301910` in the observed session). Two genuinely different matrices result:

```
Matrix A (1st call): rotation+translation, valid.
  Last observed: [1,0,0,0 | 0,0,1,0 | 0,-1,0,0 | 67,2000,9,1]

Matrix B (2nd call): rotation rows visually zero, separate translation.
  Last observed: [0,0,0,0 | 0,0,0,0 | 0,0,0,0 | 46,25,49,1]
```

**Caveat:** Pre-v16-lite-r5, the matrix dump used `(int)mat[i]` integer truncation. Float values in `(-1.0, 1.0)` truncated to `0` for display. So "Matrix B looks all-zero" may mean either (a) genuinely zero rotation, or (b) sub-1.0 fractional rotation that *displays* as zero. **The v16-lite-r5 dump uses `(int)(mat[i]*10000)` — next run resolves this ambiguity.**

**Original pre-patch behaviour:** When the matrix-inverse guard inside `0x0061EA00` failed, the fallback wrote `0x7F7FC99E` (≈ `FLT_MAX`, ~3.39 × 10³⁸) into multiple matrix slots. Vertices transformed by such a matrix project to infinity.

**Patches applied (see Section 4):**
- `0x0061EA20`: widened the guard trigger (`je → jbe`) and tweaked an epsilon constant.
- `0x0061EFD0`: NOPed out the writes to diagonal slots `[0][0]`, `[1][1]`, `[2][2]`; replaced FLT_MAX writes elsewhere with zero. This is the "zeroscale" fallback — preserves whatever was at the diagonal slots before, zeroes everything else.

**Cross-version note:** This entire mechanism is identical in PSOBB (Section 2.5). Same guard, same fallback, same constants. Sonic Team did not address this bug between releases.

**Caveat with the zeroscale patch:** It assumes the caller pre-initialized the diagonal slots with sane values (typically `1.0f` for identity). If the buffer is uninitialized heap or stale, the diagonals contain garbage. The `[BONE-DIAG]` line in v16-lite-r5 samples the live matrix every 60 frames specifically to verify this — last observed values were `[894/1000, -993/1000, -889/1000]`, i.e., `0.894, -0.993, -0.889`. Plausible cosines. Patch appears to be working.

**Status:** Worst behaviour gone. Final verification awaiting next-run x10000 matrix dump.

---

### 3.3 Bug C — Clear-Screen Body Bleed

**Mechanism (current hypothesis, ~40% confidence):** During the post-death clear-screen overlay phase, the opaque dragon body mesh (caller `0x00639F2A`, FVF `0x0142`, `zwriteenable = 1`, `alphablendenable = 0`, `cullmode = 1`) continues to render and depth-write while overlapping player / loot boxes. The interaction with the clear-screen overlay produces visible texture bleed.

**Competing hypotheses:**
- **Z-buffer pollution** from sub-threshold XYZRHW quads or another path leaving bad depth data, causing later opaque draws to misrender.
- **Render-state leakage** — pso.exe sets a state for dragon death rendering and never restores it.
- **The "pink" is intentional** death-dissolve material that *looks* corrupt because depth state from earlier in the frame is already broken.

**Discriminator test:** Force a `D3DCLEAR_ZBUFFER` immediately before the suspected body draw. If bleed disappears, Z-pollution is the cause. If not, eliminate that hypothesis.

**Status:** Unresolved. Awaiting clean DWIN-SIG capture (with `last_frame` field, added in v16-lite-r5) to identify which draw signatures fire *only* during the visible bleed window.

---

## 4. Binary Patches Applied to pso.exe

All three regions verified via byte-level diff against `pso_exe.original`. Total 88 byte differences across three regions in `.text` section.

### 4.1 Patch 1 — Matrix Guard Widening
**Address:** `0x0061EA20` (file offset `0x0021EA20`)
**Bytes changed:** 4 of 16 in window
**Purpose:** Widen guard trigger and tweak epsilon comparison.

| Offset (VA) | Original | Patched | Meaning |
|-------------|----------|---------|---------|
| 0x0061EA20 | `16` | `D4` | Low byte of 16-bit immediate in prior instruction |
| 0x0061EA21 | `7C` | `64` | High byte (`0x7C16` → `0x64D4`, ~80% of original threshold) |
| 0x0061EA27 | `84` | `86` | `0F 84 → 0F 86` (`JE rel32` → `JBE rel32`) |

The `je → jbe` change widens the conditional jump that leads to the fallback path, so more degenerate inputs route through the safe fallback instead of the original FLT_MAX-write path.

**BB equivalent:** `0x0083C6A1` (the `je 0x83CCDB` to FLT_MAX fallback). Same patch logic would apply.

### 4.2 Patch 2 — Zeroscale Fallback (the big one)
**Address:** `0x0061EFD0` onwards (file offset `0x0021EFD0`)
**Bytes changed:** ~80 in window
**Purpose:** Replace FLT_MAX-fill matrix fallback with zeroscale (preserve diagonals, zero everything else).

**Original behaviour:** A series of `MOV [esi+offset], 0x7F7FC99E` instructions filled the matrix at offsets `0x00, 0x04, 0x08, 0x0C, 0x10, 0x14, 0x18, 0x1C, 0x20, 0x24, 0x28, ...` with values close to FLT_MAX.

**Patched behaviour:**
- Writes at offsets `0x00`, `0x14`, `0x28` are NOPed out (these are the diagonal slots `[0][0]`, `[1][1]`, `[2][2]` of a row-major 4×4 matrix).
- All other writes changed to store `0x00000000` instead of `0x7F7FC99E`.

**Risk:** Assumes caller pre-initialized diagonals. Unverified across all callers but `[BONE-DIAG]` data so far suggests it holds.

**BB equivalent:** `0x0083CCDD` onwards. Identical 16-write structure, identical offsets `0x00–0x3C`, identical `0x7F7FC99E` constant. Same patch logic applies byte-for-byte at the BB address.

### 4.3 Patch 3 — Perspective Divide Near-Z Fallback (clean win)
**Address:** `0x00634A4A` (file offset `0x00234A4A`)
**Bytes changed:** 4 in window
**Purpose:** Change `65535.0f` fallback to `0.0f` when view-space Z is near zero.

| Offset (VA) | Original | Patched |
|-------------|----------|---------|
| 0x00634A4A | `00 FF 7F 47` | `00 00 00 00` |

`0x477FFF00` (= 65535.0f as IEEE 754) → `0x00000000` (= 0.0f).

This is the cleanest, most surgical patch. Single immediate change. Sole side effect: outlier particles fall to z=0 / origin instead of producing screen-spanning quads. No fades broken.

**BB equivalent:** Not yet located. The function may have been restructured along with the per-mesh culling rewrite (Section 2.5). A separate hunt in BB would be required.

---

## 5. Key Code Addresses in pso.exe (PE image base 0x00400000)

### Sections
- `.text`: `0x0040E000`, vsize `0x23ED70`
- `.rdata`: `0x0064D000`, vsize `0x2AAB6`
- `.data`: `0x00678000`, vsize `0x148560`
- `.data1`: `0x007C1000`, vsize `0x2420`

### Functions (suspected/identified)
| Address | Description | Status |
|---------|-------------|--------|
| `0x0061EA00` | Matrix-inverse guard / bone fn | Patched at +0x20 and +0x1D0. **Identical in BB at `0x0083C680`.** |
| `0x00634A30` | Perspective divide near-Z fallback (particles) | Patched at +0x1A |
| `0x00638360` | Second perspective-divide function (shared with fades) | NOT patched. Shared call site at `0x006382FE`/`0x00638303` |
| `0x00639F2A` | Suspected dragon body draw caller (Bug C hypothesis) | Suspect, not confirmed |
| `0x006405E0` | Suspected XYZRHW particle draw caller (Bug A) | Suspect |
| `0x00617455` | Bone fn call site (returns to `0x0061745A`) | Hooked. Fires for ALL boned entities, not dragon-exclusive |
| `0x00617535` | Bone fn call site (returns to `0x0061753A`) | Hooked. Lower frequency |

### Other bone fn call sites (also hooked)
`0x004D9043`, `0x004D90A1`, `0x004E0AD5`, `0x004F0C91`, `0x00574C37`. Low-frequency in dragon scenarios.

**Note for cross-version work:** Comparing V2's bone-fn call sites against BB's would reveal whether Sonic Team changed *who calls* the bone matrix function, even though the function itself is unchanged. A different caller layout in BB could be relevant to the "different inputs" hypothesis from Section 2.5.

### Data / fixed addresses
| Address | Purpose | Notes |
|---------|---------|-------|
| `0x007BB7E0` | `BONE_GLOBAL_PTR` fallback | Used when bone hook receives `self == NULL` |
| `0x01301910` | Dragon entity matrix ptr (one observed session) | **NOT stable across game launches.** Heap allocation; address depends on session state. Currently hard-coded in bone_hook.h dragon-active filter — must be made adaptive. |
| `0x0064FA70` | Frustum constant | Patched at runtime by proxy from `1.0f` to `1.4f` for 14:9 widescreen |

### Widescreen-related patches (applied at runtime by proxy, not in saved binary)
Mostly width/height/centre constants in the `0x00650000`–`0x00672000` range. See `do_patchf` calls in `hook_CreateDevice`. Includes second per-mesh culling sin/cos at `0x00441EDF` / `0x00441EE4` to widen frustum half-angle from 17.35° to 21.0° (14:9), 24.83° (16:9 calibrated), or 26.74° (16:9 aggressive). **Note:** the per-mesh cone-setup pattern around `0x00441EDE` was rewritten in PSOBB (Section 2.5) — porting BB's culling logic is not straightforward and likely not worth pursuing for the dragon investigation.

---

## 6. D3D8 Proxy Architecture (current build: v16-lite-r5)

### File layout
```
pso.exe                       ← patched binary (3 binary patches applied)
pso_exe.original              ← unpatched reference (for diff)
D3D8.dll                      ← built from d3d8_widescreen.c (this proxy)
D3D8_dgvoodoo.dll             ← dgvoodoo D3D8→D3D9/11 wrapper
widescreen_res.cfg            ← INI config for resolution + diagnostics
pso-peeps-d3d8-wsh.log        ← output log, wiped on each launch
```

### Hooks installed (vtable patching)
- IDirect3D8 vtable slot 15: `CreateDevice`
- IDirect3DDevice8 vtable: slots 15 (Present), 23 (CreateVertexBuffer), 36 (Clear), 37 (SetTransform), 40 (SetViewport), 41 (GetViewport), 50 (SetRenderState), 61 (SetTexture), 70 (DrawPrimitive), 71 (DrawIndexedPrimitive), 72 (DrawPrimitiveUP), 76 (SetVertexShader), 83 (SetStreamSource)
- IDirect3DVertexBuffer8 vtable: slots 11 (Lock), 12 (Unlock) — patched on first use of any VB

### Bone hooks (direct CALL-site rewriting in pso.exe)
Seven 5-byte CALL instruction sites are rewritten in-place to redirect to `hook_bone_fn`. Original target (`0x0061EA00`) is preserved as `g_real_bone_fn` and called from inside the hook.

### Logging
- 128 KB ring buffer in memory.
- Single `WriteFile` per `Present` (no per-call file I/O).
- Timestamps logged every 60 frames in both UTC and LOCAL (UTC is required for newserv chat correlation since Bruce is EDT/UTC-4 but server logs UTC).

### Suppression flow (hook_VBUnlock)
1. Reads from VB shadow buffer.
2. Scans for any vertex with `|x|` or `|y|` > 5000 → logs `[OUTLIER]`.
3. If `FVF == 0x0144` and `stride == 28`, processes per-quad:
   - Hard threshold (>5000) → `[SUPPRESS]`
   - Near-outlier (bbox beyond `x<-1024, x>1664, y<-1024, y>1504` or w/h > 1500) → `[SUPPRESS-NEAR]`
   - Suppression collapses 4 verts to (0,0) and zeroes alpha in diffuse.
4. Tracks per-frame suppress count and unique tex0 set for DWIN state machine.
5. Calls `correct_xyzrhw` for widescreen X/Y correction (orthogonal to suppression).

---

## 7. Diagnostic Findings From Latest Run (UTC 16:53:23 – 16:56:22)

### Frame ↔ UTC mapping (selected)
| Frame | UTC | Bruce's narration |
|-------|-----|-------------------|
| F00000 | 16:53:23 | Proxy startup |
| F00516 | 16:53:47 | DWIN false-fired (SHIP/lobby) |
| F00522 | 16:53:48 | First bone hit on caller `0x0061745A` (lobby) |
| F01380 | 16:54:24 | Room creation |
| F02376 | 16:54:57 | First "JUMPf" matrix delta = 1920 |
| F03420 | 16:55:33 | Dragon arena entered, fight begins |
| F04646–F04750 | 16:56:12–16:56:18 | Suppress hot zone (manual trigger or death — unannotated by Bruce) |
| F05744+ | 16:56:22+ | Persistent Matrix A/B alternation continues |

### Critical findings

**Finding 1: DWIN auto-detector was fundamentally broken in v16-lite-r3.**
- Opened at F00551 (UTC 16:53:49) while Bruce was on the SHIP, 96 seconds before dragon arena.
- Never closed for the entire 3-minute run.
- Cause: `is_fight_frame = (sup_uniq >= 3)` fires on lobby effects with multiple textures; auto-close `fsd >= 120` failed because `g_dragon_active=1` was set by *any* boned entity (lobby NPCs, Hilda bears in Forest 2, etc.) using the same call site.
- **Mitigation in v16-lite-r5:** `FIGHT_MIN_FRAME = 2700` (~90 sec) gates fight-confirm. Plus `g_dragon_active` now requires both call site `0x0061745A` AND ptr `0x01301910` (dragon entity).
- **Still pending:** add `g_dragon_active` requirement to the FIGHT_SEEN gate itself; replace hard-coded ptr with adaptive capture.

**Finding 2: Matrix B is real (visually) but its actual float values are unknown.**
- 7,389 events on caller `0x0061745A`, ptr `01301910`.
- Two matrices alternate per frame on same ptr.
- Matrix A: clean rotation, translation `(67, 2000, 9)`.
- Matrix B: rotation rows display as `0,0,0`, translation `(46, 25, 49)`.
- `zero_rot=0` reported throughout — but the predicate uses exact float `== 0.0f` while the dump format uses `(int)`-truncated floats. So Matrix B may be sub-1.0 fractional values that look zero in display.
- **v16-lite-r5 changes the dump format to `(int)(mat[i]*10000)`** — next run resolves this.
- `[BONE-DIAG]` periodic samples show live diagonal `[0.894, -0.993, -0.889]` — fractional cosines. Plausible.

**Finding 3: Bone caller frequency.**
| Caller (return address) | Hits |
|-------------------------|------|
| `0x0061745A` | 7,389 |
| `0x0061753A` | 20 |
| `0x00574C3C` | 17 |
| `0x004F0C96` | 13 |

**Finding 4: Suppression histogram by 100-frame bucket (UTC mapping in narration column).**
| Frame range | Count | Game phase |
|-------------|-------|------------|
| F0400–499 | 633 | Ship/lobby (shouldn't fire — false positives) |
| F0500–599 | 188 | Still on ship |
| F1200–1299 | 12 | Walking around |
| F2600–2999 | 1,089 | Forest 2 / Hilda bears |
| F3000–3699 | 720 | Dragon arena entry, fight start |
| F4600–4799 | 244 | Post-narration (manual trigger or death) |

Suppression fires throughout the run, including non-dragon contexts. Implies either (a) proxy is silently preventing latent bugs in lobby/forest, or (b) heuristic is over-aggressive. **Untested A/B: disable SUPPRESS-NEAR, keep hard SUPPRESS, see if anything visibly degrades.**

---

## 8. Failed Approaches (do not repeat)

| Approach | Result |
|----------|--------|
| Patch `0x00638360` globally to write 0.0f instead of 65535.0f | Broke screen fades and Forest 2 environmental lighting. Reverted. |
| Use return-address discriminator at `0x006382FE` / `0x00638303` to distinguish fades from particles | Same return address used by both. Cannot disambiguate without vertex/struct context. |
| Pure identity matrix fallback for Matrix B | Erased translation. Bad component rendered at world origin (0,0,0) instead of dragon's actual position. |
| Translation-preserving identity fallback | Improved scope; localized artifact remained at correct world position. |
| Use `g_death_window` as suppression gate for body draws | DWIN was open for the entire 3-minute run — would suppress dragon body during normal fight. |
| Bone hook `degen` predicate as `(mat[0] == 1.0f && mat[5] == 1.0f && mat[10] == 1.0f)` (v3) | Wrong direction — checks for identity, not zero rotation. Renamed `is_identity` in v16-lite-r5; new `zero_rot` predicate added. |
| Looking at PSOBB binary for an upstream fix to backport | Sonic Team did not fix the bone matrix code between V2 and BB (Section 2.5). The hunt was useful for validation but did not yield a portable fix. |

---

## 9. Open Questions

1. **What does Matrix B actually contain in float-precision?** Awaiting v16-lite-r5's `mat(x10k)` dump.
2. **Is `0x00639F2A` dragon-specific or a generic mesh renderer?** Currently a guess. Need DWIN-SIG with `last_frame` filtered to clear-screen-only window. The `last_frame` field is added in v16-lite-r5; need a clean DWIN open/close cycle to populate it.
3. **What's the exact RGB of "pink"?** Never sampled. Magenta `#FF00FF` would indicate D3D missing-texture default. PSO-specific pink would indicate intentional damage/death material. Drifting/varied pink would indicate reading from valid-but-wrong texture memory. **Bruce: pixel-sample a screenshot during bug.**
4. **What's the death-fade overlay's draw signature?** Likely a single fullscreen quad at FVF `0x0144`, alpha-blended. Identifying it gives a precise phase trigger that needs no chat marker.
5. **Does the dragon entity ptr `0x01301910` stay stable across game launches?** Probably not — heap allocation. Bone hook's hard-coded filter must be made adaptive.
6. **Which dragon body part triggers the bug visually?** Bruce noted "when a part of the body goes out of frame, the bug stops." Localizing this to a specific bone (head? wing? front-left claw?) would give a per-bone gate, much cleaner than caller+FVF.
7. **Is the lobby/forest suppression activity preventing latent bugs, or is the heuristic over-aggressive?** A/B test pending.
8. **Why doesn't BB visibly show dragon corruption despite having identical broken bone matrix code?** Three competing hypotheses (Section 2.5): different inputs to the bone matrix function in BB, downstream NaN/infinity guards in BB's rendering pipeline, or a different dragon model entirely. Each is testable but requires different methodology (dynamic analysis of BB, static analysis of BB's vertex transform code, or asset comparison respectively).
9. **Does BB's rendering pipeline add NaN/infinity guards downstream of the bone matrix?** If yes, those guards could be ported to V2 as a complementary fix that doesn't depend on the zeroscale assumption holding. Search BB's vertex-skinning and draw-submission code for finite-value checks that don't exist in V2.
10. **Do the dragon model files differ between V2 and BB?** If BB's dragon has different bone weights or skeleton structure, the degenerate state may not be reachable in BB. This would be testable as a pure asset swap with no code changes — substitute BB's dragon model into V2 and see if the bug becomes unreachable.
11. **Compare V2 and BB callers of the bone matrix function.** V2 has 7 hooked call sites (Section 5). BB's caller layout might differ. If BB calls the bone matrix function fewer times per frame, or from different code paths, that could explain why BB doesn't reach the degenerate state.

---

## 10. Recommended Next Steps (when resuming)

In rough priority order:

### Code fixes before next test run
1. **Add `g_dragon_active` requirement to FIGHT_SEEN gate** in `hook_Present` DWIN state machine. Currently:
   ```c
   if (is_fight_frame && g_frame >= FIGHT_MIN_FRAME) {
   ```
   Change to:
   ```c
   if (is_fight_frame && g_frame >= FIGHT_MIN_FRAME && g_dragon_active) {
   ```
2. **Replace hard-coded ptr `0x01301910` in bone_hook.h** with adaptive capture: lock the first ptr seen from caller `0x0061745A` after frame 2700 as the dragon ptr for the rest of the session.
3. **Add `is_identity` to BONE log dump format** in `bone_hook_flush()` — currently captured but not printed.
4. **Cleanup stale comments:** file header `v16-lite-r3` vs DllMain `v16-lite-r5`; `[NEAR_OUTLIER] logged, NOT suppressed` comment in `hook_VBUnlock` is stale (it IS suppressed); chat-marker comment in file header references unimplemented feature.
5. **Fix `wsprintfA(reason, "JUMP%.0f", delta_max)`** — `wsprintfA` doesn't support `%f`; use `wsprintfA(reason, "JUMP%d", (int)delta_max)`.

### Test session 1 (after code fixes)
1. Bruce launches with v16-lite-r6 (post-fixes).
2. Bruce types phase markers in chat as game proceeds:
   - `PRE` — before approaching dragon
   - `TRIGGER NOW` — when starting to rub dragon body
   - `STOPPED` — when bug clears (note which body part went off-screen)
   - `DEAD` — dragon HP zero
   - `CLEAR` — fade overlay begins
   - `DONE` — fade ends
3. Newserv logs the chat with UTC timestamps. Cross-reference with proxy's `[TIME] tick` lines.
4. Bruce takes a pixel-sampled screenshot during the visible bug.

### Analysis after test 1
- Verify Matrix B values from `mat(x10k)` dump. True zero, fractional nonzero, or garbage?
- Filter DWIN-SIG entries by `last_frame` ≥ start-of-clear-screen. Surviving signatures are candidates for body-draw or fade-overlay identification.
- Identify dragon body part from Bruce's STOPPED narration.
- Sample pink RGB from screenshot.

### After test 1: branch on Matrix B verdict
- **If `zero_rot=1` fires** → Matrix B is real corruption. Investigate which sub-mesh consumes this matrix; consider patching the matrix-write path to never produce zero rotation.
- **If matrix is small fractional** → Matrix B is a child bone with small local rotation. Bug B is fully patched; remaining bleed is Bug C territory.

### Bug C investigation (parallel or after Bug B closes)
- Try forcing `D3DCLEAR_ZBUFFER` immediately before the suspected body draw. If bleed disappears, Z-pollution is root cause.
- Explore identifying dragon-specific draws by texture set rather than caller address. Record texture pointers seen during early-fight calm period; they form the "dragon texture set." A draw is "dragon" if it binds a tex0 from that set.
- Consider memory-scanning for dragon HP via Cheat Engine. PSO PC V2 enemy struct layout is documented in Sylverant/Newserv community sources. One read per Present gives a precise alive/dead bit and replaces all the heuristic detection with ground truth.

### BB-derived investigation paths (lower priority, higher novelty)
- **Hunt for downstream finite-value guards in BB's vertex pipeline.** If BB checks for NaN/infinity in vertex transform output before drawing, that check could be ported to V2 as a complementary fix. Static analysis of BB's draw-submission code path is the starting point.
- **Compare bone-matrix caller layouts between V2 and BB.** If BB has fewer or different callers, that points at the "different inputs" hypothesis (Section 2.5) and shifts the investigation upstream of `0x0061EA00`.
- **Asset comparison: V2 dragon model vs BB dragon model.** If they differ, attempt a swap to test whether the bug is data-driven rather than code-driven. This requires familiarity with PSO model formats (NJ/NJM or similar Sega formats).

---

## 11. Build & Run

### Build the proxy
```
i686-w64-mingw32-gcc -shared -O2 -o D3D8.dll d3d8_widescreen.c d3d8.def \
    -lkernel32 -Wl,--enable-stdcall-fixup
```

### Config file (`widescreen_res.cfg`)
```ini
[Resolution]
Width=3840
Height=2160

[DragonDiag]
EnableDeathDrawSigs=1
EnableUPScan=1
MaxUPLogsPerFrame=5
```

### Files placed alongside `pso.exe`
- `D3D8.dll` (this proxy)
- `D3D8_dgvoodoo.dll` (real D3D8 wrapper)
- `widescreen_res.cfg`

The proxy's `Direct3DCreate8` export forwards to `LoadLibraryA("D3D8_dgvoodoo.dll")`.

### Log output
`pso-peeps-d3d8-wsh.log` in pso.exe's working directory. Wiped on each launch.

---

## 12. Source File Status

| File | Version | Notes |
|------|---------|-------|
| `d3d8_widescreen.c` | v16-lite-r5 | File comment header still says v16-lite-r3 — update on next edit. Includes `last_frame` in DrawSig16, `FIGHT_MIN_FRAME=2700` gate. |
| `bone_hook.h` | v3 + v16-lite-r5 fixes | `zero_rot` predicate added. Matrix dump uses `(int)(mat[i]*10000)`. Dragon-active filter requires both caller `0x0061745A` AND ptr `0x01301910` (hard-coded — make adaptive). `[BONE-DIAG]` periodic dump every 60 frames. |
| `pso.exe` | Patched | Three binary patches applied. Diff vs `pso_exe.original` = 88 byte differences across three regions. |
| `pso_exe.original` | Original | Reference for diff verification. |

---

## 13. Tooling References

- **Disassembly:** Ghidra (free) or IDA Pro. PSO PC V2 is 32-bit PE, no obfuscation, identifies as MSVC 6 build. PSOBB is similarly 32-bit PE, with additional packed/protected sections for online security but the dragon-relevant code lives in plain `.text`.
- **Memory inspection:** Cheat Engine for finding dragon HP / entity struct addresses.
- **Binary patching:** Direct hex editor (HxD) for static patches; runtime patches via `VirtualProtect` + `memcpy` in proxy DllMain or hook_CreateDevice.
- **D3D8 reference:** `IDirect3DDevice8` vtable layout — slot numbers in proxy patches are MSDN-documented but easy to drift; verify with dgvoodoo source.
- **Wine config:** Bruce runs on Wine. Proxy must work both native Windows and Wine.
- **Server-side correlation:** Newserv logs UTC. Proxy logs both UTC and LOCAL (Bruce is EDT). Phase markers via in-game chat appear in newserv log with UTC timestamps; align manually with proxy `[TIME] tick` lines.
- **Cross-version comparison:** `objdump -d -M intel` works on both V2 and BB binaries. Python `struct` + byte-pattern search is sufficient for finding equivalent code regions across versions when function addresses shift.

---

## 14. Investigation Methodology Notes

- **Three-way collaboration:** Bruce (drives, runs tests, narrates), Sonnet (writes proxy code, does most implementation), GPT (reviews logs, sanity-checks hypotheses, frames investigation), Opus (architecture-level review on demand).
- **Pitfall observed:** AI-generated detectors can be confidently wrong in ways that produce clean-looking-but-meaningless logs. The bone_hook v3 `degen` predicate checked for identity rotation when the actual bug was zero rotation — and produced "all clear" output that was misleading until reviewed. *Always check whether the detector can even see the thing it's supposed to detect.*
- **Pitfall observed:** Display formatting can lie. The original `(int)mat[i]` matrix dump made non-zero-but-small floats look exactly like zero. Whenever a hypothesis depends on a value being precisely zero (or precisely something), use a format that distinguishes.
- **Test cycle cost:** Each diagnostic run is ~15 minutes setup + 5 minutes gameplay + 30+ minutes log analysis. Iteration is expensive. Code review before run > extra runs.
- **Bug families:** Decomposition is mandatory. "The dragon bug" is at least three bugs. Treating it as one was probably why prior community attempts didn't progress.
- **Cross-version binary comparison is high-yield.** Comparing V2 against PSOBB took a few hours of static analysis and produced unambiguous validation that the bone matrix bug is real and unfixed upstream (Section 2.5). When investigating long-standing bugs in old games, compare against successor versions: same engine, possibly fixed bugs, possibly not. Either outcome — fix found or fix absent — is informative. The methodology is straightforward: identify a unique byte pattern in the original (constants, opcode sequences, distinctive control flow), search for the same pattern in the successor, and compare structures around hits.

---

## 15. Estimated Remaining Effort

| Outcome | Likelihood | Estimated additional hours |
|---------|------------|---------------------------|
| Reduced visible severity beyond current | ~85% | 10–20 |
| "Patch-server ready" — bug rare and mild | 60–70% | 30–50 |
| Complete elimination of all visual artifacts | 30–40% | 60–100+ |

Investigation has reached the steepest part of the value curve: harness is built, decomposition is done, three patches landed, diagnostics produce signal, and cross-version verification confirms the bugs are real and unaddressed by Sonic Team. Marginal hours from here are far more productive than the early hours.

---

## 16. Pickup Notes for Resuming

When you (or the next investigator) resume:

1. **Start by reading this document.** It encodes everything known to date, including the cross-version comparison in Section 2.5 — read that early; it sets the strategic frame for everything else.
2. **Re-read your existing logs** — `pso-peeps-d3d8-wsh.log` from the most recent run, and any newserv chat log from the same session.
3. **Apply the v16-lite-r6 code fixes** listed in Section 10 before any new test.
4. **Run one diagnostic session with phase markers** (Bruce types in chat).
5. **Branch on Matrix B verdict** as described in Section 10.
6. **Don't trust DWIN as a phase gate** until the FIGHT_SEEN guard includes `g_dragon_active`.
7. **Don't expect BB to provide a fix.** Section 2.5 concludes that BB has the same broken code. BB is useful for additional investigation paths (downstream guards, caller layout, asset comparison) but not as a source of patches to backport.

---

## 17. Definitions / Glossary

- **Bug A / B / C** — the three identified bug families (see Section 3).
- **DWIN** — "Death WINdow" state machine in proxy. Currently misnamed; actually detects "dragon outlier window." Renaming pending.
- **Matrix A / Matrix B** — two genuinely different bone matrices computed per frame on the same ptr. Names are local convention to this investigation.
- **FVF** — D3D8 Flexible Vertex Format. `0x0144` = XYZRHW + diffuse + 1 tex coord set. `0x0142` = XYZ + diffuse + 1 tex coord set.
- **XYZRHW** — pre-transformed vertex format (already in screen space, perspective divide already applied). Bypasses GPU vertex pipeline.
- **dgvoodoo** — D3D8/9-to-modern-API wrapper used as the actual D3D8 implementation. Proxy forwards real calls to `D3D8_dgvoodoo.dll`.
- **`[SUPPRESS]` / `[SUPPRESS-NEAR]`** — log tags for proxy suppression actions on outlier vertex buffer quads.
- **`[BONE]` / `[BONE-DIAG]`** — bone hook log tags. `[BONE]` is event-driven on suspicious matrices; `[BONE-DIAG]` is periodic sampling.
- **`[DWIN-SIG]`** — draw signature accumulated during the death window, dumped on window close.
- **Caller / return address** — in this doc, `caller` usually means `__builtin_return_address(0)` — the address in pso.exe immediately after the CALL instruction. This is the address logged as `caller0=...` in proxy output.
- **V2 vs BB** — V2 = Phantasy Star Online PC Version 2 (this investigation's target). BB = Phantasy Star Online Blue Burst (the next-generation client, used in Section 2.5 for cross-version verification).

---

*End of technical reference.*
