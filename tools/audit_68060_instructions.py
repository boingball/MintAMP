#!/usr/bin/env python3
"""Audit a MintAMP m68k binary for register-pair long multiply/divide.

Background
----------
The Motorola 68060 does NOT implement the 64-bit-result long multiply and the
64-bit-dividend long divide in hardware:

    muls.l Dn,Dl:Dh     32x32 -> 64 signed product
    mulu.l Dn,Dl:Dh     32x32 -> 64 unsigned product
    divs.l <ea>,Dr:Dq   64/32 signed  (Dr:Dq is a 64-bit dividend)
    divu.l <ea>,Dr:Dq   64/32 unsigned

Every execution of one of these traps into the operating system's software
emulation.  In MintAMP's hot MP3 decode loops (polyphase synthesis above all)
that is catastrophic on a real ~75 MHz 68060: mouse lag, audio-buffer
starvation and dropouts.  GCC's own -m68060 codegen already avoids the
64-bit muls.l (it calls __muldi3 instead); the offenders are the hand-written
.S kernels and inline asm that force the instruction regardless of -mcpu.

GNU m68k objdump prints these in a three-operand form, e.g.

    4c01 0c02   mulsl %d1,%d2,%d0     <- 64-bit product (EMULATED)
    4c01 0800   mulsl %d1,%d0         <- 32x32->32 (fine, NOT emulated)
    4c41 0c02   divsl %d1,%d2,%d0     <- 64-bit dividend (EMULATED)
    4c41 0000   divull %d1,%d0,%d0    <- 32/32 quotient-only (fine)
    4c41 0802   divsll %d1,%d2,%d0    <- 32/32 rem:quo (fine)

so the audit must recognise the three-operand representation, not only the
older colon (``%d2:%d0``) form.

What this reports
-----------------
* total offending instructions, multiply and divide counted separately;
* counts grouped by symbol/function;
* the source address and disassembly of each offender (``--verbose``);
* decoder hot-path results separately from unrelated app/library code;
* the difference between a clean baseline binary and a candidate binary
  (``--baseline`` / ``--candidate``).

Usage
-----
    audit_68060_instructions.py BINARY [--verbose] [--hot-only]
    audit_68060_instructions.py --baseline CLEAN --candidate CANDIDATE
    audit_68060_instructions.py --self-test

Set the disassembler with ``--objdump`` (default: $AUDIT_OBJDUMP or
``m68k-amigaos-objdump``, falling back to ``m68k-linux-gnu-objdump``).
"""

import argparse
import json
import os
import re
import shutil
import subprocess
import sys

# --------------------------------------------------------------------------
# Detection
# --------------------------------------------------------------------------

# A symbol header line in `objdump -d` output, e.g.
#   "00000e2c <PolyphaseStereo>:"
_SYMBOL_RE = re.compile(r"^[0-9a-fA-F]+\s+<(?P<name>[^>]+)>:\s*$")

# A disassembled instruction line, e.g.
#   "    e2c:\t4c01 0c02 \tmulsl %d1,%d2,%d0"
# The bytes column is optional/variable; we split on the LAST tab to get the
# mnemonic+operands, which is stable across binutils versions.
_INSN_RE = re.compile(
    r"^\s*(?P<addr>[0-9a-fA-F]+):\s+(?P<rest>.*\S)\s*$"
)

_DREG_RE = re.compile(r"^%?d[0-7]$", re.IGNORECASE)


def _split_operands(operands):
    """Split an operand string on top-level commas (m68k has no nested commas
    that matter here; address modes use () and @, not bare commas)."""
    return [p.strip() for p in operands.split(",") if p.strip()]


