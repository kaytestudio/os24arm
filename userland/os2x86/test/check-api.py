#!/usr/bin/env python3
# userland/os2x86/test/check-api.py - the names of pm_api.inc against OS/2's
# own headers (OpenWatcom's h/os2: pmwin.h, pmgpi.h, pmstddlg.h, ...): each
# enum or #define there with the value the headers give it.
#   check-api.py HEADERDIR pm_api.inc
# Names ending in _ are ours (WS_VISIBLE_, HWND_DESKTOP_), not checked; a
# name the headers don't have is listed, and fails unless it's in OURS.
import glob, re, sys

OURS = {'HLPM_WINDOW'}          # (IBM's pmhelp.h has it, OpenWatcom's doesn't)

def header_values(d):
    raw = {}
    for f in sorted(glob.glob(d + '/*.h')):
        for l in open(f, errors='replace'):
            m = re.match(r'\s*#\s*define\s+([A-Z][A-Za-z0-9_]+)\s+(.+?)\s*(/\*.*)?$', l)
            if m and m.group(1) not in raw: raw[m.group(1)] = m.group(2)
    def ev(e, depth=0):
        e = re.sub(r'\(\s*[A-Z][A-Z0-9]*\s*\)(?=\s*[-~(0-9A-Za-z_])', '', e)   # (a cast: (ULONG), (HBITMAP), ...)
        e = re.sub(r'\b(0x[0-9a-fA-F]+|\d+)[uUlL]+\b', r'\1', e)
        def name(m):
            if m.group(0) in raw and depth < 8: return '(' + str(ev(raw[m.group(0)], depth + 1)) + ')'
            raise ValueError(m.group(0))
        e = re.sub(r'\b[A-Z_][A-Za-z0-9_]*\b', name, e)
        if not re.fullmatch(r'[\s0-9a-fA-Fx()+\-|&<>~*]+', e): raise ValueError(e)
        return eval(e)
    out = {}
    for n, e in raw.items():
        try: out[n] = ev(e)
        except Exception: pass
    return out

def our_values(path):
    s = re.sub(r'/\*.*?\*/', '', open(path).read(), flags=re.S)
    ours = {}
    def ev(e):
        e = re.sub(r'(\d)u\b', r'\1', e)
        return eval(re.sub(r'\b[A-Za-z_][A-Za-z0-9_]*\b', lambda m: str(ours[m.group(0)]), e))
    for body in re.findall(r'enum\s*\{(.*?)\}', s, re.S):        # (C's counting: one more than the last)
        v = -1
        for item in [x.strip() for x in body.split(',') if x.strip()]:
            n, _, e = item.partition('=')
            v = ev(e) if e.strip() else v + 1
            ours[n.strip()] = v
    for n, e in re.findall(r'#define\s+([A-Z][A-Za-z0-9_]+)\s+(.+)', s):
        ours[n] = ev(e)
    return ours

hdr, ours = header_values(sys.argv[1]), our_values(sys.argv[2])
bad = [f"{n}: {v:#x}, OS/2's {hdr[n] & 0xFFFFFFFF:#x}" for n, v in ours.items()
       if n in hdr and (hdr[n] & 0xFFFFFFFF) != (v & 0xFFFFFFFF)]
unknown = sorted(n for n in ours if n not in hdr and not n.endswith('_') and n not in OURS)
checked = sum(1 for n in ours if n in hdr)
for b in bad: print("differs:", b)
for n in unknown: print("not in OS/2's headers:", n)
print(f"{checked} names as OS/2's headers have them" + ("" if not bad and not unknown else f"; {len(bad)} differ, {len(unknown)} unknown"))
sys.exit(1 if bad or unknown else 0)
