#!/usr/bin/env python3
"""Build a GitHub Dependency Submission API snapshot for the C++ build.

GitHub's dependency graph only parses manifests it knows (here: the workflow
action pins). This script reports what actually ships: the Debian packages in
debian/control, the CMake FetchContent libraries and the vendored SDKs in
AlpacaCore/external/ (versions from scripts/dependency_sdk_versions.json).

  dependency_snapshot.py [--sha S] [--ref R] [--output FILE]   write the snapshot
  dependency_snapshot.py --check                               drift check
  dependency_snapshot.py --spdx DEB [--output FILE]            SPDX SBOM for a built .deb
  dependency_snapshot.py --pin                                 rewrite the SDK sha256 pins
  dependency_snapshot.py --self-test

Rules: Build-Depends become scope "development", Depends become "runtime".
For an alternative group ``a | b`` the FIRST alternative is reported. A version
is put in a deb purl only for an exact ``(= X)`` pin; ``${...}`` substitutions
and ``debhelper-compat`` are skipped. ``${shlibs:Depends}`` is resolved only at
package build time and is out of scope. deb and generic purls raise no
Dependabot alerts; they document the inventory.
"""

from __future__ import annotations

import argparse
import datetime
import hashlib
import json
import os
import re
import subprocess
import sys
import tempfile
import urllib.parse
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TABLE = "scripts/dependency_sdk_versions.json"
EXTERNAL = "AlpacaCore/external"
DEB_QUALIFIERS = "arch=arm64&distro=trixie"
# Package directories holding vendor binaries and firmware copied from AlpacaCore/external/.
SHIPPED_VENDOR_DIRS = ("usr/lib/alpacabridge", "usr/lib/firmware")
SKIP = {"debhelper-compat"}
DETECTOR = {"name": "alpacabridge-dependency-snapshot", "version": "1",
            "url": "https://github.com/open-astro/AlpacaBridge/blob/main/scripts/dependency_snapshot.py"}


def parse_relations(value: str) -> list[tuple[str, str | None]]:
    """Return (package, exact version or None) per comma group, first alternative."""
    out = []
    for group in value.replace("\n", " ").split(","):
        group = group.strip()
        if not group:
            continue
        first = group.split("|")[0].strip()
        # "${...}" substitutions fail the name pattern below and are skipped.
        m = re.match(r"^([a-z0-9][a-z0-9+.\-]*)\s*(?:\[[^\]]*\])?\s*(?:\(\s*([<>=]+)\s*([^)\s]+)\s*\))?", first)
        if not m or m.group(1) in SKIP:
            continue
        out.append((m.group(1), m.group(3) if m.group(2) == "=" else None))
    return out


def parse_control(text: str) -> dict[str, list[tuple[str, str | None]]]:
    """Return {"development": Build-Depends, "runtime": Depends of the binary stanza}."""
    fields: dict[str, dict[str, str]] = {}
    for stanza in re.split(r"\n\s*\n", text.strip()):
        key = "source" if stanza.startswith("Source:") else "binary"
        cur = None
        for line in stanza.splitlines():
            if line[:1] in " \t" and cur:
                fields.setdefault(key, {})[cur] += " " + line.strip()
            elif ":" in line:
                cur, _, val = line.partition(":")
                fields.setdefault(key, {})[cur] = val.strip()
    return {"development": parse_relations(fields.get("source", {}).get("Build-Depends", "")),
            "runtime": parse_relations(fields.get("binary", {}).get("Depends", ""))}


def parse_fetchcontent(text: str) -> list[tuple[str, str]]:
    """Return (owner/repo, tag) per FetchContent_Declare of a github.com repository."""
    out = []
    for block in re.findall(r"FetchContent_Declare\((.*?)\)", text, re.S):
        repo = re.search(r"GIT_REPOSITORY\s+https://github\.com/([\w.\-]+/[\w.\-]+?)(?:\.git)?\s", block + " ")
        tag = re.search(r"GIT_TAG\s+(\S+)", block)
        if repo and tag:
            out.append((repo.group(1), tag.group(1)))
    return out