def classify_instruction(text):
    """Classify one disassembled instruction (mnemonic + operands).

    Returns 'mul', 'div', or None. 'mul'/'div' mean an emulated 68060
    register-pair long multiply / long divide (64-bit product or 64-bit
    dividend). Anything else -- including the 32-bit muls.l/divs.l forms and
    the .w/.b variants -- returns None.
    """
    text = text.strip()
    if not text:
        return None
    parts = text.split(None, 1)
    mnem = parts[0].lower()
    operands = parts[1] if len(parts) > 1 else ""

    # Normalise "muls.l"/"divsl.l" -> "mulsl"/"divsll" so both objdump styles
    # (dotted and undotted) collapse to the same token.
    mnem_flat = mnem.replace(".", "")

    if len(mnem_flat) < 5:
        return None
    base = mnem_flat[:4]
    suffix = mnem_flat[4:]
    if base not in ("muls", "mulu", "divs", "divu"):
        return None
    # Only the 32-bit-size ('l') encodings are the long mul/div. The
    # 32/32-with-remainder divide disassembles as 'll' (divsll/divull) and the
    # word/byte multiplies as 'w'/'b'; none of those is the emulated 64-bit op.
    if suffix != "l":
        return None

    # Determine whether the operands encode a register pair (Dh:Dl or the
    # three-operand src,Dl,Dh form).
    pair = None  # (reg_a, reg_b)
    if ":" in operands:
        # Colon form, possibly "%d1,%d2:%d0" or "%d2:%d0".
        last = _split_operands(operands)[-1]
        halves = [h.strip() for h in last.split(":")]
        if len(halves) == 2 and all(_DREG_RE.match(h) for h in halves):
            pair = (halves[0], halves[1])
    else:
        ops = _split_operands(operands)
        if len(ops) == 3 and _DREG_RE.match(ops[1]) and _DREG_RE.match(ops[2]):
            pair = (ops[1], ops[2])

    if pair is None:
        return None

    if base in ("muls", "mulu"):
        # A three-operand / colon long multiply is always the 64-bit product.
        return "mul"

    # Divide: the 64-bit-dividend form uses two DISTINCT registers (Dr:Dq).
    # objdump renders the 32-bit quotient-only form as "...,%dq,%dq" (same
    # register twice), which is hardware on the 68060 -- exclude it.
    if pair[0].lower() == pair[1].lower():
        return None
    return "div"


def parse_objdump(text, hot_matcher):
    """Parse `objdump -d` text into a list of offender dicts and a per-symbol
    aggregate. `hot_matcher(symbol) -> bool` decides hot-path membership."""
    current = "<unknown>"
    offenders = []
    for line in text.splitlines():
        msym = _SYMBOL_RE.match(line)
        if msym:
            current = msym.group("name")
            continue
        minsn = _INSN_RE.match(line)
        if not minsn:
            continue
        # The disassembly is after the last tab (bytes column precedes it).
        rest = minsn.group("rest")
        if "\t" in rest:
            disasm = rest.rsplit("\t", 1)[1].strip()
        else:
            disasm = rest.strip()
        kind = classify_instruction(disasm)
        if kind is None:
            continue
        offenders.append(
            {
                "symbol": current,
                "addr": minsn.group("addr"),
                "disasm": disasm,
                "kind": kind,
                "hot": bool(hot_matcher(current)),
            }
        )
    return offenders


# --------------------------------------------------------------------------
# Hot-path classification
# --------------------------------------------------------------------------

# Decoder hot-path symbols. These are the RealNetworks/Helix fixed-point MP3
# stages plus MintAMP's own m68k kernels. Everything else (GUI, networking,
# libc/libnix, compiler support) is "other". Case-insensitive substring/regex.
DEFAULT_HOT_PATTERNS = [
    r"polyphase",
    r"Subband",
    r"dct32",
    r"FDCT32",
    r"\bDCT4\b",
    r"imdct",
    r"IMDCT",
    r"antialias",
    r"Dequant",
    r"dequant",
    r"DqChan",
    r"[Hh]uffman",
    r"MidSide",
    r"Intensity",
    r"Stereo[Pp]roc",
    r"stproc",
    r"scalefac",
    r"Scalefac",
    r"MULSHIFT",
    r"_Amiga_m68k",
    r"Xform",
    r"Window",
]


def make_hot_matcher(extra_patterns):
    pats = [re.compile(p, re.IGNORECASE) for p in DEFAULT_HOT_PATTERNS]
    pats += [re.compile(p, re.IGNORECASE) for p in (extra_patterns or [])]

    def matcher(symbol):
        return any(p.search(symbol) for p in pats)

    return matcher


