#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""scripts/pr-title.py on the titles pull requests have: a donotmerge mark anywhere, however it is written, fails;
any other title passes, and a call without one title is refused. Offline."""

import subprocess
import sys
from pathlib import Path

SCRIPT = Path(__file__).resolve().parent.parent / 'scripts' / 'pr-title.py'
CASES = [
    ('[donotmerge needs #374] [mii, am] Random Miis, the Mii editor, an empty database', 1),
    ("[dist, donotmerge] the AppImage's update information", 1),
    ('WIP: DO NOT MERGE', 1),
    ('[gui] do-not-merge until the art is in', 1),
    ('[mii, am] Random Miis, the Mii editor, an empty database', 0),
    ('[kernel] Do not wait for a merged core', 0),
    ('Merge pull request #381 from scarletnx/dravee/service/mii-editor', 0),
]


def run(*args):
    return subprocess.run([sys.executable, str(SCRIPT), *args], capture_output=True, text=True).returncode


def main():
    failed = 0
    for title, want in CASES:
        got = run(title)
        if got != want:
            print(f'FAIL {title!r}: exit {got}, wanted {want}')
            failed += 1
    if run() != 2 or run('a', 'b') != 2:
        print('FAIL a call without exactly one title is not refused with 2')
        failed += 1
    print(f'pr_title: {len(CASES) + 1 - failed} of {len(CASES) + 1} checks passed')
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
