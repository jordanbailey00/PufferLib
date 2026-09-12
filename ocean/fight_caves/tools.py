#!/usr/bin/env python3
"""Fight Caves asset installation, release bundles and validation helpers."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import gzip
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import shlex
import shutil
import subprocess
import sys
import tarfile
import tempfile
from typing import Any
from urllib.error import HTTPError, URLError
from urllib.request import Request, urlopen

# Asset installation

REPO_ROOT = Path(__file__).resolve().parents[2]
RESOURCE_ROOT = REPO_ROOT / "resources" / "fight_caves"
DEFAULT_MANIFEST = RESOURCE_ROOT / "asset_manifest.json"
BUFFER_SIZE = 1024 * 1024


class AssetError(RuntimeError):
    pass


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        while chunk := handle.read(BUFFER_SIZE):
            digest.update(chunk)
    return digest.hexdigest()


def load_manifest(path: Path) -> dict:
    try:
        manifest = json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError as exc:
        raise AssetError(f"asset manifest not found: {path}") from exc
    except (OSError, json.JSONDecodeError) as exc:
        raise AssetError(f"could not read asset manifest {path}: {exc}") from exc

    if manifest.get("schema_version") != 1:
        raise AssetError("unsupported Fight Caves asset manifest schema")
    if not isinstance(manifest.get("bundles"), dict):
        raise AssetError("asset manifest has no bundle definitions")
    return manifest


def checked_relative_path(value: str) -> PurePosixPath:
    path = PurePosixPath(value)
    if path.is_absolute() or not path.parts or ".." in path.parts:
        raise AssetError(f"unsafe path in asset manifest or archive: {value!r}")
    return path


def expected_files(bundle: dict) -> dict[str, dict]:
    result: dict[str, dict] = {}
    for entry in bundle.get("files", []):
        relative = checked_relative_path(entry.get("path", "")).as_posix()
        if relative in result:
            raise AssetError(f"duplicate file in asset manifest: {relative}")
        if not isinstance(entry.get("size_bytes"), int) or not entry.get("sha256"):
            raise AssetError(f"incomplete asset metadata for {relative}")
        result[relative] = entry
    if not result:
        raise AssetError("asset bundle contains no files")
    return result


def verify_tree(root: Path, bundle: dict, *, exact: bool) -> list[str]:
    expected = expected_files(bundle)
    errors: list[str] = []
    for relative, entry in expected.items():
        path = root.joinpath(*PurePosixPath(relative).parts)
        if not path.is_file():
            errors.append(f"missing {relative}")
            continue
        actual_size = path.stat().st_size
        if actual_size != entry["size_bytes"]:
            errors.append(
                f"wrong size for {relative}: expected {entry['size_bytes']}, "
                f"got {actual_size}"
            )
            continue
        actual_hash = sha256_file(path)
        if actual_hash != entry["sha256"]:
            errors.append(f"checksum mismatch for {relative}")

    if exact:
        actual = {
            path.relative_to(root).as_posix()
            for path in root.rglob("*")
            if path.is_file()
        }
        for relative in sorted(actual - set(expected)):
            errors.append(f"unexpected file in bundle: {relative}")
    return errors


def download(url: str, destination: Path) -> None:
    print(f"Downloading {url}")
    request = Request(url, headers={"User-Agent": "PufferLib-Fight-Caves-assets/1"})
    try:
        with urlopen(request, timeout=60) as response, destination.open("wb") as out:
            shutil.copyfileobj(response, out, BUFFER_SIZE)
    except (HTTPError, URLError, TimeoutError, OSError) as exc:
        raise AssetError(f"download failed for {url}: {exc}") from exc


def extract_checked(archive: Path, destination: Path, bundle: dict) -> None:
    expected = set(expected_files(bundle))
    archived_files: set[str] = set()

    try:
        with tarfile.open(archive, "r:gz") as source:
            for member in source.getmembers():
                relative = checked_relative_path(member.name)
                relative_name = relative.as_posix()
                if member.isdir():
                    continue
                if not member.isfile():
                    raise AssetError(
                        f"unsupported non-file entry in asset archive: {relative_name}"
                    )
                if relative_name not in expected:
                    raise AssetError(
                        f"unexpected file in asset archive: {relative_name}"
                    )
                if relative_name in archived_files:
                    raise AssetError(f"duplicate file in asset archive: {relative_name}")

                target = destination.joinpath(*relative.parts)
                target.parent.mkdir(parents=True, exist_ok=True)
                extracted = source.extractfile(member)
                if extracted is None:
                    raise AssetError(f"could not extract {relative_name}")
                with extracted, target.open("wb") as out:
                    shutil.copyfileobj(extracted, out, BUFFER_SIZE)
                target.chmod(0o644)
                archived_files.add(relative_name)
    except (OSError, tarfile.TarError) as exc:
        raise AssetError(f"could not extract {archive}: {exc}") from exc

    missing = expected - archived_files
    if missing:
        raise AssetError(f"asset archive is missing {sorted(missing)[0]}")


def replace_bundle(staged_root: Path, bundle: dict) -> None:
    prefix = checked_relative_path(bundle.get("install_prefix", ""))
    if len(prefix.parts) != 1:
        raise AssetError("bundle install prefix must be one directory name")

    staged = staged_root.joinpath(*prefix.parts)
    destination = RESOURCE_ROOT.joinpath(*prefix.parts)
    incoming = RESOURCE_ROOT / f".{prefix.name}.installing"
    backup = RESOURCE_ROOT / f".{prefix.name}.backup"

    if not staged.is_dir():
        raise AssetError(f"archive did not contain expected {prefix}/ directory")
    if incoming.exists() or backup.exists():
        raise AssetError(
            f"stale installer directory found under {RESOURCE_ROOT}; "
            "remove it and retry"
        )

    shutil.copytree(staged, incoming)
    try:
        if destination.exists():
            destination.rename(backup)
        incoming.rename(destination)
        if backup.exists():
            shutil.rmtree(backup)
    except Exception:
        if incoming.exists():
            shutil.rmtree(incoming)
        if backup.exists() and not destination.exists():
            backup.rename(destination)
        raise


def install_bundle(name: str, bundle: dict, *, force: bool) -> None:
    if not force:
        current_errors = verify_tree(RESOURCE_ROOT, bundle, exact=False)
        if not current_errors:
            print(f"Fight Caves {name} assets are already installed and verified.")
            return

    archive_name = bundle.get("archive", "")
    archive_hash = bundle.get("sha256", "")
    archive_size = bundle.get("size_bytes")
    url = bundle.get("url", "")
    if not archive_name or not archive_hash or not url or not isinstance(archive_size, int):
        raise AssetError(f"incomplete archive metadata for {name}")

    RESOURCE_ROOT.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="fight-caves-assets-") as temp_value:
        temp = Path(temp_value)
        archive = temp / archive_name
        extracted = temp / "extracted"
        extracted.mkdir()
        download(url, archive)

        if archive.stat().st_size != archive_size:
            raise AssetError(
                f"wrong archive size for {archive_name}: expected {archive_size}, "
                f"got {archive.stat().st_size}"
            )
        if sha256_file(archive) != archive_hash:
            raise AssetError(f"checksum mismatch for downloaded {archive_name}")

        extract_checked(archive, extracted, bundle)
        staged_errors = verify_tree(extracted, bundle, exact=True)
        if staged_errors:
            raise AssetError(staged_errors[0])
        replace_bundle(extracted, bundle)

    installed_errors = verify_tree(RESOURCE_ROOT, bundle, exact=False)
    if installed_errors:
        raise AssetError(installed_errors[0])
    print(f"Installed and verified Fight Caves {name} assets.")


def setup_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Install pinned Fight Caves runtime and viewer assets."
    )
    selection = parser.add_mutually_exclusive_group()
    selection.add_argument("--core", action="store_true", help="select runtime maps")
    selection.add_argument("--viewer", action="store_true", help="select viewer assets")
    selection.add_argument("--all", action="store_true", help="select both bundles")
    parser.add_argument(
        "--verify-only",
        action="store_true",
        help="verify installed files without downloading or changing them",
    )
    parser.add_argument(
        "--force", action="store_true", help="download and reinstall selected bundles"
    )
    parser.add_argument(
        "--manifest",
        type=Path,
        default=DEFAULT_MANIFEST,
        help=argparse.SUPPRESS,
    )
    return parser.parse_args()


def setup_main() -> int:
    args = setup_args()
    try:
        manifest = load_manifest(args.manifest)
        names = (
            ["core", "viewer"]
            if args.all or not (args.core or args.viewer)
            else []
        )
        if args.core:
            names = ["core"]
        elif args.viewer:
            names = ["viewer"]

        for name in names:
            bundle = manifest["bundles"].get(name)
            if not isinstance(bundle, dict):
                raise AssetError(f"asset manifest has no {name} bundle")
            if args.verify_only:
                errors = verify_tree(RESOURCE_ROOT, bundle, exact=False)
                if errors:
                    raise AssetError(f"{name}: {errors[0]}")
                print(f"Fight Caves {name} assets are installed and verified.")
            else:
                install_bundle(name, bundle, force=args.force)
    except (AssetError, KeyError, OSError) as exc:
        print(f"Fight Caves asset setup failed: {exc}", file=sys.stderr)
        return 1
    return 0


# Release bundles

RUNTIME_FILES = (
    "fightcaves.collision",
    "fightcaves.movement",
    "fightcaves.los",
)


@dataclass(frozen=True)
class BundleFile:
    source: Path
    archive_path: str


def collect_runtime(source: Path) -> list[BundleFile]:
    result = []
    for name in RUNTIME_FILES:
        path = source / name
        if not path.is_file():
            raise SystemExit(f"missing runtime asset: {path}")
        result.append(BundleFile(path, f"runtime/{name}"))
    return result


def collect_viewer(source: Path) -> list[BundleFile]:
    result = [
        BundleFile(path, f"viewer/{path.relative_to(source).as_posix()}")
        for path in source.rglob("*")
        if path.is_file()
    ]
    if not result:
        raise SystemExit(f"no viewer assets found under {source}")
    return sorted(result, key=lambda entry: entry.archive_path)


def write_archive(destination: Path, files: list[BundleFile]) -> None:
    temporary = destination.with_suffix(destination.suffix + ".tmp")
    destination.parent.mkdir(parents=True, exist_ok=True)
    with temporary.open("wb") as raw:
        with gzip.GzipFile(filename="", mode="wb", fileobj=raw, mtime=0) as compressed:
            with tarfile.open(
                mode="w", fileobj=compressed, format=tarfile.PAX_FORMAT
            ) as archive:
                for entry in sorted(files, key=lambda value: value.archive_path):
                    info = archive.gettarinfo(str(entry.source), entry.archive_path)
                    info.uid = 0
                    info.gid = 0
                    info.uname = ""
                    info.gname = ""
                    info.mode = 0o644
                    info.mtime = 0
                    with entry.source.open("rb") as source:
                        archive.addfile(info, source)
    temporary.replace(destination)


def bundle_manifest(
    archive: Path,
    files: list[BundleFile],
    repository: str,
    release_tag: str,
    install_prefix: str,
) -> dict:
    return {
        "archive": archive.name,
        "url": (
            f"https://github.com/{repository}/releases/download/"
            f"{release_tag}/{archive.name}"
        ),
        "size_bytes": archive.stat().st_size,
        "sha256": sha256_file(archive),
        "install_prefix": install_prefix,
        "files": [
            {
                "path": entry.archive_path,
                "size_bytes": entry.source.stat().st_size,
                "sha256": sha256_file(entry.source),
            }
            for entry in sorted(files, key=lambda value: value.archive_path)
        ],
    }


def bundle_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--runtime-source", type=Path, required=True)
    parser.add_argument("--viewer-source", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--release-repository", required=True)
    parser.add_argument("--release-tag", required=True)
    parser.add_argument("--source-revision", required=True)
    parser.add_argument("--bundle-version", default="v2")
    return parser.parse_args()


def bundle_main() -> int:
    args = bundle_args()
    runtime_files = collect_runtime(args.runtime_source)
    viewer_files = collect_viewer(args.viewer_source)
    runtime_archive = (
        args.output_dir / f"fight-caves-runtime-assets-{args.bundle_version}.tar.gz"
    )
    viewer_archive = (
        args.output_dir / f"fight-caves-viewer-assets-{args.bundle_version}.tar.gz"
    )

    write_archive(runtime_archive, runtime_files)
    write_archive(viewer_archive, viewer_files)
    manifest = {
        "schema_version": 1,
        "release_tag": args.release_tag,
        "source": {
            "repository": f"https://github.com/{args.release_repository}",
            "revision": args.source_revision,
        },
        "bundles": {
            "core": bundle_manifest(
                runtime_archive,
                runtime_files,
                args.release_repository,
                args.release_tag,
                "runtime",
            ),
            "viewer": bundle_manifest(
                viewer_archive,
                viewer_files,
                args.release_repository,
                args.release_tag,
                "viewer",
            ),
        },
    }
    args.manifest.parent.mkdir(parents=True, exist_ok=True)
    args.manifest.write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    print(runtime_archive)
    print(viewer_archive)
    print(args.manifest)
    return 0


# Dependency preflight

def command_name(value: str | None, default: str) -> str:
    words = shlex.split(value or default)
    return words[0] if words else default


def command_words(value: str | None, default: str) -> list[str]:
    words = shlex.split(value or default)
    return words or [default]


def require_command(errors: list[str], name: str, purpose: str) -> None:
    if shutil.which(name) is None:
        errors.append(f"required command '{name}' is unavailable ({purpose})")


def verify_assets(errors: list[str], names: tuple[str, ...]) -> None:
    try:
        manifest = load_manifest(DEFAULT_MANIFEST)
        for name in names:
            bundle = manifest["bundles"].get(name)
            if not isinstance(bundle, dict):
                errors.append(f"asset manifest has no {name} bundle")
                continue
            failures = verify_tree(
                RESOURCE_ROOT, bundle, exact=False
            )
            if failures:
                errors.append(f"{name} asset bundle is invalid: {failures[0]}")
    except Exception as exc:
        errors.append(f"could not verify Fight Caves assets: {exc}")


def check_linux_viewer_link(
    errors: list[str], compiler_value: str | None
) -> None:
    if sys.platform != "linux":
        return
    x11_header = Path("/usr/include/X11/Xlib.h")
    if not x11_header.is_file():
        errors.append(
            "X11 development headers are unavailable; on Ubuntu install "
            "libx11-dev libxrandr-dev libxi-dev libxcursor-dev libxinerama-dev"
        )
        return
    compiler = command_words(compiler_value, "clang")
    source = "int main(void) { return 0; }\n"
    try:
        with tempfile.TemporaryDirectory(prefix="fight-caves-preflight-") as value:
            root = Path(value)
            result = subprocess.run(
                [*compiler, "-x", "c", "-", "-o", str(root / "link-test"),
                 "-lGL", "-lX11"],
                input=source,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=False,
            )
    except OSError as exc:
        errors.append(
            f"could not run viewer link check with {' '.join(compiler)}: {exc}"
        )
        return
    if result.returncode != 0:
        detail = result.stderr.strip().splitlines()
        suffix = f": {detail[-1]}" if detail else ""
        errors.append(
            "OpenGL/X11 development libraries cannot be linked; on Ubuntu "
            "install libgl1-mesa-dev and the X11 development packages"
            f"{suffix}"
        )


def check_openmp(errors: list[str], compiler_value: str | None) -> None:
    compiler = command_words(compiler_value, "clang")
    flags = ["-fopenmp"]
    try:
        if sys.platform == "darwin":
            prefix = subprocess.check_output(
                ["brew", "--prefix", "libomp"], text=True, stderr=subprocess.PIPE
            ).strip()
            flags = ["-Xclang", "-fopenmp", f"-I{prefix}/include",
                     f"-L{prefix}/lib", "-lomp"]
        with tempfile.TemporaryDirectory(prefix="fight-caves-openmp-") as value:
            result = subprocess.run(
                [*compiler, "-x", "c", "-", *flags, "-o", str(Path(value) / "test")],
                input="#include <omp.h>\nint main(void) { return omp_get_max_threads() < 1; }\n",
                text=True, capture_output=True, check=False,
            )
    except (OSError, subprocess.CalledProcessError) as exc:
        errors.append(f"could not run C OpenMP check: {exc}")
        return
    if result.returncode != 0:
        errors.append(
            "C compiler cannot build and link OpenMP; install libomp-dev "
            "(Ubuntu) or brew install libomp (macOS), or select a compatible CC:\n"
            + result.stderr.strip()
        )


def check_graphical_display(errors: list[str]) -> None:
    if sys.platform != "linux":
        return
    display = os.environ.get("DISPLAY")
    if not display:
        errors.append(
            "no graphical DISPLAY is configured; run under xvfb-run for "
            "headless validation or launch from a graphical session"
        )
        return
    xdpyinfo = shutil.which("xdpyinfo")
    if xdpyinfo is None:
        errors.append(
            "'xdpyinfo' is required to validate the X11 display; on Ubuntu "
            "install x11-utils"
        )
        return
    result = subprocess.run(
        [xdpyinfo, "-display", display],
        text=True,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
        check=False,
    )
    if result.returncode != 0:
        errors.append(
            f"X11 display {display!r} is not accessible; run under xvfb-run "
            "for headless validation or fix the display authorization"
        )


def preflight_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Check Fight Caves build dependencies and installed assets."
    )
    parser.add_argument(
        "--mode",
        choices=("core", "cpu", "native", "viewer", "viewer-runtime"),
        required=True,
        help="build path to validate",
    )
    return parser.parse_args()


def preflight_main() -> int:
    return run_preflight(preflight_args().mode)


def run_preflight(mode: str) -> int:
    errors: list[str] = []
    if sys.version_info < (3, 10):
        errors.append(
            f"Python 3.10 or newer is required; found {sys.version.split()[0]}"
        )

    if mode not in ("core", "cpu", "native", "viewer", "viewer-runtime"):
        raise ValueError(f"unsupported preflight mode: {mode}")
    compiler = command_name(os.environ.get("CC"), "clang")
    if mode != "viewer-runtime":
        require_command(errors, compiler, "C compilation")

    verify_assets(errors, ("core",) if mode == "core" else ("core", "viewer"))
    if mode in ("cpu", "native"):
        check_openmp(errors, os.environ.get("CC"))
    if mode in ("cpu", "native", "viewer"):
        check_linux_viewer_link(errors, os.environ.get("CC"))
    if mode == "native":
        cuda_home = os.environ.get("CUDA_HOME") or os.environ.get("CUDA_PATH")
        nvcc = str(Path(cuda_home) / "bin" / "nvcc") if cuda_home else "nvcc"
        require_command(errors, nvcc, "native CUDA compilation; set CUDA_HOME if needed")
        require_command(errors, "ccache", "native build.sh compiler wrapper")
    if mode == "viewer":
        require_command(errors, "cmake", "optional standalone viewer/test build")
    if mode == "viewer-runtime":
        check_graphical_display(errors)

    if errors:
        print("Fight Caves preflight failed:", file=sys.stderr)
        for error in errors:
            print(f"  - {error}", file=sys.stderr)
        if any("asset bundle" in error or "verify Fight Caves assets" in error
               for error in errors):
            print(
                "Install assets with: python3 ocean/fight_caves/tools.py setup --all",
                file=sys.stderr,
            )
        return 1

    print(f"Fight Caves {mode} preflight passed.")
    if mode == "native":
        print("Compiler/assets checks only. Run ./build.sh fight_caves to verify "
              "the complete CUDA/NCCL build; this check does not test GPU runtime access.")
    return 0


# Compiled environment inspection (not checkpoint compatibility)

class ContractError(RuntimeError):
    pass


REQUIRED_FIELDS = (
    "contract_dump_schema_version",
    "policy_obs_size",
    "puffer_obs_size",
    "puffer_action_dims",
    "puffer_mask_size",
    "observation_version",
    "action_version",
    "reward_version",
    "prayer_timing_version",
    "state_hash_version",
    "active_loadout",
)


def contract_identity(contract: dict[str, Any]) -> str:
    encoded = json.dumps(contract, sort_keys=True, separators=(",", ":")).encode()
    return hashlib.sha256(encoded).hexdigest()


def load_compiled_contract(executable_path: str | Path) -> dict[str, Any]:
    executable = Path(executable_path).resolve()
    if not executable.is_file():
        raise ContractError(
            f"Fight Caves executable is unavailable: {executable}; "
            "run ./build.sh fight_caves --cpu"
        )
    try:
        result = subprocess.run(
            [str(executable), "--contract"], cwd=REPO_ROOT,
            capture_output=True, text=True, timeout=10, check=False,
        )
    except (OSError, subprocess.TimeoutExpired, UnicodeError) as exc:
        raise ContractError(f"cannot inspect Fight Caves executable {executable}: {exc}") from exc
    if result.returncode != 0:
        raise ContractError(
            f"Fight Caves executable contract command failed ({result.returncode}): "
            f"{executable}\n{result.stderr.strip()}"
        )
    try:
        contract = json.loads(result.stdout)
    except json.JSONDecodeError as exc:
        raise ContractError("compiled Fight Caves contract is invalid JSON") from exc
    if not isinstance(contract, dict):
        raise ContractError("compiled Fight Caves contract is not an object")
    return contract


def validate_compiled_contract(
    contract: dict[str, Any], expected_active_loadout: str | None = None
) -> None:
    missing = [field for field in REQUIRED_FIELDS if field not in contract]
    if missing:
        raise ContractError(f"compiled contract omits {missing[0]}")
    schema = contract["contract_dump_schema_version"]
    if type(schema) is not int or schema != 1:
        raise ContractError("unsupported compiled contract schema")

    policy_obs_size = contract["policy_obs_size"]
    puffer_obs_size = contract["puffer_obs_size"]
    mask_size = contract["puffer_mask_size"]
    action_dims = contract["puffer_action_dims"]
    if not all(type(value) is int and value > 0 for value in (
        policy_obs_size, puffer_obs_size, mask_size
    )):
        raise ContractError("compiled contract contains invalid observation sizes")
    if (
        not isinstance(action_dims, list)
        or not action_dims
        or not all(type(value) is int and value > 0 for value in action_dims)
    ):
        raise ContractError("compiled contract contains invalid action dimensions")
    if sum(action_dims) != mask_size:
        raise ContractError("compiled action dimensions do not match mask size")
    if policy_obs_size + mask_size != puffer_obs_size:
        raise ContractError("compiled policy observation and mask sizes do not add up")
    if expected_active_loadout is not None:
        actual = contract["active_loadout"]
        if actual != expected_active_loadout:
            raise ContractError(
                "compiled active loadout mismatch: "
                f"expected={expected_active_loadout!r}, actual={actual!r}"
            )


def contract_main() -> int:
    parser = argparse.ArgumentParser(
        description="Inspect the compiled Fight Caves CPU executable. "
                    "This is not a checkpoint or CUDA trainer compatibility check."
    )
    parser.add_argument("--executable", type=Path, default=REPO_ROOT / "fight_caves")
    parser.add_argument("--active-loadout", help="optional expected compiled loadout")
    args = parser.parse_args()
    try:
        executable = args.executable.resolve()
        contract = load_compiled_contract(executable)
        validate_compiled_contract(contract, args.active_loadout)
        print(json.dumps({
            "contract": contract,
            "contract_identity": contract_identity(contract),
            "executable_path": str(executable),
            "executable_sha256": sha256_file(executable),
        }, sort_keys=True, indent=2))
    except (ContractError, OSError) as exc:
        print(f"Fight Caves contract check failed: {exc}", file=sys.stderr)
        return 1
    return 0


def main() -> int:
    commands = {"setup": setup_main, "bundle": bundle_main,
                "preflight": preflight_main, "contract": contract_main}
    if len(sys.argv) < 2 or sys.argv[1] in ("-h", "--help"):
        print("Usage: python3 ocean/fight_caves/tools.py "
              "{setup,bundle,preflight,contract} [options]\n"
              "Use COMMAND --help for command options.")
        return 0 if len(sys.argv) > 1 else 2
    command = sys.argv.pop(1)
    if command not in commands:
        print(f"Unknown Fight Caves command: {command}", file=sys.stderr)
        return 2
    return commands[command]() or 0


if __name__ == "__main__":
    raise SystemExit(main())
