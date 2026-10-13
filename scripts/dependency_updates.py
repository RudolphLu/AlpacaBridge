#!/usr/bin/env python3
"""Open one GitHub issue per dependency that has a newer upstream version.

The dependencies are the ones scripts/dependency_snapshot.py submits to the dependency
graph, so there is one inventory. What is checked:

- every ``pkg:github/OWNER/REPO@VERSION`` entry (cppcheck, zizmor, OSV-Scanner, nlohmann/json,
  ConformU): the repository's latest GitHub release, or its highest version tag without one;
- every SDK in scripts/dependency_sdk_versions.json, through its ``updates`` block: provider
  ``release-page`` (the highest version a regex matches on a page) or ``json`` (a field of the
  matching entry in a JSON list, ZWO's SDK endpoint);
- every entry in that file's ``documents`` list (protocol documents, the INDI sources drivers
  were derived from, the ConformU beta build): provider ``github-file`` (the newest commit on a
  file), ``hash`` (sha256 of the document, or of an HTML page's visible text) or ``page`` (the
  highest version a regex matches), against the recorded ``baseline``.

Debian, Ubuntu and docker entries are not checked: apt and the image tag already resolve to
the newest package at build time, and .github/workflows/osv-scan.yml reports their
vulnerabilities. An SDK with no ``updates`` block is listed as a manual check.

Issues (state lives in a marker comment in each issue body; nothing is committed):
- a newer version opens "Update <name> <current> -> <latest>" (dependencies, enhancement, P4),
  and a changed document "Upstream document changed: <name> ..." (dependencies, documentation, P4);
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
import contextlib
import gzip
import hashlib
import html
import io
import json
import os
import re
import subprocess
import sys
import urllib.error
import urllib.parse
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
DOC_LABELS = ("dependencies", "documentation", "P4")
OSV_LABELS = ("dependencies", "bug", "P2")
TAG_PAGES = 10
LIST_LIMIT = 5000  # issues read back for their markers; the OSV dedupe needs closed ones too
OSV_THRESHOLD = 7.0  # CVSS: High is 7.0-8.9, Critical 9.0-10.0
OSV_MAX_ROWS = 100  # rows per summary issue; keeps the body far below GitHub's 65536 characters

HttpGet = Callable[[str], str]
Fetch = Callable[[str], "tuple[bytes, str]"]  # url -> (body, content type)


class FetchError(Exception):
    pass


def parse_version(v: str) -> tuple[tuple[int, ...], bool]:
    """(numeric parts, is a pre-release) for '3.11.3', 'v1.25.2', '4.5.1-beta', '2.3'."""
    m = re.match(r"^[vV]?(\d+(?:\.\d+)*)(.*)$", v.strip())
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


def http_fetch(url: str) -> tuple[bytes, str]:
    """(body, content type). A gzip body is decompressed: urllib does not, and some vendor
    sites send one even unasked."""
    headers = {"User-Agent": "Mozilla/5.0 (compatible; alpacabridge-dependency-updates)"}
    token = os.environ.get("GH_TOKEN") or os.environ.get("GITHUB_TOKEN")
    if token and url.startswith("https://api.github.com/"):
        headers["Authorization"] = f"Bearer {token}"
    try:
        with urllib.request.urlopen(urllib.request.Request(url, headers=headers), timeout=120) as r:
            body, ctype = r.read(), r.headers.get("Content-Type", "")
            if r.headers.get("Content-Encoding", "").lower() == "gzip":
                body = gzip.decompress(body)
            return body, ctype
    except urllib.error.HTTPError as e:
        raise FetchError(f"HTTP {e.code} from {url}") from e
    except (urllib.error.URLError, TimeoutError, OSError) as e:
        raise FetchError(f"{url}: {e}") from e


def http_get(url: str) -> str:
    return http_fetch(url)[0].decode("utf-8", errors="replace")


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
    else:
        print(f"warning: {repo}: read only the first {TAG_PAGES * 100} tags; a higher version "
              "may be beyond them", file=sys.stderr)
    best = highest(tags)
    if not best:
        raise FetchError(f"{repo}: no release and no version tag")
    return best


def latest_json(url: str, path: str, match: dict, field: str, get: HttpGet) -> str:
    """The ``field`` of the entry matching ``match`` in the list at dotted ``path`` (ZWO's SDK list)."""
    node = json.loads(get(url))
    try:
        for seg in path.split("."):
            node = node[int(seg)] if seg.isdigit() else node[seg]
    except (KeyError, IndexError, TypeError) as e:
        raise FetchError(f"{url}: no {path!r} in the response (has the format changed?)") from e
    for entry in node if isinstance(node, list) else []:
        if isinstance(entry, dict) and all(entry.get(k) == v for k, v in match.items()):
            if field in entry and highest([str(entry[field])]):
                return str(entry[field])
            raise FetchError(f"{url}: entry {match} has no version in {field!r}")
    raise FetchError(f"{url}: no entry matches {match} (has the list changed?)")


def latest_file_commit(repo: str, path: str, get: HttpGet) -> str:
    """The newest commit on the default branch that touched ``path``."""
    commits = json.loads(get(f"https://api.github.com/repos/{repo}/commits?path={urllib.parse.quote(path)}&per_page=1"))
    if not commits:
        raise FetchError(f"{repo}: no commit touches {path} (moved or deleted?)")
    return commits[0]["sha"]


def normalised_text(html_text: str) -> str:
    """Visible text of an HTML page: no scripts, styles, comments or tags, whitespace collapsed."""
    t = re.sub(r"(?is)<(script|style|noscript)[^>]*>.*?</\1>", " ", html_text)
    t = re.sub(r"(?s)<!--.*?-->", " ", t)
    t = re.sub(r"(?s)<[^>]+>", " ", t)
    return re.sub(r"\s+", " ", html.unescape(t)).strip()


def content_hash(url: str, fetch: Fetch) -> str:
    """sha256 of a document: its bytes, or for an HTML page its normalised visible text."""
    body, ctype = fetch(url)
    if "html" in ctype.lower():
        body = normalised_text(body.decode("utf-8", errors="replace")).encode("utf-8")
    return hashlib.sha256(body).hexdigest()


def latest_page(url: str, pattern: str, get: HttpGet) -> str:
    best = highest(re.findall(pattern, get(url)))
    if not best:
        raise FetchError(f"{url}: no version matches {pattern!r} (has the page changed?)")
    return best


def targets(snap: dict, table: dict) -> tuple[list[dict], list[str]]:
    """(checkable dependencies, names of SDKs with no update source).

    Each target has ``compare``: "version" (an update is a higher version) or "equal" (a
    document: a commit SHA or content hash that differs from the recorded baseline).
    """
    out: dict[str, dict] = {}
    for path, man in snap["manifests"].items():
        for purl in man["resolved"]:
            m = re.match(r"^pkg:github/([^/@]+/[^/@?]+)@([^?#]+)$", purl)
            if m:
                repo, ver = m.groups()
                out.setdefault(repo.lower(), {"key": repo.lower(), "name": repo, "current": ver, "where": path,
                                              "check": ("github", repo), "source": f"https://github.com/{repo}/releases",
                                              "compare": "version", "document": False})
    manual = []
    for e in table["sdks"]:
        up = e.get("updates") or {}
        base = {"key": e["name"], "name": e["name"], "current": e.get("version"), "where": snapshot.TABLE,
                "source": up.get("url", ""), "compare": "version", "document": False}
        if up.get("provider") == "release-page" and e.get("version"):
            out.setdefault(e["name"], {**base, "check": ("page", up["url"], up["pattern"])})
        elif up.get("provider") == "json" and e.get("version"):
            out.setdefault(e["name"], {**base, "check": ("json", up["url"], up["path"], up["match"], up["field"])})
        else:
            manual.append(e["name"])
    for d in table.get("documents", []):
        c = d["check"]
        base = {"key": "doc:" + d["name"], "name": d["name"], "current": d["baseline"], "where": snapshot.TABLE,
                "document": True, "path": d.get("path")}
        if c["provider"] == "github-file":
            out["doc:" + d["name"]] = {**base, "check": ("github-file", c["repo"], c["file"]), "compare": "equal",
                                       "source": f"https://github.com/{c['repo']}/commits/HEAD/{c['file']}"}
        elif c["provider"] == "hash":
            out["doc:" + d["name"]] = {**base, "check": ("hash", c["url"]), "compare": "equal", "source": c["url"]}
        elif c["provider"] == "page":
            out["doc:" + d["name"]] = {**base, "check": ("page", c["url"], c["pattern"]), "compare": "version",
                                       "source": c["url"]}
        else:
            manual.append(d["name"])
    return sorted(out.values(), key=lambda t: t["key"]), manual


def check(t: dict, get: HttpGet, fetch: Fetch | None = None) -> dict:
    fetch = fetch or http_fetch
    try:
        if t.get("compare", "version") == "version":
            parse_version(t["current"])  # a pin that is no version (a commit SHA) is a broken entry
        kind = t["check"][0]
        if kind == "github":
            latest = latest_github(t["check"][1], get)
        elif kind == "json":
            latest = latest_json(*t["check"][1:], get)
        elif kind == "github-file":
            latest = latest_file_commit(t["check"][1], t["check"][2], get)
        elif kind == "hash":
            latest = content_hash(t["check"][1], fetch)
        else:
            latest = latest_page(t["check"][1], t["check"][2], get)
        return {**t, "latest": latest, "error": None}
    except (FetchError, KeyError, ValueError, json.JSONDecodeError) as e:
        return {**t, "latest": None, "error": str(e)}


def differs(r: dict) -> bool:
    """True when upstream is ahead of the repository (or, for a document, has changed)."""
    if r.get("compare") == "equal":
        return r["latest"] != r["current"]
    return is_newer(r["latest"], r["current"])


def short(v: str) -> str:
    return v[:12] if re.fullmatch(r"[0-9a-f]{40,64}", v or "") else v


def title_of(r: dict) -> str:
    if r.get("document"):
        return f"Upstream document changed: {r['name']} {short(r['current'])} -> {short(r['latest'])}"
    return f"Update {r['name']} {r['current']} -> {r['latest']}"


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
        out = self._gh("issue", "list", "--label", "dependencies", "--state", state, "--limit", str(LIST_LIMIT),
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

    def edit_body(self, number: int, body: str) -> None:
        self._gh("issue", "edit", str(number), "--body-file", "-", stdin=body)

    def close(self, number: int, comment: str, body: str | None = None) -> None:
        if body is not None:
            self._gh("issue", "edit", str(number), "--body-file", "-", stdin=body)
        self._gh("issue", "close", str(number), "--comment", comment)


def update_body(r: dict) -> str:
    if r.get("document"):
        return document_body(r)
    return (f"{marker({'kind': 'update', 'key': r['key'], 'version': r['latest'], 'current': r['current']})}\n"
            f"`{r['name']}` has a newer upstream version.\n\n"
            f"| | |\n|---|---|\n| In this repository | `{r['current']}` |\n| Upstream | `{r['latest']}` |\n"
            f"| Pinned in | `{r['where']}` |\n| Source | {r['source']} |\n\n"
            "Bump the pin where it is declared (for a vendored SDK, also its directory and the "
            "`--pin` digest, see docs/development.md \"Dependency graph\"), check the upstream "
            "changelog, and let CI run. This issue closes by itself once the repository is at "
            "that version.\n\nOpened by `.github/workflows/dependency-updates.yml`.\n")


def document_body(r: dict) -> str:
    local = f"| Local copy | `{r['path']}` |\n" if r.get("path") else ""
    return (f"{marker({'kind': 'update', 'key': r['key'], 'version': r['latest'], 'current': r['current']})}\n"
            f"An upstream document a driver is built from has changed: `{r['name']}`.\n\n"
            f"| | |\n|---|---|\n| Recorded baseline | `{r['current']}` |\n| Upstream now | `{r['latest']}` |\n"
            f"{local}| Source | {r['source']} |\n\n"
            "Review what changed upstream against the driver (and refresh the local copy if there is one), "
            "then set this document's `baseline` in `scripts/dependency_sdk_versions.json` to the upstream "
            "value above. This issue closes by itself once the baseline matches.\n\n"
            "Opened by `.github/workflows/dependency-updates.yml`.\n")


def broken_body(r: dict) -> str:
    return (f"{marker({'kind': 'broken', 'key': r['key']})}\n"
            f"The weekly update check could not read the upstream version of `{r['name']}`:\n\n"
            f"```\n{r['error']}\n```\n\nSource: {r['source']}\n\n"
            "Until this is fixed a new release of it goes unreported. If the page or repository "
            "moved, update the source in `scripts/dependency_sdk_versions.json` or the pin it "
            "comes from. This issue closes by itself when the source reads again.\n")


def resolved_body(issue: dict, state: dict) -> str:
    """The issue body with its marker flagged ``resolved``: closed by this script, not by hand."""
    return MARKER_RE.sub(lambda _: marker({**state, "resolved": True}), issue.get("body", ""), count=1)


def reconcile(results: list[dict], issues, all_issues: list[dict]) -> list[str]:
    """Open, retitle and close issues so they match results. Returns one line per action;
    a line starting with ``FAILED`` is an issue call that failed (the others still run).

    all_issues holds open and closed issues. A closed issue whose marker is not ``resolved``
    was closed by a maintainer. For an update issue that means "won't bump": its (current,
    latest) pair is skipped until upstream releases a newer version or the repository moves
    (a "Closes #N" PR that bumped only part of the way). For a broken-source issue it means
    "known": no new copy opens while the source stays unreadable, and the hand-close is
    cleared once the source reads again. An open issue for a dependency no longer in results
    closes.
    """
    by, declined, declined_broken = {}, set(), {}
    for i in all_issues:
        st = read_marker(i.get("body", ""))
        if not (st and st.get("kind") in ("update", "broken") and "key" in st):
            continue
        if i.get("state", "OPEN").upper() == "OPEN":
            by[(st["kind"], st["key"])] = (i, st)
        elif st.get("resolved"):
            continue
        elif st["kind"] == "update":
            declined.add((st["key"], st.get("version"), st.get("current")))
        else:
            declined_broken[st["key"]] = (i, st)
    log = []
    for r in results:
        try:
            log += _reconcile_one(r, issues, by, declined, declined_broken)
        except Exception as e:  # one failed gh call must not stop the other dependencies
            log.append(f"FAILED {r['name']}: {getattr(e, 'stderr', None) or e}")
    tracked = {r["key"] for r in results}
    for (kind, key), (issue, st) in sorted(by.items(), key=lambda kv: kv[1][0]["number"]):
        if key not in tracked:
            try:
                issues.close(issue["number"], f"`{key}` is no longer in the dependency inventory.",
                             resolved_body(issue, st))
                log.append(f"closed {kind} issue #{issue['number']} (no longer tracked)")
            except Exception as e:
                log.append(f"FAILED closing #{issue['number']}: {getattr(e, 'stderr', None) or e}")
    return log


def _reconcile_one(r: dict, issues, by: dict, declined: set, declined_broken: dict) -> list[str]:
    log = []
    broken = by.get(("broken", r["key"]))
    if r["error"]:
        if not broken and r["key"] not in declined_broken:
            issues.create(f"Update check broken: {r['name']}", broken_body(r), BROKEN_LABELS)
            log.append(f"opened broken-source issue for {r['name']}")
        return log
    if broken:
        issues.close(broken[0]["number"], f"The source reads again (`{r['latest']}`).", resolved_body(*broken))
        log.append(f"closed broken-source issue #{broken[0]['number']}")
    if r["key"] in declined_broken:
        issues.edit_body(declined_broken[r["key"]][0]["number"], resolved_body(*declined_broken[r["key"]]))
        log.append(f"cleared the hand-close of broken-source issue #{declined_broken[r['key']][0]['number']}")
    upd = by.get(("update", r["key"]))
    newer = differs(r)
    title = title_of(r)
    if newer and not upd:
        if (r["key"], r["latest"], r["current"]) not in declined:
            issues.create(title, update_body(r), DOC_LABELS if r.get("document") else UPDATE_LABELS)
            log.append(f"opened update issue for {r['name']} {r['latest']}")
    elif newer and upd is not None and (upd[1].get("version"), upd[1].get("current")) != (r["latest"], r["current"]):
        note = (f"Upstream is now at `{r['latest']}`." if upd[1].get("version") != r["latest"]
                else f"The repository is now at `{r['current']}`, still behind `{r['latest']}`.")
        issues.update(upd[0]["number"], title, update_body(r), note)
        log.append(f"retitled #{upd[0]['number']} to {r['current']} -> {r['latest']}")
    elif not newer and upd is not None:
        issues.close(upd[0]["number"], f"The repository is now at `{r['current']}`; "
                                       f"upstream latest is `{r['latest']}`.", resolved_body(*upd))
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
    found = [f for f in osv_findings(report) if not reported.intersection(f["ids"])]
    if not found:
        return []
    # Keep the body under GitHub's 65536-character limit: list at most OSV_MAX_ROWS; the rest
    # stay unreported and open the next run's issue.
    new, rest = found[:OSV_MAX_ROWS], len(found) - OSV_MAX_ROWS
    rows = "\n".join(f"| {f['severity']:.1f} | `{f['package']}` {f['version']} | {', '.join(f['ids'])} |" for f in new)
    body = (f"{marker({'kind': 'osv', 'ids': sorted({i for f in new for i in f['ids']})})}\n"
            f"The OSV scan of the nightly `.deb`'s installed runtime packages (Debian trixie) found "
            f"{len(new)} new High or Critical finding(s):\n\n| CVSS | Package | Advisories |\n|---|---|---|\n"
            f"{rows}\n\n"
            + (f"{rest} more are not listed here; the next scan opens an issue for them.\n\n" if rest > 0 else "")
            + f"All findings, including lower and unscored ones, are under "
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
    # Every page full: the cap is reported, not silent.
    for page in range(1, TAG_PAGES + 1):
        pages[f"https://api.github.com/repos/m/n/tags?per_page=100" + (f"&page={page}" if page > 1 else "")] = \
            json.dumps([{"name": f"{page}.{i}"} for i in range(100)])
    err = io.StringIO()
    with contextlib.redirect_stderr(err):
        assert latest_github("m/n", get) == f"{TAG_PAGES}.99"
    assert "first 1000 tags" in err.getvalue(), err.getvalue()
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

        def edit_body(self, number, body):
            self.issues[number - 1]["body"] = body
            self.calls.append(("edit_body", number))

        def close(self, number, comment, body=None):
            self.issues[number - 1]["state"] = "CLOSED"
            if body is not None:
                self.issues[number - 1]["body"] = body
            self.calls.append(("close", number))

        def open(self):
            return [i for i in self.issues if i["state"] == "OPEN"]

    f = Fake()
    reconcile(results, f, f.issues)
    titles = sorted(i["title"] for i in f.issues)
    assert titles == ["Update a/b v1.0.0 -> v2.0.0", "Update check broken: e/f",
                      "Update libgpiod 2.2.4 -> 2.3.1"], titles
    assert {i["title"]: i["labels"] for i in f.issues}["Update check broken: e/f"] == BROKEN_LABELS
    assert {i["title"]: i["labels"] for i in f.issues}["Update a/b v1.0.0 -> v2.0.0"] == UPDATE_LABELS
    # Same results again: nothing to do (no duplicate issues).
    f.calls.clear()
    assert reconcile(results, f, f.issues) == [] and f.calls == []
    # A still newer upstream retitles the open issue instead of opening a second one.
    pages["https://api.github.com/repos/a/b/releases/latest"] = json.dumps({"tag_name": "v2.1.0"})
    pages["https://api.github.com/repos/e/f/releases/latest"] = json.dumps({"tag_name": "1.0"})
    results = [check(t, get) for t in ts]
    f.calls.clear()
    reconcile(results, f, f.issues)
    assert ("update", 1, "Update a/b v1.0.0 -> v2.1.0") in f.calls, f.calls
    assert ("close", 2) in f.calls, "the broken source reads again: its issue closes"
    assert not any(c[0] == "create" for c in f.calls), f.calls
    # The repository bumps to an intermediate release still behind upstream: the title follows.
    snap["manifests"]["ci.yml"]["resolved"] = {"pkg:github/a/b@v2.0.0": {}}
    ts, _ = targets(snap, table)
    f.calls.clear()
    reconcile([check(t, get) for t in ts], f, f.issues)
    assert ("update", 1, "Update a/b v2.0.0 -> v2.1.0") in f.calls, f.calls
    # The repository catches up: the update issue closes.
    snap["manifests"]["ci.yml"]["resolved"] = {"pkg:github/a/b@v2.1.0": {}}
    ts, _ = targets(snap, table)
    f.calls.clear()
    reconcile([check(t, get) for t in ts], f, f.issues)
    assert ("close", 1) in f.calls, f.calls
    # A maintainer closes an update issue by hand ("won't bump"): the same version stays skipped,
    # and only a newer upstream release opens a new issue.
    snap["manifests"]["ci.yml"]["resolved"] = {"pkg:github/a/b@v1.0.0": {}}
    ts, _ = targets(snap, table)
    reconcile([check(t, get) for t in ts], f, f.issues)
    reopened = [i for i in f.issues if i["state"] == "OPEN" and i["title"].startswith("Update a/b")]
    assert len(reopened) == 1, reopened
    reopened[0]["state"] = "CLOSED"
    f.calls.clear()
    reconcile([check(t, get) for t in ts], f, f.issues)
    assert f.calls == [], "a hand-closed issue for this version is not reopened"
    # A "Closes #N" PR that bumps only part of the way also closes the issue without the
    # resolved flag; the repository moved, so the remaining gap is reported again.
    snap["manifests"]["ci.yml"]["resolved"] = {"pkg:github/a/b@v1.5.0": {}}
    ts, _ = targets(snap, table)
    reconcile([check(t, get) for t in ts], f, f.issues)
    assert ("create", "Update a/b v1.5.0 -> v2.1.0") in f.calls, f.calls
    [i for i in f.issues if i["title"] == "Update a/b v1.5.0 -> v2.1.0"][0]["state"] = "CLOSED"
    snap["manifests"]["ci.yml"]["resolved"] = {"pkg:github/a/b@v1.0.0": {}}
    ts, _ = targets(snap, table)
    pages["https://api.github.com/repos/a/b/releases/latest"] = json.dumps({"tag_name": "v2.2.0"})
    reconcile([check(t, get) for t in ts], f, f.issues)
    assert ("create", "Update a/b v1.0.0 -> v2.2.0") in f.calls, f.calls
    # A pin that is not a version (a commit SHA) is a broken entry, not a crash of the whole run.
    snap["manifests"]["ci.yml"]["resolved"] = {"pkg:github/a/b@deadbeef": {}}
    ts, _ = targets(snap, table)
    res = {r["key"]: r for r in (check(t, get) for t in ts)}
    assert res["a/b"]["error"] and "deadbeef" in res["a/b"]["error"], res["a/b"]
    f.calls.clear()
    reconcile(list(res.values()), f, f.issues)
    assert ("create", "Update check broken: a/b") in f.calls, f.calls
    # A dependency that leaves the inventory: its open update and broken issues close.
    snap["manifests"]["ci.yml"]["resolved"] = {}
    ts, _ = targets(snap, table)
    f.calls.clear()
    reconcile([check(t, get) for t in ts], f, f.issues)
    assert not [i for i in f.issues if i["state"] == "OPEN" and "a/b" in i["title"]], \
        [i["title"] for i in f.issues if i["state"] == "OPEN"]
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
    # --- SDK and document providers ---
    assert parse_version("V1.41") == ((1, 41), False) and highest(["2.0.2.1", "2.1.0.1"]) == "2.1.0.1"
    zwo = "https://zwo.test/sdk"
    pages[zwo] = json.dumps([{"acf": {"software_downloads": [
        {"software_download_title": "EAF SDK", "software_version": "V1.8.1"},
        {"software_download_title": "ASI Camera SDK", "software_version": "V1.41"}]}}])
    assert latest_json(zwo, "0.acf.software_downloads", {"software_download_title": "ASI Camera SDK"},
                       "software_version", get) == "V1.41"
    for path, match in (("0.acf.gone", {"software_download_title": "EAF SDK"}),
                        ("0.acf.software_downloads", {"software_download_title": "Renamed SDK"})):
        try:
            latest_json(zwo, path, match, "software_version", get)
        except FetchError:
            pass
        else:
            raise AssertionError(f"{path} {match} must be a broken source")
    pages["https://api.github.com/repos/indilib/indi/commits?path=drivers/x.cpp&per_page=1"] = json.dumps([{"sha": "b" * 40}])
    pages["https://api.github.com/repos/indilib/indi/commits?path=drivers/gone.cpp&per_page=1"] = "[]"
    assert latest_file_commit("indilib/indi", "drivers/x.cpp", get) == "b" * 40
    try:
        latest_file_commit("indilib/indi", "drivers/gone.cpp", get)
    except FetchError:
        pass
    else:
        raise AssertionError("a file no commit touches is a broken source")
    docs = {"https://d.test/a.html": (b"<html><script>var t=1;</script><p>Cmd  :GR#</p><!-- 12:00 --></html>", "text/html"),
            "https://d.test/b.html": (b"<html><script>var t=2;</script><p>Cmd\n:GR#</p><!-- 12:01 --></html>", "text/html"),
            "https://d.test/c.pdf": (b"%PDF-1", "application/pdf")}
    fetch = docs.__getitem__
    assert content_hash("https://d.test/a.html", fetch) == content_hash("https://d.test/b.html", fetch), \
        "scripts, comments and whitespace do not change an HTML page's hash"
    assert content_hash("https://d.test/c.pdf", fetch) == hashlib.sha256(b"%PDF-1").hexdigest()
    pages["https://o.test/dl"] = "Astroasis_Oasis_Focuser_ASCOM_2.1.0.1_Setup.zip Oasis_Focuser_Firmware_Ver_1.4.0.zip"
    table = {"sdks": [{"name": "zwo-eaf-sdk", "version": "1.7.7",
                       "updates": {"provider": "json", "url": zwo, "path": "0.acf.software_downloads",
                                   "match": {"software_download_title": "EAF SDK"}, "field": "software_version"}}],
             "documents": [
                 {"name": "indi-x", "check": {"provider": "github-file", "repo": "indilib/indi", "file": "drivers/x.cpp"},
                  "baseline": "a" * 40},
                 {"name": "pdf", "path": "AlpacaCore/external/x.md", "check": {"provider": "hash", "url": "https://d.test/c.pdf"},
                  "baseline": hashlib.sha256(b"%PDF-1").hexdigest()},
                 {"name": "oasis", "check": {"provider": "page", "url": "https://o.test/dl",
                                             "pattern": r"Astroasis_Oasis_Focuser_ASCOM_(\d+(?:\.\d+)+)_Setup"},
                  "baseline": "2.0.2.1"}]}
    ts, manual = targets({"manifests": {}}, table)
    assert manual == [] and [t["key"] for t in ts] == ["doc:indi-x", "doc:oasis", "doc:pdf", "zwo-eaf-sdk"], ts
    results = [check(t, get, fetch) for t in ts]
    res = {r["key"]: r for r in results}
    assert all(r["error"] is None for r in results), results
    assert [k for k in res if differs(res[k])] == ["doc:indi-x", "doc:oasis", "zwo-eaf-sdk"], \
        "a changed commit, a newer installer and a newer SDK differ; an unchanged hash does not"
    f = Fake()
    reconcile(results, f, f.issues)
    by_title = {i["title"]: i for i in f.issues}
    assert set(by_title) == {"Upstream document changed: indi-x aaaaaaaaaaaa -> bbbbbbbbbbbb",
                             "Upstream document changed: oasis 2.0.2.1 -> 2.1.0.1",
                             "Update zwo-eaf-sdk 1.7.7 -> V1.8.1"}, set(by_title)
    assert by_title["Upstream document changed: oasis 2.0.2.1 -> 2.1.0.1"]["labels"] == DOC_LABELS
    assert by_title["Update zwo-eaf-sdk 1.7.7 -> V1.8.1"]["labels"] == UPDATE_LABELS
    # The baseline is set to upstream in a PR: the document issue closes.
    table["documents"][0]["baseline"] = "b" * 40
    ts, _ = targets({"manifests": {}}, table)
    f.calls.clear()
    reconcile([check(t, get, fetch) for t in ts], f, f.issues)
    assert ("close", 1) in f.calls or any(c[0] == "close" for c in f.calls), f.calls
    assert not [i for i in f.issues if i["state"] == "OPEN" and "indi-x" in i["title"]]

    # A large first scan: the issue body stays under GitHub's 65536-character limit, and the
    # findings it could not list stay unreported for the next run instead of failing every day.
    big = {"results": [{"packages": [{"package": {"name": f"lib{n}", "version": "1"},
                                      "groups": [{"ids": [f"CVE-2026-{n:05d}"], "max_severity": "8.0"}]}
                                     for n in range(5000)]}]}
    f = Fake()
    osv_summary(big, f, f.issues, "o/r")
    assert len(f.issues) == 1 and len(f.issues[0]["body"]) < 65536, len(f.issues[0]["body"])
    listed = read_marker(f.issues[0]["body"])["ids"]
    assert 0 < len(listed) < 5000 and f"CVE-2026-{0:05d}" in listed, len(listed)
    osv_summary(big, f, f.issues, "o/r")
    assert len(f.issues) == 2 and not set(listed) & set(read_marker(f.issues[1]["body"])["ids"])

    # A broken-source issue closed by hand stays closed while the source is still unreadable,
    # and is cleared once the source reads again, so a later break is reported.
    f = Fake()
    bad = [{"key": "x/y", "name": "x/y", "current": "1.0", "where": "ci.yml", "source": "s",
            "latest": None, "error": "HTTP 500"}]
    good = [{**bad[0], "latest": "1.0", "error": None}]
    reconcile(bad, f, f.issues)
    f.issues[0]["state"] = "CLOSED"
    f.calls.clear()
    reconcile(bad, f, f.issues)
    assert f.calls == [], "a hand-closed broken-source issue is not reopened"
    reconcile(good, f, f.issues)
    assert read_marker(f.issues[0]["body"]).get("resolved"), "a readable source clears the hand-close"
    f.calls.clear()
    reconcile(bad, f, f.issues)
    assert ("create", "Update check broken: x/y") in f.calls, f.calls

    # One failed issue call does not stop the others; the failure is returned.
    class Flaky(Fake):
        def create(self, title, body, labels):
            if "first" in title:
                raise subprocess.CalledProcessError(1, ["gh"], stderr="label not found")
            super().create(title, body, labels)

    f = Flaky()
    two = [{**bad[0], "key": "first", "name": "first"}, {**bad[0], "key": "second", "name": "second"}]
    log = reconcile(two, f, f.issues)
    assert [i["title"] for i in f.issues] == ["Update check broken: second"], f.issues
    assert any(line.startswith("FAILED") and "first" in line for line in log), log
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
            ("changed" if r.get("document") else "update available") if differs(r) else "current")
        print(f"| {r['name']} | {short(r['current'])} | {short(r['latest'] or '-')} | {status} |")
    for name in manual:
        print(f"| {name} | - | - | manual check (no `updates` source) |")
    log = reconcile(results, issues, issues.list("all"))
    print("\n" + ("\n".join(f"- {line}" for line in log) or "- no issue changes"))
    return 1 if any(line.startswith("FAILED") for line in log) else 0


if __name__ == "__main__":
    sys.exit(main())
