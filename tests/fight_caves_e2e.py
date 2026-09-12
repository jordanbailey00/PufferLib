"""Explicit native 5.0 acceptance test; stdlib only, no dependency installation.

Run via tests/fight_caves.sh. All downloads, builds and faults belong to an
isolated checkout. Keep its logs/checkpoints on success AND failure.
"""
import argparse
import configparser
import hashlib
import json
import math
import os
from pathlib import Path
import resource
import re
import shlex
import shutil
import signal
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]
ENV_METRICS = (
    "zero_progress_ticks", "wave_reached", "wrong_prayer_hits", "reached_wave_63",
    "jad_kill_rate", "prayer_uptime_range", "prayer_uptime_melee",
    "prayer_uptime_magic", "npc_healing_total", "jad_healing_total", "episode_length",
)


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def git(root, *args):
    return subprocess.check_output(["git", "-C", str(root), *args])


def source_digest(root):
    """Exclude only upstream's tracked prebuilt executable, which is rebuilt."""
    paths = git(root, "ls-files", "--cached", "--others", "--exclude-standard", "-z")
    digest = hashlib.sha256()
    for name in sorted(set(paths.split(b"\0")) - {b"", b"puffer"}):
        path = root / os.fsdecode(name)
        if not path.exists() and not path.is_symlink():  # locally deleted source
            continue
        data = os.fsencode(os.readlink(path)) if path.is_symlink() else path.read_bytes()
        digest.update(name + b"\0" + hashlib.sha256(data).digest())
    return digest.hexdigest()


def prepare_clone(source, ref, working_tree):
    local = Path(source).is_dir()
    require(not working_tree or (local and Path(source).resolve() == ROOT and ref == "HEAD"),
            "--working-tree only supports this repository at HEAD")
    if local and not working_tree:
        require(not git(source, "status", "--porcelain").strip(),
                "source is dirty; use --working-tree explicitly to test uncommitted changes")
    folder = Path(tempfile.mkdtemp(prefix="fight-caves-clean-clone-"))
    checkout = folder / "PufferLib"
    print(f"Retained clean-clone workspace: {folder}", flush=True)
    subprocess.run(["git", "clone", "--quiet", "--no-hardlinks", source, str(checkout)],
                   check=True, timeout=300)
    subprocess.run(["git", "-C", str(checkout), "checkout", "--quiet", "--detach", ref],
                   check=True, timeout=60)
    require(not git(checkout, "status", "--porcelain").strip(), "new clone is not clean")
    if working_tree:
        patch = git(ROOT, "diff", "--binary", "HEAD", "--", ".", ":(exclude)puffer")
        if patch:
            subprocess.run(["git", "-C", str(checkout), "apply", "--binary", "-"],
                           input=patch, check=True, timeout=60)
        for name in git(ROOT, "ls-files", "--others", "--exclude-standard", "-z").split(b"\0"):
            if name:
                relative = os.fsdecode(name)
                dest = checkout / relative
                dest.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(ROOT / relative, dest, follow_symlinks=False)
        require(source_digest(checkout) == source_digest(ROOT), "source snapshot differs")
    (folder / "source.json").write_text(json.dumps({
        "source": source, "ref": ref, "head": git(checkout, "rev-parse", "HEAD").decode().strip(),
        "working_tree_applied": working_tree, "source_sha256": source_digest(checkout),
    }, indent=2) + "\n")
    return checkout


def read_run(path, steps):
    ini = configparser.ConfigParser(interpolation=None)
    require(ini.read(path), f"native training log missing: {path}")
    metrics = {key: [float(v) for v in values.split(",")]
               for key, values in ini["metrics"].items()}
    require(all(values and all(math.isfinite(v) for v in values) for values in metrics.values()),
            "native log contains empty/non-finite metrics")
    timeline = metrics["agent_steps"]
    require(timeline == sorted(timeline) and timeline[-1] == steps, "wrong training step history")
    require(all(len(values) == len(timeline) for values in metrics.values()), "unequal histories")
    for name in ENV_METRICS:
        require(f"env/{name}" in metrics, f"missing native episode metric: {name}")
    require(metrics["env/perf"] == metrics["env/score"] == metrics["env/jad_kill_rate"],
            "Jad metric aliases differ")
    return ini, metrics


