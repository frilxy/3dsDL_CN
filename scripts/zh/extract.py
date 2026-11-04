#!/usr/bin/env python3
"""Extract the Chinese catalog from a localized tree.

Run once, from the repository, with two revisions: the upstream baseline and
the hand-translated tree. It writes zh/zh.tsv, one line per replaced block:

    <path>\t<old text>\t<new text>

Tabs, newlines and backslashes are escaped so a block is always one line.
Blocks come from a line diff, so a translated line and its neighbours travel
together, and the catalog is readable and easy to hand-edit later.

    python3 scripts/zh/extract.py 68d7a80 a8caceb > zh/zh.tsv

Two directories work too, which is how the catalog is seeded from a tree that
was translated before the catalog existed:

    python3 scripts/zh/extract.py --dirs /tmp/upstream /tmp/translated > zh/zh.tsv
"""

import difflib
import pathlib
import subprocess
import sys


def show(rev, path):
    return subprocess.run(
        ["git", "show", f"{rev}:{path}"],
        check=True, capture_output=True,
    ).stdout.decode("utf-8")


def escape(text):
    return (text.replace("\\", "\\\\").replace("\t", "\\t")
            .replace("\n", "\\n").replace("\r", "\\r"))


def read_dir(root, path):
    return (pathlib.Path(root) / path).read_text("utf-8")


def is_text(path):
    try:
        (pathlib.Path(path)).read_text("utf-8")
        return True
    except (UnicodeDecodeError, OSError):
        return False


GAP = 0


def main():
    global GAP
    args = [a for a in sys.argv[1:] if not a.startswith("--gap=")]
    for a in sys.argv[1:]:
        if a.startswith("--gap="):
            GAP = int(a.split("=", 1)[1])
    sys.argv = [sys.argv[0]] + args
    gap = GAP
    if len(sys.argv) > 3 and sys.argv[1] == "--dirs":
        import pathlib
        base_dir, target_dir = sys.argv[2], sys.argv[3]
        names = []
        for path in sorted(pathlib.Path(target_dir).rglob("*")):
            if not path.is_file() or not is_text(path):
                continue
            if (pathlib.Path(base_dir) / path.relative_to(target_dir)).exists():
                names.append(str(path.relative_to(target_dir)))
        read = lambda root, path: read_dir(root, path)
    else:
        import pathlib
        base_dir, target_dir = sys.argv[1], sys.argv[2]
        names = subprocess.run(
            ["git", "diff", "--name-only", base_dir, target_dir],
            check=True, capture_output=True, text=True,
        ).stdout.split()
        read = show

    seen = set()
    out = []
    for path in names:
        # Tests stay as upstream wrote them, and the page editor's bundle is
        # built from its source, so neither belongs in the catalog.
        if path.startswith("tests/") or path == "VERSION" or path.endswith(".gz"):
            continue
        if path == ".github/workflows/ci.yml":
            continue
        try:
            old = read(base_dir, path).split("\n")
            new = read(target_dir, path).split("\n")
        except subprocess.CalledProcessError:
            continue

        matcher = difflib.SequenceMatcher(None, old, new, autojunk=False)
        # Neighbouring changes are one block, so a deleted or inserted line
        # is expressed as the replacement it really is.
        hunk = None
        hunks = []
        for tag, a0, a1, b0, b1 in matcher.get_opcodes():
            if tag == "equal":
                # A short unchanged run between two changes is held with them,
                # so an added line becomes part of a block that can be found
                # again rather than an insertion nothing can match.
                if hunk and (a1 - a0) > gap:
                    hunks.append(hunk)
                    hunk = None
                continue
            if hunk is None:
                hunk = [a0, a1, b0, b1]
            else:
                hunk[1], hunk[3] = a1, b1
        if hunk:
            hunks.append(hunk)

        for a0, a1, b0, b1 in hunks:
            block_old = "\n".join(old[a0:a1])
            block_new = "\n".join(new[b0:b1])
            if not block_old:
                # A pure insertion has no text to find later; it cannot be
                # replayed, so the catalog does not pretend it can.
                print(f"skip insert-only hunk in {path}: {block_new.splitlines()[:1]}",
                      file=sys.stderr)
                continue
            key = (path, block_old)
            if key in seen:
                continue
            seen.add(key)
            out.append(f"{path}\t{escape(block_old)}\t{escape(block_new)}")

    sys.stdout.write("\n".join(out) + "\n")


if __name__ == "__main__":
    main()
