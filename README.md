# PSO PC V2 Dragon Corruption Bug — Technical Reference

<img width="1698" height="900" alt="image" src="https://github.com/user-attachments/assets/85e2ea75-bae6-4baa-a399-07e12e43d87e" />

**Disclaimer:** This report was prepared by me using LLMs (Sonnet, Opus, and ChatGPT) as research, drafting, review, and code-analysis assistants. I directed the investigation, ran the tests, validated results, and made the technical decisions. My intent is to either figure out a way to get this patched on my own, or have enough information archived that someone else with deeper knowledge can utilize this in the future.

**Status:** Working fix found and repeatedly validated in-game. The visible PC V2 Forest 2 Dragon corruption disappears when the client is prevented from selecting the PC-only `bm_boss1_dragon_b.bml` asset path by clearing bit `0x08000000` from `HKCU\Software\SonicTeam\PSOV2\CTRLFLAG0`.

**Target:** Phantasy Star Online PC V2 client  
**Last updated:** Post-root-cause registry / asset selector discovery.

**Current known-good tested baseline:**
- Wine prefix: `<WINEPREFIX>`
- Registry key: `HKCU\Software\SonicTeam\PSOV2`
- Previous bug-triggering value: `CTRLFLAG0 = 0x080b0004`
- Known-good value tested: `CTRLFLAG0 = 0x000b0004`
- Exact bit cleared: `0x08000000`
- `pso.exe` was still patched with `apply_dragon_fix.py` v5 during the successful test.
- D3D8 proxy was still present during the successful test.
- D3D8 proxy: `v16-lite-r9-v3.4`
- `bone_hook.h`: v3.4
- Widescreen mode under test: 14:9 viewport/bars. 16:9 introduces too much culling.
- Dragon was tested multiple times after the registry change. Manual mid-fight trigger attempts, death animation, and clear-screen phase all rendered correctly.

**Important isolation caveat:** The confirmed fix was tested on the current patched/proxied working baseline. The decisive variable changed was `CTRLFLAG0: 0x080b0004 → 0x000b0004`, which prevents selection of `bm_boss1_dragon_b.bml`. A future clean-room test should verify whether the registry bit clear alone fixes an unpatched, no-proxy client. The present conclusion is stronger than a cosmetic workaround but should not yet be described as fully isolated from all previous patches.

> **Note for the impatient reader (post-root-cause discovery):**
>
> The classic PSO PC V2 Dragon corruption was traced to a registry-controlled asset selector.
>
> `pso.exe` reads `HKCU\Software\SonicTeam\PSOV2\CTRLFLAG0`. If bit `0x08000000` is set, the Dragon loader selects `bm_boss1_dragon_b.bml`, a PC-only Dragon BML variant. On the tested setup, this path causes the long-standing Dragon texture / geometry corruption. Clearing only that bit forces the normal Dragon BML path and the visible bug disappears.
>
> The high-Y matrix / D3D8 / software-skinning investigation was still valuable: it ruled out several attractive but wrong fix paths and pushed the investigation into `pso.exe`, where the actual selector was found. But the practical fix is now the registry-controlled asset path, not D3D8 matrix mutation.

---

## Quick commands

### Check current PSO V2 registry flags under Wine

```bash
export WINEPREFIX="<WINEPREFIX>"

wine reg query 'HKCU\Software\SonicTeam\PSOV2'
```

Look for:

```text
CTRLFLAG0    REG_DWORD    0x80b0004
```

If `CTRLFLAG0` contains bit `0x08000000`, the client can select the PC-only Dragon B path.

### Apply the known-good Dragon fix for this setup

```bash
export WINEPREFIX="<WINEPREFIX>"

wine reg add 'HKCU\Software\SonicTeam\PSOV2' /v CTRLFLAG0 /t REG_DWORD /d 0x000b0004 /f
```

This clears the `0x08000000` bit from Bruce's observed value while preserving the remaining lower bits from `0x080b0004`.

### Restore the old bug-triggering value for controlled testing only

```bash
export WINEPREFIX="<WINEPREFIX>"

wine reg add 'HKCU\Software\SonicTeam\PSOV2' /v CTRLFLAG0 /t REG_DWORD /d 0x080b0004 /f
```

### Build the D3D8 proxy

```bash
i686-w64-mingw32-gcc -shared -O2 -o D3D8.dll d3d8_widescreen.c d3d8.def -lkernel32 -Wl,--enable-stdcall-fixup
```

### Apply / verify / revert pso.exe binary patches

```bash
python3 apply_dragon_fix.py pso.exe           # apply
python3 apply_dragon_fix.py pso.exe --verify  # check state
python3 apply_dragon_fix.py pso.exe --revert  # restore from .original backup
```

---

## 1. TL;DR

<img width="1698" height="900" alt="image" src="https://github.com/user-attachments/assets/93c27cf1-dbf1-46a0-b245-05f1e7f5b046" />

A long-standing visual corruption bug in the PC V2 Forest 2 Dragon fight was ultimately traced to a PC-only Dragon asset variant selected by a registry control flag.

The practical root-cause chain is:

```text
HKCU\Software\SonicTeam\PSOV2\CTRLFLAG0 = 0x080b0004
  → CTRLFLAG0 & 0x08000000 is true
  → pso.exe selects bm_boss1_dragon_b.bml
  → PC-only Dragon BML path is used
  → Dragon renders with severe texture / geometry corruption

HKCU\Software\SonicTeam\PSOV2\CTRLFLAG0 = 0x000b0004
  → CTRLFLAG0 & 0x08000000 is false
  → pso.exe avoids bm_boss1_dragon_b.bml
  → normal Dragon BML path is used
  → visible Dragon corruption disappears in repeated tests
```

Earlier diagnostic work decomposed the visible bug into three symptom families:

- **Bug A — XYZRHW Particle Near-Z Explosion.** Largely contained at the D3D8 proxy layer plus pso.exe Patch D.
- **Bug B — Matrix Alternation / High-Y Ghost Transform.** Characterized in detail; D3D8 matrix mutation confirmed ineffective because the Dragon path is software-skinned.
- **Bug C — Clear-Screen Body Bleed.** Previously unresolved as a D3D8 symptom, but disappeared in the known-good test once the `bm_boss1_dragon_b.bml` path was avoided.

**Current result:** With `CTRLFLAG0 = 0x000b0004`, Dragon was tested repeatedly. Attempts to trigger the mid-fight corruption failed; death animation and clear-screen rendering were clean. The visible bug is fixed in the current baseline.

**Most important artifact-side finding:** After correctly calibrating Dreamcast GD-ROM LBA extraction, the normal Dreamcast V2 Dragon assets match the PC V2 normal Dragon assets byte-for-byte. The exception is `bm_boss1_dragon_b.bml`, which exists on PC and is referenced by both original and patched `pso.exe`, but was not present on the tested Dreamcast V2 disc.

**Most important code-side finding:** `pso.exe` contains a Dragon BML selector near `0x004202f0`. The `_b` path is reached through a runtime flag check equivalent to:

```c
if (CTRLFLAG0 & 0x08000000) {
    load bm_boss1_dragon_b.bml;
} else {
    load bm_boss1_dragon.bml;
}
```

There is also an earlier special path that appears to select `bm_boss1_dragon_a.bml` under a separate condition.

---

## 2. User-visible symptoms and current state

The bug could be triggered manually mid-fight or manifest during Dragon's death sequence.

### Pre-fix behavior

- Dragon turns pink or visually corrupt.
- Textures / particles / geometry bleed across the entire map.
- Loot boxes near Dragon contaminate visually.
- Post-death / clear-screen phase strobes / flashes violently.
- Objects intersecting Dragon body show texture bleed.
- Dragon head/neck can appear stuck on the ground while the rendered body rises.
- Standing in or near the head causes strobing / texture bleed.
- Camera-dependent behavior: when part of the Dragon body goes out of frame, the bug can stop.

### State before root-cause discovery, with pso.exe patches + D3D8 proxy only

- Catastrophic strobing from Bug A was largely contained.
- The manual mid-fight trigger still worked.
- Clear-screen / post-death texture bleed still occurred.
- Dragon could still die pink.
- Texture bleed still appeared when Dragon body intersected player or loot boxes during clear-screen.
- Matrix-mutation attempts at the D3D8 proxy layer did not improve the visible Dragon corruption.

### Current known-good state after `CTRLFLAG0` fix

After changing:

```text
CTRLFLAG0 = 0x080b0004
```

to:

```text
CTRLFLAG0 = 0x000b0004
```

Bruce ran Dragon multiple times and deliberately tried to trigger the bug mid-fight. The visible bug did not reproduce. Death animation and clear-screen phase rendered correctly.

### Confirmed ineffective or superseded approaches

- Zero-rotation suppression of the high-Y matrix at the bone hook return point.
- Translation-preserving identity fallback for the high-Y matrix.
- Any D3D8 proxy-layer matrix-mutation strategy.
- Direct BB asset transplant into V2.
- Direct BB Dragon-family BML swap.
- Direct BB arena / map replacement.
- Dreamcast core Dragon asset swap, because calibrated extraction proved the normal core Dragon files are already identical between Dreamcast V2 and PC V2.

---

## 2.5 Cross-version verification: Blue Burst comparison

PSOBB (Phantasy Star Online Blue Burst) is built on top of the V2→V3 codebase; BB is V4. Episode 1 content, including the Forest 2 Dragon, was carried forward. Static comparison between V2 and BB remains useful context, even though the final fix was not BB-derived.

### What's identical between V2 and BB

The bone-matrix degenerate-case code from Bug B is structurally unchanged across releases:

| Item | V2 location | BB location | Status |
|------|-------------|-------------|--------|
| FLT_MAX fallback: 16 writes of `0x7F7FC99E` | `0x0061EFD2` onwards | `0x0083CCDD` onwards | Identical structure |
| Guard `fcom; fnstsw; sahf; je → fallback` | `0x0061EA1D` | `0x0083C698` | Identical opcode sequence |
| Epsilon constant for comparison | `0.0f` at `0x007C1674` | `0.0f` at `0x0098AF70` | Same value |
| Divisor constant for `1/x` | `1.0f` at `0x007C1670` | `1.0f` at `0x0098AF68` | Same value |
| Null-input early-out | `test esi,esi; je null_handler` | `test esi,esi; je null_handler` | Identical |

Sonic Team shipped this code twice without changing it.

### What's different between V2 and BB

The per-mesh culling system was rewritten:

- V2's per-mesh cone-setup pattern at `0x00441EDE` does not exist anywhere in BB.
- The V2 cone-setup function at `0x00619100` has no signature equivalent in BB.
- BB has a graduated float table at `0x0090332C–0x00903364`, but closer inspection indicates that table is read by `fdiv` instructions, not cone tests. The value match to our frustum calibration is likely coincidental.

### What this means after the registry / asset-selector fix

The BB comparison was not the fix, but it was useful because it proved there was no obvious "fixed Sonic Team version" of the bone-matrix code to backport. The final fix came from PC V2's own asset-selection logic, not from BB.

BB-derived investigation paths are now parked unless someone wants to answer a different historical question: why BB does not visibly show this same bug in ordinary play despite retaining the same bone-matrix fallback code.

---

## 2.6 D3D8 matrix-mutation endpoint

**Conclusion:** The D3D8 proxy layer cannot deliver a structural fix for the bone-matrix side of the Dragon corruption by mutating matrices after the bone-hook return point. The mutation is mechanically correct, but the renderer never reads from that path.

### Evidence

