#!/usr/bin/env python3
from pathlib import Path
import sys

src = Path(sys.argv[1])
dst = Path(sys.argv[2])
text = src.read_text(encoding="utf-8")
lines = []
for line in text.splitlines():
    escaped = (
        line.replace("\\", "\\\\")
        .replace('"', '\\"')
        .replace("\r", "")
    )
    lines.append(f'  "{escaped}\\n"')
if not lines:
    lines.append('  ""')
dst.write_text("\n".join(lines) + "\n", encoding="utf-8")
print(f"wrote {dst} ({len(text)} bytes js, {len(lines)} lines)")
