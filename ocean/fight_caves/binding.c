/*
 * binding.c — PufferLib 5.0 initialization and logging for Fight Caves.
 *
 * Implements puf_init/puf_log using Puffer's native configuration dictionary.
 * No Python extension or legacy vecenv interface is used.
 */

/* This translation unit is the sole implementation owner in native builds. */
#define FC_SIMULATION_IMPLEMENTATION
#include "fight_caves.h"
#undef FC_SIMULATION_IMPLEMENTATION
#define FC_VIEWER_EMBEDDED
#include "viewer.c"
#undef FC_VIEWER_EMBEDDED

static void fc_puffer_accumulate_episode_summary(
    Log* log, const FcEpisodeSummary* summary) {
    log->zero_progress_ticks += (float)summary->zero_progress_ticks;
    log->wave_reached += (float)summary->wave_reached;
    log->wrong_prayer_hits += (float)summary->wrong_prayer_hits;
    log->reached_wave_63 += (float)summary->reached_wave_63;
    log->jad_kill_rate += (float)summary->jad_kill_rate;
    log->prayer_uptime_range += (float)summary->prayer_uptime_range;
    log->prayer_uptime_melee += (float)summary->prayer_uptime_melee;
    log->prayer_uptime_magic += (float)summary->prayer_uptime_magic;
    log->npc_healing_total += (float)summary->npc_healing_total;
    log->jad_healing_total += (float)summary->jad_healing_total;
    log->episode_length += (float)summary->episode_length;
}

static void fc_puffer_write_obs(FightCaves* env) {
    Agent* agent = &env->agents[0];
    float* obs = agent->observations;

    /* Policy observations */
    fc_write_obs(&env->state, obs);

    /* Optional obs ablation (zero specific feature slots in-place) */
    fc_apply_obs_ablation(obs,
                          env->obs_ablate_npc_distance,
                          env->obs_ablate_incoming_aggregates,
                          env->obs_ablate_npc_valid);

    /* Keep the float mask in observations for checkpoint compatibility, and
     * publish the same legality flags through PufferLib's native mask channel. */
    float full_mask[FC_ACTION_MASK_SIZE];
    fc_write_mask(&env->state, full_mask);
    memcpy(obs + FC_POLICY_OBS_SIZE, full_mask, sizeof(float) * FC_PUFFER_MASK_SIZE);
    if (agent->action_mask != NULL) {
        for (int i = 0; i < FC_PUFFER_MASK_SIZE; i++) {
            agent->action_mask[i] = (unsigned char)(full_mask[i] != 0.0f);
        }
    }
}

/* ======================================================================== */
/* Reward computation from reward features                                   */
/* ======================================================================== */

static float fc_puffer_compute_reward(FightCaves* env) {
    FcRewardBreakdown breakdown = fc_reward_compute_breakdown(
        &env->state, &env->reward_params, &env->reward_runtime);
    if (env->viewer) env->viewer->pending_reward_breakdown = breakdown;
    fc_reward_sync_progress_state(&env->state, &env->reward_runtime);
    return breakdown.total;
}


/* ======================================================================== */
/* PufferLib interface: puf_reset, puf_step, puf_render, puf_close           */
/* ======================================================================== */

static uint32_t fc_puffer_mix_reset_seed(uint32_t env_rng, uint32_t episode) {
    uint32_t x = env_rng + 0x9E3779B9u * (episode + 1u);
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    x ^= x >> 16;
    return (x != 0u) ? x : 0x12345678u;
}

void puf_reset(Env* env) {
    env->seed_counter++;
    fc_reset(&env->state,
             fc_puffer_mix_reset_seed((uint32_t)env->rng, env->seed_counter));
    if (env->initial_sharks < 0) env->initial_sharks = 0;
    if (env->initial_sharks > FC_MAX_SHARKS) env->initial_sharks = FC_MAX_SHARKS;
    if (env->initial_prayer_doses < 0) env->initial_prayer_doses = 0;
    if (env->initial_prayer_doses > FC_MAX_PRAYER_DOSES)
        env->initial_prayer_doses = FC_MAX_PRAYER_DOSES;
    fc_set_initial_supplies(&env->state, env->initial_sharks,
                            env->initial_prayer_doses);

    env->ep_length = 0;
    fc_reward_runtime_begin_episode(&env->reward_runtime, &env->state);
    /* Compute initial observations */
    fc_puffer_write_obs(env);
    if (env->viewer) env->viewer->reset_state = env->state;
}