Across v3.1 through v3.4 of `bone_hook.h`, the proxy narrowed the set of viable D3D8-layer interventions until only one remained: mutate the high-Y matrix at the return point of `0x61EA00`.

v3.2 implemented suppression. v3.3 added three independent diagnostics:

- `[BONE-POST]` confirmed the write landed in `*actual` (`mutation_took=1` in 100% of relevant samples).
- `[BONE-PRE]` confirmed nothing restored the high-Y matrix between calls (`matches_pre_high_y=0` in 100% of samples).
- `[SETXFORM-HIGHY]` watched for any D3D8 `SetTransform` call with the high-Y signature. `setxform_highy_seen_total = 0` across a full Dragon arena run.

The third marker is decisive. The high-Y matrix is never submitted to D3D8 `SetTransform`. PSO V2 software-skins the Dragon on the CPU: the bone matrix is consumed by CPU code that produces already-transformed vertices. D3D8 sees post-skinned vertex buffers, not the original bone matrix.

### Current interpretation after the final fix

The D3D8 endpoint remains valid. It explains why the proxy could not fix the Dragon by mutating matrix output. But it was not the final root-cause layer. The decisive layer was earlier in the resource-selection path: the client was choosing `bm_boss1_dragon_b.bml`, a PC-only Dragon package whose rendering path corrupts on the tested setup.

D3D8 diagnostics still matter because they mapped the symptoms and proved that visible corruption was downstream of asset / CPU-side processing, not a simple D3D transform submission error.

---

## 2.7 Dreamcast / PC asset comparison and final root-cause discovery

This is the section that turned the investigation.

### Initial false signal

An initial raw extraction from the Dreamcast Track 3 ISO appeared to show the Dreamcast Dragon files were wildly different from PC V2: same sizes but different hashes and random-looking first bytes.

That result was wrong. The file records were real, but the data offsets were not calibrated for the Dreamcast / GD-ROM layout.

### Calibration

Known-good files from the earlier Dreamcast-to-PC hair texture work were used to calibrate extraction:

- `PLATEX.AFS`
- `PLZSMPNJ.AFS`

Both exact blobs were found in the ISO. The directory-record nominal offsets differed from the real blob positions by the same amount:

```text
selected_delta = -0x57E4000 (-92160000)
```

After applying this delta, named file extraction produced sane BML/PVM headers and correct hashes.

### Calibrated Dreamcast vs PC result

After calibration, these files are byte-identical between the tested Dreamcast V2 disc and PC V2:

| File | Result |
|------|--------|
| `BM_BOSS1_DRAGON.BML` / `bm_boss1_dragon.bml` | Same size, same SHA256 |
| `BM_BOSS1_DRAGON_A.BML` / `bm_boss1_dragon_a.bml` | Same size, same SHA256 |
| `BM_OBJ_BOSS1_COMMON.BML` / `bm_obj_boss1_common.bml` | Same size, same SHA256 |
| `BM_OBJ_BOSS1_COMMON_A.BML` / `bm_obj_boss1_common_a.bml` | Same size, same SHA256 |
| `OBJ_BOSS1_COMMON.PVM` / `obj_boss1_common.pvm` | Same size, same SHA256 |
| `OBJ_BOSS1_COMMON_A.PVM` / `obj_boss1_common_a.pvm` | Same size, same SHA256 |
| `BOSSOP_CAM_INT1.BML` / `bossop_cam_int1.bml` | Same size, same SHA256 |
| `MAP_FOREST02D.DAT` / `map_forest02d.dat` | Same size, same SHA256 |
| `MAP_FOREST02AD.DAT` / `map_forest02ad.dat` | Same size, same SHA256 |

`GSL_FOREST02.GSL` differs in size/hash, but the Dragon-specific core BML/PVM/map assets listed above match exactly.

### The PC-only anomaly

The tested Dreamcast disc did **not** contain:

```text
BM_BOSS1_DRAGON_B.BML
```

PC V2 does contain:

```text
bm_boss1_dragon_b.bml
```

PC `_b` file properties:

```text
size     = 1,818,016 bytes
sha256   = ccdcfe8f7739e51aaca149e65b963c1c0e1cf794e09475699d4f7db7dbd841f5
entries  = 29
main NJ  = boss1_s_dragon.nj
flags    = mostly 0x200
NMDM hits = 27
```

For comparison:

```text
bm_boss1_dragon.bml
  size   = 808,288 bytes
  main NJ = boss1_s_nb_dragon.nj
  flags  = mostly 0x38

bm_boss1_dragon_a.bml
  size   = 746,368 bytes
  main NJ = boss1_s_nb_dragon.nj
  flags  = mostly 0x30
```

The `_b` file is not junk. It is a real PC V2 Dragon BML family package with different flags, a different main model name, much larger embedded data, and an explicit reference in both original and patched `pso.exe`.

### pso.exe selector discovery
The string `bm_boss1_dragon_b.bml` has a single xref at:

```text
0x00420328
```
<img width="1528" height="928" alt="02_ghidra_dragon_bml_string_xref" src="https://github.com/user-attachments/assets/135e507d-e17e-49ed-9669-53baf59917f2" />  
This is inside Dragon BML selection logic around:

```text
0x004202f0
```

Observed control flow:
```asm
00420317  PUSH 0x08000000
0042031c  CALL FUN_00566c20
00420321  ADD ESP,0x4
00420324  TEST EAX,EAX
00420326  JZ LAB_0042033c

00420328  PUSH bm_boss1_dragon_b.bml
0042032d  PUSH bm_boss1_dragon.bml
00420332  CALL FUN_004947b0
```

The helper chain resolves as:

```text
FUN_00566c20(0x08000000)
  → FUN_0061BE10(0x08000000, 0)
    → FUN_006402F0(0x08000000, 0)
```

`FUN_006402F0` decompiles to the equivalent of:

```c
if ((param_2 < 4) && ((param_1 & (&DAT_007b8ce0)[param_2]) != 0)) {
    return 1;
}
return 0;
```