# --------------------------------------------------------------------------
# objdump invocation
# --------------------------------------------------------------------------

def find_objdump(explicit):
    candidates = []
    if explicit:
        candidates.append(explicit)
    env = os.environ.get("AUDIT_OBJDUMP")
    if env:
        candidates.append(env)
    candidates += ["m68k-amigaos-objdump", "m68k-linux-gnu-objdump", "objdump"]
    for c in candidates:
        if shutil.which(c) or os.path.isfile(c):
            return c
    return candidates[0]


def run_objdump(objdump, binary):
    try:
        out = subprocess.run(
            [objdump, "-d", binary],
            check=True,
            capture_output=True,
            text=True,
        )
    except FileNotFoundError:
        sys.exit("error: objdump not found: %s" % objdump)
    except subprocess.CalledProcessError as exc:
        sys.exit("error: %s -d %s failed:\n%s" % (objdump, binary, exc.stderr))
    return out.stdout


# --------------------------------------------------------------------------
# Aggregation and reporting
# --------------------------------------------------------------------------

def aggregate(offenders):
    """Return per-symbol dict: name -> {mul, div, total, hot, insns[]}."""
    syms = {}
    for o in offenders:
        s = syms.setdefault(
            o["symbol"],
            {"mul": 0, "div": 0, "total": 0, "hot": o["hot"], "insns": []},
        )
        s[o["kind"]] += 1
        s["total"] += 1
        s["insns"].append(o)
    return syms


def totals(offenders):
    mul = sum(1 for o in offenders if o["kind"] == "mul")
    div = sum(1 for o in offenders if o["kind"] == "div")
    hot_mul = sum(1 for o in offenders if o["kind"] == "mul" and o["hot"])
    hot_div = sum(1 for o in offenders if o["kind"] == "div" and o["hot"])
    return {
        "total": mul + div,
        "mul": mul,
        "div": div,
        "hot_total": hot_mul + hot_div,
        "hot_mul": hot_mul,
        "hot_div": hot_div,
        "other_total": (mul + div) - (hot_mul + hot_div),
        "other_mul": mul - hot_mul,
        "other_div": div - hot_div,
    }


def _sym_table(syms, hot, verbose, top):
    rows = [(n, d) for n, d in syms.items() if d["hot"] == hot]
    rows.sort(key=lambda kv: (-kv[1]["total"], kv[0]))
    if top:
        rows = rows[:top]
    lines = []
    for name, d in rows:
        lines.append(
            "  %5d  mul=%-5d div=%-5d  %s" % (d["total"], d["mul"], d["div"], name)
        )
        if verbose:
            for o in d["insns"]:
                lines.append("           %8s: %s" % (o["addr"], o["disasm"]))
    return lines


def report_single(binary, offenders, args):
    syms = aggregate(offenders)
    t = totals(offenders)
    out = []
    out.append("== 68060 register-pair mul/div audit: %s ==" % binary)
    out.append(
        "total offending: %d   (mul %d, div %d)" % (t["total"], t["mul"], t["div"])
    )
    out.append(
        "  hot-path (decoder): %d   (mul %d, div %d)"
        % (t["hot_total"], t["hot_mul"], t["hot_div"])
    )
    out.append(
        "  other (app/lib):    %d   (mul %d, div %d)"
        % (t["other_total"], t["other_mul"], t["other_div"])
    )
    out.append("")
    out.append("decoder hot-path symbols:")
    hot_lines = _sym_table(syms, True, args.verbose, args.top)
    out += hot_lines if hot_lines else ["  (none)"]
    if not args.hot_only:
        out.append("")
        out.append("other (app/library/compiler) symbols:")
        other_lines = _sym_table(syms, False, args.verbose, args.top)
        out += other_lines if other_lines else ["  (none)"]
    return "\n".join(out)


