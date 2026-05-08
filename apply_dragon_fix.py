apply_dragon_fix.py v5 -- PSO PC V2 Dragon bone corruption fix.

Changes from v4:
  C group: now patches full instructions (not just the 4-byte immediate).
    This is required to NOP the translation slots correctly, and lets
    verify read unambiguous state.

  C00/C05/C10 (rotation diagonal -- [0][0], [1][1], [2][2]):
    v4 wrote 1.0f (identity).  v5 writes 0.0f.
    Rationale: the degenerate bone fallback shares its output buffer with
    the valid Matrix A call that fires the same frame.  NOPing the diagonal
    write (as in intermediate builds) leaves Matrix A's value (~0.9-1.0)
    in the diagonal, so zero_rot never detects the corrupt frame.
    Explicit 0.0f collapses all vertices to a single world point (zero-area
    triangles -> invisible) without affecting Matrix A output.

  C12/C13/C14 (translation tx/ty/tz):
    NOP -- preserve Dragon's actual world position.
    Zeroing the translation sent the collapsed mesh to world origin,
    exposing the inner skin mesh below the arena floor.

  C15 ([3][3] homogeneous w): write 1.0f (unchanged from v4).

  Net fallback output: [0,0,0,0 | 0,0,0,0 | 0,0,0,0 | tx,ty,tz,1]
  All vertices collapse to Dragon world position. Zero-area triangles.
  Dragon Matrix B component is invisible.

  Patch D added: near-z particle scale guard (0x634A48).
    65535.0f -> 0.0f.  Particles at view-space Z~0 previously projected
    to giant XYZRHW quads filling the screen.  0.0f collapses them.

Patches applied:
  A  fcomps threshold redirect         0x61EA1D
  B  je -> jbe                         0x61EA27
  C00-C15  degenerate bone fallback    0x61EFD2-0x61F040
  D  near-z particle scale guard       0x634A48
