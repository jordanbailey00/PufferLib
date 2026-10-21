# Fight Caves 5.0 baseline

## Current baseline — c303_0056

Adopted on 2026-10-19 after the completed 2026-10-11 multi-seed comparison.
The main [configuration](config/fight_caves.ini) uses these exact values:

```ini
learning_rate = 0.000800000038
momentum = 0.994000018
ent_coef = 0.000221859314
replay_ratio = 2.08926511
clip_coef = 0.0466209911
max_grad_norm = 0.400000006
vf_coef = 0.158951879
vf_clip_coef = 0.185629278
```

Only these eight hyperparameters changed. The requested budget remains 500M
(499,122,176 actual steps), with seed 73, 4,096 agents, two buffers, 16 threads,
256-step horizons, 32,768-step minibatches, and a 512-wide three-layer policy.
Synchronous training, horizon resets, CUDA graphs, V-trace, rewards, observations,
actions and gameplay are unchanged. The candidate's `c303` name identifies its
discovery seed; it does not change the default training seed to 303.

| Held-out training seeds 404 / 505 / 606 / 707 | c303_0056 | Previous 0024 |
| --- | ---: | ---: |
| Mean final Jad kill rate | **86.99%** | 72.87% |
| Worst-seed final Jad kill rate | **85.16%** | 68.43% |
| Jad kill rate sample standard deviation | **1.71 pp** | 4.59 pp |
| Mean rate of reaching wave 63 | **89.00%** | 76.84% |
| Worst-seed rate of reaching wave 63 | **87.20%** | 72.58% |
| Wave 63 sample standard deviation | **1.26 pp** | 3.47 pp |

The study screened 13 candidates and two controls on seeds 73/101/202/303,
then tested three locked finalists and both controls on the four held-out seeds.
`c303_0056` beat 0024 on all four held-out seeds and led the held-out comparison
on mean, worst seed and standard deviation for both requested metrics.
Across all eight seeds its Jad mean / worst / sample SD was
85.86% / 76.55% / 4.33 pp, versus 71.78% / 38.51% / 14.89 pp for 0024.
Seed 73 alone scored 83.59% Jad and 85.58% wave 63, below 0024's original
single-seed result; promotion is based on the multi-seed evidence.

This is a provisional relative improvement, not attainment of the stricter
90%-mean / 85%-minimum target across both seed sets. Keep the full 500M budget:
some winning-config seeds learn Jad late. No additional sweep or tuning is
implied by adopting these defaults.

The comparison used the frozen historical native 5.0 executable, SHA256
`474850506a21b5022e2ed4b598e997ef1fc0c895945e37e81d2ee0514d2eb64d`,
with exact checkpoint reproduction checks before reusing historical results.
The local, Git-ignored [study summary](build/fc5-validation-20261011/executive_summary.txt),
[decision](build/fc5-validation-20261011/decision.json),
[results](build/fc5-validation-20261011/results.json), and
[verification](build/fc5-validation-20261011/verification.json) retain the
complete evidence. Their statements that the main INI was unchanged describe
the study's state before this adoption. Preserve this evidence during cleanup.

Build with `./build.sh fight_caves`, then run `./puffer train`. Native training
and final evaluation use different recurrent-reset settings; do not use a
post-evaluation INI as a fresh training config. The 2026-10-19 current-build
integration results are retained separately in `build/fc5-integration-20261019/`.

### Current upstream compatibility

The vendored code was synchronized to PufferLib default branch `5.0`, commit
`ea7f989371270d2948f96728bda8aec5e7634a4c`, on 2026-10-19. This adds the
upstream cuBLAS stream/workspace fix after the previous `6ffa5b10` snapshot.
All 823 upstream files match that revision except the intentional local
`.gitignore` and `build.sh` integration changes.

A fresh 500M seed-73 run on `6ffa5b10` reproduced all ten frozen-study
checkpoints and all environment metric histories exactly: 83.5857% Jad and
85.5756% wave 63. The new cuBLAS revision changes the numerical training
trajectory, so the eight-seed study statistics above remain evidence for the
frozen runtime. They must not be relabeled as a new eight-seed study on
`ea7f9893`. Current-build validation is recorded separately in
[the integration report](build/fc5-integration-20261019/integration-summary.txt).

