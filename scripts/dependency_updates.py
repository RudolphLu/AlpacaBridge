#!/usr/bin/env python3
"""Open one GitHub issue per dependency that has a newer upstream version.

The dependencies are the ones scripts/dependency_snapshot.py submits to the dependency
graph, so there is one inventory. What is checked:

- every ``pkg:github/OWNER/REPO@VERSION`` entry (cppcheck, zizmor, nlohmann/json, ConformU):
  the repository's latest GitHub release, or its highest version tag when it has no releases;
- every SDK in scripts/dependency_sdk_versions.json with an ``updates`` block
  (``{"provider": "release-page", "url": ..., "pattern": ...}``): the highest version the
  pattern's first group matches on that page.

Debian, Ubuntu and docker entries are not checked: apt and the image tag already resolve to
the newest package at build time, and .github/workflows/osv-scan.yml reports their
vulnerabilities. An SDK with no ``updates`` block is listed as a manual check.

Issues (state lives in a marker comment in each issue body; nothing is committed):
- a newer version opens "Update <name> <current> -> <latest>" (dependencies, enhancement, P4);
  a still newer one retitles it with a comment; the repository reaching it closes it;
- a source that cannot be read opens "Update check broken: <name>" (dependencies, bug, P4),
  closed when the source reads again, so a changed page is never mistaken for "no update".

--osv-summary reads osv-scanner JSON and opens one issue (dependencies, bug, P2) listing the
High/Critical findings (CVSS >= 7.0) no earlier summary issue reported.

  dependency_updates.py [--dry-run]            check and update issues (gh, GH_TOKEN, GH_REPO)
  dependency_updates.py --osv-summary FILE [--dry-run]
  dependency_updates.py --self-test
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
import urllib.error
import urllib.request
from pathlib import Path
from typing import Callable

sys.path.insert(0, str(Path(__file__).resolve().parent))
import dependency_snapshot as snapshot  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
MARKER = "dependency-updates"
MARKER_RE = re.compile(r"<!-- " + MARKER + r": (\{.*?\}) -->")
UPDATE_LABELS = ("dependencies", "enhancement", "P4")
BROKEN_LABELS = ("dependencies", "bug", "P4")
OSV_LABELS = ("dependencies", "bug", "P2")
TAG_PAGES = 10
OSV_THRESHOLD = 7.0  # CVSS: High is 7.0-8.9, Critical 9.0-10.0

HttpGet = Callable[[str], str]


class FetchError(Exception):
    pass


def parse_version(v: str) -> tuple[tuple[int, ...], bool]:
    """(numeric parts, is a pre-release) for '3.11.3', 'v1.25.2', '4.5.1-beta', '2.3'."""
    m = re.match(r"^v?(\d+(?:\.\d+)*)(.*)$", v.strip())
    if not m:
        raise ValueError(f"not a version: {v!r}")
    parts = tuple(int(x) for x in m.group(1).split("."))
    while len(parts) > 1 and parts[-1] == 0:
        parts = parts[:-1]
    return parts, bool(re.search(r"(alpha|beta|rc|pre|dev)", m.group(2), re.I))


def is_newer(latest: str, current: str) -> bool:
    (lt, lpre), (ct, cpre) = parse_version(latest), parse_version(current)
    return lt > ct or (lt == ct and cpre and not lpre)


def highest(versions: list[str]) -> str | None:
    stable = []
    for v in versions:
        try:
            parts, pre = parse_version(v)
        except ValueError:
            continue
        if not pre:
            stable.append((parts, v))
    return max(stable)[1] if stable else None


def http_get(url: str) -> str:
    headers = {"User-Agent": "alpacabridge-dependency-updates"}
    token = os.environ.get("GH_TOKEN") or os.environ.get("GITHUB_TOKEN")
    if token and url.startswith("https://api.github.com/"):
        headers["Authorization"] = f"Bearer {token}"
    try:
        with urllib.request.urlopen(urllib.request.Request(url, headers=headers), timeout=30) as r:
            return r.read().decode("utf-8", errors="replace")
    except urllib.error.HTTPError as e:
        raise FetchError(f"HTTP {e.code} from {url}") from e
    except (urllib.error.URLError, TimeoutError, OSError) as e:
        raise FetchError(f"{url}: {e}") from e


def latest_github(repo: str, get: HttpGet) -> str:
    try:
        tag = json.loads(get(f"https://api.github.com/repos/{repo}/releases/latest"))["tag_name"]
        if highest([tag]):
            return tag
    except FetchError as e:
        if "HTTP 404" not in str(e):
            raise
    # /tags is ordered by ref, not by version, so read every page (up to TAG_PAGES).
    tags: list[str] = []
    for page in range(1, TAG_PAGES + 1):
        url = f"https://api.github.com/repos/{repo}/tags?per_page=100" + (f"&page={page}" if page > 1 else "")
        try:
            batch = [t["name"] for t in json.loads(get(url))]
        except FetchError as e:
            if page > 1 and "HTTP 404" in str(e):
                break
            raise
        tags += batch
        if len(batch) < 100:
            break
    best = highest(tags)
    if not best:
        raise FetchError(f"{repo}: no release and no version tag")
    return best


def latest_page(url: str, pattern: str, get: HttpGet) -> str:
    best = highest(re.findall(pattern, get(url)))
    if not best:
        raise FetchError(f"{url}: no version matches {pattern!r} (has the page changed?)")
    return best


def targets(snap: dict, table: dict) -> tuple[list[dict], list[str]]:
    """(checkable dependencies, names of SDKs left to a manual check)."""
    out: dict[str, dict] = {}
    for path, man in snap["manifests"].items():
        for purl in man["resolved"]:
            m = re.match(r"^pkg:github/([^/@]+/[^/@?]+)@([^?#]+)$", purl)
            if m:
                repo, ver = m.groups()
                out.setdefault(repo.lower(), {"key": repo.lower(), "name": repo, "current": ver, "where": path,
                                              "check": ("github", repo), "source": f"https://github.com/{repo}/releases"})
    manual = []
    for e in table["sdks"]:
        up = e.get("updates")
        if up and up.get("provider") == "release-page" and e.get("version"):
            out.setdefault(e["name"], {"key": e["name"], "name": e["name"], "current": e["version"],
                                       "where": snapshot.TABLE, "check": ("page", up["url"], up["pattern"]),
                                       "source": up["url"]})
        else:
            manual.append(e["name"])
    return sorted(out.values(), key=lambda t: t["key"]), manual


def check(t: dict, get: HttpGet) -> dict:
    try:
        if t["check"][0] == "github":
            latest = latest_github(t["check"][1], get)
        else:
            latest = latest_page(t["check"][1], t["check"][2], get)
        return {**t, "latest": latest, "error": None}
    except (FetchError, KeyError, ValueError, json.JSONDecodeError) as e:
        return {**t, "latest": None, "error": str(e)}


def marker(state: dict) -> str:
    return f"<!-- {MARKER}: {json.dumps(state, sort_keys=True)} -->"


def read_marker(body: str) -> dict | None:
    m = MARKER_RE.search(body or "")
    if not m:
        return None
    try:
        d = json.loads(m.group(1))
    except json.JSONDecodeError:
        return None
    return d if isinstance(d, dict) else None


class GhIssues:
    """Issue operations through the gh CLI (GH_TOKEN and GH_REPO from the environment)."""

    def __init__(self, dry_run: bool):
        self.dry_run = dry_run

    def _gh(self, *args: str, stdin: str | None = None) -> str:
        if self.dry_run and args[1] != "list":
            print("DRY RUN: gh " + " ".join(args) + (f"\n{stdin}" if stdin else ""))
            return ""
        return subprocess.run(["gh", *args], input=stdin, check=True, capture_output=True, text=True).stdout

    def list(self, state: str) -> list[dict]:
        out = self._gh("issue", "list", "--label", "dependencies", "--state", state, "--limit", "500",
                       "--json", "number,title,body,state")
        return json.loads(out or "[]")

    def create(self, title: str, body: str, labels: tuple[str, ...]) -> None:
        args = ["issue", "create", "--title", title, "--body-file", "-"]
        for label in labels:
            args += ["--label", label]
        self._gh(*args, stdin=body)

    def update(self, number: int, title: str, body: str, comment: str) -> None:
        self._gh("issue", "edit", str(number), "--title", title, "--body-file", "-", stdin=body)
        self._gh("issue", "comment", str(number), "--body-file", "-", stdin=comment)

    def close(self, number: int, comment: str) -> None:
        self._gh("issue", "close", str(number), "--comment", comment)


def update_body(r: dict) -> str:
    return (f"{marker({'kind': 'update', 'key': r['key'], 'version': r['latest'], 'current': r['current']})}\n"
            f"`{r['name']}` has a newer upstream version.\n\n"
            f"| | |\n|---|---|\n| In this repository | `{r['current']}` |\n| Upstream | `{r['latest']}` |\n"
            f"| Pinned in | `{r['where']}` |\n| Source | {r['source']} |\n\n"
            "Bump the pin where it is declared (for a vendored SDK, also its directory and the "
            "`--pin` digest, see docs/development.md \"Dependency graph\"), check the upstream "
            "changelog, and let CI run. This issue closes by itself once the repository is at "
            "that version.\n\nOpened by `.github/workflows/dependency-updates.yml`.\n")


def broken_body(r: dict) -> str:
    return (f"{marker({'kind': 'broken', 'key': r['key']})}\n"
            f"The weekly update check could not read the upstream version of `{r['name']}`:\n\n"
            f"```\n{r['error']}\n```\n\nSource: {r['source']}\n\n"
            "Until this is fixed a new release of it goes unreported. If the page or repository "
            "moved, update the source in `scripts/dependency_sdk_versions.json` or the pin it "
            "comes from. This issue closes by itself when the source reads again.\n")


def reconcile(results: list[dict], issues, open_issues: list[dict]) -> list[str]:
    """Open, retitle and close issues so they match results. Returns one line per action."""
    by = {}
    for i in open_issues:
        st = read_marker(i.get("body", ""))
        if st and st.get("kind") in ("update", "broken") and "key" in st:
            by[(st["kind"], st["key"])] = (i, st)
    log = []
    for r in results:
        broken = by.get(("broken", r["key"]))
        if r["error"]:
            if not broken:
                issues.create(f"Update check broken: {r['name']}", broken_body(r), BROKEN_LABELS)
                log.append(f"opened broken-source issue for {r['name']}")
            continue
        if broken:
            issues.close(broken[0]["number"], f"The source reads again (`{r['latest']}`).")
            log.append(f"closed broken-source issue #{broken[0]['number']}")
        upd = by.get(("update", r["key"]))
        newer = is_newer(r["latest"], r["current"])
        title = f"Update {r['name']} {r['current']} -> {r['latest']}"
        if newer and not upd:
            issues.create(title, update_body(r), UPDATE_LABELS)
            log.append(f"opened update issue for {r['name']} {r['latest']}")
        elif newer and upd is not None and (upd[1].get("version"), upd[1].get("current")) != (r["latest"], r["current"]):
            note = (f"Upstream is now at `{r['latest']}`." if upd[1].get("version") != r["latest"]
                    else f"The repository is now at `{r['current']}`, still behind `{r['latest']}`.")
            issues.update(upd[0]["number"], title, update_body(r), note)
            log.append(f"retitled #{upd[0]['number']} to {r['current']} -> {r['latest']}")
        elif not newer and upd is not None:
            issues.close(upd[0]["number"], f"The repository is now at `{r['current']}`; "
                                           f"upstream latest is `{r['latest']}`.")
            log.append(f"closed update issue #{upd[0]['number']}")
    return log


def osv_findings(report: dict) -> list[dict]:
    """High/Critical groups (max_severity >= OSV_THRESHOLD) from osv-scanner --format json."""
    out = []
    for res in report.get("results") or []:
        for p in res.get("packages") or []:
            pkg = p.get("package", {})
            for g in p.get("groups") or []:
                try:
                    sev = float(g.get("max_severity") or "")
                except ValueError:
                    continue
                ids = sorted(g.get("ids") or [])
                if sev >= OSV_THRESHOLD and ids:
                    out.append({"id": ids[0], "ids": ids, "severity": sev, "package": pkg.get("name", "?"),
                                "version": pkg.get("version", "?")})
    return sorted(out, key=lambda f: (-f["severity"], f["id"]))


def osv_summary(report: dict, issues, all_issues: list[dict], repo: str) -> list[str]:
    reported = set()
    for i in all_issues:
        st = read_marker(i.get("body", ""))
        if st and st.get("kind") == "osv":
            reported.update(st.get("ids", []))
    new = [f for f in osv_findings(report) if not reported.intersection(f["ids"])]
    if not new:
        return []
    rows = "\n".join(f"| {f['severity']:.1f} | `{f['package']}` {f['version']} | {', '.join(f['ids'])} |" for f in new)
    body = (f"{marker({'kind': 'osv', 'ids': sorted({i for f in new for i in f['ids']})})}\n"
            f"The OSV scan of the nightly `.deb`'s installed runtime packages (Debian trixie) found "
            f"{len(new)} new High or Critical finding(s):\n\n| CVSS | Package | Advisories |\n|---|---|---|\n"
            f"{rows}\n\nAll findings, including lower and unscored ones, are under "
            f"https://github.com/{repo}/security/code-scanning (tool `osv-scanner`). These packages come "
            "from Debian, so a fix usually reaches users through `apt upgrade`; act here when AlpacaBridge "
            "pins, bundles or builds against an affected version.\n\nOpened by `.github/workflows/osv-scan.yml`.\n")
    issues.create(f"Vulnerabilities: {len(new)} new High/Critical in the nightly .deb's packages", body, OSV_LABELS)
    return [f"opened OSV summary issue for {len(new)} finding(s)"]


def self_test() -> int:
    assert is_newer("3.12.0", "v3.11.3") and not is_newer("v3.11.3", "3.11.3")
    assert is_newer("4.5.1", "4.5.1-beta") and not is_newer("v4.5.0", "4.5.1-beta")
    assert not is_newer("2.3.0", "2.3") and is_newer("2.3.1", "2.3")
    assert highest(["v1.2", "v1.10", "v2.0-rc1", "nightly"]) == "v1.10"

    pages = {
        "https://api.github.com/repos/a/b/releases/latest": json.dumps({"tag_name": "v2.0.0"}),
        "https://api.github.com/repos/c/d/tags?per_page=100": json.dumps([{"name": "1.9"}, {"name": "1.10"}]),
        "https://k.org/gpiod/": "libgpiod-2.2.4.tar.xz libgpiod-2.3.1.tar.xz libgpiod-2.3.tar.xz",
    }

    def get(url: str) -> str:
        if url not in pages:
            raise FetchError(f"HTTP 404 from {url}")
        return pages[url]

    assert latest_github("a/b", get) == "v2.0.0"
    assert latest_github("c/d", get) == "1.10", "no releases: highest version tag"
    # /tags is not sorted by version: a later page can hold the highest one.
    pages["https://api.github.com/repos/g/h/tags?per_page=100"] = json.dumps([{"name": f"0.{i}"} for i in range(100)])
    pages["https://api.github.com/repos/g/h/tags?per_page=100&page=2"] = json.dumps([{"name": "3.0"}])
    assert latest_github("g/h", get) == "3.0", "tags on page 2 are read"
    assert latest_page("https://k.org/gpiod/", r"libgpiod-(\d[\d.]*\d)\.tar\.xz", get) == "2.3.1"

    snap = {"manifests": {"ci.yml": {"resolved": {"pkg:github/a/b@v1.0.0": {}, "pkg:deb/ubuntu/x?arch=arm64": {}}},
                          "doc.md": {"resolved": {"pkg:github/c/d@1.10": {}}},
                          "gone.yml": {"resolved": {"pkg:github/e/f@1.0": {}}}}}
    table = {"sdks": [{"name": "libgpiod", "version": "2.2.4",
                       "updates": {"provider": "release-page", "url": "https://k.org/gpiod/",
                                   "pattern": r"libgpiod-(\d[\d.]*\d)\.tar\.xz"}},
                      {"name": "vendor-sdk", "version": "1"}]}
    ts, manual = targets(snap, table)
    assert [t["key"] for t in ts] == ["a/b", "c/d", "e/f", "libgpiod"] and manual == ["vendor-sdk"], (ts, manual)
    results = [check(t, get) for t in ts]
    res = {r["key"]: r for r in results}
    assert res["a/b"]["latest"] == "v2.0.0" and res["c/d"]["latest"] == "1.10" and res["libgpiod"]["latest"] == "2.3.1"
    assert res["e/f"]["error"] and "404" in res["e/f"]["error"], res["e/f"]

    class Fake:
        def __init__(self):
            self.issues: list[dict] = []
            self.calls: list[tuple] = []

        def create(self, title, body, labels):
            self.issues.append({"number": len(self.issues) + 1, "title": title, "body": body,
                                "state": "OPEN", "labels": labels})
            self.calls.append(("create", title))

        def update(self, number, title, body, comment):
            self.issues[number - 1].update(title=title, body=body)
            self.calls.append(("update", number, title))

        def close(self, number, comment):
            self.issues[number - 1]["state"] = "CLOSED"
            self.calls.append(("close", number))

        def open(self):
            return [i for i in self.issues if i["state"] == "OPEN"]

    f = Fake()
    reconcile(results, f, f.open())
    titles = sorted(i["title"] for i in f.issues)
    assert titles == ["Update a/b v1.0.0 -> v2.0.0", "Update check broken: e/f",
                      "Update libgpiod 2.2.4 -> 2.3.1"], titles
    assert {i["title"]: i["labels"] for i in f.issues}["Update check broken: e/f"] == BROKEN_LABELS
    assert {i["title"]: i["labels"] for i in f.issues}["Update a/b v1.0.0 -> v2.0.0"] == UPDATE_LABELS
    # Same results again: nothing to do (no duplicate issues).
    f.calls.clear()
    assert reconcile(results, f, f.open()) == [] and f.calls == []
    # A still newer upstream retitles the open issue instead of opening a second one.
    pages["https://api.github.com/repos/a/b/releases/latest"] = json.dumps({"tag_name": "v2.1.0"})
    pages["https://api.github.com/repos/e/f/releases/latest"] = json.dumps({"tag_name": "1.0"})
    results = [check(t, get) for t in ts]
    f.calls.clear()
    reconcile(results, f, f.open())
    assert ("update", 1, "Update a/b v1.0.0 -> v2.1.0") in f.calls, f.calls
    assert ("close", 2) in f.calls, "the broken source reads again: its issue closes"
    assert not any(c[0] == "create" for c in f.calls), f.calls
    # The repository bumps to an intermediate release still behind upstream: the title follows.
    snap["manifests"]["ci.yml"]["resolved"] = {"pkg:github/a/b@v2.0.0": {}}
    ts, _ = targets(snap, table)
    f.calls.clear()
    reconcile([check(t, get) for t in ts], f, f.open())
    assert ("update", 1, "Update a/b v2.0.0 -> v2.1.0") in f.calls, f.calls
    # The repository catches up: the update issue closes.
    snap["manifests"]["ci.yml"]["resolved"] = {"pkg:github/a/b@v2.1.0": {}}
    ts, _ = targets(snap, table)
    f.calls.clear()
    reconcile([check(t, get) for t in ts], f, f.open())
    assert ("close", 1) in f.calls, f.calls
    # A body someone edited into garbage is not mistaken for a tracked issue.
    assert read_marker("<!-- dependency-updates: {not json} -->") is None and read_marker("") is None

    report = {"results": [{"packages": [
        {"package": {"name": "libfoo", "version": "1.0"},
         "groups": [{"ids": ["DSA-2", "CVE-2026-1"], "max_severity": "9.8"},
                    {"ids": ["CVE-2026-2"], "max_severity": "5.0"},
                    {"ids": ["DEBIAN-CVE-2026-3"], "max_severity": ""}]},
        {"package": {"name": "libbar", "version": "2.0"},
         "groups": [{"ids": ["CVE-2026-4"], "max_severity": "7.0"}]}]}]}
    assert [x["id"] for x in osv_findings(report)] == ["CVE-2026-1", "CVE-2026-4"], osv_findings(report)
    f = Fake()
    assert osv_summary(report, f, f.issues, "o/r") and len(f.issues) == 1 and f.issues[0]["labels"] == OSV_LABELS
    assert "CVE-2026-2" not in f.issues[0]["body"] and "DEBIAN-CVE-2026-3" not in f.issues[0]["body"]
    f.issues[0]["state"] = "CLOSED"
    assert osv_summary(report, f, f.issues, "o/r") == [], "findings already reported, even in a closed issue"
    # OSV adds an alias that sorts first: still the same, already reported finding.
    report["results"][0]["packages"][0]["groups"][0]["ids"].append("AAA-2026-1")
    assert osv_summary(report, f, f.issues, "o/r") == [], "a reported group matched on any of its ids"
    report["results"][0]["packages"][1]["groups"].append({"ids": ["CVE-2026-5"], "max_severity": "8.1"})
    assert osv_summary(report, f, f.issues, "o/r") and "CVE-2026-5" in f.issues[1]["body"]
    assert "CVE-2026-4" not in f.issues[1]["body"], "only the new finding is listed"
    print("dependency_updates self-test: OK")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--osv-summary", metavar="FILE")
    a = ap.parse_args()
    if a.self_test:
        return self_test()
    issues = GhIssues(a.dry_run)
    if a.osv_summary:
        report = json.loads(Path(a.osv_summary).read_text(encoding="utf-8"))
        repo = os.environ.get("GH_REPO", "open-astro/AlpacaBridge")
        log = osv_summary(report, issues, issues.list("all"), repo)
        print("\n".join(log) or "no new High/Critical findings")
        return 0
    snap = snapshot.build_snapshot(ROOT, "0" * 40, "refs/heads/main", "local", "1970-01-01T00:00:00Z")
    table = json.loads((ROOT / snapshot.TABLE).read_text(encoding="utf-8"))
    ts, manual = targets(snap, table)
    results = [check(t, http_get) for t in ts]
    print("| Dependency | Repository | Upstream | Status |\n|---|---|---|---|")
    for r in results:
        status = ("source unreadable: " + r["error"]) if r["error"] else (
            "update available" if is_newer(r["latest"], r["current"]) else "current")
        print(f"| {r['name']} | {r['current']} | {r['latest'] or '-'} | {status} |")
    for name in manual:
        print(f"| {name} | - | - | manual check (no `updates` source) |")
    log = reconcile(results, issues, issues.list("open"))
    print("\n" + ("\n".join(f"- {line}" for line in log) or "- no issue changes"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
