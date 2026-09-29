#!/usr/bin/env bash
set -euo pipefail

launcher="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/_launch.sh"
bash -n "$launcher"

python3 - "$launcher" <<'PY'
from pathlib import Path
import sys

text = Path(sys.argv[1]).read_text()
needle = '''else
\t# spring-headless supports the same explicit write-dir switch.'''
if needle not in text:
    raise SystemExit("headless write-dir branch missing")
branch = text.split(needle, 1)[1].split("fi", 1)[0]
if 'LAUNCH_ARGS+=(--write-dir "$WRITEDIR")' not in branch:
    raise SystemExit("spring-headless launch omits explicit --write-dir")
print("test_launch_writedir: PASS")
PY
