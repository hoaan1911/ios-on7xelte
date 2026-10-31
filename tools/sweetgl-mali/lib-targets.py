#!/usr/bin/env python3
"""Print the ninja target of every static/shared library meson knows about, one line, from stdin
(`meson introspect build --targets`). Mesa's default ninja target skips the core libraries (state tracker,
gallium auxiliary, NIR, GLSL) when no EGL/DRI/GBM target consumes them, so they are requested by name."""
import json
import sys

targets = json.load(sys.stdin)
out = []
for t in targets:
    if t['type'] in ('static library', 'shared library'):
        path = t['filename'][0]
        out.append(path.split('/build/', 1)[-1] if '/build/' in path else path)
print(' '.join(out))