def deb_purl(name: str, version: str | None) -> str:
    name = urllib.parse.quote(name, safe="")  # purl: "+" in g++ is %2B
    return f"pkg:deb/debian/{name}{'@' + version if version else ''}?{DEB_QUALIFIERS}"


def manifest(path: str, entries: list[tuple[str, str]]) -> dict:
    """entries: (purl, scope). Skips duplicates."""
    resolved = {}
    for purl, scope in entries:
        resolved.setdefault(purl, {"package_url": purl, "relationship": "direct", "scope": scope})
    return {"name": path, "file": {"source_location": path}, "resolved": resolved}


def build_snapshot(root: Path, sha: str, ref: str, job_id: str, scanned: str) -> dict:
    manifests = {}
    control = parse_control((root / "debian/control").read_text(encoding="utf-8"))
    entries = [(deb_purl(n, v), scope) for scope, rels in control.items() for n, v in rels]
    manifests["debian/control"] = manifest("debian/control", entries)
    cm = "AlpacaHTTP/CMakeLists.txt"
    fetched = parse_fetchcontent((root / cm).read_text(encoding="utf-8"))
    manifests[cm] = manifest(cm, [(f"pkg:github/{repo}@{tag}", "runtime") for repo, tag in fetched])
    table = json.loads((root / TABLE).read_text(encoding="utf-8"))
    sdk = []
    for e in table["sdks"]:
        ver = e.get("version")
        sdk.append((f"pkg:generic/{e['name']}{'@' + ver if ver else ''}", "runtime"))
    manifests[TABLE] = manifest(TABLE, sdk)
    return {"version": 0, "job": {"correlator": "dependency-submission", "id": job_id},
            "sha": sha, "ref": ref, "detector": DETECTOR, "scanned": scanned,
            "manifests": manifests}


def file_digests(data: bytes) -> dict[str, str]:
    return {"sha1": hashlib.sha1(data).hexdigest(), "sha256": hashlib.sha256(data).hexdigest()}


def tree_digest(root: Path, rel: str) -> str:
    """SHA-256 over the git-tracked files under rel: path, then file bytes (or symlink target)."""
    out = subprocess.run(["git", "-C", str(root), "ls-files", "-s", "-z", "--", rel],
                         check=True, capture_output=True, text=True).stdout
    h = hashlib.sha256()
    for entry in sorted(e for e in out.split("\0") if e):
        meta, path = entry.split("\t", 1)
        p = root / path
        if meta.split()[0] == "120000":
            data = b"L" + os.readlink(p).encode()
        else:
            data = b"F" + hashlib.sha256(p.read_bytes()).digest()
        h.update(path.encode() + b"\0" + data + b"\n")
    return h.hexdigest()


def pin_sdk_hashes(root: Path) -> None:
    """Rewrite every 'sdks' entry's sha256 pin, keeping the table's one-entry-per-line layout."""
    table = json.loads((root / TABLE).read_text(encoding="utf-8"))
    for e in table["sdks"]:
        e["sha256"] = tree_digest(root, e["path"])
    lines = ["{"]
    if "_comment" in table:
        lines.append(f'  "_comment": {json.dumps(table["_comment"])},')
    lines.append('  "sdks": [')
    lines.append(",\n".join(f"    {json.dumps(e)}" for e in table["sdks"]))
    lines.append("  ],")
    lines.append('  "doc_only": [')
    lines.append(",\n".join(f"    {json.dumps(p)}" for p in table["doc_only"]))
    lines.append("  ]")
    lines.append("}")
    (root / TABLE).write_text("\n".join(lines) + "\n", encoding="utf-8")


