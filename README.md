# PSO PC V2 Dragon Corruption Bug — Technical Reference
<img width="1698" height="900" alt="image" src="https://github.com/user-attachments/assets/85e2ea75-bae6-4baa-a399-07e12e43d87e" />  
  
**Disclaimer:** This report was prepared by me using LLMs (Sonnet, Opus, and ChatGPT) as research, drafting, review, and code-analysis assistants. I directed the investigation, ran the tests, validated results, and made the technical decisions. My intent is to either figure out a way to get this patched on my own, or have enough information archived that someone else with deeper knowledge can utilize this in the future. 

**Status:** D3D8 matrix-mutation / proxy-layer fix path closed as a structural fix path; proxy retained for Bug A mitigation and diagnostics. pso.exe machine-code analysis/patching is the active phase.  
**Target:** Phantasy Star Online PC V2 client  
**Last updated:** Post-v3.4 D3D8 diagnostic completion. Software-skinning path strongly indicated by diagnostics; pso.exe disassembly is the next phase.  

**Current tested baseline:**
- `pso.exe` patched with `apply_dragon_fix.py` v5, with patches A/B/C/D applied.
- D3D8 proxy: `v16-lite-r9-v3.4`.
- `bone_hook.h`: v3.4.
- `BONE_SUPPRESS_HIGH_Y=0` for the final v3.4 diagnostic run.
- Widescreen mode under test: 14:9 viewport/bars, 16:9 introduces too much culling  


> **Note for the impatient reader (revised post-v3.4):** Two structural conclusions now stand:
>
> 1. **PSOBB shares V2's broken bone matrix code byte-identically.** Sonic Team did not fix this between releases. There is no upstream fix to backport. (Section 2.5, unchanged.)
> 2. **The D3D8 / proxy layer cannot fix the bone-matrix side of the visible bug by mutating matrices after the bone function returns.** PSO V2 software-skins the dragon's bone matrix on the CPU; D3D8 only sees post-skinned vertices. The "high-Y ghost" matrix observed in v3.x is structurally suspicious, but suppressing or mutating it at the proxy hook point has no visible effect because the matrix never traverses D3D8 SetTransform en route to the GPU. v3.3 confirmed this with three independent diagnostic markers showing zero hits across a full arena run. (New Section 2.6.)
>
> The investigation has reached the point where further progress requires reading and patching pso.exe machine code. The D3D8 proxy stays in place as a diagnostic harness and as the housing for Bug A particle suppression (which IS effective at the proxy layer).

Quick commands:

Build the D3D8 proxy:
```
i686-w64-mingw32-gcc -shared -O2 -o D3D8.dll d3d8_widescreen.c d3d8.def -lkernel32 -Wl,--enable-stdcall-fixup
```

Apply / verify / revert pso.exe binary patches:
```
python3 apply_dragon_fix.py pso.exe           # apply
python3 apply_dragon_fix.py pso.exe --verify  # check state
python3 apply_dragon_fix.py pso.exe --revert  # restore from .original backup
```

---

## 1. TL;DR
<img width="1698" height="900" alt="image" src="https://github.com/user-attachments/assets/93c27cf1-dbf1-46a0-b245-05f1e7f5b046" />  
A 20-year-old visual corruption bug specific to the PC V2 release of PSO. Dragon (Forest 2 boss) renders with severe texture/geometry corruption in three distinct ways. After ~50 hours of investigation across four bone-hook iterations and four binary patches, Bug A is largely contained and Bugs B/C are characterized but not fixed.

**Bug decomposes into three families:**
- **Bug A — XYZRHW Particle Near-Z Explosion.** Largely contained at the D3D8 proxy layer plus the pso.exe Patch D.
- **Bug B — Matrix Alternation / High-Y Ghost Transform.** Characterized in detail; runtime suppression confirmed ineffective (Section 7.2). Fix path is now upstream in pso.exe.
- **Bug C — Clear-Screen Body Bleed.** Unresolved. v3.4 produced a complete catalog of dragon-active draw signatures (Section 7.3) which is the orientation material for pso.exe disassembly.

**Four binary patches applied to pso.exe** at addresses `0x0061EA1D`, `0x0061EA27`, `0x0061EFD2–0x0061F040`, and `0x00634A48`. Patches verified by `apply_dragon_fix.py --verify` and by `cmp(1)` diff against `pso.exe.original`.

**Cross-version verification (Section 2.5):** Static binary comparison against PSOBB confirms the bone matrix bug is present and unfixed in both releases. The investigation is on the correct code path.

**The D3D8 matrix-mutation fix path is closed (Section 2.6).** No further proxy-layer matrix-mutation fixes are viable for Bug B/C. The proxy remains useful for Bug A mitigation and diagnostics, and v3.4 produced an actionable map for pso.exe analysis.

**Remaining real fix work** lives in pso.exe disassembly, not the proxy.

---

## 2. User-visible symptoms
<img width="3839" height="2160" alt="Screenshot From 2026-05-07 18-17-09" src="https://github.com/user-attachments/assets/346a023d-7779-4d86-9108-5cfaf833590e" />  
  
The bug can be triggered manually mid-fight or it manifests during dragon's death sequence.

**Pre-mitigation (original behaviour):**
- Dragon turns "pink" or visually corrupt
- Textures / particles / geometry bleed across the entire map
- Loot boxes near dragon contaminate visually
- Post-death / clear-screen phase strobes / flashes violently
- Objects intersecting dragon body show texture bleed

**Post-mitigation (current state, with all four binary patches + proxy):**
- Catastrophic strobing (Bug A) largely gone
- Bruce can still trigger the bug manually mid-fight by rubbing the player against dragon body and adjusting camera
- Clear-screen / post-death texture bleed still occurs
- Dragon dies "pink" but less chaotic
- Texture bleed still appears when dragon body intersects player or loot boxes during clear-screen
- Dragon head/neck visibly stays on the ground while the rendered body rises; standing in/near the head causes strobing
- **Critical observation:** *"When a part of the dragon's body goes out of frame the bug stops."* This is a load-bearing clue — see Section 7.

**Confirmed not improved by:**
- Zero-rotation suppression of the high-Y matrix at the bone hook (v3.2)
- Translation-preserving identity fallback for the matrix
- Any matrix-mutation strategy at the D3D8 proxy layer (Section 2.6)

---

## 2.5 Cross-version verification (Blue Burst comparison)

PSOBB (Phantasy Star Online Blue Burst) is built on top of the V2→V3 codebase; BB is V4. Episode 1 content, including the Forest 2 dragon, was carried forward. A static comparison between V2 and BB binaries provides important validation, with mixed implications.

### What's identical between V2 and BB

The bone-matrix degenerate-case code from Bug B is structurally unchanged across releases:

| Item | V2 location | BB location | Status |
|------|-------------|-------------|--------|
| FLT_MAX fallback (16 writes of `0x7F7FC99E`) | `0x0061EFD2` onwards | `0x0083CCDD` onwards | identical structure |
| Guard `fcom; fnstsw; sahf; je →fallback` | `0x0061EA1D` | `0x0083C698` | identical opcode sequence |
| Epsilon constant for the comparison | `0.0f` at `0x007C1674` | `0.0f` at `0x0098AF70` | same value |
| Divisor constant for `1/x` | `1.0f` at `0x007C1670` | `1.0f` at `0x0098AF68` | same value |
| Null-input early-out | `test esi,esi; je null_handler` | `test esi,esi; je null_handler` | identical |