void puf_step(Env* env) {
    Agent* agent = &env->agents[0];
    agent->rewards[0] = 0.0f;
    agent->terminals[0] = 0.0f;

    /* Convert float actions from network to int action heads.
     * PufferLib sends actions as floats in a flat array.
     * Puffer-facing no-supplies policy uses only move/attack/prayer.
     * Core heads 3-6 are left as zero: no eat, no drink, no walk-to-tile. */
    int actions[FC_NUM_ACTION_HEADS];
    memset(actions, 0, sizeof(actions));
    for (int h = 0; h < FC_PUFFER_NUM_ATNS; h++) {
        actions[h] = (int)agent->actions[h];
    }
    /* Heads 5+6 (walk-to-tile) always 0 — not used in v1 */

    /* Step the game simulation */
    fc_step(&env->state, actions);

    /* Compute reward */
    float reward = fc_puffer_compute_reward(env);
    agent->rewards[0] = reward;
    env->ep_length++;

    /* Write the current tick's observation. On terminal steps, puf_reset()
     * below replaces it with the next episode's initial observation. */
    fc_puffer_write_obs(env);

    /* A value snapshot survives same-step autoreset. Worker threads only copy
     * data here; all graphics and frame pacing remain inside puf_render(). */
    if (env->viewer) {
        env->viewer->pending_state = env->state;
        env->viewer->pending_reward_runtime = env->reward_runtime;
        memcpy(env->viewer->pending_actions, actions, sizeof(actions));
        env->viewer->pending_frame = 1;
    }

    /* Check terminal */
    if (fc_is_terminal(&env->state)) {
        FcEpisodeSummary summary;
        fc_episode_summary_build(&env->state, &env->reward_runtime,
                                 env->ep_length, &summary);
        agent->terminals[0] = 1.0f;
        fc_puffer_accumulate_episode_summary(&env->log, &summary);
        env->log.n += 1.0f;

        /* Same-step autoreset: return the completed episode's reward and
         * terminal flag alongside the next episode's initial observation. */
        puf_reset(env);
    }
}

void puf_render(Env* env) {
    if (!env->viewer) {
        env->viewer = fc_viewer_create(1);
        if (!env->viewer) exit(EXIT_FAILURE);
        ViewerState* v = env->viewer;
        v->state = v->reset_state = env->state;
        v->reward_params = env->reward_params;
        v->reward_runtime = env->reward_runtime;
        v->active_loadout = env->state.active_loadout;
        snprintf(v->reward_config_path, sizeof(v->reward_config_path),
                 "Puffer environment configuration");
        v->reward_config_loaded = 1;
        fc_viewer_reset_presentation(v);
    }
    ViewerState* v = env->viewer;
    if (!v->pending_frame &&
        (v->state.rng_seed != env->state.rng_seed ||
         v->state.tick != env->state.tick)) {
        /* Also support an explicit puf_reset() between render calls. */
        v->state = env->state;
        v->reward_runtime = env->reward_runtime;
        memset(&v->reward_breakdown, 0, sizeof(v->reward_breakdown));
        fc_viewer_reset_presentation(v);
    }
    fc_viewer_present_pending(v);
    int frame;
    do {
        frame = fc_viewer_frame(env->viewer, 1);
    } while (frame == 0);
    if (frame < 0) {
        fc_viewer_destroy(env->viewer);
        env->viewer = NULL;
        exit(EXIT_SUCCESS);
    }
}

void puf_close(Env* env) {
    fc_viewer_destroy(env->viewer);
    env->viewer = NULL;
    fc_destroy(&env->state);
}

/* Cold-path metadata for C callers and the standalone --contract command. */

#define FC_STRINGIFY_INNER(value) #value
#define FC_STRINGIFY(value) FC_STRINGIFY_INNER(value)

