#!/usr/bin/env python3
"""Print the -Dname=value options Mesa's meson.options actually defines.

  mesa-opts.py MESA_DIR -Dfoo=bar -Dbaz=qux ...

Mesa adds and removes options between releases (the Pixel repo's set had display-info, 24.3.4 has none),
and meson stops at the first unknown one. Unknown options are dropped and named on stderr instead.
"""
import re
import sys
from pathlib import Path

mesa = Path(sys.argv[1])
src = next((mesa / n for n in ('meson.options', 'meson_options.txt') if (mesa / n).is_file()), None)
if src is None:
    sys.exit('no meson.options in ' + str(mesa))
known = set(re.findall(r"option\(\s*'([^']+)'", src.read_text()))
if len(known) < 20:
    sys.exit(f'only {len(known)} options parsed from {src}: format changed?')
for arg in sys.argv[2:]:
    name = arg[2:].split('=', 1)[0] if arg.startswith('-D') else None
    if name is None or name in known:
        print(arg)
    else:
        print(f'dropped unknown Mesa option: {name}', file=sys.stderr)
