#!/usr/bin/env python3
from pathlib import Path

src = Path(__file__).with_name("qpk_hap_shim.js")
dst = Path(__file__).with_name("qpk_hap_shim.inc")
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