"""
import sys, os, shutil

ONE  = bytes([0x00, 0x00, 0x80, 0x3F])  # 1.0f
ZRO  = bytes([0x00, 0x00, 0x00, 0x00])  # 0.0f
SENT = bytes([0x9E, 0xC9, 0x7F, 0x7F])  # FLT_MAX sentinel (original)
BASE = 0x400000

# C group -- degenerate bone matrix fallback block (0x61EFD2 - 0x61F040)
# Each entry: (instruction_VA, disp8_or_None, action)
#   action 'zero': write 0.0f   action 'nop': NOP x7   action 'one': write 1.0f
_C = [
    (0x61EFD2, None, 'zero'),  # C00 [0][0] diagonal  -- explicit 0.0f (not NOP)
    (0x61EFD8, 0x04, 'zero'),  # C01 [0][1]
    (0x61EFDF, 0x08, 'zero'),  # C02 [0][2]
    (0x61EFE6, 0x0C, 'zero'),  # C03 [0][3]
    (0x61EFED, 0x10, 'zero'),  # C04 [1][0]
    (0x61EFF4, 0x14, 'zero'),  # C05 [1][1] diagonal  -- explicit 0.0f (not NOP)
    (0x61EFFB, 0x18, 'zero'),  # C06 [1][2]
    (0x61F002, 0x1C, 'zero'),  # C07 [1][3]
    (0x61F009, 0x20, 'zero'),  # C08 [2][0]
    (0x61F010, 0x24, 'zero'),  # C09 [2][1]
    (0x61F017, 0x28, 'zero'),  # C10 [2][2] diagonal  -- explicit 0.0f (not NOP)
    (0x61F01E, 0x2C, 'zero'),  # C11 [2][3]
    (0x61F025, 0x30, 'nop'),   # C12 tx  -- NOP: preserve Dragon world position
    (0x61F02C, 0x34, 'nop'),   # C13 ty  -- NOP
    (0x61F033, 0x38, 'nop'),   # C14 tz  -- NOP
    (0x61F03A, 0x3C, 'one'),   # C15 w   -- 1.0f: valid homogeneous component
]

def _c_patch(va, d, action):
    n = 6 if d is None else 7
    if d is None:
        orig = bytes([0xC7, 0x06]) + SENT
        pat  = bytes([0xC7, 0x06]) + ZRO
    else:
        orig = bytes([0xC7, 0x46, d]) + SENT
        pat  = (bytes([0x90]*7) if action == 'nop' else
                bytes([0xC7, 0x46, d]) + (ONE if action == 'one' else ZRO))
    disp = '    ' if d is None else f'+{d:#04x}'
    return {"label": f"C[esi{disp}]={action}", "file_offset": va-BASE,
            "original": orig, "patched": pat}

PATCHES = [
    {"label":    "A: fcomps redirect [0x7C1674]=0.0 -> [0x64D4F0]=0.5",
     "file_offset": 0x61EA1D - BASE,
     "original": bytes([0xD8,0x1D,0x74,0x16,0x7C,0x00]),
     "patched":  bytes([0xD8,0x1D,0xF0,0xD4,0x64,0x00])},
    {"label":    "B: je -> jbe (0x61EA27)",
     "file_offset": 0x61EA27 - BASE,
     "original": bytes([0x84]),
     "patched":  bytes([0x86])},
] + [_c_patch(va, d, a) for va, d, a in _C] + [
    {"label":    "D: near-z scale 65535.0f -> 0.0f (0x634A48)",
     "file_offset": 0x634A48 - BASE,
     "original": bytes([0x00,0xFF,0x7F,0x47]),
     "patched":  bytes([0x00,0x00,0x00,0x00])},
]

def verify(data):
    all_orig = all_pat = True
    print(f"{'Status':<12} Label"); print("-"*60)
    for p in PATCHES:
        cur = data[p["file_offset"]:p["file_offset"]+len(p["original"])]
        if   cur == p["original"]: s = "ORIGINAL";      all_pat  = False
        elif cur == p["patched"]:  s = "PATCHED";       all_orig = False
        else:                      s = f"?{cur.hex()}"; all_orig = all_pat = False
        print(f"  {s:<10}  {p['label']}")
    print()
    if all_pat:    print("STATE: fully patched")
    elif all_orig: print("STATE: clean original")
    else:          print("STATE: partial/unknown -- revert and re-apply")
    return all_orig, all_pat

def apply(data):
    data = bytearray(data)
    for p in PATCHES:
        off, n = p["file_offset"], len(p["original"])
        cur = bytes(data[off:off+n])
        if   cur == p["patched"]:  print(f"  SKIP  {p['label']}")
        elif cur == p["original"]: data[off:off+n] = p["patched"]; print(f"  OK    {p['label']}")
        else:                      print(f"  WARN  unexpected {cur.hex()} -- {p['label']}")
    return bytes(data)

def revert(data):
    data = bytearray(data)
    for p in PATCHES:
        off, n = p["file_offset"], len(p["patched"])
        cur = bytes(data[off:off+n])
        if   cur == p["original"]: print(f"  SKIP  {p['label']}")
        elif cur == p["patched"]:  data[off:off+n] = p["original"]; print(f"  OK    {p['label']}")
        else:                      print(f"  WARN  unexpected {cur.hex()} -- {p['label']}")
    return bytes(data)

def main():
    if len(sys.argv) < 2: print(__doc__); sys.exit(1)
    exe  = sys.argv[1]
    mode = sys.argv[2] if len(sys.argv) > 2 else "--apply"
    bak  = exe + ".original"
    with open(exe, "rb") as f: data = f.read()
    print(f"{exe} ({len(data):,} bytes)\n")
    if mode == "--verify": verify(data); return
    if mode == "--revert":
        if not os.path.exists(bak): print(f"No backup: {bak}"); sys.exit(1)
        with open(bak,"rb") as f: orig = f.read()
        with open(exe,"wb") as f: f.write(orig)
        print(f"Restored from {bak}"); return
    verify(data); print()
    if not os.path.exists(bak): shutil.copy2(exe, bak); print(f"Backed up -> {bak}")
    print("\nApplying:")
    with open(exe, "wb") as f: f.write(apply(data))
    print(f"\nDone.  Revert: python3 {sys.argv[0]} {exe} --revert")

if __name__ == "__main__": main()
#!/usr/bin/env python3
"""
apply_dragon_fix.py v5 -- PSO PC V2 Dragon bone corruption fix.

