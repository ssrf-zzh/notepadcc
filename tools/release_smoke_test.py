# -*- coding: utf-8 -*-
"""B13 release-bundle acceptance: hashes, manifest, SBOM, page and licenses."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
import zipfile
from pathlib import Path


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def fail(message: str) -> int:
    print(f"  FAIL: {message}")
    return 1


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--release-dir", required=True)
    parser.add_argument("--version", default="1.0.0")
    parser.add_argument("--expect-signing", choices=["any", "signed", "unsigned"],
                        default="any")
    args = parser.parse_args()

    release = Path(args.release_dir).resolve()
    version = args.version
    required = [
        "notepadcc.exe",
        f"notepadcc-{version}-portable-x64.zip",
        f"notepadcc-{version}-x64-peruser.msi",
        f"notepadcc-{version}-x64-permachine.msi",
        f"notepadcc-{version}-source.zip",
        "RELEASE_NOTES.txt",
        "LICENSE",
        "NOTICE",
        "sbom.cdx.json",
        "SHA256SUMS",
        "release-manifest.json",
        "index.html",
        "licenses/Scintilla-License.txt",
        "licenses/Lexilla-License.txt",
        "licenses/PCRE2-LICENCE.md",
        "licenses/SLJIT-LICENSE.txt",
        "licenses/MSVC-RUNTIME-NOTICE.txt",
    ]
    for relative in required:
        path = release / relative
        if not path.is_file() or path.stat().st_size == 0:
            return fail(f"missing or empty release file: {relative}")

    sums = {}
    for line in (release / "SHA256SUMS").read_text(encoding="ascii").splitlines():
        match = re.fullmatch(r"([0-9a-f]{64})  (.+)", line)
        if not match:
            return fail(f"bad SHA256SUMS line: {line!r}")
        digest, relative = match.groups()
        path = release / relative
        if not path.is_file():
            return fail(f"SHA256SUMS references missing file: {relative}")
        actual = sha256(path)
        if actual != digest:
            return fail(f"hash mismatch for {relative}: {actual} != {digest}")
        sums[relative] = digest

    expected_artifacts = {
        "notepadcc.exe",
        f"notepadcc-{version}-portable-x64.zip",
        f"notepadcc-{version}-x64-peruser.msi",
        f"notepadcc-{version}-x64-permachine.msi",
        f"notepadcc-{version}-source.zip",
    }
    if not expected_artifacts.issubset(sums):
        return fail("SHA256SUMS does not cover all five release artifacts")

    manifest = json.loads((release / "release-manifest.json").read_text(encoding="utf-8"))
    if manifest.get("product") != "notepadcc" or manifest.get("version") != version:
        return fail("release-manifest product/version mismatch")
    if set(manifest.get("artifacts", [])) != expected_artifacts:
        return fail("release-manifest artifacts mismatch")
    signing_status = manifest.get("signing", {}).get("status")
    if signing_status not in ("signed", "unsigned"):
        return fail(f"unexpected signing status {signing_status!r}")
    if args.expect_signing != "any" and signing_status != args.expect_signing:
        return fail(f"signing status {signing_status!r}, expected {args.expect_signing!r}")
    manifest_files = {item["path"]: item for item in manifest.get("files", [])}
    for relative in sums:
        if manifest_files.get(relative, {}).get("sha256") != sums[relative]:
            return fail(f"manifest hash differs from SHA256SUMS for {relative}")
    for relative in ("RELEASE_NOTES.txt", "SHA256SUMS", "index.html"):
        item = manifest_files.get(relative)
        if not item or item.get("sha256") != sha256(release / relative):
            return fail(f"manifest does not authenticate {relative}")

    sbom = json.loads((release / "sbom.cdx.json").read_text(encoding="utf-8"))
    if sbom.get("bomFormat") != "CycloneDX" or sbom.get("specVersion") != "1.5":
        return fail("SBOM is not CycloneDX 1.5")
    components = {item.get("name"): item for item in sbom.get("components", [])}
    for name in ("Scintilla", "Lexilla", "PCRE2", "SLJIT", "Microsoft Visual C++ Runtime"):
        if name not in components:
            return fail(f"SBOM is missing component {name}")
    for name in expected_artifacts:
        component = components.get(name)
        if not component:
            return fail(f"SBOM is missing artifact component {name}")
        digest = component.get("hashes", [{}])[0].get("content")
        if digest != sums[name]:
            return fail(f"SBOM hash differs for {name}")

    notes = (release / "RELEASE_NOTES.txt").read_text(encoding="utf-8")
    page = (release / "index.html").read_text(encoding="utf-8")
    for relative, digest in sums.items():
        if relative not in notes or digest not in notes:
            return fail(f"release notes omit checksum for {relative}")
        if relative not in page or digest not in page:
            return fail(f"download page omits checksum for {relative}")
    if "{{" in page or "}}" in page:
        return fail("download page has unresolved placeholders")
    for name in expected_artifacts:
        if f'href="{name}"' not in page:
            return fail(f"download page has no download link for {name}")

    notice = (release / "NOTICE").read_text(encoding="utf-8")
    for marker in ("Scintilla", "Lexilla", "PCRE2", "SLJIT", "Microsoft Visual C++ Runtime"):
        if marker not in notice:
            return fail(f"NOTICE omits {marker}")
    if "proprietary" in notice.casefold():
        return fail("NOTICE still declares the project as proprietary")
    project_component = sbom.get("metadata", {}).get("component", {})
    license_ids = {
        str(entry.get("license", {}).get("id", ""))
        for entry in project_component.get("licenses", [])
    }
    if "GPL-3.0-only" not in license_ids:
        return fail("SBOM does not declare the project as GPL-3.0-only")
    if "GNU General Public License" not in notice:
        return fail("NOTICE does not state the project's GNU GPL license")
    for entry in project_component.get("licenses", []):
        license_name = str(entry.get("license", {}).get("name", "")).casefold()
        if license_name == "proprietary":
            return fail("SBOM still declares the project as proprietary")

    with zipfile.ZipFile(release / f"notepadcc-{version}-portable-x64.zip") as archive:
        names = archive.namelist()
        for name in ("notepadcc.exe", "portable", "说明.txt"):
            if name not in names:
                return fail(f"portable zip is missing {name}")
        with archive.open("notepadcc.exe") as stream:
            digest = hashlib.sha256(stream.read()).hexdigest()
    if digest != sums["notepadcc.exe"]:
        return fail("portable zip executable differs from the release executable")

    license_text = (release / "LICENSE").read_text(encoding="utf-8")
    if "GNU GENERAL PUBLIC LICENSE" not in license_text or "Version 3, 29 June 2007" not in license_text:
        return fail("release LICENSE is not the GNU GPL v3 text")
    if 'href="LICENSE"' not in page:
        return fail("download page does not link the project license")

    source_name = f"notepadcc-{version}-source.zip"
    source_prefix = f"notepadcc-{version}-source/"
    with zipfile.ZipFile(release / source_name) as archive:
        source_names = set(archive.namelist())
        required_source = [
            "LICENSE", "CMakeLists.txt", "SOURCE_MANIFEST.txt",
            "scripts/package_source.ps1", "scripts/release.ps1",
            "third_party/scintilla/License.txt", "third_party/pcre2/LICENCE.md",
        ]
        for name in required_source:
            if source_prefix + name not in source_names:
                return fail(f"corresponding source zip is missing {name}")
        with archive.open(source_prefix + "LICENSE") as stream:
            source_license = stream.read().decode("utf-8")
    if source_license != license_text:
        return fail("source zip LICENSE differs from the release LICENSE")

    print("  B13 发布包：五件产物、SHA256SUMS、manifest、CycloneDX SBOM、NOTICE、"
          "项目 GPL 许可证、对应源码包、第三方许可证、发布说明与静态下载页逐项一致；"
          "zip 内程序哈希相同，"
          f"签名状态为 {signing_status}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
