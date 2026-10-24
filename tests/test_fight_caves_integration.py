"""Fight Caves maintenance suite: assets, native C/C++ and CPU replay.

Run python3 -m pytest -q tests/test_fight_caves_integration.py.
Set FC_TEST_RENDER=1 to require the graphical checks too (DISPLAY or xvfb-run).
Native prerequisites are required, never silently skipped. GPU training is
reserved for the explicit disposable-checkout acceptance runner.
"""
from __future__ import annotations

import configparser
import hashlib
import importlib.util
from io import BytesIO
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tarfile

import pytest


REPO_ROOT = Path(__file__).resolve().parents[1]
SETUP_DATA = REPO_ROOT / "ocean" / "fight_caves" / "tools.py"


def native_command(label, *args, timeout=180, env=None):
    result = subprocess.run(list(map(str, args)), cwd=REPO_ROOT,
                            capture_output=True, text=True, timeout=timeout, env=env)
    assert result.returncode == 0, f"{label}:\n{result.stdout}\n{result.stderr}"
    return result.stdout


@pytest.fixture(scope="module")
def executable(tmp_path_factory):
    path = tmp_path_factory.mktemp("fight-caves-cpu") / "fight_caves"
    native_command("CPU build", "./build.sh", "fight_caves", path, "--cpu", timeout=600)
    return path


@pytest.fixture(scope="module")
def native_suites(executable, tmp_path_factory):
    output = tmp_path_factory.mktemp("fight-caves-native")
    return load_e2e().build_native_tests(REPO_ROOT, output, native_command)


@pytest.mark.parametrize("suite", ["simulation", "linkage"])
def test_native_suite(native_suites, suite):
    native_command(suite, native_suites[suite])


@pytest.mark.skipif(os.environ.get("FC_TEST_RENDER") != "1",
                    reason="Set FC_TEST_RENDER=1 to require graphical validation")
def test_graphical_suite(native_suites, tmp_path):
    prefix = ["xvfb-run", "-a"] if shutil.which("xvfb-run") else []
    assert prefix or os.environ.get("DISPLAY"), "Graphical tests require DISPLAY or xvfb-run"
    screenshot = tmp_path / "equipment-appearance.png"
    env = dict(os.environ, FC_TEST_OUTPUT=str(screenshot))
    native_command("graphics", *prefix, native_suites["simulation"], "--render", env=env)
    assert screenshot.read_bytes().startswith(b"\x89PNG\r\n\x1a\n")


def test_built_executable_contract(executable):
    tools = load_setup_data()
    contract = tools.load_compiled_contract(executable)
    tools.validate_compiled_contract(contract)
    assert (contract["policy_obs_size"], contract["puffer_obs_size"],
            contract["puffer_mask_size"], contract["puffer_action_dims"]) == (286, 320, 34, [17, 9, 8])


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

    installed.write_bytes(b"x" * len(payload))
    assert "checksum mismatch" in setup_data.verify_tree(install_root, bundle, exact=True)[0]
    setup_data.install_bundle("core", bundle, force=False)
    assert installed.read_bytes() == payload
    monkeypatch.setattr(setup_data, "download", lambda *args: pytest.fail("valid assets must work offline"))
    setup_data.install_bundle("core", bundle, force=False)


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


def test_contract_identity_is_exact(compiled_contract):
    tools = load_setup_data()
    same = dict(reversed(list(compiled_contract.items())))
    assert tools.contract_identity(same) == tools.contract_identity(compiled_contract)
    for field in compiled_contract:
        changed = dict(compiled_contract, **{field: "changed"})
        assert tools.contract_identity(changed) != tools.contract_identity(compiled_contract)


ENV_ROOT = REPO_ROOT / "ocean" / "fight_caves"
RESOURCE_ROOT = REPO_ROOT / "resources" / "fight_caves"