Changes from v4:
  C group: now patches full instructions (not just the 4-byte immediate).
    This is required to NOP the translation slots correctly, and lets
    verify read unambiguous state.

  C00/C05/C10 (rotation diagonal -- [0][0], [1][1], [2][2]):
    v4 wrote 1.0f (identity).  v5 writes 0.0f.
    Rationale: the degenerate bone fallback shares its output buffer with
    the valid Matrix A call that fires the same frame.  NOPing the diagonal
    write (as in intermediate builds) leaves Matrix A's value (~0.9-1.0)
    in the diagonal, so zero_rot never detects the corrupt frame.
    Explicit 0.0f collapses all vertices to a single world point (zero-area
    triangles -> invisible) without affecting Matrix A output.

  C12/C13/C14 (translation tx/ty/tz):
    NOP -- preserve Dragon's actual world position.
    Zeroing the translation sent the collapsed mesh to world origin,
    exposing the inner skin mesh below the arena floor.

  C15 ([3][3] homogeneous w): write 1.0f (unchanged from v4).

  Net fallback output: [0,0,0,0 | 0,0,0,0 | 0,0,0,0 | tx,ty,tz,1]
  All vertices collapse to Dragon world position. Zero-area triangles.
  Dragon Matrix B component is invisible.

  Patch D added: near-z particle scale guard (0x634A48).
    65535.0f -> 0.0f.  Particles at view-space Z~0 previously projected
    to giant XYZRHW quads filling the screen.  0.0f collapses them.

Patches applied:
  A  fcomps threshold redirect         0x61EA1D
  B  je -> jbe                         0x61EA27
  C00-C15  degenerate bone fallback    0x61EFD2-0x61F040
  D  near-z particle scale guard       0x634A48