With `param_2 == 0`, this checks:

```c
CTRLFLAG0 & 0x08000000
```

<img width="1628" height="940" alt="03_ghidra_ctrlflag0_bitmask_check" src="https://github.com/user-attachments/assets/957184d7-16df-4ea4-89f1-8e24c4711a0c" />

### Registry source

`` is initialized from the registry:

```text
FUN_0063FD30("CTRLFLAG0", &)
FUN_0063FD30("CTRLFLAG1", &DAT_007b8ce4)
FUN_0063FD30("CTRLFLAG2", &DAT_007b8ce8)
FUN_0063FD30("CTRLFLAG3", &DAT_007b8cec)
```

`FUN_0063FD30` uses `RegQueryValueExA`.

The registry handle is opened under:

```text
HKEY_CURRENT_USER\Software\SonicTeam\PSOV2
```

Therefore:

```text
HKCU\Software\SonicTeam\PSOV2\CTRLFLAG0
```

controls whether `bm_boss1_dragon_b.bml` is selected.

### Final tested fix

Bruce's registry value before the fix:

```text
CTRLFLAG0 = 0x080b0004
```

This contains the selector bit:

```text
0x080b0004 & 0x08000000 != 0
```

Known-good value after fix:

```text
CTRLFLAG0 = 0x000b0004
```

This clears the selector bit:

```text
0x000b0004 & 0x08000000 == 0
```

After this change, repeated Dragon tests were clean.

---

## 3. Final bug model

### 3.1 Original decomposition

The visible Dragon corruption was originally decomposed into three families because it behaved like multiple interacting bugs:

- **Bug A:** XYZRHW particle near-Z explosion.
- **Bug B:** Matrix alternation / high-Y ghost transform.
- **Bug C:** Clear-screen body bleed.

That decomposition was productive. It led to binary patches, the D3D8 proxy diagnostic harness, and the eventual pso.exe investigation.

### 3.2 Updated root cause for the visible Dragon corruption

The strongest current model is:

```text
PC V2 has at least three Dragon BML variants:
  bm_boss1_dragon.bml    normal path; Dreamcast-equivalent
  bm_boss1_dragon_a.bml  alternate path; Dreamcast-equivalent
  bm_boss1_dragon_b.bml  PC-only path; selected by CTRLFLAG0 bit 0x08000000

The visible classic Dragon corruption occurs when the PC-only _b path is active.
```

The exact internal meaning of `CTRLFLAG0 & 0x08000000` is not yet named. It is likely a graphics/control capability bit. In practice it behaves like a switch for the PC-only Dragon BML path.

### 3.3 What the `_b` file probably is

`bm_boss1_dragon_b.bml` appears to be a PC-specific Dragon package, possibly intended for an enhanced PC graphics mode or alternate rendering capability.

Evidence:

- It is referenced by original `pso.exe`, not introduced by our patches.
- It is selected by a registry `CTRLFLAG0` bit.
- It is much larger than the normal Dragon BMLs.
- Its main model is `boss1_s_dragon.nj`, while normal/A use `boss1_s_nb_dragon.nj`.
- Its table flags are mostly `0x200`, while normal/A use mostly `0x38` / `0x30`.
- It was not found on the tested Dreamcast V2 disc.
- Preventing the client from selecting it fixes the visible corruption.

### 3.4 What remains unknown

The fix is real, but the exact semantic label for `0x08000000` is still unknown. It may represent a rendering feature, memory/capability tier, or a PC graphics setting.

Also unknown: whether `bm_boss1_dragon_b.bml` is intrinsically bad, or whether it is only bad when combined with modern Wine / dgvoodoo / widescreen / GPU behavior. The safest operational conclusion is that the `_b` path should be disabled for PSO Peeps PC V2 builds unless a future test proves otherwise.

---

## 4. Registry / asset-path fix

### 4.1 Registry key

```text
HKEY_CURRENT_USER\Software\SonicTeam\PSOV2
```

Value:

```text
CTRLFLAG0
```

Bug-triggering tested value:

```text
0x080b0004
```

Known-good tested value:

```text
0x000b0004
```

Conceptual fix:

```c
CTRLFLAG0 &= ~0x08000000;
```

### 4.2 Wine command for Bruce's prefix

```bash
export WINEPREFIX="<WINEPREFIX>"

wine reg add 'HKCU\Software\SonicTeam\PSOV2' /v CTRLFLAG0 /t REG_DWORD /d 0x000b0004 /f
```

### 4.3 Verification

```bash
export WINEPREFIX="<WINEPREFIX>"

wine reg query 'HKCU\Software\SonicTeam\PSOV2'
```

Expected relevant output:

```text
CTRLFLAG0    REG_DWORD    0xb0004
CTRLFLAG1    REG_DWORD    0x6
CTRLFLAG2    REG_DWORD    0x0
CTRLFLAG3    REG_DWORD    0x0
```

### 4.4 Restore for regression testing

```bash
export WINEPREFIX="<WINEPREFIX>"

wine reg add 'HKCU\Software\SonicTeam\PSOV2' /v CTRLFLAG0 /t REG_DWORD /d 0x080b0004 /f
```

Restoring this value should re-enable the `_b` path and is useful for A/B testing, but should not be used for normal play.

### 4.5 Operational recommendation

For PSO Peeps PC V2 distributions, launchers, or patch scripts:

1. Read `HKCU\Software\SonicTeam\PSOV2\CTRLFLAG0`.
2. Clear bit `0x08000000`.
3. Write the value back.
4. Optionally log the before/after values.

Do not blindly set `CTRLFLAG0` to `0x000b0004` for every user unless the intent is to reproduce Bruce's exact known-good baseline. A generic tool should preserve unrelated bits:

```c
new_value = old_value & ~0x08000000;
```

---

## 5. Binary patches applied to pso.exe