On `ea7f9893`, the fresh 500M seed-73 run completed with 77.8588% Jad kills
and 78.6262% wave-63 reach over 10,424 episodes. Its ten checkpoints were
finite and structurally valid, and CUDA/CPU reload checks passed. The latest
evaluator reproduced the historical checkpoint's 83.5857% Jad score, while
the newly trained checkpoint reproduced its own 77.8588% score. This isolates
the observed difference to training behavior. The full environment suite
(70 Python checks plus C core, adapter, CPU and graphical checks) passed.
Repeat the locked held-out winner/control comparison on this exact revision
before claiming that the previous ranking and consistency carry over.

## Previous baseline — Protein trial 0024

Selected on 2026-09-13: **`sweep_1789246135940_0024`**, W&B run
[`f146cdcf307ed881`](https://wandb.ai/jbailey8531-oakton-college/fight%20caves%20rl/runs/f146cdcf307ed881),
from group `fc5_protein_90x500m_async0_reset1_r2`. This replaces
`sync_async0_reset1` as the 5.0 tuning baseline, not any separate 4.0 baseline.

| Metric | Historical 0024 baseline |
| --- | --- |
| Requested / actual training steps | 500,000,000 / 499,122,176 |
| Seed | 73 |
| Final-checkpoint Jad completion | **88.6111%** |
| Completed caves / evaluated episodes | 9,570 / 10,800 |
| Reached wave 63 | 89.2500% |
| Average wave reached | 62.3151 |
| Average episode length | 4,930.82 ticks |
| Average wrong-prayer hits | 218.84 per episode |
| Average Jad healing | 2,765.59 simulation HP units per episode |
| Training time, excluding final evaluation | 434.08 seconds |

At adoption, the main configuration used the exact logged hyperparameter values
below and the 500M requested budget:

```ini
learning_rate = 0.00123960734
momentum = 0.991584122
ent_coef = 0.00026363763
replay_ratio = 2.20972681
clip_coef = 0.0250000004
max_grad_norm = 0.217330709
vf_coef = 0.275081992
vf_clip_coef = 0.0884169564
```

Training remains synchronous (`base.async=0`) with horizon resets
(`base.reset_every_horizon=1`). Gamma, GAE lambda, network architecture, agent
count, horizon, minibatches, gameplay, rewards, observations and seed are
unchanged at that adoption. These historical values have since been replaced
by `c303_0056` above. The result in this section is the completed original sweep trial.

This is the strongest final evaluation in the 90-trial sweep, not a guarantee
across training seeds. The saved learning history contains four training-bin
averages plus final evaluation, not dense checkpoint evaluations. Trial `0084`
is the late-consistency alternative (83.7502% final); `0028` is the
earlier-learning alternative (80.2272% final). Multi-seed confirmation and
checkpoint replay were follow-up work at that time; the multi-seed comparison
has now been completed as described above.

Retained artifacts, unchanged and local:

- [Native config and metrics](../fc5-protein-90x500m/runs/fc5_protein_90x500m_async0_reset1_r2/logs/fight_caves/sweep_1789246135940_0024.ini).
- [Final checkpoint](../fc5-protein-90x500m/runs/fc5_protein_90x500m_async0_reset1_r2/checkpoints/fight_caves/sweep_1789246135940_0024/0000000499122176.bin).
- [Sweep launch settings](../fc5-protein-90x500m/runs/fc5_protein_90x500m_async0_reset1_r2/config/default.ini)
  and [provenance manifest](../fc5-protein-90x500m/runs/fc5_protein_90x500m_async0_reset1_r2/manifest.json).

Final checkpoint SHA256:
`963d9949a3c1d954fb2c746860a226c0c34915f1f19b1b2f4ffb15e56a97d568`.
The executable SHA256 is unchanged from the previous baseline recorded below.
Do not use the native post-evaluation INI as a fresh training config: it
contains evaluation-only reset and checkpoint-path overrides. The old sweep
launcher is pinned to the previous baseline; a follow-up sweep needs separately
reviewed configuration and provenance, not a blind rerun of that launcher or
the inherited broad default search.

## Previous baseline — sync_async0_reset1

Selected on 2026-09-12 with **async off** and **horizon memory reset on**.
The following results and artifacts are retained as historical comparison;
they are no longer the current main configuration.

### Recorded result

| Metric | Baseline |
| --- | --- |
| Requested training steps | 500,000,000 |
| Actual training steps | 499,122,176 (476 full rollouts) |
| Seed | 73 |
| Final-checkpoint Jad completion | **72.4608%** |
| Completed caves / evaluated episodes | 7,441 / 10,269 |
| Reached wave 63 | 74.3987% |
| Average wave reached | 60.8863 |
| Average episode length | 4,705.17 ticks |
| Average wrong-prayer hits | 250.69 per episode |
| Average Jad healing | 1,551.90 simulation HP units per episode |
| Training time, excluding final evaluation | 461.13 seconds |
| Mean logged Jad completion over final approximately 100M steps | 71.86% |
| Highest rounded training-dashboard Jad completion | 79.6% |

Final evaluation numbers are full-precision native results. Dashboard statistics
describe training, not separate checkpoint evaluations. Evaluation requested at
least 10,000 completed episodes and finished with 10,269.

All four tested combinations used the same seed, model, training budget,
hyperparameters and executable:

| Async | Reset each horizon | Final Jad completion |
| --- | --- | --- |
| On | Off | 0.00% |
| Off | Off | 63.53% |
| On | On | 0.04% |
| **Off** | **On** | **72.46%** |

This establishes the selected starting setup, not an optimum across seeds or
after further hyperparameter tuning.

### Historical configuration and reproduction

At this selection, the main configuration adopted `base.async=0` and
`base.reset_every_horizon=1` while retaining a **750M** default budget. The
experiment overrode that budget to **500M** and did not establish a result at
750M. The main config has since been replaced by trial `0024` above.

Remaining settings are unchanged: 4,096 agents, two buffers, 16 threads,
256-step horizon, 32,768-step minibatches, 512-wide three-layer policy, seed 73,
graph capture and V-trace enabled. No rewards, observations, actions, loadout,
simulation or trainer implementation changed when adopting this baseline.

The old training hyperparameters are retained in the bundle's `post_eval.ini`
and its launch overrides in `command.json`; use both to reconstruct the old
training setup, excluding the evaluation-only settings noted below. Changing
only the budget on the current INI will not reproduce this previous baseline.
Identical floating-point results on different hardware or toolchains are not
guaranteed. The recorded run used the native BF16 executable on the local
RTX 5070 Ti.

### Saved artifacts and provenance

The retained local bundle is
`checkpoints/baselines/fight_caves_5_0_500m/`. It contains:

- `0000000499122176.bin`: final checkpoint, 10,164,224 bytes / 2,541,056 floats.
- `command.json`: original training invocation, including both training flags.
- `post_eval.ini`: original native log with effective settings and metrics.
- `console.log`, `result.json`, `train_result.bin`: original run output/results.
- `manifest.json`, `source_hashes.json`, `verification.json`, `analysis.json`:
  original experiment provenance and all-four-run comparison/verification.

The bundle is local and Git-ignored; it has not been published. This native run
ID is not a W&B run ID.

Checkpoint SHA256:
`e1782f8d2bc87c33d26ab4272c7a0c3f99586ee4c9a0a0aae1e93c210957952d`

Training source commit: `9ad3633d3563a8e467026160e90209146d2a733c`.
Both new defaults were CLI overrides during the experiment; that commit still
contains the previous default flags. The stored source hashes describe the
unchanged checkout at experiment time, before adopting these new defaults.

Training executable SHA256:
`474850506a21b5022e2ed4b598e997ef1fc0c895945e37e81d2ee0514d2eb64d`

Original experiment directory (retained unchanged):
`../fc5-500m-ablation-20260912-a1AIvE/`.
Its `comparison.md` is the historical report written before selecting this
baseline; statements there about unchanged defaults refer to that earlier point.

**Native logging detail:** the trainer sets `reset_every_horizon=0` for final
evaluation and writes its INI afterward. Therefore `post_eval.ini` reports 0
even though this run trained with reset **1**. It also contains an evaluation
checkpoint path. Use `command.json` and the historical hyperparameters for the training
settings, not the post-evaluation INI as a fresh training config. All four runs
used the same native evaluation behavior; no vendored code was modified.
