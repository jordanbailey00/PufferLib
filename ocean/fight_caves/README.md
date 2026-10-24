# Fight Caves

A native C Fight Caves environment for PufferLib 5.0, with all 63 waves,
a playable Raylib viewer, and native checkpoint replay. Training and the viewer
share the same simulation.

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
Rebuild when selecting a different environment.

The main INI pins the selected hyperparameters, 4,096 agents, a 256-step horizon,
32,768-step minibatches, and a 512-wide, three-layer policy. Native training uses
`momentum`, V-trace, synchronous execution (`async=0`), and recurrent-state resets
at each horizon (`reset_every_horizon=1`). The requested 500M budget becomes
499,122,176 steps after rounding down to complete rollouts.

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
through the playable executable, using PufferLib's C inference:

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
architecture/game-version metadata.

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
batched.

## PufferTank 5.0

Fight Caves runs in [PufferTank's 5.0 branch](https://github.com/PufferAI/PufferTank/tree/5.0)
with its standard Dockerfile and no extra dependencies:

```bash
git clone --branch 5.0 https://github.com/PufferAI/PufferTank.git PufferTank-5.0
cd PufferTank-5.0
./docker.sh build -n fight-caves-tank5 -t 5.0
./docker.sh test -n fight-caves-tank5 -t 5.0
```

Inside the container, use a checkout containing Fight Caves and run the same
build/train/play/replay commands above. The launcher provides GPU/display access;
each new graphical shell may still need `unset __GLX_VENDOR_LIBRARY_NAME`.

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

The installed asset directories are ignored by Git. For an explicit reinstall,
use `python3 ocean/fight_caves/tools.py setup --all --force`.
Development overrides are `FC_COLLISION_PATH`, `FC_MOVEMENT_PATH`, `FC_LOS_PATH`,
and `FC_ASSET_ROOT` (the viewer asset directory). Run commands from the repository
root: assets and `config/fight_caves.ini` are found relative to it.

OSRS assets are distributed separately and are not covered by PufferLib's
software license; see `resources/fight_caves/ASSET_NOTICE.md`.

## Tests and maintenance

With pytest and the normal CPU build dependencies installed:

```bash
python3 -m pytest -q tests/test_fight_caves_integration.py
# Include graphical regressions (requires DISPLAY or xvfb-run):
FC_TEST_RENDER=1 python3 -m pytest -q tests/test_fight_caves_integration.py
```

The suite builds a fresh CPU executable in pytest's temporary directory and
installs/verifies assets through the standard build. It checks the asset installer,
reproducible release bundles, invalid checkpoints, CPU replay,
and two C suites: simulation/adapter and independent C/C++ linkage. The
simulation suite includes wave/equipment/analytics fixtures, observations,
masks, terminal rewards, autoreset, metrics, viewer snapshots and the CPU
policy's recurrent-state carry and reset. Graphical checks also cover
replay controls, picking/click feedback and equipment appearance.

CC/CXX select the C and C++ compilers (defaults: clang/clang++). Requested native
or graphical checks fail when prerequisites are missing. Only the graphical
case is skipped by default; set `FC_TEST_RENDER=1` to require it. No GPU training
runs in pytest. The stdlib acceptance runner below and pytest share the same
C compilation recipe.

Useful standalone diagnostics:

```bash
./fight_caves --benchmark
```

Release bundles can be generated reproducibly with
`python3 ocean/fight_caves/tools.py bundle --help`. The installer and bundle
generator are both in this one helper.

### Disposable-checkout acceptance

In a Linux/CUDA environment with graphics access, run explicitly:

```bash
unset __GLX_VENDOR_LIBRARY_NAME
python3 -S tests/fight_caves_e2e.py clean-clone
# Include this checkout's uncommitted source changes:
python3 -S tests/fight_caves_e2e.py clean-clone --working-tree
```

This stdlib-only runner clones into a retained temporary directory, downloads
assets/Raylib, builds the native and CPU executables, runs the C suites (including
NVCC/C linkage), and exercises the graphical viewer. It trains four full batches
(4,194,304 steps with this INI), verifies native logs and two checkpoints, then
reloads them on CPU and GPU. It checks clear failures for missing assets,
compiler/display and missing/truncated checkpoints. This is a functional
integration check, not a policy-performance benchmark.

No GPU, display or build check is silently skipped. Logs, screenshots, checkpoints
and `build/fight-caves-e2e/report.json` remain in the disposable checkout on success
or failure. The source/configuration digest must remain unchanged; the built
`puffer` executable is excluded from it.

A dirty local source is rejected unless `--working-tree` is explicit. That option
applies the tracked diff and copies non-ignored untracked sources, then verifies
the source digest. Existing builds, assets and virtualenvs are not copied. It does
not commit or push.

`--source PATH_OR_GIT_URL --ref REF` selects another committed source.
`--prepare-only` prepares the clone for validation elsewhere, such as PufferTank.
Inside that fresh disposable copy, run `python3 -S tests/fight_caves_e2e.py checkout`.
It refuses a checkout with existing build/assets/virtualenv directories.