These patches were part of the successful tested baseline. They are still documented because they may mitigate separate edge cases, especially Bug A / FLT_MAX behavior. They should not be confused with the final Dragon asset-path fix.

Four patches are applied via `apply_dragon_fix.py` v5. All four were verified with `apply_dragon_fix.py --verify` and by `cmp(1)` diff against `pso.exe.original`.

### 5.A — fcomps comparison redirect

**Address:** `0x0061EA1D`  
**Bytes changed:** 4 of 6 in window  
**Original:** `D8 1D 74 16 7C 00` (`fcom dword ptr [0x007C1674]`, comparing against literal `0.0f`)  
**Patched:** `D8 1D F0 D4 64 00` (`fcom dword ptr [0x0064D4F0]`, comparing against `0.5f`)

Redirects the matrix-inverse guard's epsilon comparison to a non-zero threshold. Combined with Patch B, this widens the set of inputs that route through the safe fallback rather than the original FLT_MAX path.

### 5.B — Conditional branch widening

**Address:** `0x0061EA27`  
**Bytes changed:** 1  
**Original:** `0F 84 ...` (`je rel32`)  
**Patched:** `0F 86 ...` (`jbe rel32`)

`je → jbe` widens the conditional jump, routing more degenerate inputs through the fallback path.

### 5.C — Degenerate bone fallback

**Address:** `0x0061EFD2` through `0x0061F040`  
**Purpose:** Replace FLT_MAX-fill matrix fallback with a position-preserving zero-rotation collapse.

Original behavior was 16 writes of `0x7F7FC99E` into the 4×4 matrix.

v5 behavior:

- Rotation diagonal slots `[0][0]`, `[1][1]`, `[2][2]`: write `0.0f`.
- Other rotation slots: write `0.0f`.
- Translation slots `tx / ty / tz`: NOPed out so the caller's translation survives.
- Homogeneous `w` slot: write `1.0f`.

Net fallback output:

```text
[0,0,0,0 | 0,0,0,0 | 0,0,0,0 | tx,ty,tz,1]
```

### 5.D — Perspective divide near-Z fallback

**Address:** `0x00634A48`  
**Bytes changed:** 4  
**Original:** `00 FF 7F 47` (`65535.0f`)  
**Patched:** `00 00 00 00` (`0.0f`)

When a perspective divide hits view-space Z near zero, this prevents a screen-spanning scale from being returned.

### Verification

```bash
python3 apply_dragon_fix.py pso.exe --verify
```

Expected:

```text
STATE: fully patched
```

### Current interpretation of these patches after the registry fix

The registry fix is the decisive visible Dragon fix. The binary patches are still useful documentation and may reduce catastrophic fallback behavior elsewhere. A future isolation matrix should test:

| Test | pso.exe patches | D3D8 proxy | CTRLFLAG0 bit `0x08000000` | Expected / purpose |
|------|-----------------|------------|-----------------------------|--------------------|
| Current known-good | Applied | Present | Cleared | Already validated clean |
| Registry-only clean test | Reverted | Removed/disabled | Cleared | Determines if registry fix alone is sufficient |
| Regression test | Applied | Present | Set | Confirms `_b` path reintroduces bug |
| Stock reference | Reverted | Removed/disabled | Set | Confirms original bug baseline |

---

## 6. Key code addresses in pso.exe

PE image base: `0x00400000`

### Sections

- `.text`: `0x0040E000`, vsize `0x23ED70`
- `.rdata`: `0x0064D000`, vsize `0x2AAB6`
- `.data`: `0x00678000`, vsize `0x148560`
- `.data1`: `0x007C1000`, vsize `0x2420`

### Dragon asset selector / registry path

| Address | Description |
|---------|-------------|
| `0x004202f0` | Dragon BML variant selector region |
| `0x00420328` | Pushes `bm_boss1_dragon_b.bml` when `CTRLFLAG0 & 0x08000000` is true |
| `0x0042033c` | Non-`_b` path target after `JZ` from the flag check |
| `0x00496230` | Earlier special-condition gate; returns true if `DAT_00691e40 != 0 && DAT_006f7b04 == 3` |
| `0x00566c20` | Wrapper for flag check |
| `0x0061BE10` | Wrapper to `0x006402F0` |
| `0x006402F0` | Generic control-flag bit test: checks `param_1 & (&)[param_2]` |
| `0x007b8ce0` | Runtime storage for `CTRLFLAG0` |
| `0x007b8ce4` | Runtime storage for `CTRLFLAG1` |
| `0x007b8ce8` | Runtime storage for `CTRLFLAG2` |
| `0x007b8cec` | Runtime storage for `CTRLFLAG3` |
| `0x00640260` | Initializes control flag globals and reads `CTRLFLAG0`–`CTRLFLAG3` |
| `0x0063FD30` | Calls `RegQueryValueExA` to read a named registry value |
| `0x0063FCA0` | Opens registry chain under `HKEY_CURRENT_USER\Software\SonicTeam\PSOV2` |
| `0x006d8e80` | ASCII string `PSOV2` |

### Bone / matrix / historical diagnostic addresses

| Address | Description | Status |
|---------|-------------|--------|
| `0x0061EA00` | Matrix-inverse guard / bone fn, called from multiple sites | Patched at +0x1D, +0x27, +0x1D2..+0x240 |
| `0x00617455` | Primary Dragon bone fn call site, returns to `0x0061745A` | Hooked in D3D8 proxy |
| `0x00617535` | Secondary Dragon bone fn call site, returns to `0x0061753A` | Hooked in D3D8 proxy |
| `0x00634A30` | Perspective divide near-Z fallback | Patched at +0x18 |
| `0x00638360` | Second perspective-divide function shared with fades | Not patched |
| `0x00619100` | V2 cone-setup / culling function | Not present in BB |

### Dragon-active draw callers from v3.4

These were useful for the pre-root-cause pso.exe pivot and remain documented for historical context.

