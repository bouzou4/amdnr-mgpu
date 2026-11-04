"""Splits R"( ... )" literals over 16000 chars into adjacent pieces at line breaks (MSVC v143 C2026).
The concatenated text is unchanged. Usage: python split_raw_literals.py <file>..."""
import re
import sys
from pathlib import Path

LIMIT = 16000
pat = re.compile(r'R"\((.*?)\)"', re.S)

def split(body: str) -> str:
    out, start = [], 0
    while len(body) - start > LIMIT:
        cut = body.rfind('\n', start, start + LIMIT) + 1
        if cut <= start:
            raise SystemExit('no line break to split at')
        out.append(body[start:cut])
        start = cut
    out.append(body[start:])
    return ')"\nR"('.join(out)

for p in map(Path, sys.argv[1:]):
    src = p.read_bytes().decode('utf-8')
    n = 0
    def rep(m):
        global n
        if len(m.group(1)) <= LIMIT:
            return m.group(0)
        n += 1
        return 'R"(' + split(m.group(1)) + ')"'
    out = pat.sub(rep, src)
    if n:
        p.write_bytes(out.encode('utf-8'))
    print(p, n)
