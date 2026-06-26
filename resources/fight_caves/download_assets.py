#!/usr/bin/env python3
"""Download and verify Fight Caves runtime assets."""

from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import sys
import tarfile
import tempfile
import urllib.request
from pathlib import Path


SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parents[1]
MANIFEST_PATH = SCRIPT_DIR / "manifest.json"


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def load_manifest() -> dict:
    with MANIFEST_PATH.open("r", encoding="utf-8") as handle:
        return json.load(handle)


def target_dir(manifest: dict) -> Path:
    return REPO_ROOT / manifest["extract_to"]


def required_files_ready(manifest: dict) -> bool:
    base = target_dir(manifest)
    for item in manifest["required_files"]:
        path = base / item["path"]
        if not path.is_file():
            return False
        if path.stat().st_size != int(item["size_bytes"]):
            return False
        if sha256(path) != item["sha256"]:
            return False
    return True


def download(url: str, dst: Path) -> None:
    with urllib.request.urlopen(url) as response:
        with dst.open("wb") as handle:
            shutil.copyfileobj(response, handle)


def safe_extract(archive: Path, dst: Path) -> None:
    dst_resolved = dst.resolve()
    with tarfile.open(archive, "r:gz") as tar:
        for member in tar.getmembers():
            out_path = (dst / member.name).resolve()
            if not out_path.is_relative_to(dst_resolved):
                raise RuntimeError(f"Archive member escapes target directory: {member.name}")
        tar.extractall(dst)


def verify_required_files(manifest: dict) -> None:
    base = target_dir(manifest)
    missing = []
    mismatched = []
    for item in manifest["required_files"]:
        path = base / item["path"]
        if not path.is_file():
            missing.append(item["path"])
            continue
        if path.stat().st_size != int(item["size_bytes"]) or sha256(path) != item["sha256"]:
            mismatched.append(item["path"])

    if missing or mismatched:
        details = []
        if missing:
            details.append("missing: " + ", ".join(missing))
        if mismatched:
            details.append("checksum/size mismatch: " + ", ".join(mismatched))
        raise RuntimeError("; ".join(details))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--force", action="store_true", help="download even if assets are already present")
    args = parser.parse_args()

    manifest = load_manifest()
    dst = target_dir(manifest)
    dst.mkdir(parents=True, exist_ok=True)

    if not args.force and required_files_ready(manifest):
        print(f"Fight Caves assets already installed in {dst}")
        return 0

    with tempfile.TemporaryDirectory(prefix="fight-caves-assets-") as tmpdir:
        archive = Path(tmpdir) / manifest["archive"]
        print(f"Downloading {manifest['url']}")
        download(manifest["url"], archive)

        actual = sha256(archive)
        if actual != manifest["sha256"]:
            raise RuntimeError(
                f"Archive checksum mismatch for {manifest['archive']}: "
                f"expected {manifest['sha256']}, got {actual}"
            )

        safe_extract(archive, dst)

    verify_required_files(manifest)
    print(f"Installed Fight Caves assets in {dst}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"error: {exc}", file=sys.stderr)
        raise SystemExit(1)