Same literal-zero epsilon. Same number of writes. Same FLT_MAX-pattern fallback. Same null-input handling. Sonic Team shipped this code twice without modifying it.

### What's different between V2 and BB

The per-mesh culling system was rewritten:

- V2's per-mesh cone-setup pattern at `0x00441EDE` does not exist anywhere in BB. Byte-pattern search returns zero hits.
- The V2 cone-setup function at `0x00619100` has no signature equivalent in BB.
- BB has a graduated table of float values at `0x0090332C–0x00903364` whose values match what we have empirically calibrated for various aspect ratios. Closer inspection shows this table is read by `fdiv` instructions (integer-conversion arithmetic), not cone tests, so the value match is likely coincidental rather than purposive.

The culling rewrite isn't directly dragon-related, but it does prove Sonic Team touched the rendering pipeline between releases. They had the opportunity to fix the bone matrix bug and elected not to.

### What this means for this investigation

1. **Validation.** The bone matrix patches address a real, unfixed bug. There is no Sonic-Team-blessed correct version of this code we missed. The investigation is on the right code path.
2. **No free reference fix.** Porting BB's solution to V2 isn't an option because BB doesn't have a solution.
3. **The bug exists in BB too** — the same code is there, with the same constants, on the same control flow path. But BB doesn't visibly show dragon corruption in normal play. This implies either (a) different gameplay inputs in BB never drive the matrix into the visible-bad state, (b) BB's rendering pipeline added downstream guards that filter the corrupt output before it affects pixels, or (c) different model/skeleton data in BB makes the bad state unreachable.

### Why the V2 community hasn't progressed on this in 20 years

Now answerable: **there was no upstream fix to backport.** Sega left the bug in V2, the bug stayed in BB, and there was no canonical "correct version" anyone could reference. Investigators looking at later versions for hints would have found the same broken code and concluded the bug must be elsewhere.

### Status of BB-derived investigation paths

After v3.x, the BB-derived investigation paths (downstream NaN/infinity guards, caller-layout comparison, asset comparison) are de-prioritized but kept on file (Section 9). The reason: v3.x established that the visible bug operates through the *software-skinning vertex pipeline*, not the matrix-output pipeline that BB might have differently-guarded. The "different inputs" hypothesis remains testable but would require dynamic analysis of BB rather than static comparison. None of these paths are an active investigation direction at this time.

---

## 2.6 D3D8 matrix-mutation endpoint (post-v3.4 conclusion)

**Conclusion:** The D3D8 / proxy layer cannot deliver a structural fix for the bone-matrix side of the visible Dragon corruption by mutating matrices after the bone-hook return point. The mutation is mechanically correct, but the renderer never reads from that path.

**Evidence:** Across v3.1 through v3.4 of `bone_hook.h`, the proxy progressively narrowed the set of viable D3D8-layer interventions until only one remained — directly mutating the bone matrix at the return point of `0x61EA00` to suppress the high-Y "ghost" transform. v3.2 implemented this. v3.3 instrumented it with three independent diagnostic markers:

- `[BONE-POST]` confirms our zero-rotation write actually lands in `*actual` (`mutation_took=1` in 100% of fires).
- `[BONE-PRE]` reads `*actual` at the start of each subsequent bone hook call (`matches_pre_high_y=0` in 100% of fires — nothing is restoring the matrix between calls).
- `[SETXFORM-HIGHY]` in `hook_SetTransform` watches for any matrix passed to D3D8 in any state matching the high-Y signature. **`setxform_highy_seen_total = 0` across an entire Dragon arena run.**

The third marker is decisive. The high-Y matrix is never submitted to D3D8 SetTransform in any state. PSO V2 inherits Dreamcast-era CPU-vertex-transform conventions: the bone matrix at `*actual` is read by software-skinning code that runs immediately after `0x61EA00` returns. That code transforms vertices on the CPU and writes them into a vertex buffer. D3D8 only sees the post-skinned positions. By the time our hook returns, the relevant work has already happened.

**What this rules out:** any proxy-layer fix that depends on changing the matrix value seen by the renderer after the bone function returns. Identity fallback, zero-scale fallback, translation-preserve fallback, off-screen translation, NaN/Inf injection — all of these mutate `*actual` after the apparent consumer has already read it.

**What remains viable at the D3D8 layer:** Bug A particle suppression at `hook_VBUnlock` (the suppression operates on already-rendered XYZRHW vertex buffers, not on the bone path), and diagnostics. The proxy's role is now fixed: house Bug A's mitigation and run as a diagnostic harness for the pso.exe code analysis to come.

**What this implies for the next phase:** A fix must be applied either upstream of `0x61EA00` (preventing the high-Y state from being computed in the first place), or at the consumer side of the matrix (modifying the software skinning code), or by suppressing whichever specific draw call submits the corrupt vertex buffer (Section 10). All three paths require pso.exe disassembly.

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
- Alpha blend on, `srcblend = 5` (SRCALPHA), `destblend = 2` (one minus)
- `zwriteenable = 0`, `lighting = 0`
- Caller around `0x006405E0`

**Source cause in pso.exe:** Two functions return `65535.0f` as a fallback when view-space Z is too small:
- `0x00634A30` — patched at file offset `0x00634A48` (Patch D, see Section 4)
- `0x00638360` — NOT patched. Shared with screen-fade rendering. Patching globally broke fades and Forest 2 environmental lighting. Return-address discriminator alone (around `0x006382FE` / `0x00638303`) cannot distinguish fades from particles.

**Mitigations stacked:**
- pso.exe Patch D: 65535.0f → 0.0f at `0x00634A48`. Eliminates one source.
- Proxy `hook_VBUnlock` suppression: scans XYZRHW buffers for outliers (|x| or |y| > 5000 hard, or bbox beyond 14:9 reasonable bounds) and collapses bad quads to zero area / alpha=0.

**Status:** Catastrophic strobing eliminated. Sub-threshold cases still possible but rare.

---

### 3.2 Bug B — Matrix Alternation / High-Y Ghost Transform

**Important note on naming.** Earlier versions of this document referred to two matrices as "Matrix A" and "Matrix B" using a display format that truncated floats to integer. That display made small fractional rotations *look* like all zeros, which led to the misleading framing "Matrix B has zero rotation and is the corrupt one." After v3.x adopted the `(int)(mat[i]*10000)` dump format, the actual mapping became visible:

- **Matrix 1** (formerly "Matrix A") = the **high-Y ghost** transform. Position constant at `(varying tx, 2000.0, varying tz)`. Rotation `[1,0,0 | 0,0,1 | 0,-1,0]` — a clean +90° rotation around X. Suspect for the visible artifact, but **not confirmed causal** as of v3.4.
- **Matrix 2** (formerly "Matrix B") = the **visible Dragon body** transform. Translation at ground level (`ty ≈ 25`). Rotation a full orthonormal basis with values like `(0.738, -0.994, -0.733)` on the diagonal — these are the small-fractional cosines that the old display format truncated to zero.

The original framing had it backwards: Matrix B (renamed Matrix 2) is the *healthy* body transform; Matrix A (renamed Matrix 1) is the *suspicious* high-Y ghost. The rest of this section uses the new naming.

