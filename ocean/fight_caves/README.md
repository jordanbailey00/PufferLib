# Fight Caves

A native C Fight Caves environment for PufferLib 5.0, with all 63 waves,
a playable Raylib viewer, and native checkpoint replay. Training and the viewer
share the same simulation.

Build, training, checkpoint replay and clean-checkout setup have been tested in
official PufferTank 5.0. Full-length learning benchmarks for 5.0 are still pending.

## Build and assets

Use PufferLib 5.0's normal native build dependencies. There is no Fight
Caves-specific Python package, trainer, or Docker image. Asset setup requires
Python 3's standard library and network access on the first build.

From the checkout root:

```bash
./build.sh fight_caves
```

This installs and verifies both asset bundles, compiles the simulation and viewer
as C, and links them into the native `./puffer` executable. Training is headless;
linking the viewer does not open a window. No editable Python installation is
needed for these native entry points.

## Train

```bash
./puffer train
```

The executable has Fight Caves compiled in and reads `config/fight_caves.ini`.
Rebuild when selecting a different environment. Old Python trainer and W&B
commands are not supported by this port.

The configuration uses 750M timesteps, seed 73, 4,096 agents, two buffers,
16 threads, a 256-step horizon, 32,768-step minibatches, and a 512-wide,
three-layer policy. Supported learning/reward settings are retained from the
previous configuration. Native 5.0 uses `momentum`, explicit `vtrace=1`,
episode-boundary recurrent resets (`reset_every_horizon=0`), and
actor/learner pipelining (`async=1`). This is a migrated starting configuration,
not a claim of identical learning results or a newly tuned 5.0 configuration.
Override the budget without editing the INI with, for example,
`./puffer train --train.total_timesteps=100000000`.

The environment reports episode averages for `zero_progress_ticks`,
`wave_reached`, `wrong_prayer_hits`, `reached_wave_63`, `jad_kill_rate`,
`prayer_uptime_range`, `prayer_uptime_melee`, `prayer_uptime_magic`,
`npc_healing_total`, `jad_healing_total`, and `episode_length`.
The native `perf` and `score` metrics both represent Jad completion.
Prayer uptime is a fraction of episode ticks; healing totals count effective HP
restored in simulation units.

## Play manually

```bash
./build.sh fight_caves --cpu
unset __GLX_VENDOR_LIBRARY_NAME
./fight_caves
```

The `unset` command removes a forced GLX-driver override, which can cause
software rendering on some systems. It does not force a GPU brand or fix
missing graphics drivers/display access. Check the startup `Renderer` line:
`llvmpipe` means software rendering. Mesa can also use GPUs.

Manual play does not require CUDA or a trained checkpoint. Add `--debug` to the
CPU build for sanitizers. It produces a separate executable and does not replace
`./puffer`. Use `./fight_caves --start-wave 63` to test a particular wave.

Press Space to start/pause, Right Arrow to advance one tick, O for debug
overlays, and Q to quit. Right-drag rotates the camera; the wheel zooms.
The viewer includes tile clicks and route previews, equipment switching,
inventory/prayer controls, right-click menus, minimap/run-energy controls,
animations, projectiles, impacts, health bars, and hitsplats. The console has
wave/target/TPS selection, god mode, observations, rewards, and an event log.

## Replay a checkpoint

Use the native executable and the architecture/configuration used for training:

```bash
unset __GLX_VENDOR_LIBRARY_NAME
./puffer eval latest
# Or select a specific native 5.0 checkpoint:
./puffer eval /path/to/checkpoint.bin
```

Replay uses Puffer's native policy inference and opens the same full viewer.
Camera, debug, pause, single-step and speed controls remain available.
Gameplay-changing controls are disabled. Keys 1/2/4/0 select 1x/2x/4x/10x;
Q closes evaluation. A display and working graphics drivers are required.

The native trainer's evaluation requires CUDA. CPU-only replay is also available
through the playable executable, using upstream 5.0 C inference:

```bash
./build.sh fight_caves --cpu
unset __GLX_VENDOR_LIBRARY_NAME
./fight_caves eval /path/to/checkpoint.bin
# An exact two-episode CPU evaluation without graphics:
./fight_caves eval /path/to/checkpoint.bin --headless --base.eval_episodes=2
```

CPU replay requires an explicit path, shares the viewer's timing/controls, uses
native action masks, and resets recurrent state between episodes. It is not an
automatic fallback if GPU replay fails. CPU float32 inference need not produce
the same sampled trajectories as CUDA/BF16 inference. Use the training
architecture and environment settings for either replay path; for example,
append `--policy.hidden_size=512 --policy.num_layers=3`.

