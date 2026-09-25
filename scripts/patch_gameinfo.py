#!/usr/bin/env python3
"""Add Ready Up's search path to CS2's gameinfo.gi (or gameinfo_branchspecific.gi).

    python3 readyup/tools/patch_gameinfo.py gameinfo.gi --game csgo/readyup
    python3 readyup/tools/patch_gameinfo.py gameinfo.gi --remove      # uninstall

Placement rules (SearchPaths block):
  * `Game csgo/readyup` must come BEFORE `Game csgo`, otherwise CS2 loads its own
    libserver.so and Ready Up never runs.
  * If Metamod is installed (`Game csgo/addons/metamod` above `Game csgo`), Ready Up goes
    directly AFTER Metamod's line. Metamod then loads first and Ready Up is what it loads as
    "the game". In the other order Metamod never loads (Ready Up warns at startup).

The patch is idempotent: an entry that already satisfies the rules is left alone, and
duplicate or misplaced entries are collapsed into one correctly placed line. A timestamped
backup (<file>.readyup-backup-<YYYYmmdd-HHMMSS>) is written next to the file whenever it
changes. --remove deletes every Ready Up entry (Metamod and the rest are left alone).

Exit codes: 0 ok (patched / already correct), 1 error or no insertion point,
3 with --check when a change would be made.
"""
from __future__ import annotations

import argparse
import datetime as _dt
import enum
import os
import pathlib
import re
import shutil
import sys
from typing import List, Optional


class PatchResult(str, enum.Enum):
    PATCHED = "patched"
    ALREADY_PRESENT = "already_present"
    SKIPPED = "skipped"
    REMOVED = "removed"
    ALREADY_ABSENT = "already_absent"


def _leading_ws(s: str) -> str:
    m = re.match(r"^\s*", s)
    return m.group(0) if m else ""


def _game_line_re(path_value: str) -> "re.Pattern[str]":
    # `Game <value>` with optional trailing comment. `Game_LowViolence csgo_lv` etc. don't match.
    return re.compile(r"^\s*Game\s+%s\s*(?://.*)?$" % re.escape(path_value))


def _find(lines: List[str], rx: "re.Pattern[str]") -> List[int]:
    return [i for i, line in enumerate(lines) if rx.match(line.rstrip("\r\n"))]


def _newline_of(lines: List[str]) -> str:
    for line in lines:
        if line.endswith("\r\n"):
            return "\r\n"
        if line.endswith("\n"):
            return "\n"
    return "\n"


def _searchpaths_insert_index(lines: List[str]) -> Optional[int]:
    """Index just inside the SearchPaths `{`, or None."""
    for i, line in enumerate(lines):
        if not re.match(r"^\s*SearchPaths\s*$", line.rstrip("\r\n")):
            continue
        for j in range(i + 1, len(lines)):
            stripped = lines[j].strip()
            if stripped.startswith("{"):
                return j + 1
            if stripped == "" or stripped.startswith("//"):
                continue
            return None
    return None


def compute_patch(text: str, game_name: str) -> Optional[str]:
    """Return the patched text, the unchanged text if already correct, or None if there is
    no place to put the entry."""
    lines = text.splitlines(keepends=True)
    nl = _newline_of(lines)
    ours_re = _game_line_re(game_name)
    csgo_re = _game_line_re("csgo")
    metamod_re = _game_line_re("csgo/addons/metamod")

    ours = _find(lines, ours_re)
    csgo = _find(lines, csgo_re)
    metamod = _find(lines, metamod_re)
    csgo_idx = csgo[0] if csgo else None
    # Metamod only counts when it can actually take effect (above `Game csgo`).
    mm_idx = metamod[0] if metamod and (csgo_idx is None or metamod[0] < csgo_idx) else None

    # Already correct: exactly one entry, below Metamod (if any) and above `Game csgo`.
    if len(ours) == 1:
        idx = ours[0]
        after_mm = mm_idx is None or idx > mm_idx
        before_csgo = csgo_idx is None or idx < csgo_idx
        if after_mm and before_csgo:
            return text

    # Otherwise drop every existing entry and insert exactly one.
    kept = [line for i, line in enumerate(lines) if i not in set(ours)]
    csgo = _find(kept, csgo_re)
    metamod = _find(kept, metamod_re)
    csgo_idx = csgo[0] if csgo else None
    mm_idx = metamod[0] if metamod and (csgo_idx is None or metamod[0] < csgo_idx) else None

    if mm_idx is not None:
        at, indent = mm_idx + 1, _leading_ws(kept[mm_idx])
    elif csgo_idx is not None:
        at, indent = csgo_idx, _leading_ws(kept[csgo_idx])
    else:
        at = _searchpaths_insert_index(kept)
        if at is None:
            return None
        indent = ""
        for k in range(at, len(kept)):
            s = kept[k].strip()
            if s and not s.startswith("//") and s != "}":
                indent = _leading_ws(kept[k])
                break
        if not indent:
            indent = "\t\t\t"

    if at > 0 and not kept[at - 1].endswith(("\n", "\r")):
        kept[at - 1] += nl
    kept.insert(at, f"{indent}Game\t{game_name}{nl}")
    return "".join(kept)