**Mechanism:** Each frame, the dragon's primary bone matrix function (called from `0x00617455` in pso.exe, returning to `0x0061745A`) fires *twice* on the same matrix pointer. Two genuinely different matrices result in alternation. v3.1 captured 2074 high-Y events and 2081 normal-Y events in one run with perfect interleaving — one of each per frame.

**High-Y characteristics:**
- ty = 2000.0 exactly (constant across the entire run)
- Rotation `[1,0,0 | 0,0,1 | 0,-1,0]` (single unique signature, no animation)
- Present from the first arena frame (`[BONE-PTR]` capture fires on F01631 with this signature)
- Absent during ship/lobby/Forest 2 (gated by Dragon entity ptr capture)

**Original pre-patch behaviour at the matrix-inverse guard inside `0x0061EA00`:** when the guard failed, the fallback wrote `0x7F7FC99E` (≈ FLT_MAX, ~3.39 × 10³⁸) into multiple matrix slots. Vertices transformed by such a matrix project to infinity.

**Patches A/B/C in pso.exe** (see Section 4): widen the guard trigger and replace the FLT_MAX fallback with a zero-rotation, position-preserving collapse. These are static binary patches and are applied to the running pso.exe. Their effect is to ensure that *if* the guard fires, the resulting matrix is benign rather than catastrophic. They do not remove the alternation pattern; they only make the worst pre-patch behaviour impossible.

**Runtime mitigation attempted (v3.2) and disproved (v3.3):** Suppressing the high-Y matrix at the bone hook return — overwriting `mat[0..10]` with zeros to collapse the rotation block — fired correctly (1888 suppressions in one run) but had no visible effect on the bug. v3.3 instrumented the consumer path and confirmed that `*actual` is consumed by software skinning, not by SetTransform. Section 2.6 has the full evidence.

**Status:** Pre-patch catastrophic FLT_MAX behaviour eliminated. High-Y alternation pattern fully characterized. Causal relationship between high-Y matrix and visible artifact is **suspected but not confirmed**. The fix is now an upstream-of-`0x61EA00` problem — find what state at the call site decides to compute the high-Y transform, and either suppress that state or short-circuit the call.

---

### 3.3 Bug C — Clear-Screen Body Bleed

**Mechanism (current hypothesis, ~40% confidence):** During the post-death clear-screen overlay phase, the opaque dragon body mesh (caller `0x00639F2A`, FVF `0x0142`, `zwriteenable = 1`, `alphablendenable = 0`, `cullmode = 1`) continues to render and depth-write while overlapping player / loot boxes. The interaction with the clear-screen overlay produces visible texture bleed.

**Competing hypotheses:**
- **Z-buffer pollution** from sub-threshold XYZRHW quads or another path leaving bad depth data, causing later opaque draws to misrender.
- **Render-state leakage** — pso.exe sets a state for dragon death rendering and never restores it.
- **The "pink" is intentional** death-dissolve material that *looks* corrupt because depth state from earlier in the frame is already broken.

**Discriminator test:** Force a `D3DCLEAR_ZBUFFER` immediately before the suspected body draw. If bleed disappears, Z-pollution is the cause.

**v3.4 produced a draw-call catalog for this investigation** (Section 7.3). Top callers seen during the death window across one run, ordered by hit count: `0x00637A90` (196), `0x006405E0` (122), `0x00639F2A` (19), `0x0063691F` (4), `0x00637BC0` (2), `0x0063694B` (1). The lower-frequency UP callers (`0x0063691F` and `0x0063694B`) are particularly interesting because `DrawPrimitiveUP` traffic is unusual and suggests one-off submissions like overlay quads or special-case death effects. These are the addresses to anchor on when reading pso.exe.

**Status:** Unresolved. Awaiting pso.exe analysis around the v3.4 callers.

---

## 4. Binary Patches Applied to pso.exe

Four patches applied via `apply_dragon_fix.py` (currently v5). All four verified by `apply_dragon_fix.py --verify` and by `cmp(1)` diff against `pso.exe.original`.

### 4.A — fcomps comparison redirect
**Address:** `0x0061EA1D`
**Bytes changed:** 4 of 6 in window
**Original:** `D8 1D 74 16 7C 00` (`fcom dword ptr [0x007C1674]`, comparing against literal `0.0f`)
**Patched:** `D8 1D F0 D4 64 00` (`fcom dword ptr [0x0064D4F0]`, comparing against `0.5f`)

Redirects the matrix-inverse guard's epsilon comparison to a non-zero threshold. Combined with Patch B, this widens the set of inputs that route through the safe fallback rather than the original FLT_MAX path.

### 4.B — Conditional branch widening
**Address:** `0x0061EA27`
**Bytes changed:** 1
**Original:** `0F 84 ...` (`je rel32`)
**Patched:** `0F 86 ...` (`jbe rel32`)

`je → jbe` widens the conditional jump, routing more degenerate inputs through the fallback path.

### 4.C — Degenerate bone fallback (the big one)
**Address:** `0x0061EFD2` through `0x0061F040` (16 instruction window)
**Purpose:** Replace FLT_MAX-fill matrix fallback with a position-preserving zero-rotation collapse.

**Original behaviour:** A series of 16 `mov dword ptr [esi+offset], 0x7F7FC99E` instructions filled the 4×4 matrix at all offsets `0x00, 0x04, ..., 0x3C` with values close to FLT_MAX.

