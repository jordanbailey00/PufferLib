# Fight Caves

Single-agent Fight Caves environment for PufferLib/Ocean. The environment
simulates a 63-wave ranged-combat encounter with NPC targeting, movement,
collision, line of sight, protection prayers, delayed hits, supplies, wave
spawns, TzTok-Jad attacks, and Jad healer behavior.

The default training build is headless. Runtime collision data is installed
through the asset downloader under `resources/fight_caves`; cache-derived
runtime data is not vendored in the source tree.

## Files

```text
ocean/fight_caves/
  binding.c              PufferLib native binding and config parsing
  fight_caves.h          PufferLib adapter around the backend
  fight_caves.c          standalone random-action smoke runner
  fight_caves_tests.c    focused C regression harness
  fc_*.h, fc_*.c         backend game simulation

config/fight_caves.ini   default PufferLib config

resources/fight_caves/
  manifest.json          asset URL, checksums, and required file list
  download_assets.py     idempotent asset downloader/verifier
```

## Assets

The environment requires `resources/fight_caves/fightcaves.collision`. It is a
64 by 64 binary collision grid used for wall, pillar, line-of-sight, and
safespot behavior.

Install assets from the PufferLib repo root:

```bash
python resources/fight_caves/download_assets.py
```

The downloader verifies the archive SHA256 and each extracted file. If assets
are missing at runtime, the environment fails with an actionable error instead
of silently using fallback geometry. For custom local testing, set
`FC_COLLISION_PATH` to an alternate collision file.

Current required file:

| File | Size | SHA256 |
| --- | ---: | --- |
| `fightcaves.collision` | 4096 bytes | `6a1bb2a81c85d53e318aad9e82adb95e2e0b1b80ba7bce4b22175921634d8591` |

The decoded collision grid has 2156 walkable tiles and 1940 blocked tiles.

## Build And Train

CPU smoke build:

```bash
./build.sh fight_caves --cpu
```

Short CPU training smoke:

```bash
python -m pufferlib.pufferl train fight_caves \
  --slowly \
  --train.total-timesteps 10000 \
  --vec.total-agents 16 \
  --vec.num-buffers 2 \
  --vec.num-threads 4 \
  --train.horizon 256 \
  --train.minibatch-size 4096
```

GPU build, when CUDA is available:

```bash
./build.sh fight_caves
```

## Regression Harness

The env-local C harness covers collision load failure/success, observation and
mask sizes, runtime loadout switching, full/no-supplies masks, per-env reset
seeds, and Jad healer threshold/re-arm behavior.

Build and run it from the PufferLib repo root:

```bash
cc -std=c11 -Wall -Wextra -I ocean/fight_caves \
  ocean/fight_caves/fight_caves_tests.c -o /tmp/fight_caves_tests -lm

FC_COLLISION_PATH=resources/fight_caves/fightcaves.collision \
  /tmp/fight_caves_tests
```

## Configuration

Runtime environment options live under `[env]` in `config/fight_caves.ini`.
Important fields:

| Field | Meaning |
| --- | --- |
| `loadout` | Numeric loadout id, clamped to the default if invalid |
| `initial_sharks` | Starting food count, clamped to `0..20` |
| `initial_prayer_doses` | Starting prayer dose count, clamped to `0..32` |
| `w_*` | Main reward-feature weights |
| `shape_*` | Additional shaping parameters |
| `obs_ablate_*` | Optional observation ablations for controlled experiments |

Loadout ids:

| Id | Label |
| ---: | --- |
| 0 | black d'hide with rune crossbow |
| 1 | fortified Masori with twisted bow |
| 2 | low-defence rune crossbow |
| 3 | rune crossbow pure |
| 4 | magic shortbow pure |
| 5 | blowpipe pure |
| 6 | Armadyl crossbow with Armadyl armour |
| 7 | bow of Faerdhinen with crystal armour |
| 8 | high-end twisted bow with fortified Masori |

The default config uses loadout id `1`.

## Observation And Action Contract

The Puffer-facing observation has 158 floats:

```text
122 policy observation floats
 36 binary action-mask floats for heads 0-4
```

The backend also defines a full internal buffer of 307 floats:

```text
122 policy observation floats
 19 raw reward-feature floats
166 full action-mask floats, including internal walk-to-tile heads
```

The PufferLib adapter exposes only the first five action heads:

| Head | Size | Values |
| --- | ---: | --- |
| move | 17 | idle, 8 walk directions, 8 run directions |
| attack | 9 | no target or visible NPC slot 0-7 |
| prayer | 5 | no change, off, magic, ranged, melee |
| eat | 3 | none, shark, combo eat |
| drink | 2 | none, prayer potion |

Mask values are appended to the observation tail and are exactly `0.0` or
`1.0`. Invalid actions are also guarded in the backend, so a bad action cannot
corrupt state even if a caller bypasses the mask.

## Rewards And Logs

Reward weights are configured in `[env]`. The scalar reward combines combat
outcomes, survival costs, prayer correctness, resource use, kiting/safespot
shaping, wave progression, Jad completion, invalid actions, and per-tick cost.

Common log keys include:

| Key | Meaning |
| --- | --- |
| `episode_length` | Episode length in ticks |
| `wave_reached` | Final wave reached |
| `max_wave` | All-time max wave reached by this process |
| `reached_wave_63` | Episode reached Jad wave |
| `jad_kill_rate` | Episode completed by killing Jad |
| `loadout_id` | Effective loadout id |
| `correct_prayer` | Correct prayer blocks |
| `wrong_prayer_hits` | Hits taken with the wrong protection prayer |
| `no_prayer_hits` | Hits taken with no protection prayer |
| `damage_blocked` | Damage prevented by protection prayers |
| `food_eaten`, `pots_used` | Supply use |

Per-channel reward totals and fire counts are also logged with
`rwd_<channel>_total` and `rwd_<channel>_fires`.

## Reset And Determinism

PufferLib supplies a stable per-env index through `env->rng`. Fight Caves uses
that value to create deterministic but distinct episode seed streams per vector
environment. Fixed env index plus fixed action sequence is reproducible, while
parallel envs do not all start from the same episode seed.

The native adapter auto-resets after terminal logging. On a terminal step,
`terminals[0]` and `rewards[0]` describe the terminal transition, and the
observation buffer has already been reset for the next episode.

## Known Limitations

- The default Puffer-facing action space does not expose the backend's
  walk-to-tile target heads.
- The environment is a headless training simulation, not a full client.
- Item visuals and other cache-derived frontend assets are intentionally not
  included in this source tree.
- The model is an approximation of encounter mechanics for reinforcement
  learning and is not a complete game server.