def build_spdx(root: Path, deb: dict[str, str], file_name: str, sha256: str,
               created: str, namespace: str, payload: dict[str, dict[str, str]] | None = None,
               installed: dict[str, str] | None = None) -> dict:
    """SPDX 2.3 SBOM for one built .deb: the same inventory as the snapshot, rooted at the package.

    deb: the package's control fields (dpkg-deb -f). Runtime dependencies come from its
    Depends, where ${shlibs:Depends} is already resolved; vendored SDKs and FetchContent
    libraries are CONTAINS; Build-Depends are BUILD_DEPENDENCY_OF, at the version in
    installed (dpkg-query in the build environment) when one is given.

    payload: {path inside the .deb: file_digests()} for the shipped vendor files. Each must be
    byte-identical to a file under a listed SDK's path, and is reported as a file CONTAINS-ed by
    that SDK; one that matches none raises ValueError.
    """
    payload = payload or {}
    installed = installed or {}
    packages: list[dict] = []
    ids: dict[str, str] = {}

    def add(purl: str, name: str, version: str | None) -> str:
        if purl in ids:
            return ids[purl]
        spdx_id = f"SPDXRef-Package-{len(packages)}"
        pkg = {"name": name, "SPDXID": spdx_id, "downloadLocation": "NOASSERTION", "filesAnalyzed": False,
               "externalRefs": [{"referenceCategory": "PACKAGE-MANAGER", "referenceType": "purl",
                                 "referenceLocator": purl}]}
        if version:
            pkg["versionInfo"] = version
        packages.append(pkg)
        ids[purl] = spdx_id
        return spdx_id

    root_id = add(deb_purl(deb["Package"], deb["Version"]), deb["Package"], deb["Version"])
    packages[0]["packageFileName"] = file_name
    packages[0]["checksums"] = [{"algorithm": "SHA256", "checksumValue": sha256}]
    rels = [("SPDXRef-DOCUMENT", "DESCRIBES", root_id)]
    for n, v in parse_relations(deb.get("Depends", "")):
        rels.append((root_id, "DEPENDS_ON", add(deb_purl(n, v), n, v)))
    for repo, tag in parse_fetchcontent((root / "AlpacaHTTP/CMakeLists.txt").read_text(encoding="utf-8")):
        rels.append((root_id, "CONTAINS", add(f"pkg:github/{repo}@{tag}", repo, tag)))
    vendored: dict[str, str] = {}
    for e in json.loads((root / TABLE).read_text(encoding="utf-8"))["sdks"]:
        ver = e.get("version")
        sdk_id = add(f"pkg:generic/{e['name']}{'@' + ver if ver else ''}", e["name"], ver)
        rels.append((root_id, "CONTAINS", sdk_id))
        for dirpath, _, names in os.walk(root / e["path"]):
            for n in names:
                p = Path(dirpath) / n
                if p.is_file() and not p.is_symlink():
                    vendored.setdefault(hashlib.sha256(p.read_bytes()).hexdigest(), sdk_id)
    files = []
    unmatched = []
    for path in sorted(payload):
        d = payload[path]
        sdk_id = vendored.get(d["sha256"])
        if sdk_id is None:
            unmatched.append(path)
            continue
        file_id = f"SPDXRef-File-{len(files)}"
        files.append({"fileName": f"./{path}", "SPDXID": file_id,
                      "checksums": [{"algorithm": "SHA1", "checksumValue": d["sha1"]},
                                    {"algorithm": "SHA256", "checksumValue": d["sha256"]}]})
        rels.append((sdk_id, "CONTAINS", file_id))
    if unmatched:
        raise ValueError("shipped vendor files match no file under a 'sdks' path in "
                         f"{TABLE} (changed in packaging, or not vendored): " + ", ".join(unmatched))
    control = parse_control((root / "debian/control").read_text(encoding="utf-8"))
    for n, v in control["development"]:
        v = installed.get(n, v)
        rels.append((add(deb_purl(n, v), n, v), "BUILD_DEPENDENCY_OF", root_id))
    rels = list(dict.fromkeys(rels))
    return {"spdxVersion": "SPDX-2.3", "dataLicense": "CC0-1.0", "SPDXID": "SPDXRef-DOCUMENT",
            "name": file_name, "documentNamespace": namespace,
            "creationInfo": {"created": created,
                             "creators": [f"Tool: {DETECTOR['name']}-{DETECTOR['version']}"]},
            "packages": packages,
            "files": files,
            "relationships": [{"spdxElementId": a, "relationshipType": t, "relatedSpdxElement": b}
                              for a, t, b in rels]}