| Address | Hits in death window | Draw type | Notes |
|---------|----------------------|-----------|-------|
| `0x00637A90` | 196 | DP | Highest-volume death-window caller |
| `0x006405E0` | 122 | DP | Known XYZRHW particle path candidate |
| `0x00639F2A` | 19 | DP | Suspected Dragon body draw caller |
| `0x0063691F` | 4 | UP | Low-frequency `DrawPrimitiveUP` traffic |
| `0x00637BC0` | 2 | DP | Clear-screen-phase candidate |
| `0x0063694B` | 1 | UP | One-shot UP candidate |

After the registry fix, these are no longer immediate fix targets for the visible Dragon corruption, but they remain useful if someone wants to understand rendering internals.

---

## 7. D3D8 proxy architecture

Current build documented here: `v16-lite-r9-v3.4`.

### File layout

```text
pso.exe                       ← patched binary in current known-good baseline
pso.exe.original              ← unpatched reference
D3D8.dll                      ← proxy built from d3d8_widescreen.c
D3D8_dgvoodoo.dll             ← real D3D8 wrapper
widescreen_res.cfg            ← INI config
pso-peeps-d3d8-wsh.log        ← output log
apply_dragon_fix.py           ← pso.exe binary patcher
```

### Hooks installed

- `IDirect3D8` vtable slot 15: `CreateDevice`
- `IDirect3DDevice8` slots: Present, CreateVertexBuffer, Clear, SetTransform, SetViewport, GetViewport, SetRenderState, SetTexture, DrawPrimitive, DrawIndexedPrimitive, DrawPrimitiveUP, SetVertexShader, SetStreamSource
- `IDirect3DVertexBuffer8` slots: Lock, Unlock
- Seven direct CALL-site rewrites in `pso.exe` redirect bone function calls to `hook_bone_fn`

### Current interpretation

The proxy is not the root Dragon fix. It remains useful for:

- Bug A particle suppression.
- Widescreen viewport/bars work.
- Diagnostics.
- Future controlled A/B runs.

Matrix mutation at this layer should not be pursued for the Dragon corruption fix; see Section 2.6.

---

## 8. Diagnostic findings

### 8.1 Original reference run

Frame ↔ UTC mapping:

| Frame | UTC | Bruce's narration |
|-------|-----|-------------------|
| F00000 | 16:53:23 | Proxy startup |
| F00516 | 16:53:47 | DWIN false-fired in SHIP/lobby |
| F01380 | 16:54:24 | Room creation |
| F02376 | 16:54:57 | First "JUMPf" matrix delta = 1920 |
| F03420 | 16:55:33 | Dragon arena entered |
| F04646–F04750 | 16:56:12–16:56:18 | Suppress hot zone |
| F05744+ | 16:56:22+ | Persistent matrix alternation continues |

Findings:

- Early DWIN detector was broken and opened in lobby.
- Matrix dump used integer truncation, causing small rotations to look like zero.
- This run led to better matrix formatting and bone hook v3.x.

### 8.2 v3.3 D3D8-layer endpoint

Hypothesis: high-Y matrix suppression at the bone hook return would propagate to rendering.

Results:

- `[BONE-POST] mutation_took=1` in 100% of relevant samples.
- `[BONE-PRE] matches_pre_high_y=0` in 100% of relevant samples.
- `[SETXFORM-HIGHY]` count was 0 across the whole arena.
- Visual bug unchanged or worse.

Conclusion: D3D8 never receives the high-Y matrix through `SetTransform`; PSO software-skins the Dragon on the CPU.

### 8.3 v3.4 final D3D8 capture

Purpose: capture Dragon-active draw signatures for pso.exe analysis.

Results:

- `setxform_highy_seen_total=0`
- `high_y_suppressed_total=0`
- `128` unique `[DRAGON-SIG]` entries
- `344` `[DWIN-SIG]` entries
- `18576` `[DRAWCTX]` entries

This was a successful diagnostic run, not a fix. The fix came later by following the pso.exe / asset selector path.

### 8.4 Dreamcast extraction / asset investigation

The Dreamcast extraction path initially produced false differences due to GD-ROM LBA offset mismatch. Calibration with known-good `PLATEX.AFS` and `PLZSMPNJ.AFS` fixed extraction. After calibration, the normal Dragon assets are byte-identical between Dreamcast V2 and PC V2.

The only major PC-only Dragon asset found was `bm_boss1_dragon_b.bml`.

### 8.5 Ghidra registry selector investigation

Ghidra was used to:

1. Locate `bm_boss1_dragon_b.bml`.
2. Follow its xref to `0x00420328`.
3. Identify the selector around `0x004202f0`.
4. Follow the flag-check chain:
   - `FUN_00566c20`
   - `FUN_0061BE10`
   - `FUN_006402F0`
5. Identify `` as `CTRLFLAG0`.
6. Follow registry initialization through `FUN_0063FD30`.
7. Identify the registry path:
   - `HKEY_CURRENT_USER`
   - `Software`
   - `SonicTeam`
   - `PSOV2`

This directly produced the tested fix.

---

## 9. Failed / superseded approaches