**Patched behaviour (v5 of `apply_dragon_fix.py`):**
- **Slots `[0][0]`, `[1][1]`, `[2][2]`** (rotation diagonal, offsets `0x00 / 0x14 / 0x28`): write `0.0f`. **(Earlier v4 wrote `1.0f` here, which produced an identity-rotation fallback that left the bad component visibly "alive at world position." v5 zeroes the diagonal so the rotation 3×3 is fully zero; collapsed to a single point.)**
- **All other rotation slots** (`0x04, 0x08, 0x0C, 0x10, 0x18, 0x1C, 0x20, 0x24, 0x2C`): write `0.0f`.
- **Translation slots `tx / ty / tz`** (offsets `0x30 / 0x34 / 0x38`): NOPed out. **(Earlier versions wrote `0.0f` to these. That sent the collapsed mesh to world origin, exposing the inner skin mesh below the arena floor. NOPing preserves whatever the caller had at those slots — the Dragon's actual world position.)**
- **`[3][3]` homogeneous w slot** (offset `0x3C`): write `1.0f`.

**Net fallback output:** `[0,0,0,0 | 0,0,0,0 | 0,0,0,0 | tx,ty,tz,1]`. All vertices collapse to the Dragon's world position. Zero-area triangles, no visible render. The Dragon's actual body (Matrix 2) renders normally; the corrupt component (when the guard fires) becomes invisible without dragging position to origin.

**Risk:** This patch and Patch A/B together assume the guard triggers correctly identify "this matrix is about to be bad." If the guard misses some bad cases or false-positives on good ones, behavior diverges. Empirically, with all four patches applied, the catastrophic FLT_MAX strobing is gone and the residual bug appears unrelated to this fallback path.

**BB equivalent:** `0x0083CCDD` onwards. Identical 16-write structure, identical offsets, identical `0x7F7FC99E` constant. Same patch logic applies byte-for-byte at the BB address.

### 4.D — Perspective divide near-Z fallback (clean win)
**Address:** `0x00634A48`
**Bytes changed:** 4
**Original:** `00 FF 7F 47` (`0x477FFF00` = `65535.0f`)
**Patched:** `00 00 00 00` (`0.0f`)

Single-immediate change. When perspective divide hits view-space Z near zero, instead of returning a screen-spanning scale of 65535, returns 0. Collapses outlier particles to z=0 / origin. No fades broken (this address is on a different code path than the shared fade/particle function at `0x00638360`).

**BB equivalent:** Not yet located. The corresponding function may have been restructured along with the per-mesh culling rewrite (Section 2.5). A separate hunt in BB would be required.

### Verification

```bash
$ python3 apply_dragon_fix.py pso.exe --verify
Status       Label
------------------------------------------------------------
  PATCHED    A: fcomps redirect [0x7C1674]=0.0 -> [0x64D4F0]=0.5
  PATCHED    B: je -> jbe (0x61EA27)
  PATCHED    C[esi    ]=zero    # ... 16 entries ...
  PATCHED    D: near-z scale 65535.0f -> 0.0f (0x634A48)

STATE: fully patched
```

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
| `0x0061EA00` | Matrix-inverse guard / bone fn (called from 7 sites; primary at `0x00617455`) | Patched at +0x1D, +0x27, +0x1D2..+0x240. **Identical in BB at `0x0083C680`.** |
| `0x00634A30` | Perspective divide near-Z fallback (particles) | Patched at +0x18 |
| `0x00638360` | Second perspective-divide function (shared with fades) | NOT patched. Shared call site at `0x006382FE`/`0x00638303` |
| `0x00619100` | V2 cone-setup function (per-mesh culling) | Not present in BB (Section 2.5) |
| `0x00617455` | Bone fn call site (returns to `0x0061745A`) | Hooked. PRIMARY Dragon caller |
| `0x00617535` | Bone fn call site (returns to `0x0061753A`) | Hooked. Lower frequency, different render flow |

### Other bone fn call sites (also hooked)
`0x004D9043`, `0x004D90A1`, `0x004E0AD5`, `0x004F0C91`, `0x00574C37`. Low-frequency in dragon scenarios.

### Dragon-active draw callers (v3.4 catalog)
These are the addresses observed submitting draws while `g_dragon_active==1` (Dragon entity bone hook firing). Across one v3.4 run, 128 unique `[DRAGON-SIG]` entries were captured. The 6 dominant callers from `[DWIN-SIG]` (death-window subset) ranked by hit count:

| Address | Hits (death window) | Draw type | Notes |
|---------|---------------------|-----------|-------|
| `0x00637A90` | 196 | DP (high frequency) | Highest-volume death-window caller. Strong candidate for body draws or persistent particle systems |
| `0x006405E0` | 122 | DP | Already in our notes as suspected XYZRHW particle caller (Bug A vertex signature). High death-window count suggests overlap with death effects |
| `0x00639F2A` | 19 | DP | Suspected dragon body draw caller. Lower count than expected; worth re-examining |
| `0x0063691F` | 4 | UP (DrawPrimitiveUP) | Low frequency. UP traffic is unusual; possible overlay/UI element or death-fade quad |
| `0x00637BC0` | 2 | DP | Very low count, dominantly clear-screen-phase |
| `0x0063694B` | 1 | UP | Single-fire. Possible one-off quad |

These addresses are the primary anchor points for pso.exe disassembly in the next phase.

### Data / fixed addresses
| Address | Purpose | Notes |
|---------|---------|-------|
| `0x007BB7E0` | `BONE_GLOBAL_PTR` fallback | Used when bone hook receives `self == NULL` |
| Dragon entity ptr | Dynamic | **Captured at runtime** by bone hook (first ptr at caller `0x0061745A` with `|ty| > 1000`, post-frame-2700). Heap address — varies across launches. Most recent observed value `0x01301910` but this is session-specific and not relied upon |
| `0x0064D4F0` | `0.5f` constant | Used by Patch A as new comparison threshold |
| `0x007C1674` | `0.0f` constant | Original (pre-patch) Patch A comparison threshold |
| `0x007C1670` | `1.0f` constant | Divisor for matrix inverse |
| `0x0064FA70` | Frustum constant | Patched at runtime by proxy from `1.0f` to `1.4f` for 14:9 widescreen |

### Widescreen-related patches (applied at runtime by proxy, not in saved binary)
Mostly width/height/centre constants in the `0x00650000`–`0x00672000` range. See `do_patchf` calls in `hook_CreateDevice`. Includes second per-mesh culling sin/cos at `0x00441EDF` / `0x00441EE4` to widen frustum half-angle from 17.35° to 21.0° (14:9), 24.83° (16:9 calibrated), or 26.74° (16:9 aggressive).

---

## 6. D3D8 Proxy Architecture (current build: v16-lite-r9-v3.4)

### File layout
```
pso.exe                       ← patched binary (4 binary patches applied)
pso.exe.original              ← unpatched reference (backup created on first apply)
D3D8.dll                      ← built from d3d8_widescreen.c (this proxy)
D3D8_dgvoodoo.dll             ← dgvoodoo D3D8→D3D9/11 wrapper
widescreen_res.cfg            ← INI config for resolution + diagnostics
pso-peeps-d3d8-wsh.log        ← output log, wiped on each launch
apply_dragon_fix.py           ← pso.exe binary patcher (v5)
```

### Hooks installed (vtable patching)
- IDirect3D8 vtable slot 15: `CreateDevice`
- IDirect3DDevice8 vtable: slots 15 (Present), 23 (CreateVertexBuffer), 36 (Clear), 37 (SetTransform), 40 (SetViewport), 41 (GetViewport), 50 (SetRenderState), 61 (SetTexture), 70 (DrawPrimitive), 71 (DrawIndexedPrimitive), 72 (DrawPrimitiveUP), 76 (SetVertexShader), 83 (SetStreamSource)
- IDirect3DVertexBuffer8 vtable: slots 11 (Lock), 12 (Unlock) — patched on first use of any VB

### Bone hooks (direct CALL-site rewriting in pso.exe)
Seven 5-byte CALL instruction sites are rewritten in-place to redirect to `hook_bone_fn`. Original target (`0x0061EA00`) is preserved as `g_real_bone_fn` and called from inside the hook. The hook fires for ALL boned entities, not dragon-exclusive — Dragon is identified by dynamic ptr capture (caller `0x0061745A` + `|ty|>1000` at post-2700-frame).

### Logging
- 128 KB ring buffer in memory.
- Single `WriteFile` per `Present` (no per-call file I/O).
- Timestamps logged every 60 frames in both UTC and LOCAL (UTC is required for newserv chat correlation since Bruce is EDT/UTC-4 but server logs UTC).

### Suppression flow (hook_VBUnlock — Bug A)
1. Reads from VB shadow buffer.
2. Scans for any vertex with `|x|` or `|y|` > 5000 → logs `[OUTLIER]`.
3. If `FVF == 0x0144` and `stride == 28`, processes per-quad:
   - Hard threshold (>5000) → `[SUPPRESS]`
   - Near-outlier (bbox beyond `x<-1024, x>1664, y<-1024, y>1504` or w/h > 1500) → `[SUPPRESS-NEAR]`
   - Suppression collapses 4 verts to (0,0) and zeroes alpha in diffuse.
4. Tracks per-frame suppress count and unique tex0 set for DWIN state machine.
5. Calls `correct_xyzrhw` for widescreen X/Y correction (orthogonal to suppression).

### Bone hook diagnostics (v3.4)
Multiple log markers, all gated to keep volume manageable:

| Marker | When emitted | Purpose |
|--------|--------------|---------|
| `[BONE]` | Suspicious matrix events (NaN/Inf, large delta, JUMP, FIRST sight) | Per-event matrix dump in `(int)(mat[i]*10000)` format |
| `[BONE-DIAG]` | Periodic (every 60 frames) when `actual==g_dragon_ptr` | Sample diagonal values to verify zeroscale patch produces sane matrix |
| `[BONE-PRE]` | Top of `hook_bone_fn` when in Dragon arena | State of `*actual` at hook entry. `matches_pre_high_y` flag |
| `[BONE-POST]` | After mutation (when suppression toggled on) | Verify mutation actually wrote to memory |
| `[BONE-PTR]` | Once per session | Dynamic Dragon ptr capture announcement |
| `[BONE-XYZ]` | Throttled (≥15 frames, +zero_rot, +new ptr) | Compact tx/ty/tz log per Dragon-armed bone fire |
| `[HIGHY-SEEN]` | When suppression OFF | Identifies high-Y matrix instances without mutation. Used in v3.4 |
| `[HIGHY-SUP]` | When suppression ON | Used in v3.2/v3.3 testing. First 10 fires log full detail |
| `[SETXFORM-HIGHY]` | hook_SetTransform sees a matrix matching the high-Y signature | Decisive marker — **always 0 across full Dragon arena**, confirms software skinning |
| `[BONE STAT]` | Every 300 frames | Per-tracker hit counts; suppression and SetTransform-HighY totals |

### Dragon-active draw correlation (v3.4)
- `[DRAGON-SIG] new ...` fires once per unique draw signature seen during `g_dragon_active`
- `[DWIN-SIG] ...` death-window summary, dumped on close
- `[DRAWCTX]` per-call detailed context (caller, FVF, render states, tex pointers)

---

## 7. Diagnostic Findings

The investigation has produced multiple diagnostic runs. Three are documented here — the original reference run, plus the two most informative v3.x runs that established the D3D8 endpoint.

### 7.1 Original reference run (UTC 16:53:23 – 16:56:22, pre-v3.x)

Frame ↔ UTC mapping:

| Frame | UTC | Bruce's narration |
|-------|-----|-------------------|
| F00000 | 16:53:23 | Proxy startup |
| F00516 | 16:53:47 | DWIN false-fired (SHIP/lobby) |
| F01380 | 16:54:24 | Room creation |
| F02376 | 16:54:57 | First "JUMPf" matrix delta = 1920 |
| F03420 | 16:55:33 | Dragon arena entered, fight begins |
| F04646–F04750 | 16:56:12–16:56:18 | Suppress hot zone (manual trigger or death) |
| F05744+ | 16:56:22+ | Persistent matrix alternation continues |

**Findings:**
- DWIN auto-detector was fundamentally broken in v16-lite-r3 (opened on the SHIP, never closed for the entire 3-minute run). Mitigated in v16-lite-r5 by adding `FIGHT_MIN_FRAME = 2700` gate plus dragon entity ptr requirement.
- 7,389 events on caller `0x0061745A`, ptr `01301910`. Two matrices alternate per frame on same ptr.
- The matrix dump at the time used `(int)mat[i]` integer truncation, which made small fractional rotations *display* as zero. This led to misleading framing of which matrix was "the bad one" — see Section 3.2.

This run informed the move to `(int)(mat[i]*10000)` dump format and to bone hook v3.x.

### 7.2 v3.3 — decisive D3D8-layer endpoint confirmation

**Hypothesis tested:** That high-Y matrix suppression at the bone hook return would propagate to the GPU and either fix or visibly change the dragon corruption.

**What we instrumented in v3.3:**
- `[BONE-PRE]` at the top of `hook_bone_fn` — reads `*actual` *before* calling the real bone function, so we can detect whether suppression from the prior frame persisted between calls or whether something restored the matrix.
- `[BONE-POST]` immediately after the mutation — re-reads `mat[]` and asserts the write actually landed.
- `[SETXFORM-HIGHY]` in `hook_SetTransform` — fires if any matrix submitted to D3D8 in any state matches the high-Y signature (`ty≈2000`, `mat[0]≈1`, `mat[5]≈0`, `mat[10]≈0`).

**Run results:**
- `[BONE-POST] mutation_took=1` in 100% of samples — our zero-rotation write reaches `*actual` correctly.
- `[BONE-PRE] matches_pre_high_y=0` in 100% of samples (across 194 entries). Nothing restores the matrix between calls; our suppression "stuck."
- `[SETXFORM-HIGHY]` count: **0 across the entire run.** `[BONE STAT] setxform_highy_seen_total=0` consistently in every periodic stat dump.
- `high_y_suppressed_total=1888` — the gate fired ~1888 times during the arena.
- Visual bug **unchanged or worse** during mid-fight manual trigger and post-death clear-screen. Bruce's eyeballs disagreed with the suppression's mechanical success.

**Conclusion:** The high-Y matrix never reaches D3D8 SetTransform in any state. PSO V2 software-skins the bone matrix on the CPU and writes post-skinned vertices to the VB. Matrix mutation at the bone hook return point is too late — the consumer (CPU vertex transform) has already read `*actual` by the time our hook returns, and the resulting transformed vertices are already in the VB pipeline.

This run closed the D3D8 / proxy-layer fix path for Bug B. See Section 2.6 for the broader implications.

### 7.3 v3.4 — final D3D8 capture (orientation for pso.exe pivot)

**Purpose:** With suppression disabled, capture a complete catalog of Dragon-active draw signatures so that the next phase (pso.exe disassembly) has concrete addresses to anchor on.

**Configuration:**
- `BONE_SUPPRESS_HIGH_Y=0` (suppression off)
- All v3.3 diagnostic markers retained (cheap to keep, useful as ongoing baseline)
- `[DRAGON-SIG]` added — fires once per unique draw signature seen during `g_dragon_active`, capturing: draw type (DP/DIP/UP), caller address, FVF, primitive type, primitive count, vertex stride, tex0 pointer, alphablend/srcblend/destblend, zenable/zwriteenable, lighting, cullmode

**Run results:**
- `setxform_highy_seen_total=0` (re-confirmed)
- `high_y_suppressed_total=0`, `suppress=0` in every `[HIGHY-SEEN]` line (confirms suppression-off mode)
- `128` unique `[DRAGON-SIG]` entries
- `344` `[DWIN-SIG]` entries (death-window subset)
- `18576` `[DRAWCTX]` entries (per-call detail during dragon-active)

**Top death-window callers (orientation for pso.exe disassembly):**

| Caller | Hits | Type | Notes |
|--------|------|------|-------|
| `0x00637A90` | 196 | DP | Highest. Dominant during dragon body / death rendering |
| `0x006405E0` | 122 | DP | Already known as XYZRHW particle path. High death-window count is interesting — likely shared with death effects |
| `0x00639F2A` | 19 | DP | Previously-suspected dragon body draw caller. Lower than expected |
| `0x0063691F` | 4 | UP | UP traffic is rare. Suggests one-off / overlay element |
| `0x00637BC0` | 2 | DP | Very low count, likely clear-screen specific |
| `0x0063694B` | 1 | UP | Single-fire UP. Possible single death-fade quad |

**Visual bug:** unchanged. Mid-fight trigger still works. Death/clear-screen bleed still visible. This run wasn't an attempted fix — it was data capture.

**Conclusion:** The D3D8 matrix-mutation/fix investigation is complete. The catalog above is the orientation material for pso.exe analysis. Each of the six callers above is a candidate for "the function that draws the corrupt component." Identifying which one (or which combination) by reading the disassembly around those addresses is the immediate next task.

---

## 8. Failed Approaches (do not repeat)
<img width="1698" height="900" alt="image" src="https://github.com/user-attachments/assets/4f779ca4-c22f-4f86-94de-4112daf489f1" />  
  
| Approach | Result |
|----------|--------|
| Patch `0x00638360` globally to write 0.0f instead of 65535.0f | Broke screen fades and Forest 2 environmental lighting. Reverted. |
| Use return-address discriminator at `0x006382FE` / `0x00638303` to distinguish fades from particles | Same return address used by both. Cannot disambiguate without vertex/struct context. |
| Pure identity matrix fallback at the matrix guard (Patch C variant) | Erased translation. Bad component rendered at world origin (0,0,0) instead of dragon's actual position. |
| Translation-preserving identity fallback (rotation diagonals = 1.0f) at Patch C | Improved scope. Localized artifact remained at correct world position because identity rotation still rendered the bad mesh. v5 of `apply_dragon_fix.py` reverted to zero diagonals + NOPed translation slots. |
| `(int)mat[i]` matrix dump format in `[BONE]` lines | Made small fractional rotation values display as zero, leading to incorrect characterization of which matrix was degenerate. Fixed in v16-lite-r5 with `(int)(mat[i]*10000)`. |
| Bone hook v3 `degen` predicate as `(mat[0] == 1.0f && mat[5] == 1.0f && mat[10] == 1.0f)` | Wrong direction — checked for identity, not zero rotation. Renamed `is_identity` in v16-lite-r5; new `zero_rot` predicate added. |
| Use `g_death_window` as a suppression gate for body draws | DWIN was open for the entire 3-minute run in early versions. Would have suppressed dragon body during normal fight. |
| **High-Y matrix zero-rotation suppression at the bone hook return** (v3.2) | Mutation lands correctly (`mutation_took=1`), nothing restores it (`matches_pre_high_y=0`), but matrix never reaches D3D8 SetTransform (`setxform_highy_seen=0`). PSO V2 is software-skinning. Visual bug entirely unchanged. (Section 7.2.) |
| **Any matrix-mutation strategy at the D3D8 proxy layer** | Same root cause as the line above. Matrix at `*actual` is consumed by CPU vertex transform code that runs before our hook returns. (Section 2.6.) |
| Looking at PSOBB binary for an upstream fix to backport | Sonic Team did not fix the bone matrix code between V2 and BB (Section 2.5). Hunt was useful for validation but not a portable fix. |

---

## 9. Open Questions
<img width="3833" height="2160" alt="Screenshot From 2026-05-07 19-19-30" src="https://github.com/user-attachments/assets/7f2dc88d-238f-4c9b-b5f0-854c0e9b2f74" />  
  
### Active
- **Q-A.** Which of the v3.4 draw callers (`0x00637A90`, `0x006405E0`, `0x00639F2A`, `0x0063691F`, `0x00637BC0`, `0x0063694B`) is responsible for rendering the stuck head/neck specifically? Each needs reading in pso.exe.
- **Q-B.** What is the relationship between the high-Y "ghost" matrix and the visible artifact, given that the matrix doesn't reach the GPU through SetTransform? Three plausible answers:
   - The high-Y matrix is consumed by software skinning code that produces the *vertex data* eventually rendered in the wrong place. Tracing the consumer would identify which submesh.
   - The high-Y matrix is read for AI / collision / pick-checking reasons and the visible artifact is unrelated geometric state.
   - The high-Y matrix is a red herring entirely; it's been correlated to the bug because both stem from the same dragon-arena setup, but the actual rendering path doesn't depend on it.
- **Q-C.** Bruce noted "when a part of the body goes out of frame, the bug stops." Localizing the specific Dragon body part (head? wing? front-left claw?) that triggers the bug visually would dramatically narrow the search.
- **Q-D.** What's the exact RGB of "pink"? Magenta (`#FF00FF`) would indicate D3D missing-texture default. PSO-specific pink would indicate intentional damage/death material. Drifting/varied pink would indicate stale texture memory. **Pixel-sample a screenshot during bug.**
- **Q-E.** What's the death-fade overlay's draw signature? Likely a single fullscreen UP quad — possibly one of the `0x0063691F` / `0x0063694B` UP callers identified in v3.4.

### Resolved by v3.x
- ~~**Q1 (original).** What does Matrix B actually contain in float-precision?~~ **Answered.** Matrix B (renamed Matrix 2) is the visible Dragon body with full orthonormal rotation, ground-level translation. The "all-zero rotation" framing was a display-truncation artifact. Matrix A (renamed Matrix 1) is the suspicious high-Y ghost at constant `ty=2000` with rotation `[1,0,0 | 0,0,1 | 0,-1,0]`. Section 3.2.
- ~~**Q5 (original).** Does the dragon entity ptr stay stable across game launches?~~ **Partially answered / resolved as not-an-issue.** The ptr varies across launches; the bone hook now does dynamic capture (first ptr at caller `0x0061745A` with `|ty|>1000` after frame 2700). No hard-coded ptr remains.

### Parked (low priority post-v3.x)
The BB-derived investigation paths from earlier in the project assumed matrix corruption was the rendering input. v3.x established the rendering input is software-skinned vertex data, not the matrix. The following questions remain open in principle but are de-prioritized — they would each require substantial new methodology to be productive:

- **Why doesn't BB visibly show dragon corruption despite identical broken bone matrix code?** (Section 2.5.) Answering would still require dynamic analysis of BB.
- **Does BB's rendering pipeline add NaN/infinity guards downstream?** Would need disassembly of BB's vertex transform code.
- **Do the dragon model files differ between V2 and BB?** Asset comparison; requires familiarity with PSO model formats.
- **Compare V2 and BB callers of the bone matrix function.** Could be informative but is now an orthogonal direction; the V2 callers are already the focus of pso.exe disassembly.

---

## 10. Recommended Next Steps (pso.exe phase)

The next phase is static and dynamic analysis of pso.exe machine code, using the v3.4 draw catalog as orientation. The high-leverage targets, in roughly priority order:

### 10.1 Identify which v3.4 caller produces the stuck head/neck

Each of the v3.4 death-window callers is a candidate. Read their disassembly with the goal of answering: which of these draws geometry that is positioned by the high-Y matrix (or by the software-skinning path downstream of it)?

Specific addresses to read first:
- `0x00637A90` (highest hit count; primary candidate for body-related draws)
- `0x0063691F` and `0x0063694B` (UP callers — unusual traffic, possible overlay or special-case quads)
- `0x00637BC0` (clear-screen-phase caller; could be the death-fade quad)

For each, build the call graph upward to find what data structures the draw consumes, what vertex buffers it submits, and how those vertex buffers are populated.

### 10.2 Trace the consumer of `*actual` at `0x61EA00`

Disassemble around the call sites of `0x61EA00`, particularly `0x00617455` (the Dragon-primary caller). The instructions immediately after the CALL are the consumer — they read the matrix from `*actual` (or copy from it) and use it for something. That "something" is the software-skinning code we've been blind to throughout the D3D8 phase.

Goal: identify the code that reads the matrix, applies it to vertex data, and writes the result. Once that code is identified:
- We can patch it to detect when the high-Y signature is present and either skip the work or write a "collapse" output, providing a real upstream fix rather than the proxy mutation that didn't reach the GPU.
- Alternatively, we can patch the call to `0x61EA00` itself to short-circuit when the input state would produce the high-Y matrix.

### 10.3 Find the per-frame logic that decides Matrix 1 vs Matrix 2

The bone function at `0x0061EA00` is called twice per frame on the same ptr at the same call site (`0x00617455` → returns to `0x0061745A`), producing two genuinely different matrices. Something at the call site (or in the entity state passed to the function) determines which output. Find that decision point. Once identified:
- If a state flag controls the alternation, suppress that flag for whichever invocation produces Matrix 1.
- If two genuinely different entities are sharing the same memory location (the alternation is two distinct entities being processed sequentially), find the second entity and either prevent its existence or its rendering.

### 10.4 Tooling and methodology for this phase

- **Ghidra (free) for primary disassembly.** PSO PC V2 is 32-bit PE, no obfuscation, identifies as MSVC 6 build. Ghidra's decompiler is good enough on this generation of code to give pseudo-C output that's mostly readable.
- **Cheat Engine for runtime memory inspection.** Find the dragon entity struct, read its layout, map fields back to function arguments seen in disassembly.
- **Existing D3D8 proxy as a runtime probe.** The bone hook can be modified to log additional state at any entry point we want to instrument. Extending it for new diagnostic markers is cheap.
- **Community resources.** Schtserv, Ephinea, Sylverant, and Newserv all have GitHub orgs with PSO reverse-engineering notes. Worth a `site:github.com bm_boss1_dragon`-style search before from-scratch reading. Someone may have already mapped these functions even if they didn't ship a fix.

### 10.5 Memory-scan for dragon HP / state

PSO PC V2 enemy struct layout is documented in Sylverant/Newserv community sources. A direct memory read per Present can replace heuristic detection of dragon-alive / dragon-dying state with ground truth. This would clean up the DWIN state machine substantially and give exact phase markers without depending on chat.

### 10.6 Bug C parallel investigation

Independent of the high-Y matrix work:
- Force a `D3DCLEAR_ZBUFFER` immediately before suspected body-draw callers. If the clear-screen bleed disappears, Z-pollution is the root cause.
- Identify draws by texture set rather than caller address. Record texture pointers seen during early-fight calm period; they form the "dragon texture set." A draw is "dragon" if it binds a tex0 from that set.

---

## 11. Build & Run

### Build the proxy
```
i686-w64-mingw32-gcc -shared -O2 -o D3D8.dll d3d8_widescreen.c d3d8.def \
    -lkernel32 -Wl,--enable-stdcall-fixup
```

### Apply the binary patches
```
python3 apply_dragon_fix.py pso.exe          # apply (creates pso.exe.original backup)
python3 apply_dragon_fix.py pso.exe --verify # check state
python3 apply_dragon_fix.py pso.exe --revert # restore from backup
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
`pso-peeps-d3d8-wsh.log` in pso.exe's working directory. Wiped on each launch via `CREATE_ALWAYS`.

---

## 12. Source File Status

| File | Version | Notes |
|------|---------|-------|
| `d3d8_widescreen.c` | v16-lite-r9-v3.4 | Hooks SetTransform with high-Y detector, dragon-active draw correlation, Bug A particle suppression. No active suppression of bone matrices (proxy-layer matrix fix path closed). |
| `bone_hook.h` | v3.4 | `BONE_SUPPRESS_HIGH_Y=0` default. `[BONE-PRE]`, `[BONE-POST]`, `[BONE-XYZ]`, `[HIGHY-SEEN]`, `[BONE-DIAG]`, `[BONE-PTR]`, `[BONE STAT]` markers active. Dynamic Dragon ptr capture. |
| `apply_dragon_fix.py` | v5 | Four patches: A (fcomps redirect), B (je→jbe), C (16-write fallback as zero rotation + NOPed translation + 1.0 w), D (near-z scale 65535→0). |
| `pso.exe` | Patched per `apply_dragon_fix.py v5` | Verify with `apply_dragon_fix.py pso.exe --verify`. |
| `pso.exe.original` | Original | Reference for diff verification. Auto-created by `apply_dragon_fix.py` on first apply. |

---

## 13. Tooling References

- **Disassembly:** Ghidra (free) or IDA Pro. PSO PC V2 is 32-bit PE, no obfuscation, identifies as MSVC 6 build. PSOBB is similarly 32-bit PE with additional packed/protected sections for online security; the dragon-relevant code lives in plain `.text`.
- **Memory inspection:** Cheat Engine for finding dragon HP / entity struct addresses.
- **Binary patching:** `apply_dragon_fix.py` for the four current patches; HxD for ad-hoc inspection; runtime patches via `VirtualProtect` + `memcpy` in proxy DllMain or hook_CreateDevice.
- **D3D8 reference:** `IDirect3DDevice8` vtable layout — slot numbers in proxy patches are MSDN-documented but easy to drift; verify with dgvoodoo source.
- **Wine config:** Bruce runs on Wine. Proxy must work both native Windows and Wine.
- **Server-side correlation:** Newserv logs UTC. Proxy logs both UTC and LOCAL (Bruce is EDT). Phase markers via in-game chat appear in newserv log with UTC timestamps; align manually with proxy `[TIME] tick` lines.
- **Cross-version comparison:** `objdump -d -M intel` works on both V2 and BB binaries. Python `struct` + byte-pattern search is sufficient for finding equivalent code regions across versions when function addresses shift.

---

## 14. Investigation Methodology Notes

- **Working process:** Bruce directed the investigation, ran tests, reviewed results, and made final technical calls. LLMs were used as support tools for drafting code, reviewing logs, checking hypotheses, and organizing findings.
- **Pitfall observed:** Automatically generated or assistant-drafted detectors can be confidently wrong in ways that produce clean-looking-but-meaningless logs. The bone_hook v3 `degen` predicate checked for identity rotation when the actual bug was zero rotation — and produced "all clear" output that was misleading until reviewed. *Always check whether the detector can even see the thing it's supposed to detect.*
- **Pitfall observed:** Display formatting can lie. The original `(int)mat[i]` matrix dump made non-zero-but-small floats look exactly like zero, leading to a multi-week mischaracterization of which matrix was degenerate (Section 3.2). Whenever a hypothesis depends on a value being precisely zero (or precisely something), use a format that distinguishes.
- **Pitfall observed (v3.x):** A working mechanical mutation does not imply a reachable mutation. v3.2's high-Y suppression fired correctly 1888 times, with `mutation_took=1` confirmed in v3.3. The bug was unchanged because the consumer of `*actual` is software-skinning code that runs before our hook's mutation completes. *Before assuming a runtime hook can fix something, instrument the consumer side as well as the producer side.*
- **Pitfall observed (v3.x):** Coincidence is not causation. The high-Y matrix is suspicious (constant ty=2000, perfect alternation, single static rotation signature) but suppressing it had zero visible effect. Suspicious signals are correlations until proven causal. The methodology to verify causality at runtime is to instrument the consumer of the suspect data, not just the producer.
- **Test cycle cost:** Each diagnostic run is ~15 minutes setup + 5 minutes gameplay + 30+ minutes log analysis. Iteration is expensive. Code review before run > extra runs.
- **Bug families:** Decomposition is mandatory. "The dragon bug" is at least three bugs. Treating it as one was probably why prior community attempts didn't progress.
- **Cross-version binary comparison is high-yield as validation.** Comparing V2 against PSOBB took a few hours of static analysis and produced unambiguous validation that the bone matrix bug is real and unaddressed by Sonic Team. When investigating long-standing bugs in old games, compare against successor versions: same engine, possibly fixed bugs, possibly not. Either outcome — fix found or fix absent — is informative. The methodology is straightforward: identify a unique byte pattern in the original (constants, opcode sequences, distinctive control flow), search for the same pattern in the successor, and compare structures around hits.

---

## 15. Estimated Remaining Effort

| Outcome | Likelihood | Estimated additional hours |
|---------|------------|---------------------------|
| Reduced visible severity beyond current | ~80% | 15–30 |
| "Patch-server ready" — bug rare and mild | 50–65% | 40–80 |
| Complete elimination of all visual artifacts | 25–40% | 80–150+ |

Investigation has reached the steepest part of the value curve. Diagnostic harness is mature. Decomposition is done. Four binary patches landed. The D3D8 matrix-mutation path is now closed — that's a *narrowing* of the search space, not a setback. The remaining hours are concentrated on pso.exe disassembly with concrete addresses to anchor on (Section 5, Section 7.3). Ranges above are wider than prior estimates because pso.exe analysis depth varies more than D3D8 instrumentation depth did.

---

## 16. Pickup Notes for Resuming

When resuming:

1. **Read this document end-to-end first.** Sections 2.5 and 2.6 set the strategic frame: BB has the same broken code (no fix to backport), and the D3D8 / proxy layer cannot reach the rendering pipeline that produces the visible artifact (software skinning).
2. **Verify pso.exe patches are still applied:** `python3 apply_dragon_fix.py pso.exe --verify`. Should report `STATE: fully patched`.
3. **Verify proxy build works:** rebuild `D3D8.dll` from `d3d8_widescreen.c` and `bone_hook.h` v3.4 with the build command in Section 11. Run a quick game launch and confirm `[F00000] [BONE] hook v3.4 installed` appears in `pso-peeps-d3d8-wsh.log`.
4. **The next phase is pso.exe disassembly.** Open pso.exe in Ghidra. Anchor addresses in Section 5 ("Dragon-active draw callers (v3.4 catalog)") and Section 10. Start with `0x00637A90` (highest hit count) or `0x00617455` (the Dragon-primary bone caller) depending on which question feels more tractable.
5. **Don't expect BB to provide a fix.** BB-derived investigation paths are parked (Section 9). Useful for additional context if later phases want it.
6. **Don't try further post-return matrix-mutation strategies at the D3D8 layer.** Section 2.6 documents why this path is closed. Save those engineer-hours for code analysis.

---

## 17. Definitions / Glossary

- **Bug A / B / C** — the three identified bug families (Section 3).
- **DWIN** — "Death WINdow" state machine in proxy. Currently misnamed; actually detects "dragon outlier window." Renaming pending.
- **Matrix 1 / Matrix 2** — the two bone matrices that alternate per frame on the same ptr. Matrix 1 = high-Y ghost (`ty=2000`, rotation `[1,0,0 | 0,0,1 | 0,-1,0]`). Matrix 2 = visible Dragon body. Earlier versions of this document called these "Matrix A / Matrix B"; the new naming clarifies which one is suspicious. (Section 3.2.)
- **Matrix A / Matrix B** — deprecated names for Matrix 1 / Matrix 2. See note above.
- **High-Y / normal-Y** — the two equivalence classes the v3.x extractor sorted matrices into. High-Y has `|ty| > 1000` (always exactly 2000.0 in observed runs). Normal-Y is the visible-body class.
- **Software skinning** — CPU-side vertex transform. The bone matrix is consumed by CPU code that produces final-positioned vertices and writes them into a vertex buffer. D3D8 only sees post-skinned positions, never the bone matrix. PSO V2 inherits this convention from its Dreamcast predecessor.
- **H1 / H2 / H3** — the three hypotheses considered for "why doesn't matrix suppression fix the bug" in v3.3. H1 = mutation reaches GPU, but high-Y matrix isn't responsible for the visible artifact. H2 = mutation never reaches GPU because PSO is software-skinning. H3 = something restores the matrix between hook fires. v3.3 confirmed H2.
- **FVF** — D3D8 Flexible Vertex Format. `0x0144` = XYZRHW + diffuse + 1 tex coord set. `0x0142` = XYZ + diffuse + 1 tex coord set. `0x0112` = XYZ + diffuse + 1 tex coord set with a different layout.
- **XYZRHW** — pre-transformed vertex format (already in screen space, perspective divide already applied). Bypasses GPU vertex pipeline.
- **dgvoodoo** — D3D8/9-to-modern-API wrapper used as the actual D3D8 implementation. Proxy forwards real calls to `D3D8_dgvoodoo.dll`.
- **`[SUPPRESS]` / `[SUPPRESS-NEAR]`** — log tags for proxy suppression actions on outlier vertex buffer quads (Bug A).
- **`[BONE]` / `[BONE-DIAG]` / `[BONE-PRE]` / `[BONE-POST]` / `[BONE-XYZ]` / `[BONE-PTR]`** — bone hook log tags. See Section 6 for which fires when.
- **`[HIGHY-SUP]` / `[HIGHY-SEEN]`** — high-Y matrix events. `SUP` fires when suppression is on (v3.2/v3.3), `SEEN` when off (v3.4). First 10 of each include full detail.
- **`[SETXFORM-HIGHY]`** — fires if any matrix submitted to D3D8 SetTransform matches the high-Y signature. **Always 0** across all observed Dragon arena runs — the decisive evidence that PSO V2 is software-skinning.
- **`[DRAGON-SIG]`** — fires once per unique draw signature observed during `g_dragon_active`. v3.4 catalog.
- **`[DWIN-SIG]`** — draw signature accumulated during the death window, dumped on window close.
- **`[DRAWCTX]`** — full per-call draw context (caller, FVF, render states, tex pointers).
- **Caller / return address** — in this doc, `caller` usually means `__builtin_return_address(0)` — the address in pso.exe immediately after the CALL instruction. This is the address logged as `caller=...` or `caller0=...` in proxy output.
- **V2 vs BB** — V2 = Phantasy Star Online PC Version 2 (this investigation's target). BB = Phantasy Star Online Blue Burst (the next-generation client, used in Section 2.5 for cross-version verification).

---

*End of technical reference.*
