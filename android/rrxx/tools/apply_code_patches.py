#!/usr/bin/env python3
"""Rumble-XX code patches, switchable at run time.

The Rumble-XX patch file (github.com/Rumble-XX/Rumble-Roses-XX,
"4B4E07D1 - Rumble Roses Double X.patch.toml") changes a few instructions in
.text. In Xenia those bytes are rewritten before the JIT sees them; here the code
is translated ahead of time, so this script edits the generated C++ at those
instructions instead, gated on bits of g_rrxx_code_patches (set by the app from
the rrxx_* cvars before the game starts; see src/rrxx_patches.h).

Run after every `rexglue codegen` (idempotent):
    python3 rrxx/tools/apply_code_patches.py rrxx/generated/default

What the patch bytes do (checked against the user's default.xex image):
  Blur Off            8226BF20, 8226C0A4: li r6,1 -> li r6,0
                      8226DE12 (be32 straddling two words): fmr f1,f31 becomes
                      fcmpu cr0,f0,f0 and the bl 0x821e92c8 at 8226DE14 becomes an
                      invalid word, which Xenia's translator skips, so the net
                      effect is that call not being made.
  Bloom Slight Off 1  8226C12C: li r6,1 -> li r6,0
  Bloom Slight Off 2  8226C150: li r6,1 -> li r6,0
  Remove Weapon Icons 8228A7EC: li r4,1 -> li r4,0
  Remove Queen Banner 822A5B84: li r4,1 -> li r4,0
(The other two patches, crowds and 21:9, change .data/.rdata and are written into
guest memory at load time instead.)
"""
import re
import sys
from pathlib import Path

BLUR, BLOOM1, BLOOM2, WEAPON, QUEEN = 0x1, 0x2, 0x4, 0x8, 0x10

# (address, bit, expected disassembly comment, original line, replacement line)
SITES = [
    (0x8226BF20, BLUR, "li r6,1", "ctx.r6.s64 = 1;",
     "ctx.r6.s64 = (g_rrxx_code_patches & 0x1u) ? 0 : 1;  // RRXX patch: Blur Off"),
    (0x8226C0A4, BLUR, "li r6,1", "ctx.r6.s64 = 1;",
     "ctx.r6.s64 = (g_rrxx_code_patches & 0x1u) ? 0 : 1;  // RRXX patch: Blur Off"),
    (0x8226DE14, BLUR, "bl 0x821e92c8", "sub_821E92C8(ctx, base);",
     "if (!(g_rrxx_code_patches & 0x1u)) sub_821E92C8(ctx, base);  // RRXX patch: Blur Off"),
    (0x8226C12C, BLOOM1, "li r6,1", "ctx.r6.s64 = 1;",
     "ctx.r6.s64 = (g_rrxx_code_patches & 0x2u) ? 0 : 1;  // RRXX patch: Bloom Slight Off 1"),
    (0x8226C150, BLOOM2, "li r6,1", "ctx.r6.s64 = 1;",
     "ctx.r6.s64 = (g_rrxx_code_patches & 0x4u) ? 0 : 1;  // RRXX patch: Bloom Slight Off 2"),
    (0x8228A7EC, WEAPON, "li r4,1", "ctx.r4.s64 = 1;",
     "ctx.r4.s64 = (g_rrxx_code_patches & 0x8u) ? 0 : 1;  // RRXX patch: Remove Weapon Icons"),
    (0x822A5B84, QUEEN, "li r4,1", "ctx.r4.s64 = 1;",
     "ctx.r4.s64 = (g_rrxx_code_patches & 0x10u) ? 0 : 1;  // RRXX patch: Remove Queen Match Banner"),
]

DECL = 'extern "C" uint32_t g_rrxx_code_patches;  // RRXX patches (rrxx/tools/apply_code_patches.py)'

ANCHOR_RE = re.compile(r"^(?:loc_([0-9A-F]{8}):|\tctx\.lr = 0x([0-9A-F]{8});|DEFINE_REX_FUNC\(sub_([0-9A-F]{8})\))")
COMMENT_RE = re.compile(r"^\t// (.*)$")


def index_anchors(files):
    """address -> (file, line index) for labels, call return points and function starts."""
    anchors = {}
    for path, lines in files.items():
        for i, line in enumerate(lines):
            m = ANCHOR_RE.match(line)
            if m:
                addr = int(next(g for g in m.groups() if g), 16)
                anchors.setdefault(addr, (path, i))
    return anchors


def locate(files, anchors, target):
    """(file, index of the instruction's comment line) for the instruction at target."""
    for start in range(target, target - 0x400, -4):
        if start not in anchors:
            continue
        path, i = anchors[start]
        lines = files[path]
        addr = start
        j = i + 1
        while j < len(lines):
            line = lines[j]
            if line.startswith("DEFINE_REX_FUNC(") or line == "}":
                break
            if COMMENT_RE.match(line):
                if addr == target:
                    return path, j
                addr += 4
            j += 1
        return None
    return None


