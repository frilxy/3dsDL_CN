#!/usr/bin/env python3
"""Applies zh/zh.tsv to the working tree.

The source stays upstream's own; the Chinese lives in the catalog. To follow a
new upstream release, merge it as usual and run this again: every block the
catalog no longer matches is listed, and those are the only things to fix.

    python3 scripts/zh/apply.py            # translate in place
    python3 scripts/zh/apply.py --check    # report what no longer matches
    python3 scripts/zh/apply.py --reverse  # back to upstream's English

Exit status is 1 under --check when a block did not apply, which is what CI
and a maintainer want to see after an upstream merge.
"""

import argparse
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
CATALOG = ROOT / "zh" / "zh.tsv"
SUFFIX = ROOT / "zh" / "VERSION_SUFFIX"


def unescape(text):
    out = []
    index = 0
    while index < len(text):
        char = text[index]
        if char != "\\":
            out.append(char)
            index += 1
            continue
        index += 1
        if index == len(text):
            out.append("\\")
            break
        code = text[index]
        out.append({"t": "\t", "n": "\n", "r": "\r", "\\": "\\"}.get(code, code))
        index += 1
    return "".join(out)


def escape(text):
    return (text.replace("\\", "\\\\").replace("\t", "\\t")
            .replace("\n", "\\n").replace("\r", "\\r"))


def load():
    entries = []
    for number, line in enumerate(CATALOG.read_text("utf-8").splitlines(), 1):
        if not line or line.startswith("#"):
            continue
        parts = line.split("\t")
        if len(parts) != 3:
            raise SystemExit(f"zh.tsv:{number}: expected 3 columns, got {len(parts)}")
        entries.append((parts[0], unescape(parts[1]), unescape(parts[2])))
    return entries


def version(apply_now):
    """Marks the build as this fork's, so an update never looks like upstream."""
    path = ROOT / "VERSION"
    original = path.read_text("utf-8").strip()
    if not SUFFIX.exists():
        return None
    suffix = SUFFIX.read_text("utf-8").strip()
    if not suffix:
        return None
    base = original.split("-")[0]
    if apply_now:
        path.write_text(f"{base}-{suffix}\n", "utf-8")
    return f"{base}-{suffix}"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true",
                        help="report unmatched blocks instead of writing")
    parser.add_argument("--reverse", action="store_true",
                        help="turn the translations back into upstream's English")
    parser.add_argument("--quiet", action="store_true")
    args = parser.parse_args()

    entries = load()
    if args.reverse:
        entries = [(path, new, old) for path, old, new in entries]

    # Longest blocks first: a wide block that already covers a smaller one
    # must claim its text before the smaller block does, or the smaller one
    # would rewrite it back out of context.
    grouped = {}
    for path, old, new in entries:
        grouped.setdefault(path, []).append((old, new))

    files = {}
    unmatched = []
    for path, blocks in grouped.items():
        target = ROOT / path
        if not target.exists():
            for old, _ in blocks:
                unmatched.append((path, old, "missing file"))
            continue
        text = target.read_text("utf-8")
        for old, new in sorted(blocks, key=lambda block: -len(block[0])):
            if text.count(old):
                text = text.replace(old, new)
            elif new not in text:
                unmatched.append((path, old, "not found"))
        files[path] = text

    applied = len(entries) - len(unmatched)
    if not args.check:
        for path, text in files.items():
            if text is not None:
                (ROOT / path).write_text(text, "utf-8")
        marked = version(apply_now=not args.reverse)
    else:
        marked = version(apply_now=False)

    if not args.quiet:
        print(f"zh: {applied}/{len(entries)} blocks applied"
              + (f", version {marked}" if marked else ""))
        for path, old, why in unmatched:
            first = old.split("\n")[0]
            print(f"  {why}: {path}: {first[:100]}")

    if args.check and unmatched:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
