#!/usr/bin/env python3
"""Check local links in the Markdown developer wiki (stdlib only)."""

import re
import sys
from pathlib import Path
from urllib.parse import unquote, urlsplit

LINK = re.compile(r"\]\(([^\n)]*)\)")


def check(root: Path) -> list[str]:
    root = root.resolve()
    pages = [root / "README.md", *sorted((root / "docs").rglob("*.md"))]
    issues = []
    for page in pages:
        for line_number, line in enumerate(page.read_text(encoding="utf-8").splitlines(), 1):
            for match in LINK.finditer(line):
                raw = match.group(1).strip()
                # Markdown's optional title is not part of the URL.
                link = raw.partition(' "')[0].strip('<>')
                parsed = urlsplit(link)
                if parsed.scheme or parsed.netloc or not parsed.path:
                    continue
                destination = (page.parent / unquote(parsed.path)).resolve()
                if not destination.is_relative_to(root) or not destination.exists():
                    issues.append(f"{page.relative_to(root)}:{line_number}: broken local link: {link}")
    return issues


def main() -> int:
    root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parents[2]
    problems = check(root)
    if problems:
        print("\n".join(problems), file=sys.stderr)
        return 1
    print("PolyGraph wiki: all local Markdown links resolve")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