def report_diff(baseline_bin, cand_bin, base_off, cand_off, args):
    base_syms = aggregate(base_off)
    cand_syms = aggregate(cand_off)
    bt = totals(base_off)
    ct = totals(cand_off)
    out = []
    out.append("== 68060 mul/div audit diff ==")
    out.append("baseline:  %s" % baseline_bin)
    out.append("candidate: %s" % cand_bin)
    out.append("")
    out.append(
        "total:    baseline %d  candidate %d  delta %+d"
        % (bt["total"], ct["total"], ct["total"] - bt["total"])
    )
    out.append(
        "  mul:    baseline %d  candidate %d  delta %+d"
        % (bt["mul"], ct["mul"], ct["mul"] - bt["mul"])
    )
    out.append(
        "  div:    baseline %d  candidate %d  delta %+d"
        % (bt["div"], ct["div"], ct["div"] - bt["div"])
    )
    out.append(
        "hot-path: baseline %d  candidate %d  delta %+d"
        % (bt["hot_total"], ct["hot_total"], ct["hot_total"] - bt["hot_total"])
    )
    out.append(
        "other:    baseline %d  candidate %d  delta %+d"
        % (bt["other_total"], ct["other_total"], ct["other_total"] - bt["other_total"])
    )
    out.append("")
    out.append("per-symbol change (candidate vs baseline):")
    names = set(base_syms) | set(cand_syms)
    changed = []
    for n in names:
        b = base_syms.get(n, {}).get("total", 0)
        c = cand_syms.get(n, {}).get("total", 0)
        if c != b:
            hot = (cand_syms.get(n) or base_syms.get(n))["hot"]
            changed.append((c - b, c, b, hot, n))
    changed.sort(key=lambda r: (-r[0], r[4]))
    if not changed:
        out.append("  (no per-symbol differences)")
    for delta, c, b, hot, n in changed:
        out.append(
            "  %+5d  (%d -> %d)  %s%s"
            % (delta, b, c, "[hot] " if hot else "", n)
        )
    return "\n".join(out)


def build_json(binary, offenders):
    syms = aggregate(offenders)
    return {
        "binary": binary,
        "totals": totals(offenders),
        "symbols": {
            n: {"mul": d["mul"], "div": d["div"], "total": d["total"], "hot": d["hot"]}
            for n, d in syms.items()
        },
    }


# --------------------------------------------------------------------------
# Self-test
# --------------------------------------------------------------------------

SELFTEST_OBJDUMP = """
sample.o:     file format elf32-m68k

Disassembly of section .text:

00000000 <PolyphaseStereo>:
   0:\t4c01 0800 \tmulsl %d1,%d0
   4:\t4c01 0c02 \tmulsl %d1,%d2,%d0
   8:\t4c03 5404 \tmulul %d3,%d4,%d5
   c:\t4c10 0c02 \tmulsl %a0@,%d2,%d0
  10:\tc1c1      \tmulsw %d1,%d0

00000020 <some_libc_helper>:
  20:\t4c41 0c02 \tdivsl %d1,%d2,%d0
  24:\t4c41 0000 \tdivull %d1,%d0,%d0
  28:\t4c41 0802 \tdivsll %d1,%d2,%d0
  2c:\t4e75      \trts

00000030 <MULSHIFT32_colonstyle>:
  30:\t0000 0000 \tmuls.l %d1,%d2:%d0
  34:\t0000 0000 \tdivs.l %d1,%d2:%d0
"""


