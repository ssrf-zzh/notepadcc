#!/usr/bin/env python3
"""Write checksums, manifest, release notes and the static download page."""

from __future__ import annotations

import argparse
import hashlib
import html
import json
import re
import sys
from datetime import datetime, timezone
from pathlib import Path


SUPPLEMENTARY_FILES = [
    "LICENSE",
    "NOTICE",
    "sbom.cdx.json",
    "licenses/Scintilla-License.txt",
    "licenses/Lexilla-License.txt",
    "licenses/PCRE2-LICENCE.md",
    "licenses/SLJIT-LICENSE.txt",
]

ARTIFACT_ORDER = [
    "notepadcc-{version}-source.zip",
    "notepadcc-{version}-portable-x64.zip",
    "notepadcc.exe",
]


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def human_size(size: int) -> str:
    if size >= 1024 * 1024:
        return f"{size / (1024 * 1024):.2f} MiB"
    if size >= 1024:
        return f"{size / 1024:.1f} KiB"
    return f"{size} B"


def artifact_metadata(name: str) -> tuple[str, str, str]:
    lower = name.lower()
    if lower.endswith("-source.zip"):
        return "SRC", "对应源代码", "GPL-3.0 要求的完整源码、构建脚本与第三方库源码"
    if lower.endswith("-portable-x64.zip"):
        return "ZIP", "绿色版", "解压即用；保留 portable 标记即可把配置写在程序目录"
    return "EXE", "主程序", "与绿色包内程序相同；供独立校验或二次分发"