### Checkpoints and logs

Native training writes `checkpoints/fight_caves/<run_id>/<16-digit-step>.bin`
and, on completion, `logs/fight_caves/<run_id>.ini`. The INI contains
settings plus downsampled `[metrics]` arrays, including the eleven environment
metrics listed above. Paths can be changed with `--base.checkpoint_dir=PATH`
and `--base.log_dir=PATH`; a named run uses `--base.run_id=NAME`.
Use a new run ID for each run to avoid overwriting earlier artifacts.

The configured checkpoint interval is 50 training epochs, and the final epoch
also saves a checkpoint. `latest` selects by filesystem change time, not
Jad kill rate; use an explicit path when comparing particular checkpoints.
Files contain flat float32 policy weights, not optimizer state or embedded
architecture/game-version metadata. Do not mix 4.0 checkpoints into this port.

An optional check, without graphics or CUDA, verifies exact size and finite
weights against the selected architecture:

```bash
./fight_caves check /path/to/checkpoint.bin
```

The configured 512-wide/three-layer policy has 2,541,056 floats (10,164,224 bytes).
The CPU checker/replayer requires a positive, 8-aligned hidden size and positive
layer count. It rejects missing, short, oversized and non-finite files. This is
structural validation, **not proof of a checkpoint's origin or gameplay
compatibility**. Keep the corresponding source revision, configuration and run
log with your checkpoints. The unchanged CUDA loader detects missing/short
files but does not enforce this stricter exact-size/finite-value check; run the
CPU check first when validating an unfamiliar file.

The native log is not a per-update history: the current `sweep.downsample=5`
setting produces five points, and loss channels are not saved there. Completed
episode metrics retain the last available snapshot when a rollout has no new
completed episodes. When final evaluation is enabled, the final metrics include
its results and the saved settings can reflect evaluation overrides (such as
`base.eval_agents`). Keep the launch command when interpreting those logs.
Headless GPU evaluation can finish more episodes than requested because it is
batched. No W&B integration or legacy Python trainer has been added.

## Official PufferTank 5.0