def self_test():
    cases = [
        ("mulsl %d1,%d0", None),
        ("mulsl %d1,%d2,%d0", "mul"),
        ("mulul %d3,%d4,%d5", "mul"),
        ("mulsl %a0@,%d2,%d0", "mul"),
        ("mulsw %d1,%d0", None),
        ("muls.w %d1,%d0", None),
        ("divsl %d1,%d2,%d0", "div"),      # 64-bit dividend, distinct regs
        ("divull %d1,%d0,%d0", None),      # 32/32 quotient-only
        ("divsll %d1,%d2,%d0", None),      # 32/32 rem:quo (dividend is 32-bit)
        ("divsl %d1,%d0,%d0", None),       # duplicate reg -> not 64-bit
        ("muls.l %d1,%d2:%d0", "mul"),     # colon style
        ("divs.l %d1,%d2:%d0", "div"),     # colon style, distinct
        ("mulu.l %d1,%d0:%d0", "mul"),     # colon mul always 64-bit
        ("move.l %d0,%d1", None),
        ("addl %d0,%d1", None),
        ("", None),
    ]
    failures = 0
    for text, expect in cases:
        got = classify_instruction(text)
        ok = got == expect
        if not ok:
            failures += 1
            print("FAIL classify %r -> %r (expected %r)" % (text, got, expect))
    # End-to-end parse of the sample dump.
    matcher = make_hot_matcher(None)
    offenders = parse_objdump(SELFTEST_OBJDUMP, matcher)
    t = totals(offenders)
    # Expected offenders: 3 muls in PolyphaseStereo (0x4,0x8,0xc),
    # 1 div in some_libc_helper (0x20), 1 mul + 1 div colon-style.
    checks = [
        ("total", t["total"], 6),
        ("mul", t["mul"], 4),
        ("div", t["div"], 2),
        ("hot_mul", t["hot_mul"], 4),   # PolyphaseStereo + MULSHIFT32_colonstyle
        ("hot_div", t["hot_div"], 1),   # MULSHIFT32_colonstyle colon div
        ("other_div", t["other_div"], 1),  # some_libc_helper
    ]
    for name, got, expect in checks:
        if got != expect:
            failures += 1
            print("FAIL totals[%s] = %d (expected %d)" % (name, got, expect))
    if failures:
        print("%d self-test failure(s)" % failures)
        return 1
    print("self-test OK (%d classify cases, end-to-end parse verified)" % len(cases))
    return 0


# --------------------------------------------------------------------------
# main
# --------------------------------------------------------------------------

def main(argv=None):
    ap = argparse.ArgumentParser(
        description="Audit a MintAMP m68k binary for emulated 68060 "
        "register-pair long multiply/divide instructions."
    )
    ap.add_argument("binary", nargs="?", help="binary to audit (report mode)")
    ap.add_argument("--baseline", help="baseline binary (diff mode)")
    ap.add_argument("--candidate", help="candidate binary (diff mode)")
    ap.add_argument("--objdump", help="objdump program to use")
    ap.add_argument(
        "--hot-regex",
        action="append",
        default=[],
        help="extra regex marking a symbol as decoder hot-path (repeatable)",
    )
    ap.add_argument(
        "--hot-only", action="store_true", help="report only hot-path symbols"
    )
    ap.add_argument(
        "--verbose",
        "-v",
        action="store_true",
        help="list each offending address and disassembly",
    )
    ap.add_argument("--top", type=int, default=0, help="limit tables to top N symbols")
    ap.add_argument("--json", action="store_true", help="emit JSON")
    ap.add_argument("--self-test", action="store_true", help="run built-in tests")
    args = ap.parse_args(argv)

    if args.self_test:
        return self_test()

    matcher = make_hot_matcher(args.hot_regex)

    if args.baseline or args.candidate:
        if not (args.baseline and args.candidate):
            ap.error("diff mode needs both --baseline and --candidate")
        objdump = find_objdump(args.objdump)
        base_off = parse_objdump(run_objdump(objdump, args.baseline), matcher)
        cand_off = parse_objdump(run_objdump(objdump, args.candidate), matcher)
        if args.json:
            print(
                json.dumps(
                    {
                        "baseline": build_json(args.baseline, base_off),
                        "candidate": build_json(args.candidate, cand_off),
                    },
                    indent=2,
                )
            )
        else:
            print(report_diff(args.baseline, args.candidate, base_off, cand_off, args))
        return 0

    if not args.binary:
        ap.error("provide a BINARY, or --baseline/--candidate, or --self-test")

    objdump = find_objdump(args.objdump)
    offenders = parse_objdump(run_objdump(objdump, args.binary), matcher)
    if args.json:
        print(json.dumps(build_json(args.binary, offenders), indent=2))
    else:
        print(report_single(args.binary, offenders, args))
    return 0


if __name__ == "__main__":
    sys.exit(main())