def compute_remove(text: str, game_name: str) -> str:
    """Return the text without any `Game <game_name>` line."""
    lines = text.splitlines(keepends=True)
    ours = set(_find(lines, _game_line_re(game_name)))
    return "".join(line for i, line in enumerate(lines) if i not in ours)


def patch_gameinfo(path: pathlib.Path, game_name: str, check_only: bool = False, remove: bool = False) -> PatchResult:
    st = path.stat()
    raw = path.read_bytes()
    text = raw.decode("utf-8", errors="surrogateescape")

    if remove:
        new_text = compute_remove(text, game_name)
        if new_text == text:
            return PatchResult.ALREADY_ABSENT
        if check_only:
            return PatchResult.REMOVED
    else:
        new_text = compute_patch(text, game_name)
        if new_text is None:
            return PatchResult.SKIPPED
        if new_text == text:
            return PatchResult.ALREADY_PRESENT
        if check_only:
            return PatchResult.PATCHED

    stamp = _dt.datetime.now().strftime("%Y%m%d-%H%M%S")
    backup = path.with_suffix(path.suffix + f".readyup-backup-{stamp}")
    n = 1
    while backup.exists():  # two changes within one second (install + uninstall in a test)
        n += 1
        backup = path.with_suffix(path.suffix + f".readyup-backup-{stamp}-{n}")
    shutil.copy2(path, backup)

    tmp = path.with_suffix(path.suffix + ".readyup-tmp")
    tmp.write_bytes(new_text.encode("utf-8", errors="surrogateescape"))
    # Preserve permissions and (when possible) ownership so running under sudo
    # doesn't leave root-owned configs behind.
    os.chmod(tmp, st.st_mode & 0o7777)
    try:
        os.chown(tmp, st.st_uid, st.st_gid)
        os.chown(backup, st.st_uid, st.st_gid)
    except PermissionError:
        pass
    os.replace(tmp, path)
    return PatchResult.REMOVED if remove else PatchResult.PATCHED


def main() -> int:
    ap = argparse.ArgumentParser(description="Add Ready Up's Game search path to CS2 gameinfo.gi.")
    ap.add_argument("path", type=pathlib.Path, help="Path to gameinfo.gi (or gameinfo_branchspecific.gi)")
    ap.add_argument("--game", default="csgo/readyup", help="Search path to add (default: csgo/readyup)")
    ap.add_argument("--check", action="store_true", help="Only report; exit 3 if a change is needed")
    ap.add_argument("--remove", action="store_true", help="Remove the search path instead of adding it")
    args = ap.parse_args()

    try:
        result = patch_gameinfo(args.path, args.game, check_only=args.check, remove=args.remove)
    except Exception as e:  # noqa: BLE001 - report any I/O problem plainly
        print(f"ERROR: {e}", file=sys.stderr)
        return 1

    if result == PatchResult.PATCHED:
        print(f"{'Needs patch' if args.check else 'Patched'}: {args.path}")
        return 3 if args.check else 0
    if result == PatchResult.ALREADY_PRESENT:
        print(f"Already present: {args.path}")
        return 0
    if result == PatchResult.REMOVED:
        print(f"{'Needs removal' if args.check else 'Removed'}: {args.path}")
        return 3 if args.check else 0
    if result == PatchResult.ALREADY_ABSENT:
        print(f"Not present: {args.path}")
        return 0
    print(f"Skipped (no SearchPaths / Game csgo entry found): {args.path}", file=sys.stderr)
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
