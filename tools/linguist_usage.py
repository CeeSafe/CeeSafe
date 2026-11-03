#!/usr/bin/env python3
"""How far CeeSafe is from GitHub Linguist's usage threshold (editors/linguist/README.md).

Linguist adds an extension once GitHub search indexes at least 2000 files with it in the last year, excluding forks,
spread across many unrelated repositories; the language owner's accounts are filtered out. This counts `.cee` files
through GitHub's code search API (`gh api`, so `gh auth login` first):

- all `.cee` files;
- those outside the owner's accounts (OWNERS);
- of those, the ones that look like CeeSafe (they contain `borrow`, as nearly every CeeSafe file does): other
  languages use `.cee` too, and Linguist's reviewers would not count them;

then lists the repositories behind the last count. The API's counts are approximate and it cannot filter by indexing
date, so treat the numbers as a guide; Linguist's reviewers use the web search.

usage: python3 tools/linguist_usage.py [--owners vkuttyp,CeeSafe] [--top 15]
"""

import argparse
import collections
import json
import subprocess
import sys
import time

THRESHOLD = 2000


def search(query, per_page=1):
    r = subprocess.run(["gh", "api", "-X", "GET", "search/code", "-f", f"q={query}", "-f", f"per_page={per_page}"],
                       capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit(f"linguist_usage: gh api failed: {r.stderr.strip()}")
    time.sleep(7)  # the code search API allows 10 requests a minute
    return json.loads(r.stdout)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--owners", default="vkuttyp,CeeSafe", help="the language owner's accounts, comma-separated")
    ap.add_argument("--top", type=int, default=15)
    args = ap.parse_args()
    exclude = " ".join(f"-user:{o}" for o in args.owners.split(","))
    total = search("extension:cee")["total_count"]
    others = search(f"extension:cee {exclude}")["total_count"]
    shaped = search(f"extension:cee borrow {exclude}", per_page=100)
    repos = collections.Counter(item["repository"]["full_name"] for item in shaped["items"])
    print(f"linguist_usage: .cee files {total}; outside {args.owners}: {others}; of those CeeSafe-like: "
          f"{shaped['total_count']}; Linguist's threshold: {THRESHOLD} ({shaped['total_count'] * 100 // THRESHOLD}%)")
    for repo, n in repos.most_common(args.top):
        print(f"  {n:4} {repo}")


if __name__ == "__main__":
    main()
