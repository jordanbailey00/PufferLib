from __future__ import annotations

import configparser
import hashlib
import importlib.util
from io import BytesIO
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tarfile

import pytest


REPO_ROOT = Path(__file__).resolve().parents[1]
SETUP_DATA = REPO_ROOT / "ocean" / "fight_caves" / "tools.py"


def load_setup_data():
    spec = importlib.util.spec_from_file_location("fight_caves_setup_data_test", SETUP_DATA)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def load_e2e():
    spec = importlib.util.spec_from_file_location("fight_caves_e2e", REPO_ROOT / "tests/fight_caves_e2e.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def test_e2e_requires_disposable_checkout(tmp_path):
    e2e = load_e2e()
    (tmp_path / "build").mkdir()
    with pytest.raises(RuntimeError, match="use clean-clone"):
        e2e.Acceptance(tmp_path).execute()
    assert not (tmp_path / "build/fight-caves-e2e").exists()


def test_e2e_rejects_dirty_source_and_wrong_overlay(monkeypatch, tmp_path):
    e2e = load_e2e()
    monkeypatch.setattr(e2e, "git", lambda *args: b" M source.c\n")
    with pytest.raises(RuntimeError, match="source is dirty"):
        e2e.prepare_clone(str(tmp_path), "HEAD", False)
    with pytest.raises(RuntimeError, match="only supports this repository at HEAD"):
        e2e.prepare_clone(str(tmp_path), "HEAD", True)


def test_e2e_expected_failure_needs_nonzero_and_explanation(tmp_path):
    e2e = load_e2e()
    test = e2e.Acceptance(tmp_path)
    test.output.mkdir(parents=True)
    test.run("expected", sys.executable, "-c", "raise SystemExit('reason')", expected="reason")
    with pytest.raises(RuntimeError, match="failed acceptance"):
        test.run("unexpected-pass", sys.executable, "-c", "print('reason')", expected="reason")
    with pytest.raises(RuntimeError, match="failed acceptance"):
        test.run("unexplained", sys.executable, "-c", "raise SystemExit(1)", expected="reason")


@pytest.mark.parametrize("fault", [None, "nan", "missing", "steps", "length", "alias"])
def test_e2e_checks_saved_metric_histories(tmp_path, fault):
    e2e = load_e2e()
    metrics = {f"env/{key}": "0,0" for key in e2e.ENV_METRICS}
    metrics.update({"env/perf": "0,0", "env/score": "0,0", "agent_steps": "1024,2048"})
    if fault == "nan":
        metrics["env/wave_reached"] = "0,nan"
    elif fault == "missing":
        del metrics["env/wave_reached"]
    elif fault == "steps":
        metrics["agent_steps"] = "1024,1024"
    elif fault == "length":
        metrics["env/wave_reached"] = "0"
    elif fault == "alias":
        metrics["env/perf"] = "1,1"
    path = tmp_path / "run.ini"
    path.write_text("[metrics]\n" + "\n".join(f"{key}={value}" for key, value in metrics.items()))
    if fault:
        with pytest.raises(RuntimeError):
            e2e.read_run(path, 2048)
    else:
        assert e2e.read_run(path, 2048)[1]["agent_steps"][-1] == 2048


def make_archive(path: Path, members: dict[str, bytes]) -> bytes:
    with tarfile.open(path, "w:gz") as archive:
        for name, contents in members.items():
            info = tarfile.TarInfo(name)
            info.size = len(contents)
            info.mode = 0o644
            archive.addfile(info, BytesIO(contents))
    return path.read_bytes()


def bundle_for(archive: Path, archive_data: bytes, payload: bytes) -> dict:
    return {
        "archive": archive.name,
        "url": archive.as_uri(),
        "size_bytes": len(archive_data),
        "sha256": sha256(archive_data),
        "install_prefix": "runtime",
        "files": [
            {
                "path": "runtime/test.map",
                "size_bytes": len(payload),
                "sha256": sha256(payload),
            }
        ],
    }


def test_install_bundle_is_verified_and_transactional(tmp_path, monkeypatch):
    setup_data = load_setup_data()
    install_root = tmp_path / "resources"
    monkeypatch.setattr(setup_data, "RESOURCE_ROOT", install_root)

    payload = b"authoritative arena data"
    archive = tmp_path / "bundle.tar.gz"
    archive_data = make_archive(archive, {"runtime/test.map": payload})
    bundle = bundle_for(archive, archive_data, payload)

    setup_data.install_bundle("core", bundle, force=False)
    installed = install_root / "runtime" / "test.map"
    assert installed.read_bytes() == payload
    assert setup_data.verify_tree(install_root, bundle, exact=True) == []

    installed.write_bytes(b"corrupt")
    assert "wrong size" in setup_data.verify_tree(install_root, bundle, exact=True)[0]


def test_bad_archive_checksum_does_not_replace_existing_assets(tmp_path, monkeypatch):
    setup_data = load_setup_data()
    install_root = tmp_path / "resources"
    existing = install_root / "runtime" / "test.map"
    existing.parent.mkdir(parents=True)
    existing.write_bytes(b"existing valid installation")
    monkeypatch.setattr(setup_data, "RESOURCE_ROOT", install_root)

    payload = b"replacement"
    archive = tmp_path / "bundle.tar.gz"
    archive_data = make_archive(archive, {"runtime/test.map": payload})
    bundle = bundle_for(archive, archive_data, payload)
    bundle["sha256"] = "0" * 64

    with pytest.raises(setup_data.AssetError, match="checksum mismatch"):
        setup_data.install_bundle("core", bundle, force=True)
    assert existing.read_bytes() == b"existing valid installation"


def test_unsafe_archive_path_is_rejected_without_partial_install(tmp_path, monkeypatch):
    setup_data = load_setup_data()
    install_root = tmp_path / "resources"
    monkeypatch.setattr(setup_data, "RESOURCE_ROOT", install_root)

    payload = b"map"
    archive = tmp_path / "bundle.tar.gz"
    archive_data = make_archive(
        archive,
        {"runtime/test.map": payload, "../outside": b"must not escape"},
    )
    bundle = bundle_for(archive, archive_data, payload)

    with pytest.raises(setup_data.AssetError, match="unsafe path"):
        setup_data.install_bundle("core", bundle, force=True)
    assert not (install_root / "runtime").exists()
    assert not (tmp_path / "outside").exists()


def test_manifest_rejects_unsafe_and_duplicate_file_paths(tmp_path):
    setup_data = load_setup_data()
    base = {
        "size_bytes": 1,
        "sha256": "a" * 64,
    }
    with pytest.raises(setup_data.AssetError, match="unsafe path"):
        setup_data.expected_files({"files": [{"path": "../bad", **base}]})
    with pytest.raises(setup_data.AssetError, match="duplicate file"):
        setup_data.expected_files(
            {"files": [{"path": "runtime/a", **base}, {"path": "runtime/a", **base}]}
        )


def test_download_failure_is_actionable(tmp_path):
    setup_data = load_setup_data()
    missing = (tmp_path / "does-not-exist.tar.gz").as_uri()
    with pytest.raises(setup_data.AssetError, match="download failed"):
        setup_data.download(missing, tmp_path / "download")


def test_release_bundle_is_reproducible_and_installs(tmp_path, monkeypatch):
    tools = load_setup_data()
    source = tmp_path / "test.map"
    source.write_bytes(b"bundle fixture")
    files = [tools.BundleFile(source, "runtime/test.map")]
    first, second = tmp_path / "a.tar.gz", tmp_path / "b.tar.gz"
    tools.write_archive(first, files)
    tools.write_archive(second, files)
    assert first.read_bytes() == second.read_bytes()
    bundle = tools.bundle_manifest(first, files, "example/repo", "test-v1", "runtime")
    assert bundle["url"] == "https://github.com/example/repo/releases/download/test-v1/a.tar.gz"
    bundle["url"] = first.as_uri()
    root = tmp_path / "installed"
    monkeypatch.setattr(tools, "RESOURCE_ROOT", root)
    tools.install_bundle("core", bundle, force=False)
    assert tools.verify_tree(root, bundle, exact=True) == []
    assert (root / "runtime/test.map").read_bytes() == source.read_bytes()


@pytest.mark.parametrize("mode, missing", [("core", "clang"), ("cpu", "clang"), ("native", "ccache")])
def test_preflight_reports_missing_commands_instead_of_continuing(mode, missing):
    preflight = REPO_ROOT / "ocean" / "fight_caves" / "tools.py"
    environment = os.environ.copy()
    environment["PATH"] = ""
    environment.pop("CC", None)
    environment.pop("CXX", None)
    result = subprocess.run(
        [sys.executable, str(preflight), "preflight", "--mode", mode],
        cwd=REPO_ROOT,
        env=environment,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    assert result.returncode != 0
    assert f"required command '{missing}' is unavailable" in result.stderr

@pytest.mark.parametrize("command", ["build-viewer", "play", "eval"])
def test_retired_python_launch_commands_are_rejected(command):
    result = subprocess.run(
        [sys.executable, str(SETUP_DATA), command], cwd=REPO_ROOT,
        capture_output=True, text=True,
    )
    assert result.returncode == 2
    assert f"Unknown Fight Caves command: {command}" in result.stderr


def test_standard_build_prepares_both_asset_bundles():
    build = (REPO_ROOT / "build.sh").read_text()
    branch = build.split('elif [ "$ENV" = "fight_caves" ]; then', 1)[1].split("elif ", 1)[0]
    assert 'tools.py" setup --all' in branch
    assert 'osrs_*|nethack|fight_caves)' in build
    assert '-c "$SRC_DIR/binding.c" -o build/fight_caves_binding.o' in build


@pytest.mark.parametrize("broken", [False, True])
def test_asset_setup_verification_is_independent_of_display(monkeypatch, broken, capsys):
    tools = load_setup_data()
    monkeypatch.delenv("DISPLAY", raising=False)
    monkeypatch.setattr(sys, "argv", [str(SETUP_DATA), "--all", "--verify-only"])
    monkeypatch.setattr(tools, "load_manifest",
                        lambda _: {"bundles": {"core": {}, "viewer": {}}})
    checked = []
    def verify(root, bundle, *, exact):
        checked.append(bundle)
        return ["missing viewer asset"] if broken else []
    monkeypatch.setattr(tools, "verify_tree", verify)
    assert tools.setup_main() == int(broken)
    assert len(checked) == (1 if broken else 2)
    if broken:
        assert "Fight Caves asset setup failed" in capsys.readouterr().err


@pytest.fixture
def compiled_contract():
    return {
        "contract_dump_schema_version": 1,
        "policy_obs_size": 286, "puffer_obs_size": 320,
        "puffer_action_dims": [17, 9, 8], "puffer_mask_size": 34,
        "observation_version": "test_obs", "action_version": "test_actions",
        "reward_version": "test_rewards", "prayer_timing_version": "test_prayer",
        "state_hash_version": 6, "active_loadout": "test_loadout",
    }


def test_executable_contract_uses_subprocess(tmp_path, monkeypatch, compiled_contract):
    tools = load_setup_data()
    executable = tmp_path / "fight_caves"
    executable.touch()
    def run(argv, **kwargs):
        assert argv == [str(executable), "--contract"]
        assert kwargs["cwd"] == REPO_ROOT
        assert kwargs["timeout"] == 10
        return subprocess.CompletedProcess(argv, 0, json.dumps(compiled_contract), "")
    monkeypatch.setattr(tools.subprocess, "run", run)
    contract = tools.load_compiled_contract(executable)
    tools.validate_compiled_contract(contract, "test_loadout")
    assert contract == compiled_contract


@pytest.mark.parametrize("output,message", [
    ("", "invalid JSON"), ("noise\\n{}", "invalid JSON"), ("[]", "not an object"),
])
def test_executable_contract_rejects_bad_output(tmp_path, monkeypatch, output, message):
    tools = load_setup_data()
    executable = tmp_path / "fight_caves"
    executable.touch()
    monkeypatch.setattr(tools.subprocess, "run", lambda *a, **k:
                        subprocess.CompletedProcess(a, 0, output, ""))
    with pytest.raises(tools.ContractError, match=message):
        tools.load_compiled_contract(executable)


@pytest.mark.parametrize("failure", ["missing", "permission", "timeout", "exit"])
def test_executable_contract_reports_failures(tmp_path, monkeypatch, failure):
    tools = load_setup_data()
    executable = tmp_path / "fight_caves"
    if failure != "missing":
        executable.touch()
    def run(*args, **kwargs):
        if failure == "permission":
            raise PermissionError("not executable")
        if failure == "timeout":
            raise subprocess.TimeoutExpired(args, 10)
        return subprocess.CompletedProcess(args, 2, "", "unsupported --contract")
    monkeypatch.setattr(tools.subprocess, "run", run)
    with pytest.raises(tools.ContractError):
        tools.load_compiled_contract(executable)


@pytest.mark.parametrize("field,value", [
    ("contract_dump_schema_version", 2), ("contract_dump_schema_version", True),
    ("policy_obs_size", 0), ("puffer_obs_size", 319), ("puffer_mask_size", -1),
    ("policy_obs_size", True), ("puffer_action_dims", []),
    ("puffer_action_dims", [17, 9, True]), ("puffer_action_dims", [17, 9, 9]),
])
def test_compiled_contract_rejects_invalid_dimensions(compiled_contract, field, value):
    tools = load_setup_data()
    compiled_contract[field] = value
    with pytest.raises(tools.ContractError):
        tools.validate_compiled_contract(compiled_contract)


def test_compiled_contract_checks_required_fields_and_loadout(compiled_contract):
    tools = load_setup_data()
    with pytest.raises(tools.ContractError, match="active loadout mismatch"):
        tools.validate_compiled_contract(compiled_contract, "another_loadout")
    for field in tools.REQUIRED_FIELDS:
        incomplete = dict(compiled_contract)
        del incomplete[field]
        with pytest.raises(tools.ContractError, match="omits"):
            tools.validate_compiled_contract(incomplete)


def test_contract_identity_is_exact_not_a_legacy_migration(compiled_contract):
    tools = load_setup_data()
    same = dict(reversed(list(compiled_contract.items())))
    assert tools.contract_identity(same) == tools.contract_identity(compiled_contract)
    for field in compiled_contract:
        changed = dict(compiled_contract, **{field: "changed"})
        assert tools.contract_identity(changed) != tools.contract_identity(compiled_contract)


def test_native_preflight_checks_both_bundles_without_python_packages(monkeypatch):
    tools = load_setup_data()
    monkeypatch.setenv("CUDA_HOME", "/example/cuda")
    checks = []
    monkeypatch.setattr(tools, "verify_assets", lambda errors, names: checks.append(names))
    monkeypatch.setattr(tools, "require_command",
                        lambda errors, name, purpose: checks.append(name))
    monkeypatch.setattr(tools, "check_openmp", lambda *args: None)
    monkeypatch.setattr(tools, "check_linux_viewer_link", lambda *args: None)
    assert tools.run_preflight("native") == 0
    assert ("core", "viewer") in checks
    assert "/example/cuda/bin/nvcc" in checks
    assert "ccache" in checks
    assert not {"python", "g++", "nvidia-smi"} & {c for c in checks if isinstance(c, str)}


def test_cpu_preflight_checks_viewer_assets_but_not_cuda(monkeypatch):
    tools = load_setup_data()
    checks = []
    monkeypatch.setattr(tools, "verify_assets", lambda errors, names: checks.append(names))
    monkeypatch.setattr(tools, "require_command",
                        lambda errors, name, purpose: checks.append(name))
    monkeypatch.setattr(tools, "check_openmp", lambda *args: None)
    monkeypatch.setattr(tools, "check_linux_viewer_link", lambda *args: None)
    assert tools.run_preflight("cpu") == 0
    assert ("core", "viewer") in checks
    assert "ccache" not in checks
    assert not any("nvcc" in c for c in checks if isinstance(c, str))


@pytest.mark.parametrize("mode", ["cuda", "web"])
def test_retired_preflight_modes_are_rejected(mode):
    result = subprocess.run(
        [sys.executable, str(SETUP_DATA), "preflight", "--mode", mode],
        cwd=REPO_ROOT, capture_output=True, text=True,
    )
    assert result.returncode == 2
    assert "invalid choice" in result.stderr


ENV_ROOT = REPO_ROOT / "ocean" / "fight_caves"
RESOURCE_ROOT = REPO_ROOT / "resources" / "fight_caves"

def test_config_contains_only_supported_native_settings():
    default = configparser.ConfigParser(interpolation=None)
    default.read(REPO_ROOT / "config" / "default.ini")
    config = configparser.ConfigParser(interpolation=None)
    config.read(REPO_ROOT / "config" / "fight_caves.ini")
    assert set(config.sections()) == {"base", "env", "vec", "train", "policy", "sweep"}
    binding = (ENV_ROOT / "binding.c").read_text()
    env_keys = set(re.findall(r'kwargs,\s*"([^"]+)"', binding))
    assert set(config["env"]) <= env_keys
    for section in config.sections():
        if section == "env":
            continue
        assert set(config[section]) <= set(default[section]), section
    assert not {"reset_state", "beta1", "beta2", "eps", "prio_alpha", "prio_beta0",
                "expansion_factor"} & {key for section in config for key in config[section]}


def test_environment_uses_flat_implementation_headers():
    for name in ("simulation.h", "fight_caves.h", "assets.h", "ui.h", "render.h",
                 "binding.c", "fight_caves.c", "viewer.c", "tools.py", "CMakeLists.txt"):
        assert (ENV_ROOT / name).is_file()
    assert not (ENV_ROOT / "sources.txt").exists()
    assert '#include "simulation.h"' in (ENV_ROOT / "fight_caves.h").read_text()
    assert 'void fc_step(' in (ENV_ROOT / "simulation.h").read_text()


def test_asset_manifest_is_complete_and_pinned():
    manifest = json.loads(
        (RESOURCE_ROOT / "asset_manifest.json").read_text(encoding="utf-8")
    )
    assert manifest["schema_version"] == 1
    assert set(manifest["bundles"]) == {"core", "viewer"}
    for name, bundle in manifest["bundles"].items():
        assert bundle["url"].startswith("https://github.com/")
        assert len(bundle["sha256"]) == 64
        assert bundle["size_bytes"] > 0
        assert bundle["install_prefix"] == ("runtime" if name == "core" else "viewer")
        paths = [entry["path"] for entry in bundle["files"]]
        assert paths
        assert len(paths) == len(set(paths))
        assert all(not Path(path).is_absolute() and ".." not in Path(path).parts for path in paths)
        assert all(len(entry["sha256"]) == 64 and entry["size_bytes"] > 0 for entry in bundle["files"])


def test_asset_manifest_includes_equipment_parts_and_menu_font():
    manifest = json.loads((RESOURCE_ROOT / "asset_manifest.json").read_text())
    files = {entry["path"] for entry in manifest["bundles"]["viewer"]["files"]}
    assert {"viewer/fc_player.parts", "viewer/fc_player.models",
            "viewer/data/fonts/runescape_bold.ttf",
            "viewer/data/sprites/items/item_28310.png"} <= files
    assert "viewer/data/sprites/items/item_25487.png" not in files


def test_fight_caves_sources_do_not_reference_local_development_trees():
    forbidden = ("/home/joe", "/v38/", "pufferlib_4", "runescape-reference")
    roots = (
        ENV_ROOT,
        RESOURCE_ROOT,
        REPO_ROOT / "config" / "fight_caves.ini",
    )
    for root in roots:
        files = [root] if root.is_file() else [p for p in root.rglob("*") if p.is_file()]
        for path in files:
            if path.suffix in {".png", ".bin", ".models", ".atlas", ".anims"}:
                continue
            try:
                text = path.read_text(encoding="utf-8")
            except UnicodeDecodeError:
                continue
            for value in forbidden:
                assert value not in text, f"{path} contains forbidden path marker {value!r}"
