"""Compares recompiler output generated with lean_codegen off and on, function by function.
    python local/lean/lean_diff.py <off output dir> <on output dir> [--show N] [--func NAME]

For every generated function present in both outputs the `on` body is normalised back to the
`off` shape (lean memory helpers -> the lambda macros, dropping the pc arguments; constant
fast writes -> the same FAST_WRITE call; GPR_*(ctx, 0) folds applied to the `off` side) and
the designed differences are removed from both sides (every `ctx->pc = <const>;`,
`ctx->in_delay_slot = ...;`, `ctx->branch_pc = ...;` line and the lean #include). Whatever
still differs is reported: it is a change this tool does not know about and the reviewer
should look at. On the `on` side it also checks the pc invariant per instruction record (the
lines after one `// 0xADDR:` comment): every line that can reach the runtime (`runtime->`,
`ps2_syscalls::`, `ps2_stubs::`, a lambda READn(/WRITEn( macro, throw) must be preceded in
its record by a `ctx->pc = 0x...u;` store of that record's address (or of the delay slot's,
inside a branch record), or be a dispatch whose target store precedes it.
Prints counts: pc stores, delay-slot stores, lambda/lean memory accesses, bytes, per side.
Exit code 1 when a residual difference or an invariant violation is found."""
import argparse
import glob
import os
import re
import sys

FUNC = re.compile(r'^void ([A-Za-z_][A-Za-z0-9_]*)\(uint8_t\* (?:__restrict )?rdram, R5900Context\* (?:__restrict )?ctx, PS2Runtime \*runtime\) \{$', re.M)
PC_STORE = re.compile(r'^\s*ctx->pc = 0x[0-9A-Fa-f]+u;\s*$')
DS_STORE = re.compile(r'^\s*ctx->(in_delay_slot = (true|false)|branch_pc = 0x[0-9A-Fa-f]+u);\s*$')
LEAN_INCLUDE = '#include "ps2_runtime_lean_macros.h"'
COMMENT = re.compile(r'^\s*// 0x([0-9a-f]+): 0x[0-9a-f]+')
OLD_MACRO = re.compile(r'(?<![A-Za-z0-9_])(READ|WRITE)(8|16|32|64|128)\(')
OLD_FAST_WRITE = re.compile(
    r'do \{ (uint(?:8|16|32|64)_t) _value = static_cast<\1>\((.*?)\); '
    r'ps2TraceGuestWrite\(rdram, (0x[0-9A-Fa-f]+u), \d+u, _value, 0u, "WRITE(\d+)", ctx\); '
    r'FAST_WRITE\4\(\3, _value\); \} while \(0\)')
OLD_FAST_WRITE128 = re.compile(
    r'do \{ __m128i _value = \((.*?)\); const uint64_t _lo = static_cast<uint64_t>\(PS2_EXTRACT_EPI64_0\(_value\)\); '
    r'const uint64_t _hi = static_cast<uint64_t>\(PS2_EXTRACT_EPI64_1\(_value\)\); '
    r'ps2TraceGuestWrite\(rdram, (0x[0-9A-Fa-f]+u), 16u, _lo, _hi, "WRITE128", ctx\); FAST_WRITE128\(\2, _value\); \} while \(0\)')
ZERO_FOLDS = [('GPR_U32(ctx, 0)', '0u'), ('GPR_S32(ctx, 0)', '0'), ('GPR_U64(ctx, 0)', '0ull'),
              ('GPR_S64(ctx, 0)', '0ll'), ('GPR_VEC(ctx, 0)', '_mm_setzero_si128()')]
RUNTIME_REACH = ('runtime->', 'ps2_syscalls::', 'ps2_stubs::', 'throw')


def split_args(text, start):
    """Top-level comma-separated arguments of the call whose '(' is at text[start]; returns
    (args, index after the closing ')')."""
    depth = 0
    args, cur = [], []
    i = start
    while i < len(text):
        c = text[i]
        if c == '(':
            depth += 1
            if depth > 1:
                cur.append(c)
        elif c == ')':
            depth -= 1
            if depth == 0:
                args.append(''.join(cur).strip())
                return args, i + 1
            cur.append(c)
        elif c == ',' and depth == 1:
            args.append(''.join(cur).strip())
            cur = []
        else:
            cur.append(c)
        i += 1
    raise ValueError('unbalanced call')


LEAN_CALL = re.compile(r'PS2X_(READ|WRITE)(8|16|32|64|128)(_DS)?\(')


def unlean(code):
    """Lean memory helpers back to the lambda macros (pc arguments dropped)."""
    out = []
    pos = 0
    while True:
        m = LEAN_CALL.search(code, pos)
        if not m:
            out.append(code[pos:])
            break
        out.append(code[pos:m.start()])
        args, end = split_args(code, m.end() - 1)
        kind, width, ds = m.group(1), m.group(2), m.group(3)
        n = (2 if kind == 'WRITE' else 1)  # arguments that are not pc/branch pc
        expected = n + (2 if ds else 1)
        if len(args) != expected:
            raise ValueError('unexpected lean call shape: ' + code[m.start():end])
        out.append('%s%s(%s)' % (kind, width, ', '.join(args[:n])))
        pos = end
    return ''.join(out)


def fold_zero(code):
    for pat, rep in ZERO_FOLDS:
        i = code.find(pat)
        while i >= 0:
            if i == 0 or not (code[i - 1].isalnum() or code[i - 1] == '_'):
                code = code[:i] + rep + code[i + len(pat):]
                i = code.find(pat, i + len(rep))
            else:
                i = code.find(pat, i + len(pat))
    return code