def test_config_contains_only_supported_native_settings():
    default = configparser.ConfigParser(interpolation=None)
    default.read(REPO_ROOT / "config" / "default.ini")
    config = configparser.ConfigParser(interpolation=None)
    config.read(REPO_ROOT / "config" / "fight_caves.ini")
    assert set(config.sections()) == {"base", "env", "vec", "train", "policy", "sweep"}
    binding = (ENV_ROOT / "binding.c").read_text()
    core = (ENV_ROOT / "core.h").read_text()
    env_keys = set(re.findall(r'kwargs,\s*"([^"]+)"', binding))
    reward_keys = set(re.findall(r'\{"(\w+)",\s*offsetof\(FcRewardParams,', core))
    assert set(config["env"]) <= env_keys | reward_keys
    for section in config.sections():
        if section == "env":
            continue
        assert set(config[section]) <= set(default[section]), section
    assert not {"reset_state", "beta1", "beta2", "eps", "prio_alpha", "prio_beta0",
                "expansion_factor"} & {key for section in config for key in config[section]}


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


def test_fight_caves_sources_do_not_reference_local_paths():
    forbidden = ("/home/", "/Users/")
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


FLOATS = 8 * (320 + 35) + 2 * 3 * 8 * 8

def run(executable, *args):
    return subprocess.run(
        [str(executable), *map(str, args)], cwd=REPO_ROOT,
        capture_output=True, text=True, timeout=30,
    )


def check(executable, path, *args):
    return run(executable, "check", path, "--policy.hidden_size=8",
               "--policy.num_layers=2", *args)


def test_valid_cpu_checkpoint_and_two_episodes(executable, tmp_path):
    path = tmp_path / "valid.bin"
    path.write_bytes(b"\0" * (FLOATS * 4))
    result = check(executable, path)
    assert result.returncode == 0, result.stderr
    assert f"file_floats={FLOATS}" in result.stdout
    result = run(executable, "eval", path, "--headless", "--policy.hidden_size=8",
                 "--policy.num_layers=2", "--base.eval_episodes=2")
    assert result.returncode == 0, result.stderr
    assert "CPU_EVAL env=fight_caves" in result.stdout
    assert "games=2 " in result.stdout


def test_manual_viewer_without_display_exits_cleanly(executable):
    result = subprocess.run([str(executable)], cwd=REPO_ROOT,
                            env=dict(os.environ, DISPLAY=""),
                            capture_output=True, text=True, timeout=10)
    assert result.returncode == 1, result.stderr
    assert "no graphical DISPLAY is configured" in result.stderr
    assert "Initializing raylib" not in result.stdout


@pytest.mark.parametrize("kind", ["missing", "short", "long", "zip", "nan", "inf", "shape"])
def test_invalid_checkpoints_fail_without_opening_viewer(executable, tmp_path, kind):
    path = tmp_path / "invalid.bin"
    data = b"\0" * (FLOATS * 4)
    if kind == "short":
        data = data[:-1]
    elif kind == "long":
        data += b"\0"
    elif kind == "zip":
        data = b"PK\x03\x04pytorch"
    elif kind in ("nan", "inf"):
        data = struct.pack("f", float(kind)) + data[4:]
    elif kind == "shape":
        data = data * 2
    if kind != "missing":
        path.write_bytes(data)
    for mode in ("check", "eval"):
        result = run(executable, mode, path, "--policy.hidden_size=8", "--policy.num_layers=2")
        assert result.returncode != 0
        assert "checkpoint rejected:" in result.stderr
        assert "Fight Caves Viewer" not in result.stdout + result.stderr


@pytest.mark.parametrize("args", [
    [], ["latest"], ["missing.bin", "--policy.hidden_size=7"],
    ["missing.bin", "--policy.hidden_size=999999999999"],
    ["missing.bin", "--policy.hidden_size=8.5"],
    ["missing.bin", "--policy.num_layers=0"],
    ["missing.bin", "--headless", "--base.eval_episodes=0"],
    ["missing.bin", "--base.seed=-1"], ["missing.bin", "--wandb"],
])
def test_invalid_cpu_replay_requests_fail(executable, args):
    result = run(executable, "eval", *args)
    assert result.returncode != 0
    assert result.stderr
    assert "CPU_EVAL" not in result.stdout
