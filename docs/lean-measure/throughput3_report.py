"""Table from throughput3.ps1 samples: python local/lean/throughput3_report.py [--tag cyc] [--from 2000] [--to 6000] [--marks 1000,2000,...]
Per run the EE thread's CPU seconds at each vsync mark (linear interpolation between the two
samples around the mark), the EE-CPU ms per 1000 vsyncs between --from and --to, and the EE
thread's saturation (CPU/host) over that stretch; then per-mode mean and spread and the on/off ratio."""
import argparse
import csv
import glob
import os
import re
import statistics

RUNS = r'C:\Users\Wyn\Desktop\dev\SRWZ-native\local\runs'


def at_mark(samples, key, mark):
    prev = None
    for s in samples:
        if s['vsync'] >= mark:
            if prev is None or s['vsync'] == mark:
                return s[key] if s['vsync'] == mark or prev is None else None
            f = (mark - prev['vsync']) / (s['vsync'] - prev['vsync'])
            return prev[key] + f * (s[key] - prev[key])
        prev = s
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--tag', default='cyc')
    ap.add_argument('--from', dest='lo', type=int, default=2000)
    ap.add_argument('--to', dest='hi', type=int, default=6000)
    ap.add_argument('--marks', default='1000,2000,3000,4000,5000,6000')
    args = ap.parse_args()
    marks = [int(m) for m in args.marks.split(',')]
    per_mode = {}
    print('%-6s %-4s ' % ('mode', 'run') + ' '.join('cpu@%d' % m for m in marks) + '   ms/1000vsync  eeSat  samples')
    for f in sorted(glob.glob(os.path.join(RUNS, 'lean-%s-*.samples.csv' % args.tag))):
        m = re.match(r'lean-%s-(\w+)-(\d+)\.samples\.csv$' % re.escape(args.tag), os.path.basename(f))
        if not m:
            continue
        mode, rnd = m.group(1), int(m.group(2))
        samples = [{k: float(v) for k, v in r.items()} for r in csv.DictReader(open(f))]
        samples = [s for s in samples if s['vsync'] >= 0]
        cpus = [at_mark(samples, 'maxThreadCpuS', mk) for mk in marks]
        c0, c1 = at_mark(samples, 'maxThreadCpuS', args.lo), at_mark(samples, 'maxThreadCpuS', args.hi)
        h0, h1 = at_mark(samples, 'hostS', args.lo), at_mark(samples, 'hostS', args.hi)
        cells = ' '.join(('%8.2f' % c) if c is not None else '       -' for c in cpus)
        if c0 is None or c1 is None:
            print('%-6s %-4d %s   (marks %d..%d not reached; %d samples)' % (mode, rnd, cells, args.lo, args.hi, len(samples)))
            continue
        ms = (c1 - c0) / (args.hi - args.lo) * 1e6
        sat = (c1 - c0) / (h1 - h0) if h1 > h0 else float('nan')
        print('%-6s %-4d %s   %12.1f  %5.2f  %7d' % (mode, rnd, cells, ms, sat, len(samples)))
        per_mode.setdefault(mode, []).append(ms)
    for mode, v in sorted(per_mode.items()):
        print('%s: mean %.1f ms EE-CPU per 1000 vsyncs over %d..%d (sd %.1f, min %.1f, max %.1f, n=%d)' %
              (mode, statistics.mean(v), args.lo, args.hi, statistics.pstdev(v), min(v), max(v), len(v)))
    if 'off' in per_mode and 'on' in per_mode:
        print('on/off: %.3f' % (statistics.mean(per_mode['on']) / statistics.mean(per_mode['off'])))


if __name__ == '__main__':
    main()
