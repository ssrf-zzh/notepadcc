#!/usr/bin/env python3
"""Generate a CycloneDX 1.5 SBOM for a notepadcc release.

The component list is intentionally explicit. A release artifact must not gain a
new statically linked dependency without updating this file, NOTICE, and
NOTICE together.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
import uuid
from datetime import datetime, timezone
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def file_component(path: Path) -> dict:
    return {
        "type": "file",
        "bom-ref": "urn:npcc:artifact:" + path.name,
        "name": path.name,
        "hashes": [{"alg": "SHA-256", "content": sha256(path)}],
        "properties": [
            {"name": "npcc:artifact-prefix", "value": path.suffix.lower() or "none"},
        ],
    }


def library(
    name: str,
    version: str,
    license_value: str,
    license_kind: str,
    url: str,
    use: str,
    license_file: str,
    source_path: str,
) -> dict:
    license_object = (
        {"expression": license_value}
        if license_kind == "expression"
        else {"id": license_value} if license_kind == "id" else {"name": license_value}
    )
    return {
        "type": "library",
        "bom-ref": f"pkg:generic/{name.lower()}@{version}",
        "name": name,
        "version": version,
        "licenses": [{"license": license_object}],
        "externalReferences": [{"type": "website", "url": url}],
        "properties": [
            {"name": "npcc:use", "value": use},
            {"name": "npcc:source-path", "value": source_path},
            {"name": "npcc:license-file", "value": license_file},
            {"name": "npcc:modifications", "value": "none"},
        ],
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--version", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--artifact", action="append", default=[])
    parser.add_argument("--commit", default="")
    parser.add_argument("--channel", default="stable")
    parser.add_argument("--toolchain-version", default="unknown")
    parser.add_argument("--repo-root", default=str(ROOT))
    args = parser.parse_args()

    repo = Path(args.repo_root).resolve()
    output = Path(args.output).resolve()
    artifacts = [Path(value).resolve() for value in args.artifact]

    required_sources = [
        repo / "LICENSE",
        repo / "third_party" / "scintilla" / "License.txt",
        repo / "third_party" / "lexilla" / "License.txt",
        repo / "third_party" / "pcre2" / "LICENCE.md",
        repo / "third_party" / "pcre2" / "deps" / "sljit" / "LICENSE",
        repo / "licenses" / "MSVC-RUNTIME-NOTICE.txt",
    ]
    missing = [str(path) for path in required_sources if not path.is_file()]
    if missing:
        print("missing third-party license files:\n  " + "\n  ".join(missing), file=sys.stderr)
        return 2
    for artifact in artifacts:
        if not artifact.is_file():
            print(f"missing release artifact: {artifact}", file=sys.stderr)
            return 2

    libraries = [
        library(
            "Scintilla", "5.6.7", "HPND", "id", "https://www.scintilla.org/",
            "source editor control", "licenses/Scintilla-License.txt",
            "third_party/scintilla",),
        library(
            "Lexilla", "5.5.4", "HPND", "id", "https://www.scintilla.org/LexillaDoc.html",
            "language lexers", "licenses/Lexilla-License.txt",
            "third_party/lexilla",),
        library(
            "PCRE2", "10.49", "BSD-3-Clause WITH PCRE2-exception", "expression",
            "https://www.pcre.org/", "8-bit static regular-expression engine with JIT",
            "licenses/PCRE2-LICENCE.md", "third_party/pcre2",),
        library(
            "SLJIT", "bundled-with-PCRE2-10.49", "BSD-2-Clause", "id",
            "https://github.com/zherczeg/sljit", "PCRE2 JIT backend",
            "licenses/SLJIT-LICENSE.txt", "third_party/pcre2/deps/sljit",),
        library(
            "Microsoft Visual C++ Runtime", args.toolchain_version,
            "Microsoft Software License Terms for MSVC redistributable files", "name",
            "https://learn.microsoft.com/cpp/windows/latest-supported-vc-redist",
            "statically linked MSVC runtime (/MT)",
            "licenses/MSVC-RUNTIME-NOTICE.txt", "MSVC static runtime",),
    ]

    artifact_components = [file_component(path) for path in artifacts]
    application_ref = f"pkg:generic/notepadcc@{args.version}?arch=x64&os=windows"
    executable = next((path for path in artifacts if path.name.lower() == "notepadcc.exe"), None)
    application = {
        "type": "application",
        "bom-ref": application_ref,
        "name": "notepadcc",
        "version": args.version,
        "description": "Native Windows x64 text and source editor",
        "licenses": [{"license": {"id": "GPL-3.0-only"}}],
        "copyright": "Copyright (C) 2026 notepadcc contributors",
        "properties": [
            {"name": "npcc:channel", "value": args.channel},
            {"name": "npcc:git-commit", "value": args.commit or "unknown"},
            {"name": "npcc:license-file", "value": "LICENSE"},
        ],
    }
    if executable is not None:
        application["hashes"] = [{"alg": "SHA-256", "content": sha256(executable)}]

    serial_input = f"notepadcc:{args.version}:{args.commit}".encode("utf-8")
    serial = uuid.UUID(hashlib.sha256(serial_input).hexdigest()[:32])
    bom = {
        "bomFormat": "CycloneDX",
        "specVersion": "1.5",
        "serialNumber": "urn:uuid:" + str(serial),
        "version": 1,
        "metadata": {
            "timestamp": datetime.now(timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z"),
            "tools": [{"vendor": "notepadcc", "name": "generate_sbom.py", "version": "1.0"}],
            "component": application,
        },
        "components": libraries + artifact_components,
        "dependencies": [
            {
                "ref": application_ref,
                "dependsOn": [item["bom-ref"] for item in libraries + artifact_components],
            }
        ]
        + [{"ref": item["bom-ref"], "dependsOn": []} for item in libraries],
    }

    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(bom, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(f"SBOM written: {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