const char* fc_training_contract_json(void) {
    static char json[2048];
    static int initialized = 0;
    if (!initialized) {
        snprintf(
            json,
            sizeof(json),
            "{"
            "\"contract_dump_schema_version\":%d,"
            "\"policy_obs_size\":%d,"
            "\"puffer_obs_size\":%d,"
            "\"puffer_action_dims\":[%d,%d,%d],"
            "\"puffer_mask_size\":%d,"
            "\"core_obs_size\":%d,"
            "\"core_action_dims\":[%d,%d,%d,%d,%d,%d,%d],"
            "\"core_action_mask\":%d,"
            "\"reward_feature_count\":%d,"
            "\"observation_version\":\"%s\","
            "\"action_version\":\"%s\","
            "\"reward_version\":\"%s\","
            "\"prayer_timing_version\":\"%s\","
            "\"state_hash_version\":%u,"
            "\"active_loadout\":\"%s\""
            "}",
            FC_CONTRACT_DUMP_SCHEMA_VERSION,
            FC_POLICY_OBS_SIZE,
            FC_PUFFER_OBS_SIZE,
            FC_PUFFER_ACTION_DIMS[0],
            FC_PUFFER_ACTION_DIMS[1],
            FC_PUFFER_ACTION_DIMS[2],
            FC_PUFFER_MASK_SIZE,
            FC_OBS_SIZE,
            FC_ACTION_DIMS[0],
            FC_ACTION_DIMS[1],
            FC_ACTION_DIMS[2],
            FC_ACTION_DIMS[3],
            FC_ACTION_DIMS[4],
            FC_ACTION_DIMS[5],
            FC_ACTION_DIMS[6],
            FC_ACTION_MASK_SIZE,
            FC_REWARD_FEATURES,
            FC_OBSERVATION_VERSION,
            FC_ACTION_VERSION,
            FC_REWARD_VERSION,
            FC_PRAYER_TIMING_VERSION,
            FC_STATE_HASH_VERSION,
            FC_STRINGIFY(FC_ACTIVE_LOADOUT));
        initialized = 1;
    }
    return json;
}


static void fc_override_float_config(
        Dict* kwargs, const char* key, float* value) {
    DictItem* item = dict_find(kwargs, key);
    if (item != NULL) *value = (float)item->value;
}

static void fc_override_int_config(Dict* kwargs, const char* key, int* value) {
    DictItem* item = dict_find(kwargs, key);
    if (item != NULL) *value = (int)item->value;
}

