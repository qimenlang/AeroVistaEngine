#!/usr/bin/env python3
"""Doc-sync audit helper, backing the doc-sync skill (.claude/skills/doc-sync).

Three mechanical checks that automate the doc-sync workflow:

  --list   list every design doc under doc/design/ (the "逐份判断" input)
  --stale  grep design docs + notes for stale symbols/terms (the "自查残留" step)
  --index  validate doc/private/README.md's file list against real files

Exit codes:
  0  clean — no findings; for --stale, also used when hits need human judgement
  1  objective findings — only --index uses this: a broken link / unlisted file
     is a fact, so this is safe to gate on
  2  scan error — a file could not be read, or the regex was invalid

--stale is report-only: a "hit" may be legitimate 已否决/已删除 explanatory
context, so hits do not change the exit code (the agent decides). --index is the
opposite: findings there are objective, so it exits 1 to allow CI gating.

Usage:
  python scripts/doc_sync_check.py --list
  python scripts/doc_sync_check.py --stale "isTcpPeerAlive|refreshConnectionState"
  python scripts/doc_sync_check.py --index
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DESIGN_DIR = ROOT / "doc" / "design"
NOTES_DIR = ROOT / "doc" / "notes"
INDEX_PATH = ROOT / "doc" / "private" / "README.md"

EXIT_OK = 0
EXIT_FINDINGS = 1  # objective findings (--index only)
EXIT_ERROR = 2     # scan could not complete

MAX_HITS = 200     # --stale 命中显示上限，超出则提示收窄 pattern


def design_docs() -> list[Path]:
    """All .md files under doc/design/, sorted."""
    return sorted(p for p in DESIGN_DIR.rglob("*.md") if p.is_file())


def note_docs() -> list[Path]:
    """All .md files under doc/notes/, sorted (empty if absent)."""
    if not NOTES_DIR.is_dir():
        return []
    return sorted(p for p in NOTES_DIR.rglob("*.md") if p.is_file())


def is_decision_log(path: Path) -> bool:
    """True when the path is a decision log (read-only for AI)."""
    return "决策日志" in path.name or "决策日志" in path.parts


def read_utf8(path: Path) -> tuple[str | None, str | None]:
    """Return (text, error). text is None and error set on failure."""
    try:
        return path.read_text(encoding="utf-8"), None
    except OSError as exc:
        return None, f"cannot read {path}: {exc}"


def window(line: str, pos: int, radius: int = 60) -> str:
    """截取以匹配位置为中心的窗口，保证匹配点不被截掉。"""
    start = max(0, pos - radius)
    end = min(len(line), pos + radius)
    prefix = "…" if start > 0 else ""
    suffix = "…" if end < len(line) else ""
    return f"{prefix}{line[start:end]}{suffix}"


def rel(path: Path) -> str:
    """Posix-style repo-relative path for display."""
    return path.relative_to(ROOT).as_posix()


def cmd_list() -> int:
    docs = design_docs()
    print(f"[doc-sync] doc/design 设计文档（共 {len(docs)} 份）：")
    for doc in docs:
        tag = "  [只读]" if is_decision_log(doc) else ""
        print(f"  {rel(doc)}{tag}")
    return EXIT_OK


def cmd_stale(pattern: str) -> int:
    if not pattern.strip():
        print("[doc-sync] --stale 需要非空 pattern")
        return EXIT_ERROR

    try:
        regex = re.compile(pattern)
    except re.error as exc:
        print(f"[doc-sync] 无效的正则 pattern {pattern!r}: {exc}")
        return EXIT_ERROR

    hits: list[str] = []
    had_error = False
    for doc in design_docs() + note_docs():
        text, err = read_utf8(doc)
        if err is not None:
            print(f"  {err}")
            had_error = True
            continue
        for line_no, line in enumerate(text.splitlines(), 1):
            match = regex.search(line)
            if match:
                tag = "[只读] " if is_decision_log(doc) else ""
                hits.append(f"  {rel(doc)}:{line_no}: {tag}{window(line, match.start())}")

    if hits:
        shown = hits[:MAX_HITS]
        print(f"[doc-sync] 残留检索 pattern={pattern!r} 命中 {len(hits)} 处（由你判断是否为「已否决/已删除」的说明性上下文）：")
        for h in shown:
            print(h)
        if len(hits) > MAX_HITS:
            print(f"  …另有 {len(hits) - MAX_HITS} 处未显示（pattern 过宽？请收窄）")
    else:
        print(f"[doc-sync] 残留检索 pattern={pattern!r} 无命中")

    return EXIT_ERROR if had_error else EXIT_OK


def cmd_index() -> int:
    text, err = read_utf8(INDEX_PATH)
    if err is not None:
        print(f"  {err}")
        return EXIT_ERROR

    index_dir = INDEX_PATH.parent
    # Markdown links to .md files, relative to the README's directory.
    # Strip only a leading "./" prefix (not a character set).
    linked = set()
    for m in re.finditer(r"\]\(([^)]*\.md)\)", text):
        target = m.group(1).strip()
        if "://" in target:  # skip external URLs
            continue
        linked.add(re.sub(r"^\./", "", target))

    broken = sorted(link for link in linked if not (index_dir / link).is_file())
    real = sorted(
        p.relative_to(index_dir).as_posix()
        for p in index_dir.rglob("*.md")
        if p.is_file() and p.relative_to(index_dir).as_posix() != "README.md"
    )
    unlisted = sorted(name for name in real if name not in linked)

    if not broken and not unlisted:
        print("[doc-sync] 索引 doc/private/README.md 与实际文件一致")
        return EXIT_OK

    if broken:
        print("[doc-sync] 失效链接（README 指向但文件不存在）：")
        for link in broken:
            print(f"  {link}")
    if unlisted:
        print("[doc-sync] 未登记文件（存在但 README 未列出）：")
        for name in unlisted:
            print(f"  {name}")
    return EXIT_FINDINGS


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--list", action="store_true", help="列出 doc/design 全部设计文档（决策日志标 [只读]）")
    group.add_argument("--stale", metavar="PATTERN", help="在设计文档+notes 检索残留符号/术语（正则，| 分隔多词）")
    group.add_argument("--index", action="store_true", help="校验 doc/private/README.md 文件清单")
    args = parser.parse_args()

    if args.list:
        return cmd_list()
    if args.stale is not None:
        return cmd_stale(args.stale)
    return cmd_index()


if __name__ == "__main__":
    sys.exit(main())