"""
import sys, os, shutil

ONE  = bytes([0x00, 0x00, 0x80, 0x3F])  # 1.0f
ZRO  = bytes([0x00, 0x00, 0x00, 0x00])  # 0.0f
SENT = bytes([0x9E, 0xC9, 0x7F, 0x7F])  # FLT_MAX sentinel (original)
BASE = 0x400000

# C group -- degenerate bone matrix fallback block (0x61EFD2 - 0x61F040)
# Each entry: (instruction_VA, disp8_or_None, action)
#   action 'zero': write 0.0f   action 'nop': NOP x7   action 'one': write 1.0f
_C = [
    (0x61EFD2, None, 'zero'),  # C00 [0][0] diagonal  -- explicit 0.0f (not NOP)
    (0x61EFD8, 0x04, 'zero'),  # C01 [0][1]
    (0x61EFDF, 0x08, 'zero'),  # C02 [0][2]
    (0x61EFE6, 0x0C, 'zero'),  # C03 [0][3]
    (0x61EFED, 0x10, 'zero'),  # C04 [1][0]
    (0x61EFF4, 0x14, 'zero'),  # C05 [1][1] diagonal  -- explicit 0.0f (not NOP)
    (0x61EFFB, 0x18, 'zero'),  # C06 [1][2]
    (0x61F002, 0x1C, 'zero'),  # C07 [1][3]
    (0x61F009, 0x20, 'zero'),  # C08 [2][0]
    (0x61F010, 0x24, 'zero'),  # C09 [2][1]
    (0x61F017, 0x28, 'zero'),  # C10 [2][2] diagonal  -- explicit 0.0f (not NOP)
    (0x61F01E, 0x2C, 'zero'),  # C11 [2][3]
    (0x61F025, 0x30, 'nop'),   # C12 tx  -- NOP: preserve Dragon world position
    (0x61F02C, 0x34, 'nop'),   # C13 ty  -- NOP
    (0x61F033, 0x38, 'nop'),   # C14 tz  -- NOP
    (0x61F03A, 0x3C, 'one'),   # C15 w   -- 1.0f: valid homogeneous component
]

def _c_patch(va, d, action):
    n = 6 if d is None else 7
    if d is None:
        orig = bytes([0xC7, 0x06]) + SENT
        pat  = bytes([0xC7, 0x06]) + ZRO
    else:
        orig = bytes([0xC7, 0x46, d]) + SENT
        pat  = (bytes([0x90]*7) if action == 'nop' else
                bytes([0xC7, 0x46, d]) + (ONE if action == 'one' else ZRO))
    disp = '    ' if d is None else f'+{d:#04x}'
    return {"label": f"C[esi{disp}]={action}", "file_offset": va-BASE,
            "original": orig, "patched": pat}

PATCHES = [
    {"label":    "A: fcomps redirect [0x7C1674]=0.0 -> [0x64D4F0]=0.5",
     "file_offset": 0x61EA1D - BASE,
     "original": bytes([0xD8,0x1D,0x74,0x16,0x7C,0x00]),
     "patched":  bytes([0xD8,0x1D,0xF0,0xD4,0x64,0x00])},
    {"label":    "B: je -> jbe (0x61EA27)",
     "file_offset": 0x61EA27 - BASE,
     "original": bytes([0x84]),
     "patched":  bytes([0x86])},
] + [_c_patch(va, d, a) for va, d, a in _C] + [
    {"label":    "D: near-z scale 65535.0f -> 0.0f (0x634A48)",
     "file_offset": 0x634A48 - BASE,
     "original": bytes([0x00,0xFF,0x7F,0x47]),
     "patched":  bytes([0x00,0x00,0x00,0x00])},
]

def verify(data):
    all_orig = all_pat = True
    print(f"{'Status':<12} Label"); print("-"*60)
    for p in PATCHES:
        cur = data[p["file_offset"]:p["file_offset"]+len(p["original"])]
        if   cur == p["original"]: s = "ORIGINAL";      all_pat  = False
        elif cur == p["patched"]:  s = "PATCHED";       all_orig = False
        else:                      s = f"?{cur.hex()}"; all_orig = all_pat = False
        print(f"  {s:<10}  {p['label']}")
    print()
    if all_pat:    print("STATE: fully patched")
    elif all_orig: print("STATE: clean original")
    else:          print("STATE: partial/unknown -- revert and re-apply")
    return all_orig, all_pat

def apply(data):
    data = bytearray(data)
    for p in PATCHES:
        off, n = p["file_offset"], len(p["original"])
        cur = bytes(data[off:off+n])
        if   cur == p["patched"]:  print(f"  SKIP  {p['label']}")
        elif cur == p["original"]: data[off:off+n] = p["patched"]; print(f"  OK    {p['label']}")
        else:                      print(f"  WARN  unexpected {cur.hex()} -- {p['label']}")
    return bytes(data)

def revert(data):
    data = bytearray(data)
    for p in PATCHES:
        off, n = p["file_offset"], len(p["patched"])
        cur = bytes(data[off:off+n])
        if   cur == p["original"]: print(f"  SKIP  {p['label']}")
        elif cur == p["patched"]:  data[off:off+n] = p["original"]; print(f"  OK    {p['label']}")
        else:                      print(f"  WARN  unexpected {cur.hex()} -- {p['label']}")
    return bytes(data)

def main():
    if len(sys.argv) < 2: print(__doc__); sys.exit(1)
    exe  = sys.argv[1]
    mode = sys.argv[2] if len(sys.argv) > 2 else "--apply"
    bak  = exe + ".original"
    with open(exe, "rb") as f: data = f.read()
    print(f"{exe} ({len(data):,} bytes)\n")
    if mode == "--verify": verify(data); return
    if mode == "--revert":
        if not os.path.exists(bak): print(f"No backup: {bak}"); sys.exit(1)
        with open(bak,"rb") as f: orig = f.read()
        with open(exe,"wb") as f: f.write(orig)
        print(f"Restored from {bak}"); return
    verify(data); print()
    if not os.path.exists(bak): shutil.copy2(exe, bak); print(f"Backed up -> {bak}")
    print("\nApplying:")
    with open(exe, "wb") as f: f.write(apply(data))
    print(f"\nDone.  Revert: python3 {sys.argv[0]} {exe} --revert")

if __name__ == "__main__": main()
#!/usr/bin/env python3
"""
apply_dragon_fix.py v5 -- PSO PC V2 Dragon bone corruption fix.