| Approach | Result |
|----------|--------|
| Patch `0x00638360` globally to write `0.0f` instead of `65535.0f` | Broke screen fades and Forest 2 environmental lighting. Reverted. |
| Use return-address discriminator at `0x006382FE` / `0x00638303` to distinguish fades from particles | Same return address used by both. Cannot disambiguate without more context. |
| Pure identity matrix fallback at the matrix guard | Erased translation; bad component rendered at world origin. |
| Translation-preserving identity fallback at Patch C | Improved scope but left bad mesh alive at world position. |
| `(int)mat[i]` matrix dump format | Hid non-zero fractional rotations. Replaced with `(int)(mat[i]*10000)`. |
| Bone hook v3 `degen` predicate checking identity rotation | Wrong direction. It checked for identity, not zero rotation. |
| Use early `g_death_window` as suppression gate | DWIN opened too broadly. Would suppress normal fight draws. |
| High-Y matrix zero-rotation suppression at bone hook return | Mutation landed but visible bug unchanged. Matrix never reaches D3D8 `SetTransform`. |
| Any D3D8 proxy-layer matrix mutation | Wrong layer; CPU software skinning consumes matrix before proxy mutation can matter. |
| BB-derived binary search for an upstream fix | BB has same relevant bone-matrix fallback code. No free fix. |
| Direct BB Dragon BML family swap | Crashed on arena load. BB asset/package format is too different for direct transplant. |
| Direct BB arena / map replacement | Crashed / not viable. |
| Dreamcast normal Dragon asset swap | Superseded. After calibrated extraction, normal DC and PC Dragon assets are already byte-identical. |

---

## 10. Open questions and next steps

### 10.1 Immediate packaging / validation work

1. **Make a small fix script** that clears `CTRLFLAG0 & 0x08000000` while preserving all other bits.
2. **Run an isolation matrix**:
   - Current known-good baseline.
   - Stock `pso.exe` + no proxy + bit cleared.
   - Patched/proxy baseline + bit restored.
   - Stock original behavior with bit restored.
3. **Document whether registry-only is sufficient** for a clean PSO PC V2 install.
4. **Verify whether the official PSO PC launcher or options tool can re-set `CTRLFLAG0`** and re-enable the `_b` path.
5. **Consider a launcher-side guard** that clears the bit every launch.

### 10.2 Binary patch option

Instead of relying on registry state, a patcher could alter the selector at `0x00420317–0x00420332` so the `_b` path is never selected.

Possible approaches:

- Force the `JZ LAB_0042033c` behavior regardless of `FUN_00566c20`.
- Replace the `_b` string pointer with normal `bm_boss1_dragon.bml`.
- Patch `FUN_006402F0` only for the `0x08000000, 0` call site. This is riskier because the same function is a generic control-flag checker.

Registry fix is preferred operationally because it is reversible, transparent, and does not require modifying `pso.exe`.

### 10.3 Identify semantic meaning of `CTRLFLAG0 & 0x08000000`

Still unknown. Likely possibilities:

- Graphics detail tier.
- Hardware capability bit.
- Memory/capability bit from original PC configuration.
- "Enhanced Dragon / high detail Dragon" asset flag.
- Renderer path selector.

To identify it, search for other checks of `0x08000000` through `FUN_006402F0` or direct accesses to `DAT_007b8ce0`.

### 10.4 Determine whether `_b` is intrinsically broken

Questions:

- Does `_b` corrupt only on Wine/dgvoodoo?
- Does `_b` corrupt on native Windows with original Direct3D 8 hardware/API?
- Does `_b` corrupt only with widescreen/culling changes?
- Does `_b` corrupt with unpatched `pso.exe` but stock rendering?
- Does `_b` depend on another registry flag being paired with it?

These are not needed for the practical fix but are useful for historical completeness.

### 10.5 Preserve the diagnostic harness

Keep the D3D8 proxy and `apply_dragon_fix.py` documented. They are no longer the primary Dragon fix, but they remain valuable for:

- Widescreen work.
- Particle near-Z suppression.
- Future reverse-engineering work.
- Reproducing the investigation.

---

## 11. Build & run

### Build the proxy

```bash
i686-w64-mingw32-gcc -shared -O2 -o D3D8.dll d3d8_widescreen.c d3d8.def \
    -lkernel32 -Wl,--enable-stdcall-fixup
```

### Apply binary patches

```bash
python3 apply_dragon_fix.py pso.exe           # apply; creates pso.exe.original on first apply
python3 apply_dragon_fix.py pso.exe --verify  # check state
python3 apply_dragon_fix.py pso.exe --revert  # restore from backup
```

### Apply the registry fix

```bash
export WINEPREFIX="<WINEPREFIX>"

wine reg add 'HKCU\Software\SonicTeam\PSOV2' /v CTRLFLAG0 /t REG_DWORD /d 0x000b0004 /f
```

### Config file: `widescreen_res.cfg`

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

- `D3D8.dll`
- `D3D8_dgvoodoo.dll`
- `widescreen_res.cfg`
- `apply_dragon_fix.py`

The proxy's `Direct3DCreate8` export forwards to `LoadLibraryA("D3D8_dgvoodoo.dll")`.

---

## 12. Source file status

| File | Version / status | Notes |
|------|------------------|-------|
| `d3d8_widescreen.c` | `v16-lite-r9-v3.4` | D3D8 proxy, widescreen, Bug A suppression, Dragon diagnostics |
| `bone_hook.h` | `v3.4` | Bone hook diagnostics. `BONE_SUPPRESS_HIGH_Y=0` in final diagnostic run |
| `apply_dragon_fix.py` | `v5` | Four pso.exe patches A/B/C/D |
| `pso.exe` | Patched in current known-good baseline | Verify with `apply_dragon_fix.py pso.exe --verify` |
| `pso.exe.original` | Original reference | Created by patcher on first apply |
| Wine registry | Known-good `CTRLFLAG0=0x000b0004` | Decisive Dragon fix in current baseline |

---

## 13. Tooling references

- **Ghidra:** Primary pso.exe disassembly tool. PSO PC V2 is 32-bit PE, no obfuscation, MSVC-era code.
- **Wine registry tools:** `wine reg query`, `wine reg add`, and direct `user.reg` backups.
- **D3D8 proxy:** Diagnostic and widescreen harness.
- **Python scripts:** Used for BML table dumps, raw ISO scans, GD-ROM LBA calibration, hash comparison, and binary patching.
- **Dreamcast extraction:** Generic ISO extractors failed on the Dreamcast disc. Correct path was raw directory-record parsing plus calibrated LBA delta from known-good AFS blobs.
- **Ghidra labels created during investigation:**
  - `dragon_primary_bone_call_wrapper` at `0x00617430`
  - `dragon_secondary_bone_call_wrapper` at `0x00617510`
  - `dragon_bone_matrix_function` at `0x0061EA00`
  - `0x004202f0` should be labeled `load_dragon_bml_variant_selector` if continuing in Ghidra.

