#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""pr-title.py TITLE: fails while a pull request's title still holds a donotmerge mark (`[donotmerge needs #N]`, on a
pull request that waits for another), so its check stays red until what it needs is in and the mark is out of the
title. .github/workflows/pr-title.yml runs it whenever the title changes."""

import re
import sys

MARK = re.compile(r'do[\s_-]*not[\s_-]*merge', re.IGNORECASE)


def main(argv):
    if len(argv) != 2:
        print('usage: pr-title.py TITLE', file=sys.stderr)
        return 2
    if MARK.search(argv[1]):
        print(f'The title still says donotmerge: {argv[1]}', file=sys.stderr)
        print('Merge what it needs first, then take the mark out of the title.', file=sys.stderr)
        return 1
    print('The title holds no donotmerge mark.')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