Changes from v4:
  C group: now patches full instructions (not just the 4-byte immediate).
    This is required to NOP the translation slots correctly, and lets
    verify read unambiguous state.

  C00/C05/C10 (rotation diagonal -- [0][0], [1][1], [2][2]):
    v4 wrote 1.0f (identity).  v5 writes 0.0f.
    Rationale: the degenerate bone fallback shares its output buffer with
    the valid Matrix A call that fires the same frame.  NOPing the diagonal
    write (as in intermediate builds) leaves Matrix A's value (~0.9-1.0)
    in the diagonal, so zero_rot never detects the corrupt frame.
    Explicit 0.0f collapses all vertices to a single world point (zero-area
    triangles -> invisible) without affecting Matrix A output.

  C12/C13/C14 (translation tx/ty/tz):
    NOP -- preserve Dragon's actual world position.
    Zeroing the translation sent the collapsed mesh to world origin,
    exposing the inner skin mesh below the arena floor.

  C15 ([3][3] homogeneous w): write 1.0f (unchanged from v4).

  Net fallback output: [0,0,0,0 | 0,0,0,0 | 0,0,0,0 | tx,ty,tz,1]
  All vertices collapse to Dragon world position. Zero-area triangles.
  Dragon Matrix B component is invisible.

  Patch D added: near-z particle scale guard (0x634A48).
    65535.0f -> 0.0f.  Particles at view-space Z~0 previously projected
    to giant XYZRHW quads filling the screen.  0.0f collapses them.

Patches applied:
  A  fcomps threshold redirect         0x61EA1D
  B  je -> jbe                         0x61EA27
  C00-C15  degenerate bone fallback    0x61EFD2-0x61F040
  D  near-z particle scale guard       0x634A48