SETJMP_CALL = "\tsub_824726C0(ctx, base);"
SETJMP_REPL = "\tRRXX_GUEST_SETJMP(rrxx_jmp_frame);  // RRXX: host-paired guest setjmp"
FRAME_DECL = "\tRrxxJmpFrame rrxx_jmp_frame;  // RRXX: host-paired guest setjmp"
LONGJMP_FUNC = "DEFINE_REX_FUNC(sub_82472470) {"
LONGJMP_CALLER = "\tconst uint32_t rrxx_lj_caller = uint32_t(ctx.lr);  // RRXX"
LONGJMP_HOOK = ("\tRrxxGuestLongjmp(ctx.r7.u32, ctx.r1.u32, rrxx_lj_caller, "
                "rrxx_lj_caller == 0x822E5354u ? REX_LOAD_U32(ctx.r7.u32 - 124) : 0u);  // RRXX")
SETJMP_INCLUDE = '#include "../../src/rrxx_setjmp.h"  // RRXX guest setjmp/longjmp'


def patch_setjmp(files):
    """Pairs the guest setjmp/longjmp (CRT 0x824726C0 / 0x82472470) with host ones (src/rrxx_setjmp.h)."""
    touched = set()
    sites = 0
    for path, lines in files.items():
        func_start = None
        i = 0
        while i < len(lines):
            line = lines[i]
            if line.startswith("DEFINE_REX_FUNC("):
                func_start = i
            if line == SETJMP_CALL and func_start is not None and "sub_824726C0)" not in lines[func_start]:
                lines[i] = SETJMP_REPL
                sites += 1
                touched.add(path)
                # the frame object, right after the function's prologue
                j = func_start
                while lines[j].strip() != "REX_FUNC_PROLOGUE();":
                    j += 1
                if lines[j + 1] != FRAME_DECL:
                    lines.insert(j + 1, FRAME_DECL)
                    i += 1
            elif line == SETJMP_REPL:
                sites += 1
            if line == LONGJMP_FUNC:
                j = i
                while lines[j].strip() != "REX_FUNC_PROLOGUE();":
                    j += 1
                if lines[j + 1] != LONGJMP_CALLER:
                    lines.insert(j + 1, LONGJMP_CALLER)
                    # the main path ends with "mr r3,r6" then blr
                    k = j
                    while not (lines[k] == "\t// mr r3,r6" and lines[k + 2] == "\t// blr "
                               and lines[k + 3] == "\treturn;"):
                        k += 1
                    lines.insert(k + 3, LONGJMP_HOOK)
                    touched.add(path)
                    print(f"  longjmp 82472470: host longjmp added ({path.name}:{k + 4})")
                else:
                    print(f"  longjmp 82472470: already patched ({path.name})")
            i += 1
    for path in touched:
        lines = files[path]
        if SETJMP_INCLUDE not in lines:
            first_include = next(i for i, line in enumerate(lines) if line.startswith("#include"))
            lines.insert(first_include + 1, SETJMP_INCLUDE)
    print(f"  setjmp 824726C0: {sites} call site(s) host-paired")
    return touched


def main():
    gen = Path(sys.argv[1] if len(sys.argv) > 1 else "rrxx/generated/default")
    files = {p: p.read_text().split("\n") for p in sorted(gen.glob("rrxx_recomp.*.cpp"))}
    anchors = index_anchors(files)
    touched = set()
    failures = 0
    for target, bit, mnemonic, original, replacement in SITES:
        found = locate(files, anchors, target)
        if not found:
            print(f"  {target:08X}: not found", file=sys.stderr)
            failures += 1
            continue
        path, j = found
        lines = files[path]
        comment = COMMENT_RE.match(lines[j]).group(1)
        if comment != mnemonic:
            print(f"  {target:08X}: expected '{mnemonic}', found '{comment}'", file=sys.stderr)
            failures += 1
            continue
        k = j + 1
        while k < len(lines) and not COMMENT_RE.match(lines[k]):
            if lines[k].strip() == replacement.strip():
                print(f"  {target:08X}: already patched ({path.name})")
                break
            if lines[k].strip() == original:
                lines[k] = "\t" + replacement
                touched.add(path)
                print(f"  {target:08X}: {mnemonic} gated on bit {bit:#x} ({path.name}:{k + 1})")
                break
            k += 1
        else:
            print(f"  {target:08X}: line '{original}' not found after the comment", file=sys.stderr)
            failures += 1
    touched |= patch_setjmp(files)
    for path in touched:
        lines = files[path]
        if DECL not in lines:
            first_include = next(i for i, line in enumerate(lines) if line.startswith("#include"))
            lines.insert(first_include + 1, DECL)
        path.write_text("\n".join(lines))
    if failures:
        print(f"{failures} patch site(s) failed", file=sys.stderr)
        return 1
    print(f"Code patches applied ({len(touched)} file(s) changed).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
