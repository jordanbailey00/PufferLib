"""Explicit compiled-CPU checks: FC_TEST_EXECUTABLE must name a freshly built binary."""
import os
from pathlib import Path
import struct
import subprocess

import pytest


ROOT = Path(__file__).resolve().parents[1]
FLOATS = 8 * (320 + 35) + 2 * 3 * 8 * 8


@pytest.fixture
def executable():
    value = os.environ.get("FC_TEST_EXECUTABLE")
    assert value, "set FC_TEST_EXECUTABLE to the freshly built Fight Caves CPU executable"
    path = Path(value).resolve()
    assert path.is_file()
    return path


def run(executable, *args):
    return subprocess.run(
        [str(executable), *map(str, args)], cwd=ROOT,
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
    result = subprocess.run([str(executable)], cwd=ROOT,
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