def normalise_off(body):
    body = OLD_FAST_WRITE.sub(lambda m: 'FAST_WRITE%s(%s, %s)' % (m.group(4), m.group(3), m.group(2)), body)
    body = OLD_FAST_WRITE128.sub(lambda m: 'FAST_WRITE128(%s, %s)' % (m.group(2), m.group(1)), body)
    return fold_zero(body)


def strip_designed(body):
    lines = [l for l in body.split('\n') if not PC_STORE.match(l) and not DS_STORE.match(l) and LEAN_INCLUDE not in l]
    return '\n'.join(lines)


def functions(folder):
    out = {}
    for f in sorted(glob.glob(os.path.join(folder, '*.cpp'))):
        if os.path.basename(f) == 'register_functions.cpp':
            continue
        s = open(f, encoding='utf-8', errors='replace').read()
        heads = list(FUNC.finditer(s))
        for i, m in enumerate(heads):
            end = heads[i + 1].start() if i + 1 < len(heads) else len(s)
            body = s[m.end():end]
            cut = body.find('\n}\n')
            out[m.group(1)] = body[:cut] if cut >= 0 else body
    return out


def counts(folder, bodies):
    pc = ds = lam = lean = 0
    for b in bodies.values():
        for l in b.split('\n'):
            if PC_STORE.match(l):
                pc += 1
            elif DS_STORE.match(l):
                ds += 1
        lam += len(OLD_MACRO.findall(b))
        lean += len(LEAN_CALL.findall(b))
    size = sum(os.path.getsize(f) for f in glob.glob(os.path.join(folder, '*.cpp')))
    return dict(functions=len(bodies), pc_stores=pc, delay_slot_stores=ds, lambda_memory=lam, lean_memory=lean, cpp_bytes=size)


def check_pc_invariant(name, body):
    """Returns a list of violation messages for one `on` function body."""
    problems = []
    records = []  # (addr or None, [lines])
    cur = (None, [])
    for line in body.split('\n'):
        m = COMMENT.match(line)
        if m and 'Delay Slot' not in line:
            records.append(cur)
            cur = (int(m.group(1), 16), [])
        else:
            cur[1].append(line)
    records.append(cur)
    for addr, lines in records:
        stored = set()
        for line in lines:
            s = line.strip()
            pm = re.match(r'ctx->pc = (0x[0-9A-Fa-f]+)u;$', s)
            if pm:
                stored.add(int(pm.group(1), 16))
                continue
            if s == 'ctx->pc = jumpTarget;':
                stored.add(-1)  # a register jump's target (jr/jalr)
                continue
            if s.startswith('//'):
                continue
            reaches = any(k in s for k in RUNTIME_REACH) or OLD_MACRO.search(s)
            if not reaches:
                continue
            if 'dispatchGuestBranch' in s or 'eeCheckpointDue' in s or re.search(r'\b[A-Za-z_][A-Za-z0-9_]*\(rdram, ctx, runtime\); return;', s):
                if not stored:
                    problems.append('%s @%s: transfer without a preceding pc store: %s' % (name, hex(addr) if addr else '?', s[:100]))
                continue
            ok = addr is None or addr in stored or (addr + 4) in stored
            if not ok:
                problems.append('%s @%s: runtime-reaching line without a pc store for its address: %s' % (name, hex(addr), s[:100]))
    return problems


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('off')
    ap.add_argument('on')
    ap.add_argument('--show', type=int, default=8, help='residual diffs to print')
    ap.add_argument('--func', help='print the normalised bodies of this function')
    args = ap.parse_args()
    foff, fon = functions(args.off), functions(args.on)
    coff, con = counts(args.off, foff), counts(args.on, fon)
    print('%-20s %14s %14s' % ('', 'off', 'on'))
    for k in coff:
        print('%-20s %14s %14s' % (k, coff[k], con[k]))
    only_off = sorted(set(foff) - set(fon))
    only_on = sorted(set(fon) - set(foff))
    print('functions only in off: %d, only in on: %d' % (len(only_off), len(only_on)))
    for n in (only_off + only_on)[:5]:
        print('   ', n)

    residual = []
    violations = []
    identical = 0
    for n in sorted(set(foff) & set(fon)):
        a = strip_designed(normalise_off(foff[n]))
        b = strip_designed(unlean(fon[n]))
        if args.func == n:
            print('----- off (normalised)\n' + a + '\n----- on (normalised)\n' + b)
        if a == b:
            identical += 1
        else:
            la, lb = a.split('\n'), b.split('\n')
            import difflib
            d = [l for l in difflib.unified_diff(la, lb, lineterm='', n=0) if l[:1] in '+-' and not l.startswith(('+++', '---'))]
            residual.append((n, d))
        violations += check_pc_invariant(n, fon[n])
    print('common functions: %d, equivalent after normalisation: %d, residual differences: %d' %
          (len(set(foff) & set(fon)), identical, len(residual)))
    for n, d in residual[:args.show]:
        print('  ', n)
        for l in d[:8]:
            print('      ', l[:200])
    print('pc invariant violations on the lean side: %d' % len(violations))
    for v in violations[:args.show]:
        print('  ', v)
    sys.exit(1 if residual or violations or only_off or only_on else 0)


if __name__ == '__main__':
    main()