def check_drift(root: Path) -> list[str]:
    table = json.loads((root / TABLE).read_text(encoding="utf-8"))
    sdks = [e["path"] for e in table["sdks"]]
    docs = list(table["doc_only"])
    listed = sdks + docs
    errors = []
    for p in listed:
        if not (root / p).is_dir():
            errors.append(f"{TABLE}: stale entry {p} (directory gone): remove it from 'sdks' or 'doc_only'")
    for e in table["sdks"]:
        if not (root / e["path"]).is_dir():
            continue
        if not e.get("sha256"):
            errors.append(f"{TABLE}: {e['path']} has no sha256 pin: run python3 scripts/dependency_snapshot.py --pin")
        elif e["sha256"] != tree_digest(root, e["path"]):
            errors.append(f"{TABLE}: the vendored files under {e['path']} changed since their sha256 pin: "
                          "check 'version' and 'source', then run python3 scripts/dependency_snapshot.py --pin")

    def walk(rel: str) -> None:
        for child in sorted((root / rel).iterdir()):
            if not child.is_dir():
                continue
            c = f"{rel}/{child.name}"
            if c in listed:
                continue
            if any(p.startswith(c + "/") for p in listed):
                walk(c)
                continue
            errors.append(f"{c} is not in {TABLE}: add it to 'sdks' (name, version, purl source) "
                          f"if it holds an SDK or library, or to 'doc_only' if it holds only documents or udev rules")

    if (root / EXTERNAL).is_dir():
        walk(EXTERNAL)
    return errors


def git_out(*args: str) -> str:
    return subprocess.run(["git", "-C", str(ROOT), *args], check=True, capture_output=True, text=True).stdout.strip()


