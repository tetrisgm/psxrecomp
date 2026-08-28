#!/usr/bin/env python3
"""Regression guard for restored replacement code below the overlay floor."""

from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "runtime" / "src" / "memory.c"


def function_body(source: str, name: str) -> str:
    match = re.search(rf"\bvoid\s+{re.escape(name)}\s*\([^;]*?\)\s*\{{", source, re.S)
    if not match:
        raise AssertionError(f"missing function definition: {name}")
    start = match.end()
    depth = 1
    for pos in range(start, len(source)):
        if source[pos] == "{":
            depth += 1
        elif source[pos] == "}":
            depth -= 1
            if depth == 0:
                return source[start:pos]
    raise AssertionError(f"unterminated function definition: {name}")


def main() -> int:
    source = SOURCE.read_text(encoding="utf-8")
    body = function_body(source, "dirty_ram_clear_image_baseline")
    mismatch = body.find("if (!text_page_matches_ref_image(page))")
    mark = body.find("dirty_ram_mark_page(page << DIRTY_RAM_PAGE_SHIFT)", mismatch)
    keep = body.find("kept++", mismatch)
    leave = body.find("continue;", mismatch)
    if min(mismatch, mark, keep, leave) < 0 or not mismatch < mark < keep < leave:
        raise AssertionError(
            "divergent text pages are not marked dirty before baseline continues"
        )

    print("PASS: divergent restored text is admitted to the overlay cache window")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except AssertionError as exc:
        print(f"FAIL: {exc}")
        sys.exit(1)