"""
import sys, os, shutil

ONE  = bytes([0x00, 0x00, 0x80, 0x3F])  # 1.0f
ZRO  = bytes([0x00, 0x00, 0x00, 0x00])  # 0.0f
SENT = bytes([0x9E, 0xC9, 0x7F, 0x7F])  # FLT_MAX sentinel (original)
BASE = 0x400000

# C group -- degenerate bone matrix fallback block (0x61EFD2 - 0x61F040)
# Each entry: (instruction_VA, disp8_or_None, action)
#   action 'zero': write 0.0f   action 'nop': NOP x7   action 'one': write 1.0f
_C = [
    (0x61EFD2, None, 'zero'),  # C00 [0][0] diagonal  -- explicit 0.0f (not NOP)
    (0x61EFD8, 0x04, 'zero'),  # C01 [0][1]
    (0x61EFDF, 0x08, 'zero'),  # C02 [0][2]
    (0x61EFE6, 0x0C, 'zero'),  # C03 [0][3]
    (0x61EFED, 0x10, 'zero'),  # C04 [1][0]
    (0x61EFF4, 0x14, 'zero'),  # C05 [1][1] diagonal  -- explicit 0.0f (not NOP)
    (0x61EFFB, 0x18, 'zero'),  # C06 [1][2]
    (0x61F002, 0x1C, 'zero'),  # C07 [1][3]
    (0x61F009, 0x20, 'zero'),  # C08 [2][0]
    (0x61F010, 0x24, 'zero'),  # C09 [2][1]
    (0x61F017, 0x28, 'zero'),  # C10 [2][2] diagonal  -- explicit 0.0f (not NOP)
    (0x61F01E, 0x2C, 'zero'),  # C11 [2][3]
    (0x61F025, 0x30, 'nop'),   # C12 tx  -- NOP: preserve Dragon world position
    (0x61F02C, 0x34, 'nop'),   # C13 ty  -- NOP
    (0x61F033, 0x38, 'nop'),   # C14 tz  -- NOP
    (0x61F03A, 0x3C, 'one'),   # C15 w   -- 1.0f: valid homogeneous component
]

def _c_patch(va, d, action):
    n = 6 if d is None else 7
    if d is None:
        orig = bytes([0xC7, 0x06]) + SENT
        pat  = bytes([0xC7, 0x06]) + ZRO
    else:
        orig = bytes([0xC7, 0x46, d]) + SENT
        pat  = (bytes([0x90]*7) if action == 'nop' else
                bytes([0xC7, 0x46, d]) + (ONE if action == 'one' else ZRO))
    disp = '    ' if d is None else f'+{d:#04x}'
    return {"label": f"C[esi{disp}]={action}", "file_offset": va-BASE,
            "original": orig, "patched": pat}

PATCHES = [
    {"label":    "A: fcomps redirect [0x7C1674]=0.0 -> [0x64D4F0]=0.5",
     "file_offset": 0x61EA1D - BASE,
     "original": bytes([0xD8,0x1D,0x74,0x16,0x7C,0x00]),
     "patched":  bytes([0xD8,0x1D,0xF0,0xD4,0x64,0x00])},
    {"label":    "B: je -> jbe (0x61EA27)",
     "file_offset": 0x61EA27 - BASE,
     "original": bytes([0x84]),
     "patched":  bytes([0x86])},
] + [_c_patch(va, d, a) for va, d, a in _C] + [
    {"label":    "D: near-z scale 65535.0f -> 0.0f (0x634A48)",
     "file_offset": 0x634A48 - BASE,
     "original": bytes([0x00,0xFF,0x7F,0x47]),
     "patched":  bytes([0x00,0x00,0x00,0x00])},
]

def verify(data):
    all_orig = all_pat = True
    print(f"{'Status':<12} Label"); print("-"*60)
    for p in PATCHES:
        cur = data[p["file_offset"]:p["file_offset"]+len(p["original"])]
        if   cur == p["original"]: s = "ORIGINAL";      all_pat  = False
        elif cur == p["patched"]:  s = "PATCHED";       all_orig = False
        else:                      s = f"?{cur.hex()}"; all_orig = all_pat = False
        print(f"  {s:<10}  {p['label']}")
    print()
    if all_pat:    print("STATE: fully patched")
    elif all_orig: print("STATE: clean original")
    else:          print("STATE: partial/unknown -- revert and re-apply")
    return all_orig, all_pat

def apply(data):
    data = bytearray(data)
    for p in PATCHES:
        off, n = p["file_offset"], len(p["original"])
        cur = bytes(data[off:off+n])
        if   cur == p["patched"]:  print(f"  SKIP  {p['label']}")
        elif cur == p["original"]: data[off:off+n] = p["patched"]; print(f"  OK    {p['label']}")
        else:                      print(f"  WARN  unexpected {cur.hex()} -- {p['label']}")
    return bytes(data)

def revert(data):
    data = bytearray(data)
    for p in PATCHES:
        off, n = p["file_offset"], len(p["patched"])
        cur = bytes(data[off:off+n])
        if   cur == p["original"]: print(f"  SKIP  {p['label']}")
        elif cur == p["patched"]:  data[off:off+n] = p["original"]; print(f"  OK    {p['label']}")
        else:                      print(f"  WARN  unexpected {cur.hex()} -- {p['label']}")
    return bytes(data)

def main():
    if len(sys.argv) < 2: print(__doc__); sys.exit(1)
    exe  = sys.argv[1]
    mode = sys.argv[2] if len(sys.argv) > 2 else "--apply"
    bak  = exe + ".original"
    with open(exe, "rb") as f: data = f.read()
    print(f"{exe} ({len(data):,} bytes)\n")
    if mode == "--verify": verify(data); return
    if mode == "--revert":
        if not os.path.exists(bak): print(f"No backup: {bak}"); sys.exit(1)
        with open(bak,"rb") as f: orig = f.read()
        with open(exe,"wb") as f: f.write(orig)
        print(f"Restored from {bak}"); return
    verify(data); print()
    if not os.path.exists(bak): shutil.copy2(exe, bak); print(f"Backed up -> {bak}")
    print("\nApplying:")
    with open(exe, "wb") as f: f.write(apply(data))
    print(f"\nDone.  Revert: python3 {sys.argv[0]} {exe} --revert")

if __name__ == "__main__": main()
