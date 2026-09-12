/* Native PufferLib 5.0 interface. Game, adapter and viewer implementations
 * are compiled as C in binding.c; the native trainer includes declarations only. */
#ifndef FIGHT_CAVES_ENV_H
#define FIGHT_CAVES_ENV_H

typedef float obs_t;

#ifdef __cplusplus
extern "C" {
#endif
#include "pufferenv.h"
#include "simulation.h"

#define OBS_SIZE FC_PUFFER_OBS_SIZE
#define NUM_ATNS FC_PUFFER_NUM_ATNS
#define ACT_SIZES FC_PUFFER_ACT_SIZES

struct Log {
    float zero_progress_ticks;
    float wave_reached;
    float wrong_prayer_hits;
    float reached_wave_63;
    float jad_kill_rate;
    float prayer_uptime_range;
    float prayer_uptime_melee;
    float prayer_uptime_magic;
    float npc_healing_total;
    float jad_healing_total;
    float episode_length;
    float n;  /* PufferLib episode count; must be last. */
};

typedef struct ViewerState ViewerState;

struct Env {
    Log log;                    /* required by PufferLib */
    Agent agents[1];            /* buffers are wired by Puffer after puf_init */
    int num_agents;             /* always 1 for Fight Caves */
    unsigned int rng;           /* per-env seed supplied by Puffer */
    int tag;                    /* Puffer policy grouping */
    int boundary_reached;       /* Puffer episode-boundary bookkeeping */

    /* Game state */
    FcState state;
    ViewerState* viewer;         /* NULL throughout headless training */

    /* Reward weights and shaping configuration, initialized once per env. */
    FcRewardParams reward_params;
    int initial_sharks;
    int initial_prayer_doses;
    FcRewardRuntime reward_runtime;

    /* Obs ablation flags (experimental — see fc_apply_obs_ablation in simulation.h).
     * When non-zero, the corresponding obs slots are zeroed AFTER fc_write_obs.
     * Used by the OBS Sweep / Ablation experiment to test which features the
     * policy actually relies on vs. which the GRU could re-derive from the rest. */
    int obs_ablate_npc_distance;
    int obs_ablate_incoming_aggregates;
    int obs_ablate_npc_valid;

    int ep_length;

    /* RNG seed counter (increments each episode for variety) */
    uint32_t seed_counter;
};
typedef Env FightCaves;

/* Environment-owned entry points shared by native tooling and manual play. */
const char* fc_training_contract_json(void);
int fc_viewer_main(int argc, char** argv);

#ifdef __cplusplus
}
#endif
#endif /* FIGHT_CAVES_ENV_H */