Use [PufferTank's 5.0 branch](https://github.com/PufferAI/PufferTank/tree/5.0),
not the 4.0 image. At validation time the 5.0 image tag was not published; build
the official Dockerfile locally:

```bash
git clone --branch 5.0 https://github.com/PufferAI/PufferTank.git PufferTank-5.0
cd PufferTank-5.0
./docker.sh build -n fight-caves-tank5 -t 5.0
./docker.sh test -n fight-caves-tank5 -t 5.0
```

Inside the container, use a checkout containing Fight Caves and run the same
build/train/play/replay commands above. PufferTank's preinstalled checkout does
not acquire environment PRs automatically. No editable Python installation is
needed. The launcher provides GPU/display access; each new graphical shell may
still need `unset __GLX_VENDOR_LIBRARY_NAME`.

Validated against official PufferTank commit
`63bfbfa5deb9e150d1ab4cfe044040877c22298d` without editing its Dockerfile
or installer: CUDA 13.0, Clang 18, NCCL 2.28.3, Python 3.12 and Raylib 5.5.
A source-only copy built successfully, downloaded both asset bundles, trained,
saved/reloaded checkpoints, and rendered through the GPU. No custom image
recipe, host-built library, Python environment, or Fight Caves-specific
dependency installation was needed. Pytest is only an optional maintainer-test
dependency; it is not required to build, train, play or replay.

## Assets

The first build installs the pinned
[Fight Caves v3 bundles](https://github.com/jordanbailey00/fc-rl/releases/tag/fight-caves-assets-v3):

- `resources/fight_caves/runtime/`: collision, movement, and line-of-sight maps.
- `resources/fight_caves/viewer/`: models, equipment parts, animations, terrain,
  textures, UI sprites/fonts, and the minimap raster.

Archive and individual-file sizes/SHA-256 hashes are checked before installation.
Valid assets are reused offline. Rerunning the build repairs missing or corrupt
bundles; download or verification failure stops the build. Runtime loads local
files only, without another repository or raw OSRS cache. Missing required data
produces an error rather than an open-map or reduced-graphics fallback.

Optional installation or read-only verification:

```bash
python3 ocean/fight_caves/tools.py setup --all
python3 ocean/fight_caves/tools.py setup --all --verify-only
```

OSRS assets are distributed separately and are not covered by PufferLib's
software license; see `resources/fight_caves/ASSET_NOTICE.md`.

## Maintainer checks

`./fight_caves --benchmark` runs a headless random-action smoke test.
The following diagnostics are optional; normal builds still install/verify
assets automatically.

```bash
python3 ocean/fight_caves/tools.py preflight --mode cpu
python3 ocean/fight_caves/tools.py preflight --mode native
./fight_caves --contract
python3 ocean/fight_caves/tools.py contract
```

`cpu` checks the C/OpenMP and graphics build prerequisites; `native` also
checks for NVCC and ccache. Native preflight is a compiler/assets check, not a
replacement for a complete CUDA/NCCL build or GPU runtime test. Neither mode
requires Torch, NumPy, pybind11, a Python Puffer package, or a graphical session.
`viewer-runtime` separately checks installed assets and display access.
That optional diagnostic uses `xdpyinfo` (`x11-utils` on Ubuntu); it is not a
runtime dependency. The full acceptance test checks the actual viewer window
instead, so it works with the official PufferTank dependencies alone.

The contract command needs the CPU executable built above. It reports its
compiled observation/action dimensions, version identifiers and loadout without
loading assets or opening a window. The Python helper adds consistency checks,
an exact metadata fingerprint, and the executable's path/SHA-256. Use
`--executable PATH` to inspect a different Fight Caves CPU build, or
`--active-loadout FC_LOADOUT_SOTA_TBOW` to assert the expected compiled loadout.
This describes **that executable**, not an arbitrary `./puffer` build or
checkpoint. It does not certify old checkpoint compatibility.

With pytest installed, run the explicit maintenance tests:

```bash
bash tests/fight_caves.sh test --core
bash tests/fight_caves.sh test --adapter
bash tests/fight_caves.sh test --all
```

`--core` covers asset/tooling fixtures and core regressions. `--adapter` also
builds the CPU executable, checks its contract and checkpoint failures, and
tests native CPU inference/reset plumbing and the 5.0 adapter.
`--all` additionally needs CMake and a working display (or xvfb-run) for the
graphical regressions. The adapter tests also require CMake. These commands
do not train, and they are not run automatically on ordinary builds.

The old Python backend tests, checkpoint-sidecar compatibility helpers, and
Python clean-clone installer have been removed. Release bundles are still built with
`python3 ocean/fight_caves/tools.py bundle`; see `bundle --help` for its
source/output arguments. No asset republishing is required for this migration.

### Clean-clone acceptance

From a Fight Caves-enabled checkout in official PufferTank 5.0 (or an equivalent
Linux/CUDA environment with display access):

```bash
unset __GLX_VENDOR_LIBRARY_NAME
bash tests/fight_caves.sh clean-clone
```

This clones the local committed revision into a retained temporary directory.
It does not install Python packages or change the source checkout. It downloads
assets/Raylib through the normal build, builds both executables, runs the C and
graphical regressions, captures the manual viewer, trains four full batches
(4,194,304 steps with this INI), and reloads checkpoints on CPU and GPU. It also
checks native logs and clear failures for missing maps, a missing viewer asset,
a missing compiler/display, and missing/truncated checkpoints. The training
budget, output paths, checkpoint interval and final-evaluation switch are the
only training overrides. This is an integration test, not a policy benchmark.

No GPU/display/build check is silently skipped. Missing requirements or a timeout
fail the command; command logs and `build/fight-caves-e2e/report.json` are kept in
the disposable clone on success or failure. Screenshots and checkpoints stay
there too. The source/configuration digest must remain unchanged; only upstream's
tracked prebuilt `puffer` is excluded because the normal build replaces it.
This acceptance command uses Python's standard library, not pytest.

Before publishing uncommitted changes, opt into testing them explicitly:

```bash
bash tests/fight_caves.sh clean-clone --working-tree
```

That makes a real Git clone, then applies the current tracked diff and copies
non-ignored untracked source files. It verifies their content digest against the
source. It does not copy installed assets, ignored build outputs, or a virtualenv,
and it makes no commits. Its `source.json` records that a working-tree snapshot
was applied; this is not a claim that the remote branch already contains it.
Without this flag, a dirty local source is rejected rather than silently testing
an older commit.

`--source PATH_OR_GIT_URL --ref BRANCH_OR_COMMIT` selects another committed source.
`--prepare-only` stops after cloning, useful for moving the isolated checkout
into PufferTank. Inside that **fresh disposable copy**, run
`bash tests/fight_caves.sh checkout`. This refuses existing build/asset/virtualenv
directories and must not be used to validate the development tree in place.
All these checks are explicitly invoked; ordinary builds do not run them.