def self_test() -> int:
    ctl = ("Source: x\nBuild-Depends: debhelper-compat (= 13),\n cmake (>= 3.20),\n g++,\n"
           " libgpiod-dev (>= 2.0),\n libfoo (= 1.2-3),\n libcurl4-openssl-dev | libcurl4-gnutls-dev\n"
           "Standards-Version: 4\n\nPackage: x\nArchitecture: arm64\n"
           "Depends: ${shlibs:Depends}, ${misc:Depends},\n libusb-1.0-0,\n adduser\nRecommends: polkitd\n")
    c = parse_control(ctl)
    assert c["development"] == [("cmake", None), ("g++", None), ("libgpiod-dev", None),
                                ("libfoo", "1.2-3"), ("libcurl4-openssl-dev", None)], c
    assert c["runtime"] == [("libusb-1.0-0", None), ("adduser", None)], c
    assert deb_purl("libfoo", "1.2-3") == "pkg:deb/debian/libfoo@1.2-3?arch=arm64&distro=trixie"
    cm = "FetchContent_Declare(\n nlohmann_json\n GIT_REPOSITORY https://github.com/nlohmann/json.git\n GIT_TAG v3.11.3\n)\n"
    assert deb_purl("g++", None).startswith("pkg:deb/debian/g%2B%2B?")
    assert parse_fetchcontent(cm) == [("nlohmann/json", "v3.11.3")]
    with tempfile.TemporaryDirectory() as d:
        r = Path(d)
        (r / "debian").mkdir()
        (r / "debian/control").write_text(ctl)
        (r / "AlpacaHTTP").mkdir()
        (r / "AlpacaHTTP/CMakeLists.txt").write_text(cm)
        (r / "scripts").mkdir()
        for p in ("A/sdk-1", "B", "Q/deep"):
            (r / EXTERNAL / p).mkdir(parents=True)
        (r / TABLE).write_text(json.dumps({"sdks": [
            {"path": f"{EXTERNAL}/A/sdk-1", "name": "a-sdk", "version": "1"},
            {"path": f"{EXTERNAL}/Q/deep", "name": "q-sdk", "version": None}],
            "doc_only": [f"{EXTERNAL}/B"]}))
        assert [e for e in check_drift(r) if "no sha256 pin" not in e] == [], check_drift(r)
        snap = build_snapshot(r, "a" * 40, "refs/heads/main", "1", "2026-01-01T00:00:00Z")
        for k in ("version", "job", "sha", "ref", "detector", "scanned", "manifests"):
            assert k in snap, k
        assert snap["version"] == 0 and {"correlator", "id"} <= set(snap["job"])
        assert {"name", "version", "url"} <= set(snap["detector"])
        for m in snap["manifests"].values():
            assert m["name"] and m["file"]["source_location"]
            for dep in m["resolved"].values():
                assert dep["relationship"] == "direct" and dep["scope"] in ("runtime", "development")
        assert "pkg:generic/q-sdk" in snap["manifests"][TABLE]["resolved"]
        assert "pkg:generic/a-sdk@1" in snap["manifests"][TABLE]["resolved"]
        assert "pkg:github/nlohmann/json@v3.11.3" in snap["manifests"]["AlpacaHTTP/CMakeLists.txt"]["resolved"]
        assert snap["manifests"]["debian/control"]["resolved"][deb_purl("cmake", None)]["scope"] == "development"
        assert snap["manifests"]["debian/control"]["resolved"][deb_purl("adduser", None)]["scope"] == "runtime"
        deb = {"Package": "alpacabridge", "Version": "5.0.0~beta2", "Architecture": "arm64",
               "Depends": "libc6 (>= 2.38), libgpiod3 (>= 2.1), libusb-1.0-0 (>= 2:1.0.16), adduser"}
        doc = build_spdx(r, deb, "x.deb", "f" * 64, "2026-01-01T00:00:00Z", "https://example.test/sbom")
        assert doc["spdxVersion"] == "SPDX-2.3" and doc["SPDXID"] == "SPDXRef-DOCUMENT"
        assert doc["dataLicense"] == "CC0-1.0" and doc["documentNamespace"] == "https://example.test/sbom"
        pkgs = {p["SPDXID"]: p for p in doc["packages"]}
        assert len(pkgs) == len(doc["packages"]), "SPDXIDs must be unique"
        purl = {p["externalRefs"][0]["referenceLocator"]: p["SPDXID"] for p in doc["packages"]}
        root_id = purl["pkg:deb/debian/alpacabridge@5.0.0~beta2?arch=arm64&distro=trixie"]
        assert pkgs[root_id]["checksums"] == [{"algorithm": "SHA256", "checksumValue": "f" * 64}]
        assert pkgs[root_id]["packageFileName"] == "x.deb"
        rels = {(x["spdxElementId"], x["relationshipType"], x["relatedSpdxElement"]) for x in doc["relationships"]}
        assert ("SPDXRef-DOCUMENT", "DESCRIBES", root_id) in rels
        # Runtime: the BUILT package's Depends (shlibs resolved), not debian/control's.
        for name in ("libc6", "libgpiod3", "libusb-1.0-0", "adduser"):
            assert (root_id, "DEPENDS_ON", purl[deb_purl(name, None)]) in rels, name
        assert deb_purl("libusb-1.0-0", None) in purl and deb_purl("libc6", None) in purl
        # Vendored SDKs and FetchContent libraries ship inside the package.
        for p in ("pkg:generic/a-sdk@1", "pkg:generic/q-sdk", "pkg:github/nlohmann/json@v3.11.3"):
            assert (root_id, "CONTAINS", purl[p]) in rels, p
        # Build-Depends are build-only.
        assert (purl[deb_purl("cmake", None)], "BUILD_DEPENDENCY_OF", root_id) in rels
        assert (purl[deb_purl("libfoo", "1.2-3")], "BUILD_DEPENDENCY_OF", root_id) in rels
        assert len(rels) == len(doc["relationships"]), "no duplicate relationships"
        for p in doc["packages"]:
            assert {"name", "SPDXID", "downloadLocation", "filesAnalyzed"} <= set(p), p
        # --- Vendored file pins and the shipped-file gate ---
        lib, fw = b"A-lib-bytes", b"Q-firmware"
        (r / EXTERNAL / "A/sdk-1/lib").mkdir()
        (r / EXTERNAL / "A/sdk-1/lib/liba.so.1.0").write_bytes(lib)
        (r / EXTERNAL / "A/sdk-1/lib/liba.so").symlink_to("liba.so.1.0")
        (r / EXTERNAL / "Q/deep/qhy.hex").write_bytes(fw)
        subprocess.run(["git", "-C", d, "init", "-q"], check=True)
        subprocess.run(["git", "-C", d, "add", "-A"], check=True)
        errs = check_drift(r)
        assert len(errs) == 2 and all("no sha256 pin" in e for e in errs), errs
        pin_sdk_hashes(r)
        assert check_drift(r) == [], check_drift(r)
        pinned = json.loads((r / TABLE).read_text())["sdks"][0]["sha256"]
        assert re.fullmatch(r"[0-9a-f]{64}", pinned) and pinned == tree_digest(r, f"{EXTERNAL}/A/sdk-1")
        (r / EXTERNAL / "A/sdk-1/lib/liba.so.1.0").write_bytes(b"swapped")
        errs = check_drift(r)
        assert len(errs) == 1 and f"{EXTERNAL}/A/sdk-1" in errs[0] and "--pin" in errs[0], errs
        (r / EXTERNAL / "A/sdk-1/lib/liba.so.1.0").write_bytes(lib)
        assert check_drift(r) == [], "restoring the bytes restores the pin"
        (r / EXTERNAL / "A/sdk-1/lib/liba.so").unlink()
        (r / EXTERNAL / "A/sdk-1/lib/liba.so").symlink_to("other")
        assert len(check_drift(r)) == 1, "a retargeted symlink changes the digest"
        (r / EXTERNAL / "A/sdk-1/lib/liba.so").unlink()
        (r / EXTERNAL / "A/sdk-1/lib/liba.so").symlink_to("liba.so.1.0")
        payload = {"usr/lib/alpacabridge/liba.so.1.0": file_digests(lib),
                   "usr/lib/firmware/qhy/qhy.hex": file_digests(fw)}
        installed = {"cmake": "3.31.6-2", "libfoo": "1.2-3", "libc6": "2.41-12"}
        doc = build_spdx(r, deb, "x.deb", "f" * 64, "2026-01-01T00:00:00Z", "https://example.test/sbom",
                         payload=payload, installed=installed)
        purl = {p["externalRefs"][0]["referenceLocator"]: p["SPDXID"] for p in doc["packages"]}
        pkgs = {p["SPDXID"]: p for p in doc["packages"]}
        files = {f["fileName"]: f for f in doc["files"]}
        assert set(files) == {"./usr/lib/alpacabridge/liba.so.1.0", "./usr/lib/firmware/qhy/qhy.hex"}, files
        f_lib = files["./usr/lib/alpacabridge/liba.so.1.0"]
        assert {"algorithm": "SHA256", "checksumValue": hashlib.sha256(lib).hexdigest()} in f_lib["checksums"]
        assert {"algorithm": "SHA1", "checksumValue": hashlib.sha1(lib).hexdigest()} in f_lib["checksums"]
        rels = {(x["spdxElementId"], x["relationshipType"], x["relatedSpdxElement"]) for x in doc["relationships"]}
        assert (purl["pkg:generic/a-sdk@1"], "CONTAINS", f_lib["SPDXID"]) in rels
        assert (purl["pkg:generic/q-sdk"], "CONTAINS", files["./usr/lib/firmware/qhy/qhy.hex"]["SPDXID"]) in rels
        # Build-Depends carry the version installed in the build environment.
        cmake = pkgs[purl[deb_purl("cmake", "3.31.6-2")]]
        assert cmake["versionInfo"] == "3.31.6-2"
        assert (purl[deb_purl("cmake", "3.31.6-2")], "BUILD_DEPENDENCY_OF", purl[
            "pkg:deb/debian/alpacabridge@5.0.0~beta2?arch=arm64&distro=trixie"]) in rels
        assert deb_purl("g++", None) in purl, "a build dep not installed keeps no version"
        # A shipped vendor file that matches no vendored file fails the SBOM.
        try:
            build_spdx(r, deb, "x.deb", "f" * 64, "t", "n",
                       payload={"usr/lib/alpacabridge/libevil.so": file_digests(b"evil")}, installed={})
        except ValueError as e:
            assert "usr/lib/alpacabridge/libevil.so" in str(e), e
        else:
            raise AssertionError("an unmatched shipped file must fail")
        (r / EXTERNAL / "New").mkdir()
        errs = check_drift(r)
        assert len(errs) == 1 and f"{EXTERNAL}/New is not in {TABLE}" in errs[0], errs
        (r / EXTERNAL / "New").rmdir()
        (r / EXTERNAL / "Q/new").mkdir()
        errs = check_drift(r)
        assert len(errs) == 1 and f"{EXTERNAL}/Q/new is not in {TABLE}" in errs[0], errs
        (r / EXTERNAL / "Q/new").rmdir()
        (r / EXTERNAL / "B").rmdir()
        errs = check_drift(r)
        assert len(errs) == 1 and "stale entry" in errs[0], errs
    print("dependency_snapshot self-test: OK")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("--sha")
    ap.add_argument("--ref")
    ap.add_argument("--output")
    ap.add_argument("--spdx", metavar="DEB")
    ap.add_argument("--pin", action="store_true")
    a = ap.parse_args()
    if a.self_test:
        return self_test()
    if a.pin:
        pin_sdk_hashes(ROOT)
        print(f"{TABLE}: sha256 pins rewritten")
        return 0
    if a.spdx:
        deb_path = Path(a.spdx)
        fields = ("Package", "Version", "Architecture", "Depends")
        out = subprocess.run(["dpkg-deb", "-f", str(deb_path), *fields],
                             check=True, capture_output=True, text=True).stdout
        deb = {k: v.strip() for k, _, v in (line.partition(":") for line in out.splitlines()) if k in fields}
        sha256 = hashlib.sha256(deb_path.read_bytes()).hexdigest()
        payload = {}
        with tempfile.TemporaryDirectory() as tmp:
            subprocess.run(["dpkg-deb", "-x", str(deb_path), tmp], check=True)
            for top in SHIPPED_VENDOR_DIRS:
                for dirpath, _, names in os.walk(Path(tmp) / top):
                    for n in names:
                        p = Path(dirpath) / n
                        if p.is_file() and not p.is_symlink():
                            payload[p.relative_to(tmp).as_posix()] = file_digests(p.read_bytes())
        query = subprocess.run(["dpkg-query", "-W", "-f=${Package}\\t${Version}\\n"],
                               check=True, capture_output=True, text=True).stdout
        installed = dict(line.split("\t", 1) for line in query.splitlines() if "\t" in line)
        repo = os.environ.get("GITHUB_REPOSITORY", "open-astro/AlpacaBridge")
        created = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
        try:
            doc = build_spdx(ROOT, deb, deb_path.name, sha256, created,
                             f"https://github.com/{repo}/sbom/{deb_path.name}-{sha256}",
                             payload=payload, installed=installed)
        except ValueError as e:
            print(f"ERROR: {e}", file=sys.stderr)
            return 1
        text = json.dumps(doc, indent=2) + "\n"
        if a.output:
            Path(a.output).write_text(text, encoding="utf-8")
        else:
            sys.stdout.write(text)
        return 0
    if a.check:
        errs = check_drift(ROOT)
        for e in errs:
            print(f"ERROR: {e}")
        print("dependency table: " + ("DRIFT" if errs else "OK"))
        return 1 if errs else 0
    sha = a.sha or os.environ.get("GITHUB_SHA") or git_out("rev-parse", "HEAD")
    ref = a.ref or os.environ.get("GITHUB_REF") or "refs/heads/main"
    job_id = os.environ.get("GITHUB_RUN_ID", "local")
    scanned = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    text = json.dumps(build_snapshot(ROOT, sha, ref, job_id, scanned), indent=2) + "\n"
    if a.output:
        Path(a.output).write_text(text, encoding="utf-8")
    else:
        sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
