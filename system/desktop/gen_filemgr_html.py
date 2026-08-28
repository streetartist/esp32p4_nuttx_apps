#!/usr/bin/env python3
from pathlib import Path

src = Path(__file__).with_name("desktop_filemgr.html")
dst = Path(__file__).with_name("desktop_filemgr_html.inc")
text = src.read_text(encoding="utf-8")
marker = "%%NAME_MAX%%"
lines = []
for line in text.splitlines():
    escaped = (
        line.replace("\\", "\\\\")
        .replace('"', '\\"')
        .replace("\r", "")
    )
    if marker in escaped:
        left, right = escaped.split(marker, 1)
        lines.append(f'  "{left}" FM_STRINGIFY(CONFIG_NAME_MAX) "{right}\\n"')
    else:
        lines.append(f'  "{escaped}\\n"')
if not lines:
    lines.append('  ""')
dst.write_text("\n".join(lines) + "\n", encoding="utf-8")
print(f"wrote {dst} ({len(text)} bytes html, {len(lines)} lines)")