def collect_files(release_dir: Path, version: str) -> tuple[list[Path], list[Path]]:
    artifacts = [release_dir / pattern.format(version=version) for pattern in ARTIFACT_ORDER]
    missing = [str(path) for path in artifacts if not path.is_file()]
    if missing:
        raise FileNotFoundError("missing artifacts:\n  " + "\n  ".join(missing))
    hashed = artifacts + [release_dir / relative for relative in SUPPLEMENTARY_FILES]
    missing = [str(path) for path in hashed if not path.is_file()]
    if missing:
        raise FileNotFoundError("missing supplementary files:\n  " + "\n  ".join(missing))
    return artifacts, hashed


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--release-dir", required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--channel", required=True)
    parser.add_argument("--release-date", required=True)
    parser.add_argument("--commit", default="unknown")
    parser.add_argument("--dirty-tracked", action="store_true")
    parser.add_argument("--notes-source", required=True)
    parser.add_argument("--page-template", required=True)
    parser.add_argument("--signing-status", choices=["signed", "signed-untrusted", "unsigned"], required=True)
    parser.add_argument("--signing-method", default="none")
    parser.add_argument("--timestamp-url", default="")
    parser.add_argument("--signer-subject", default="")
    parser.add_argument("--signer-thumbprint", default="")
    args = parser.parse_args()
    signed = args.signing_status in ("signed", "signed-untrusted")
    trusted = args.signing_status == "signed"
    untrusted = args.signing_status == "signed-untrusted"

    release_dir = Path(args.release_dir).resolve()
    notes = Path(args.notes_source).resolve().read_text(encoding="utf-8")
    template = Path(args.page_template).resolve().read_text(encoding="utf-8")
    artifacts, hashed = collect_files(release_dir, args.version)
    if not (release_dir / "RELEASE_NOTES.txt").is_file():
        raise FileNotFoundError("missing release notes copy")
    entries = []
    for path in hashed:
        relative = path.relative_to(release_dir).as_posix()
        entries.append(
            {
                "path": relative,
                "sha256": sha256(path),
                "size": path.stat().st_size,
            }
        )

    sums_text = "".join(f"{entry['sha256']}  {entry['path']}\n" for entry in entries)
    sums_path = release_dir / "SHA256SUMS"
    sums_path.write_text(sums_text, encoding="ascii", newline="\n")

    notes = notes.rstrip() + "\n\n文件校验值\n----------\n\n"
    notes += "".join(f"{item['path']}  {item['sha256']}\n" for item in entries)
    if trusted:
        notes += "\n说明：以上哈希同时写入 SHA256SUMS；本发布包已完成 Authenticode 签名与时间戳。\n"
    elif signed:
        notes += "\n说明：以上哈希同时写入 SHA256SUMS；本候选包已完成 Authenticode 签名与时间戳，但证书链不受 Windows 信任，不得作为正式可信版本发布。\n"
    else:
        notes += "\n说明：以上哈希同时写入 SHA256SUMS；正式发布前须完成 Authenticode 签名与时间戳。\n"
    if signed and args.signer_subject:
        notes += f"签名主体：{args.signer_subject}\n"
    if signed and args.signer_thumbprint:
        notes += f"证书 SHA-1 指纹：{args.signer_thumbprint}\n"
    (release_dir / "RELEASE_NOTES.txt").write_text(notes, encoding="utf-8", newline="\n")

    entry_by_path = {item["path"]: item for item in entries}
    artifact_rows = []
    for path in artifacts:
        name = path.name
        item = entry_by_path[name]
        icon, title, description = artifact_metadata(name)
        artifact_rows.append(
            '<a class="download-row" href="%s">'
            '<span class="icon" aria-hidden="true">%s</span>'
            '<span><span class="download-title">%s</span>'
            '<span class="download-meta">%s · %s</span></span>'
            '<span class="button">下载</span></a>'
            % (html.escape(name, quote=True), icon, html.escape(title),
               html.escape(description), human_size(item["size"]))
        )
    checksum_rows = "".join(
        "<tr><td><a href=\"%s\">%s</a></td><td>%s</td><td><code>%s</code></td></tr>"
        % (html.escape(item["path"], quote=True), html.escape(item["path"]),
           human_size(item["size"]), item["sha256"])
        for item in entries
    )

    replacements = {
        "{{VERSION}}": html.escape(args.version),
        "{{CHANNEL}}": html.escape(args.channel),
        "{{DATE}}": html.escape(args.release_date),
        "{{COMMIT}}": html.escape(args.commit[:12] if args.commit else "unknown"),
        "{{DOWNLOAD_ROWS}}": "\n".join(artifact_rows),
        "{{CHECKSUM_ROWS}}": checksum_rows,
        "{{SIGNING_BADGE}}": (
            '<span class="pill ok">Authenticode 已签名</span>' if trusted else
            '<span class="pill warn">Authenticode 已签名 · 证书链不受信任</span>' if untrusted else
            '<span class="pill warn">候选包 · 未签名</span>'
        ),
        "{{SIGNING_CLASS}}": "ok" if trusted else "",
        "{{SIGNING_NOTE}}": (
            "\u6b64\u53d1\u5e03\u5305\u5df2\u901a\u8fc7 Authenticode \u7b7e\u540d\u4e0e\u65f6\u95f4\u6233\u3002\u4e0b\u8f7d\u540e\u4ecd\u8bf7\u6838\u5bf9\u54c8\u5e0c\u4e0e\u7b7e\u540d\u4e3b\u4f53\u3002" if trusted else
            "\u6b64\u5019\u9009\u5305\u5df2\u5b8c\u6210 Authenticode \u7b7e\u540d\u4e0e\u65f6\u95f4\u6233\uff0c\u4f46\u7b7e\u540d\u8bc1\u4e66\u94fe\u4e0d\u53d7 Windows \u4fe1\u4efb\u3002\u8bf7\u6838\u5bf9\u7b7e\u540d\u4e3b\u4f53\u4e0e SHA-256\uff1b\u4e0d\u5f97\u4f5c\u4e3a\u6b63\u5f0f\u53ef\u4fe1\u7248\u672c\u516c\u5f00\u4e0b\u8f7d\u3002"
            if untrusted else
            "\u6b64\u5019\u9009\u5305\u672a\u9644\u6b63\u5f0f Authenticode \u8bc1\u4e66\uff0c\u53ea\u7528\u4e8e\u5185\u90e8\u9a8c\u6536\u6216\u6d4b\u8bd5\uff1b\u4e0d\u5f97\u4f5c\u4e3a\u6b63\u5f0f\u7248\u672c\u516c\u5f00\u4e0b\u8f7d\u3002"
        ),
    }
    page = template
    for key, value in replacements.items():
        page = page.replace(key, value)
    unresolved = sorted(set(re.findall(r"\{\{[^}]+\}\}", page)))
    if unresolved:
        print("unresolved page placeholders: " + ", ".join(unresolved), file=sys.stderr)
        return 2
    (release_dir / "index.html").write_text(page, encoding="utf-8", newline="\n")

    manifest_files = entries + [
        {
            "path": "RELEASE_NOTES.txt",
            "sha256": sha256(release_dir / "RELEASE_NOTES.txt"),
            "size": (release_dir / "RELEASE_NOTES.txt").stat().st_size,
        },
        {
            "path": "SHA256SUMS",
            "sha256": sha256(sums_path),
            "size": sums_path.stat().st_size,
        },
        {
            "path": "index.html",
            "sha256": sha256(release_dir / "index.html"),
            "size": (release_dir / "index.html").stat().st_size,
        },
    ]
    manifest = {
        "schema": 1,
        "product": "notepadcc",
        "version": args.version,
        "channel": args.channel,
        "releaseDate": args.release_date,
        "generatedAt": datetime.now(timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z"),
        "gitCommit": args.commit,
        "dirtyTracked": args.dirty_tracked,
        "signing": {
            "status": args.signing_status,
            "trusted": trusted,
            "method": args.signing_method,
            "timestampUrl": args.timestamp_url,
            "signerSubject": args.signer_subject,
            "signerThumbprint": args.signer_thumbprint,
        },
        "sbom": "sbom.cdx.json",
        "artifacts": [path.name for path in artifacts],
        "files": manifest_files,
    }
    (release_dir / "release-manifest.json").write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8", newline="\n"
    )
    print(f"release metadata written: {release_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