void puf_init(Env* env, Dict* kwargs) {
    env->num_agents = 1;  /* Fight Caves is single-agent */
    env->reward_params = fc_reward_default_params();

    /* Reward shaping weights (from config/fight_caves.ini [env] section) */
    fc_override_float_config(
        kwargs, "w_damage_dealt", &env->reward_params.w_damage_dealt);
    fc_override_float_config(
        kwargs, "w_progress", &env->reward_params.w_progress);
    fc_override_float_config(kwargs, "negative_progress_multiplier",
        &env->reward_params.negative_progress_multiplier);
    fc_override_float_config(
        kwargs, "w_damage_taken", &env->reward_params.w_damage_taken);
    fc_override_float_config(
        kwargs, "w_npc_kill", &env->reward_params.w_npc_kill);
    fc_override_float_config(
        kwargs, "w_wave_clear", &env->reward_params.w_wave_clear);
    fc_override_float_config(
        kwargs, "w_jad_kill", &env->reward_params.w_jad_kill);
    fc_override_float_config(
        kwargs, "w_cave_complete", &env->reward_params.w_cave_complete);
    fc_override_float_config(
        kwargs, "w_player_death", &env->reward_params.w_player_death);
    fc_override_int_config(kwargs, "scale_player_death_with_progress",
        &env->reward_params.scale_player_death_with_progress);
    fc_override_float_config(kwargs, "player_death_min_scale",
        &env->reward_params.player_death_min_scale);
    fc_override_float_config(kwargs, "w_correct_jad_prayer",
        &env->reward_params.w_correct_jad_prayer);
    fc_override_float_config(kwargs, "w_correct_danger_prayer",
        &env->reward_params.w_correct_danger_prayer);
    fc_override_float_config(
        kwargs, "w_prayer_lost", &env->reward_params.w_prayer_lost);
    fc_override_float_config(
        kwargs, "w_invalid_action", &env->reward_params.w_invalid_action);
    fc_override_float_config(
        kwargs, "w_tick_penalty", &env->reward_params.w_tick_penalty);

    /* Configurable shaping terms */
    fc_override_float_config(kwargs, "shape_unnecessary_prayer_penalty",
        &env->reward_params.shape_unnecessary_prayer_penalty);
    fc_override_float_config(kwargs, "shape_wave_stall_base_penalty",
        &env->reward_params.shape_wave_stall_base_penalty);
    fc_override_float_config(kwargs, "shape_wave_stall_cap",
        &env->reward_params.shape_wave_stall_cap);
    fc_override_float_config(kwargs, "shape_jad_heal_penalty",
        &env->reward_params.shape_jad_heal_penalty);
    fc_override_float_config(kwargs, "shape_npc_heal_penalty",
        &env->reward_params.shape_npc_heal_penalty);
    fc_override_float_config(kwargs, "shape_no_progress_penalty_1",
        &env->reward_params.shape_no_progress_penalty_1);
    fc_override_float_config(kwargs, "shape_no_progress_penalty_2",
        &env->reward_params.shape_no_progress_penalty_2);
    fc_override_float_config(kwargs, "shape_no_progress_penalty_3",
        &env->reward_params.shape_no_progress_penalty_3);
    fc_override_float_config(kwargs, "shape_no_attack_base_penalty",
        &env->reward_params.shape_no_attack_base_penalty);
    fc_override_float_config(kwargs, "shape_no_attack_wave_scale",
        &env->reward_params.shape_no_attack_wave_scale);
    fc_override_int_config(kwargs, "shape_wave_stall_start",
        &env->reward_params.shape_wave_stall_start);
    fc_override_int_config(kwargs, "shape_wave_stall_ramp_interval",
        &env->reward_params.shape_wave_stall_ramp_interval);
    fc_override_int_config(kwargs, "shape_no_progress_start_1",
        &env->reward_params.shape_no_progress_start_1);
    fc_override_int_config(kwargs, "shape_no_progress_start_2",
        &env->reward_params.shape_no_progress_start_2);
    fc_override_int_config(kwargs, "shape_no_progress_start_3",
        &env->reward_params.shape_no_progress_start_3);
    fc_override_int_config(kwargs, "shape_no_attack_start",
        &env->reward_params.shape_no_attack_start);

    DictItem* item = dict_find(kwargs, "initial_sharks");
    env->initial_sharks = item ? (int)item->value : 0;
    item = dict_find(kwargs, "initial_prayer_doses");
    env->initial_prayer_doses = item ? (int)item->value : 0;

    /* Obs ablation flags (default 0 — i.e. no ablation, full obs).
     * See fc_apply_obs_ablation in simulation.h for what each zeroes. */
    item = dict_find(kwargs, "obs_ablate_npc_distance");
    env->obs_ablate_npc_distance = item ? (int)item->value : 0;
    item = dict_find(kwargs, "obs_ablate_incoming_aggregates");
    env->obs_ablate_incoming_aggregates = item ? (int)item->value : 0;
    item = dict_find(kwargs, "obs_ablate_npc_valid");
    env->obs_ablate_npc_valid = item ? (int)item->value : 0;

    /* Puffer may initialize and close a probe before wiring any buffers. */
    env->agents[0].policy = 0;
    env->tag = 0;
    env->boundary_reached = 0;
    env->seed_counter = 0;
    fc_init(&env->state);
}

void puf_log(Log* log, Dict* out) {
    /* Native evaluation requires perf; use the existing success metric for
     * both standard aliases without adding counters or changing rewards. */
    dict_set(out, "perf", log->jad_kill_rate);
    dict_set(out, "score", log->jad_kill_rate);
    dict_set(out, "zero_progress_ticks", log->zero_progress_ticks);
    dict_set(out, "wave_reached", log->wave_reached);
    dict_set(out, "wrong_prayer_hits", log->wrong_prayer_hits);
    dict_set(out, "reached_wave_63", log->reached_wave_63);
    dict_set(out, "jad_kill_rate", log->jad_kill_rate);
    dict_set(out, "prayer_uptime_range", log->prayer_uptime_range);
    dict_set(out, "prayer_uptime_melee", log->prayer_uptime_melee);
    dict_set(out, "prayer_uptime_magic", log->prayer_uptime_magic);
    dict_set(out, "npc_healing_total", log->npc_healing_total);
    dict_set(out, "jad_healing_total", log->jad_healing_total);
    dict_set(out, "episode_length", log->episode_length);
}