---

## 14. Investigation methodology notes

- **Working process:** Bruce directed the investigation, ran tests, reviewed results, and made final technical calls. LLMs were used as support tools for drafting code, reviewing logs, checking hypotheses, and organizing findings.
- **Do not trust first extraction results from Dreamcast GD-ROM images.** The initial raw extraction produced convincing-looking filenames and wrong data. Calibration against known-good blobs was required.
- **Display formatting can lie.** The original `(int)mat[i]` dump hid fractional rotations and led to incorrect characterization of Matrix 1 / Matrix 2.
- **A working mutation is not necessarily a reachable fix.** High-Y suppression landed in memory but did not affect rendering because the matrix did not traverse D3D8 `SetTransform`.
- **Negative results were productive.** The BB comparison, D3D8 endpoint, and Dreamcast calibrated comparison all reduced the search space and led toward the registry-controlled `_b` selector.
- **Always preserve state before testing.** Registry backups were taken before changing `CTRLFLAG0`. This made the final fix reversible and testable.
- **The correct fix can be boring.** After hours of matrix/D3D8 work, the practical fix was a control flag selecting the wrong PC-only asset path.

---

## 15. Estimated remaining effort

These estimates replace the older pre-root-cause estimates.

| Outcome | Likelihood | Estimated additional hours |
|---------|------------|----------------------------|
| Package a safe registry-bit clearing script for Bruce's setup | Very high | 1–3 |
| Verify registry-only fix on a clean/unpatched/no-proxy client | High | 2–6 |
| Make a public/user-safe patcher that preserves unrelated `CTRLFLAG0` bits | High | 4–10 |
| Identify the semantic name of `CTRLFLAG0 & 0x08000000` | Medium | 4–20 |
| Create optional pso.exe binary patch to permanently bypass `_b` path | Medium-high | 6–20 |
| Fully explain why `_b` corrupts at the renderer/model level | Medium-low | 20–80+ |

The visible Dragon bug is fixed in the current baseline. Remaining work is packaging, isolation, and historical explanation.

---

## 16. Pickup notes for resuming

When resuming:

1. **Verify the registry value first:**

   ```bash
   export WINEPREFIX="<WINEPREFIX>"
   wine reg query 'HKCU\Software\SonicTeam\PSOV2'
   ```

   Known-good: `CTRLFLAG0 = 0x000b0004`.

2. **If the bug returns, immediately check whether `CTRLFLAG0` has regained bit `0x08000000`.**

3. **Do not resume from the old assumption that the next phase is pso.exe draw-call disassembly.** That was correct before the registry selector discovery, but the current active path is packaging and validating the `CTRLFLAG0` fix.

4. **Do not do more D3D8 matrix-mutation tests for this bug.** That path is closed.

5. **If deeper explanation is desired, start at `0x004202f0` in Ghidra, not at the v3.4 draw callers.** The Dragon asset selector is now the primary code object.

6. **Preserve both values for regression testing:**
   - Known-good: `0x000b0004`
   - Bug-triggering: `0x080b0004`

7. **For public documentation, phrase the fix carefully:**
   - Strong claim: clearing `CTRLFLAG0 & 0x08000000` fixed the Dragon corruption in repeated tests on the current baseline.
   - Do not yet claim: the registry fix alone fixes every possible PC V2 install under every renderer without further testing.

---

## 17. Definitions / glossary

- **`CTRLFLAG0`** — Registry DWORD under `HKCU\Software\SonicTeam\PSOV2`. Loaded into runtime global `DAT_007b8ce0`.
- **`0x08000000` bit** — Control flag bit that causes pso.exe to select `bm_boss1_dragon_b.bml`.
- **`bm_boss1_dragon.bml`** — Normal Dragon BML package. Byte-identical between tested Dreamcast V2 disc and PC V2 after calibrated extraction.
- **`bm_boss1_dragon_a.bml`** — Alternate Dragon BML package. Also byte-identical between tested Dreamcast V2 disc and PC V2.
- **`bm_boss1_dragon_b.bml`** — PC-only Dragon BML package selected by `CTRLFLAG0 & 0x08000000`. Avoiding this file fixes the visible Dragon corruption in the current baseline.
- **BML** — PSO asset package/container used for model and animation data.
- **PVM** — PowerVR texture container.
- **GD-ROM LBA calibration** — Required adjustment for raw Dreamcast Track 3 extraction. Selected delta in this investigation: `-0x57E4000`.
- **Matrix 1 / Matrix 2** — The two bone matrices observed alternating during earlier diagnostics. Matrix 1 = high-Y ghost. Matrix 2 = visible Dragon body.
- **High-Y** — Matrix class with `ty=2000.0`.
- **Software skinning** — CPU-side vertex transformation. Explains why D3D8 `SetTransform` never saw the high-Y matrix.
- **D3D8 proxy** — Local `D3D8.dll` shim used for widescreen, diagnostics, and Bug A suppression.
- **Bug A / B / C** — Earlier symptom decomposition. Still useful historically, but the visible corruption is now controlled by the Dragon BML selector.
- **`[SETXFORM-HIGHY]`** — Diagnostic marker that never fired, proving the high-Y matrix was not submitted through D3D8 transforms.
- **`[DRAGON-SIG]` / `[DWIN-SIG]` / `[DRAWCTX]`** — D3D8 draw-call diagnostics from v3.4.
- **V2 vs BB** — V2 = Phantasy Star Online PC Version 2. BB = Phantasy Star Online Blue Burst.

---

*End of technical reference.*