class Acceptance:
    def __init__(self, root):
        self.root = root
        self.output = root / "build/fight-caves-e2e"
        self.env = {k: v for k, v in os.environ.items() if not k.startswith("FC_")}
        self.report = {"passed": False, "commands": []}

    def run(self, label, *command, expected=None, timeout=180, env=None):
        command = [str(arg) for arg in command]
        log = self.output / f"{label}.log"
        print(f"[e2e] {label}: {shlex.join(command)}", flush=True)
        with log.open("w") as stream:
            proc = subprocess.Popen(command, cwd=self.root, env=env or self.env,
                                    stdout=stream, stderr=subprocess.STDOUT, start_new_session=True)
            try:
                status = proc.wait(timeout=timeout)
            except BaseException:
                os.killpg(proc.pid, signal.SIGKILL)
                proc.wait()
                raise
        text = log.read_text(errors="replace")
        self.report["commands"].append({"label": label, "argv": command, "exit": status})
        ok = status != 0 and expected in text if expected else status == 0
        require(ok, f"{label} failed acceptance; see {log}\n{text[-3000:]}")
        return text

    def execute(self):
        # Refuse to mutate an existing development installation or reuse its caches.
        cached = [self.root / p for p in ("build", ".venv", "fight_caves",
                  "resources/fight_caves/runtime", "resources/fight_caves/viewer")]
        cached += list(self.root.glob("raylib*"))
        require(not any(p.exists() for p in cached),
                "checkout must have no builds/assets/venv; use clean-clone, not your development tree")
        self.output.mkdir(parents=True)
        try:
            before = source_digest(self.root)
            self.report["source_sha256"] = before
            self.validate()
            require(source_digest(self.root) == before, "acceptance changed source files")
            self.report["passed"] = True
        except BaseException as exc:
            self.report["error"] = str(exc)
            raise
        finally:
            (self.output / "report.json").write_text(json.dumps(self.report, indent=2) + "\n")
            print(f"[e2e] Retained artifacts: {self.output}", flush=True)

    def validate(self):
        require(sys.platform == "linux", "full native acceptance requires Linux/CUDA (use PufferTank 5.0)")
        for command in ("git", "clang", "cmake", "ccache", "python3", "curl", "tar"):
            require(shutil.which(command), f"missing {command}; use official PufferTank 5.0 dependencies")
        require(self.env.get("DISPLAY"), "DISPLAY is required; provide X11 access or explicitly use xvfb-run")
        # No core dumps for deliberate native assertion failures in this test process.
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
        tool = [sys.executable, "-S", "ocean/fight_caves/tools.py"]
        self.run("missing-assets", *tool, "preflight", "--mode", "core",
                 expected="Install assets with:")
        self.run("build-native", "./build.sh", "fight_caves", timeout=1800)
        self.run("build-cpu", "./build.sh", "fight_caves", "--cpu", timeout=600)
        self.run("assets-verified", *tool, "setup", "--all", "--verify-only")
        self.run("native-preflight", *tool, "preflight", "--mode", "native")
        self.run("contract", *tool, "contract")
        # Test the real window; do not require the optional xdpyinfo diagnostic.
        # Raylib TakeScreenshot writes a basename in the current directory.
        self.run("manual-play", "./fight_caves", "--screenshot", "playable.png")
        screenshot = self.root / "playable.png"
        require(screenshot.is_file() and screenshot.read_bytes().startswith(b"\x89PNG\r\n\x1a\n"),
                "manual viewer did not save its screenshot")
        screenshot.rename(self.output / screenshot.name)
        self.run("core-build", "clang", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                 "-Iocean/fight_caves", "tests/fight_caves.c", "-lm", "-o", self.output / "core")
        self.run("core", self.output / "core")
        cmake = self.output / "cmake"
        self.run("cmake", "cmake", "-S", "ocean/fight_caves", "-B", cmake, "-DCMAKE_BUILD_TYPE=Release")
        self.run("test-build", "cmake", "--build", cmake, "--target", "fc_integration_tests",
                 "fc_cpu_tests", "fc_viewer_tests", "--parallel", "2", timeout=600)
        for name in ("fc_integration_tests", "fc_cpu_tests", "fc_viewer_tests"):
            self.run(name, cmake / name)
        (self.root / "equipment-appearance.png").rename(self.output / "equipment-appearance.png")
        self.run("render-integration", cmake / "fc_integration_tests", "--render")

        for name in ("collision", "movement", "los"):
            asset = self.root / f"resources/fight_caves/runtime/fightcaves.{name}"
            saved = self.output / asset.name
            asset.rename(saved)
            try:
                self.run(f"missing-{name}", "./fight_caves", "--benchmark",
                         expected=f"required Fight Caves arena asset 'fightcaves.{name}' is missing")
            finally:
                saved.rename(asset)
        asset = self.root / "resources/fight_caves/viewer/fightcaves.minimap.png"
        saved = self.output / asset.name
        asset.rename(saved)
        try:
            self.run("missing-viewer-asset", "./fight_caves", "--screenshot", "unused.png",
                     expected="viewer startup aborted instead of using reduced graphics")
        finally:
            saved.rename(asset)
        missing_cc = dict(self.env, CC="fight-caves-intentionally-missing-compiler")
        self.run("missing-compiler", *tool, "preflight", "--mode", "core", env=missing_cc,
                 expected="required command 'fight-caves-intentionally-missing-compiler' is unavailable")
        self.run("missing-display", "./fight_caves", env=dict(self.env, DISPLAY=""),
                 expected="no graphical DISPLAY is configured")

        config = configparser.ConfigParser(interpolation=None)
        config.read([self.root / "config/default.ini", self.root / "config/fight_caves.ini"])
        batch = int(config["vec"]["total_agents"]) * int(config["train"]["horizon"])
        steps = batch * 4
        run_id = "fc5-clean-clone"
        checkpoints, logs = self.output / "checkpoints", self.output / "logs"
        self.run("training", "./puffer", "train", f"--train.total_timesteps={steps}",
                 f"--base.run_id={run_id}", "--base.checkpoint_interval=2", "--base.eval_episodes=0",
                 f"--base.checkpoint_dir={checkpoints}", f"--base.log_dir={logs}", timeout=600)
        ini, metrics = read_run(logs / "fight_caves" / f"{run_id}.ini", steps)
        for section in ("env", "vec", "train", "policy"):
            for key, value in config[section].items():
                if (section, key) != ("train", "total_timesteps"):
                    require(float(ini[section][key]) == float(value), f"training changed {section}.{key}")
        folder = checkpoints / "fight_caves" / run_id
        expected = [folder / f"{n * batch:016d}.bin" for n in (2, 4)]
        require(sorted(folder.iterdir()) == expected, "expected two complete checkpoints, no partial files")
        for index, path in enumerate(expected):
            self.run(f"checkpoint-{index}", "./fight_caves", "check", path)
        path = expected[-1]
        cpu = self.run("cpu-replay", "./fight_caves", "eval", path, "--headless", "--base.eval_episodes=2")
        require("CPU_EVAL env=fight_caves" in cpu and "games=2 " in cpu, "CPU replay did not complete two episodes")
        native = ["./puffer", "eval", str(path), "--headless", "--base.eval_episodes=2", "--base.eval_agents=64"]
        for label, command in (
            ("gpu-replay", native),
            ("gpu-latest", ["./puffer", "eval", "latest", "--headless", "--base.eval_episodes=2",
                            "--base.eval_agents=64", f"--base.checkpoint_dir={checkpoints}"]),
        ):
            text = self.run(label, *command)
            match = re.search(r"CUDA_EVAL env=fight_caves .*games=(\d+)", text)
            require(match and int(match[1]) >= 2, f"{label} did not complete evaluation")
        invalid = self.output / "invalid.bin"
        invalid.write_bytes(b"\0" * 64)
        for label, bad in (("missing", self.output / "not-a-checkpoint.bin"), ("short", invalid)):
            self.run(f"cpu-{label}", "./fight_caves", "eval", bad, expected="checkpoint rejected:")
            self.run(f"gpu-{label}", *native[:2], bad, *native[3:],
                     expected="failed to open weights" if label == "missing" else "failed to read weights")
        self.run("final-asset-verification", *tool, "setup", "--all", "--verify-only")
        self.report.update(steps=steps, history_points=len(metrics["agent_steps"]),
                           checkpoints={p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in expected})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="mode", required=True)
    commands.add_parser("checkout", help="validate a fresh disposable checkout in place")
    clone = commands.add_parser("clean-clone", help="clone, then validate without touching the source")
    clone.add_argument("--source", default=str(ROOT), help="local repository or Git URL")
    clone.add_argument("--ref", default="HEAD", help="commit or branch to validate")
    clone.add_argument("--working-tree", action="store_true", help="explicitly include local uncommitted sources")
    clone.add_argument("--prepare-only", action="store_true", help="prepare clone for testing inside PufferTank")
    args = parser.parse_args()
    try:
        if args.mode == "clean-clone":
            checkout = prepare_clone(args.source, args.ref, args.working_tree)
            if args.prepare_only:
                print(f"Prepared source checkout: {checkout}")
                return 0
            subprocess.run([sys.executable, "-S", str(checkout / "tests/fight_caves_e2e.py"), "checkout"],
                           check=True)
        else:
            Acceptance(ROOT).execute()
        print("Fight Caves native clean-checkout acceptance passed.")
        return 0
    except (OSError, RuntimeError, ValueError, KeyError, subprocess.SubprocessError) as exc:
        print(f"Fight Caves acceptance failed: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
