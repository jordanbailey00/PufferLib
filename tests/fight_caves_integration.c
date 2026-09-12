/* White-box C tests for private viewer snapshots; --render exercises graphics.
 * The implementation is included only for these private assertions. Independent
 * public C/C++ linkage is tested separately in fight_caves_linkage.c. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "binding.c"
#include <assert.h>
#include <stddef.h>

_Static_assert(OBS_SIZE == 320, "observation contract changed");
_Static_assert(NUM_ATNS == 3, "action head count changed");
_Static_assert(sizeof(obs_t) == sizeof(float), "observations must be floats");
_Static_assert(offsetof(Log, n) + sizeof(float) == sizeof(Log), "n must be last");

static FightCaves* test_env(Dict* kwargs) {
    FightCaves* env = calloc(1, sizeof(*env));
    assert(env);
    env->rng = 73;
    puf_init(env, kwargs);
    /* Match Puffer: initialize before assigning agent buffers. */
    assert(env->num_agents == 1 && env->agents[0].policy == 0);
    assert(env->tag == 0 && env->boundary_reached == 0 && env->rng == 73);
    assert(env->viewer == NULL && env->agents[0].observations == NULL);
    Agent* a = &env->agents[0];
    a->observations = calloc(OBS_SIZE, sizeof(obs_t));
    a->actions = calloc(NUM_ATNS, sizeof(float));
    a->rewards = calloc(1, sizeof(float));
    a->terminals = calloc(1, sizeof(float));
    a->action_mask = calloc(FC_PUFFER_MASK_SIZE, 1);
    assert(a->observations && a->actions && a->rewards && a->terminals && a->action_mask);
    puf_reset(env);
    return env;
}

static void test_free(FightCaves* env) {
    puf_close(env);
    Agent* a = &env->agents[0];
    free(a->observations);
    free(a->actions);
    free(a->rewards);
    free(a->terminals);
    free(a->action_mask);
    free(env);
}

static void check_obs(FightCaves* env) {
    float obs[FC_OBS_SIZE], mask[FC_ACTION_MASK_SIZE];
    fc_write_obs(&env->state, obs);
    fc_apply_obs_ablation(obs, env->obs_ablate_npc_distance,
        env->obs_ablate_incoming_aggregates, env->obs_ablate_npc_valid);
    fc_write_mask(&env->state, mask);
    Agent* a = &env->agents[0];
    assert(memcmp(obs, a->observations, FC_POLICY_OBS_SIZE * sizeof(float)) == 0);
    for (int i = 0; i < FC_PUFFER_MASK_SIZE; i++) {
        assert(a->observations[FC_POLICY_OBS_SIZE + i] == mask[i]);
        assert(a->action_mask[i] == (mask[i] != 0.0f));
    }
}

/* Independent core calls check action translation and returned outputs, not
 * equivalence between the 4.0 and 5.0 learning algorithms. */
static void checked_step(FightCaves* env) {
    FcState expected = env->state;
    FcRewardRuntime runtime = env->reward_runtime;
    int actions[FC_NUM_ACTION_HEADS] = {0};
    for (int h = 0; h < NUM_ATNS; h++) actions[h] = (int)env->agents[0].actions[h];
    fc_step(&expected, actions);
    FcRewardBreakdown reward = fc_reward_compute_breakdown(
        &expected, &env->reward_params, &runtime);
    fc_reward_sync_progress_state(&expected, &runtime);
    float prior_episodes = env->log.n;
    uint32_t prior_seed_counter = env->seed_counter;
    int terminal = fc_is_terminal(&expected);
    puf_step(env);
    assert(env->agents[0].rewards[0] == reward.total);
    assert(env->agents[0].terminals[0] == (float)terminal);
    assert(env->log.n == prior_episodes + terminal);
    if (terminal) {
        assert(env->seed_counter == prior_seed_counter + 1);
        assert(env->state.tick == 0 && !fc_is_terminal(&env->state));
        assert(env->ep_length == 0 && env->state.player.current_hp > 0);
    } else {
        assert(fc_state_hash(&expected) == fc_state_hash(&env->state));
        assert(env->seed_counter == prior_seed_counter);
    }
    check_obs(env);
    if (env->viewer) {
        assert(env->viewer->pending_frame);
        assert(fc_state_hash(&env->viewer->pending_state) == fc_state_hash(&expected));
        assert(env->viewer->pending_reward_breakdown.total == reward.total);
        assert(memcmp(env->viewer->pending_actions, actions, sizeof(actions)) == 0);
    }
}

static void native_config_test(void) {
    Ini ini = {0};
    puf_ini_load_env(&ini, "fight_caves", 0, NULL);
    const struct { const char* section; const char* key; double value; } expected[] = {
        {"base", "seed", 73}, {"base", "cudagraphs", 1},
        {"base", "reset_every_horizon", 0}, {"base", "async", 1},
        {"vec", "total_agents", 4096}, {"vec", "num_buffers", 2},
        {"vec", "num_threads", 16}, {"policy", "hidden_size", 512},
        {"policy", "num_layers", 3}, {"train", "total_timesteps", 750000000},
        {"train", "horizon", 256}, {"train", "minibatch_size", 32768},
        {"train", "momentum", 0.9832670364021693}, {"train", "vtrace", 1},
        {"train", "vtrace_rho_clip", 2.0},
        {"train", "vtrace_c_clip", 0.9746667741536915},
    };
    for (size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); i++)
        assert(puf_ini_get(&ini, expected[i].section, expected[i].key) == expected[i].value);
    assert(strcmp(puf_ini_get_str(&ini, "sweep", "metric"), "jad_kill_rate") == 0);
    int agents = (int)puf_ini_get(&ini, "vec", "total_agents");
    int horizon = (int)puf_ini_get(&ini, "train", "horizon");
    int minibatch = (int)puf_ini_get(&ini, "train", "minibatch_size");
    assert(minibatch % horizon == 0 && agents % (minibatch / horizon) == 0);
    assert(horizon % 8 == 0 && minibatch <= agents * horizon);
    puf_ini_free(&ini);

    /* Native CLI overrides must apply after the environment file. */
    char* args[] = {"--train.total_timesteps=100_000_000", "--train.momentum=0.95",
                   "--train.vtrace=0", "--base.reset_every_horizon=1"};
    puf_ini_load_env(&ini, "fight_caves", 4, args);
    assert(puf_ini_get(&ini, "train", "total_timesteps") == 100000000);
    assert(puf_ini_get(&ini, "train", "momentum") == 0.95);
    assert(puf_ini_get(&ini, "train", "vtrace") == 0);
    assert(puf_ini_get(&ini, "base", "reset_every_horizon") == 1);
    assert(puf_ini_get(&ini, "env", "w_correct_danger_prayer") == 0);
    assert(puf_ini_get(&ini, "env", "obs_ablate_incoming_aggregates") == 1);
    puf_ini_free(&ini);
    puts("native config: merged settings, batch geometry and CLI precedence passed");
}

static void init_and_config_test(void) {
    Dict empty = {0};
    Env* probe = calloc(1, sizeof(*probe));
    assert(probe);
    puf_init(probe, &empty);
    puf_close(probe); /* Native graphical evaluation does this without buffers. */
    free(probe);

    int sizes[] = ACT_SIZES;
    assert(sizes[0] == 17 && sizes[1] == 9 && sizes[2] == 8);
    Env* env = test_env(&empty);
    FcRewardParams defaults = fc_reward_default_params();
    assert(env->reward_params.w_progress == defaults.w_progress);
    assert(env->initial_sharks == 0 && env->initial_prayer_doses == 0);
    assert(!env->obs_ablate_npc_distance && !env->obs_ablate_incoming_aggregates);
    assert(!env->obs_ablate_npc_valid);
    check_obs(env);
    test_free(env);

    Dict overrides = {0};
    dict_set(&overrides, "w_progress", 0.25);
    dict_set(&overrides, "shape_no_attack_start", 17);
    dict_set(&overrides, "initial_sharks", FC_MAX_SHARKS + 1);
    dict_set(&overrides, "initial_prayer_doses", -1);
    dict_set(&overrides, "obs_ablate_npc_distance", 1);
    dict_set(&overrides, "obs_ablate_incoming_aggregates", 1);
    dict_set(&overrides, "obs_ablate_npc_valid", 1);
    env = test_env(&overrides);
    assert(env->reward_params.w_progress == 0.25f);
    assert(env->reward_params.shape_no_attack_start == 17);
    assert(env->reward_params.w_player_death == defaults.w_player_death);
    assert(env->state.player.sharks_remaining == FC_MAX_SHARKS);
    assert(env->state.player.prayer_doses_remaining == 0);
    check_obs(env);
    checked_step(env);
    test_free(env);
    dict_clear(&overrides);
    puts("adapter init/config: bufferless probe, defaults, overrides, supplies, ablations passed");
}

static void terminal_and_log_test(void) {
    Dict empty = {0};
    Env* env = test_env(&empty);
    env->viewer = calloc(1, sizeof(*env->viewer));
    assert(env->viewer);
    /* Isolate terminal rewards so reset cannot silently erase them. */
    memset(&env->reward_params, 0, sizeof(env->reward_params));
    env->reward_params.w_player_death = -2.0f;
    env->reward_params.w_cave_complete = 3.0f;
    env->state.player.current_hp = 0;
    checked_step(env);
    assert(env->viewer->pending_state.terminal == TERMINAL_PLAYER_DEATH);
    assert(env->agents[0].rewards[0] == -2.0f && env->agents[0].terminals[0] == 1.0f);
    checked_step(env);
    assert(env->agents[0].rewards[0] == 0.0f && env->agents[0].terminals[0] == 0.0f);

    env->state.current_wave = FC_NUM_WAVES;
    env->state.npcs_remaining = 0;
    env->state.ep_jad_killed = 1; /* Fixture: Jad is dead and the last wave is empty. */
    memset(env->state.npcs, 0, sizeof(env->state.npcs));
    checked_step(env);
    assert(env->viewer->pending_state.terminal == TERMINAL_CAVE_COMPLETE);
    assert(env->agents[0].rewards[0] == 3.0f && env->agents[0].terminals[0] == 1.0f);
    assert(env->log.n == 2.0f && env->log.jad_kill_rate == 1.0f);
    assert(env->viewer->reset_state.tick == 0);

    /* Puffer normalizes Log before calling puf_log; pass known averaged values. */
    Log averaged = {.jad_kill_rate = 0.5f, .episode_length = 123.0f, .n = 1.0f};
    Dict out = {0};
    puf_log(&averaged, &out);
    assert(out.size == 13);
    assert(dict_get(&out, "perf") == 0.5 && dict_get(&out, "score") == 0.5);
    assert(dict_get(&out, "jad_kill_rate") == 0.5);
    assert(dict_get(&out, "episode_length") == 123.0);
    dict_clear(&out);
    checked_step(env);
    assert(env->log.n == 2.0f);
    free(env->viewer);
    env->viewer = NULL;
    test_free(env);
    puts("adapter terminals/logs: death, completion, autoreset, snapshots and perf/score passed");
}

static uint32_t digest(uint32_t hash, const void* data, size_t size) {
    const unsigned char* bytes = data;
    for (size_t i = 0; i < size; i++) hash = (hash ^ bytes[i]) * 16777619u;
    return hash;
}

static void replay_controls_test(FightCaves* env) {
    ViewerState* v = env->viewer;
    assert(v->policy_replay);
    uint32_t hash = fc_state_hash(&v->state);
    queue_player_tile_request(v, 20, 20, 0, 0);
    queue_player_attack_request(v, 0, 0, 0);
    use_inventory_slot(v, 0);
    viewer_jump_to_wave(v, 63);
    toggle_godmode(v);
    assert(v->pending_tile_x == -1 && v->pending_attack_npc == -1);
    assert(!v->godmode && fc_state_hash(&v->state) == hash);
    v->paused = 1;
    assert(fc_viewer_frame(v, 1) == 0);
    v->step_once = 1;
    assert(fc_viewer_frame(v, 1) == 1);
    assert(fc_viewer_frame(v, 1) == 0);
    assert(fc_state_hash(&v->state) == hash);
    assert(fc_state_hash(&env->state) == hash);
    v->paused = 0;
    puts("native replay controls: read-only gameplay, pause and single-step passed");
}

int main(int argc, char** argv) {
    native_config_test();
    init_and_config_test();
    terminal_and_log_test();
    int graphical = argc > 1 && strcmp(argv[1], "--render") == 0;
    Ini ini = {0};
    puf_ini_load_env(&ini, "fight_caves", 0, NULL);
    Dict* kwargs = puf_ini_section(&ini, "env", 0);
    FightCaves* env = test_env(kwargs);
    FightCaves* rendered = test_env(kwargs);
    assert(env->reward_params.w_progress == (float)dict_get(kwargs, "w_progress"));
    assert(env->obs_ablate_incoming_aggregates == 1);
    puf_ini_free(&ini);
    uint32_t trace = 2166136261u;
    int episodes = 0;
    if (graphical) {
        puf_render(rendered);
        replay_controls_test(rendered);
        rendered->viewer->tps = 60.0f;
    } else {
        rendered->viewer = calloc(1, sizeof(*rendered->viewer));
        assert(rendered->viewer);
    }
    Agent* a = &env->agents[0];
    Agent* b = &rendered->agents[0];
    for (int tick = 0; tick < 2048; tick++) {
        a->actions[0] = tick % 17;
        a->actions[1] = (tick / 3) % 9;
        a->actions[2] = (tick / 7) % 8;
        memcpy(b->actions, a->actions, sizeof(float) * NUM_ATNS);
        checked_step(env);
        checked_step(rendered);
        uint32_t hash = fc_state_hash(&env->state);
        trace = digest(trace, &hash, sizeof(hash));
        trace = digest(trace, a->observations, sizeof(obs_t) * OBS_SIZE);
        trace = digest(trace, a->action_mask, FC_PUFFER_MASK_SIZE);
        trace = digest(trace, a->rewards, sizeof(float));
        trace = digest(trace, a->terminals, sizeof(float));
        episodes += a->terminals[0] != 0;
        assert(hash == fc_state_hash(&rendered->state));
        assert(memcmp(a->observations, b->observations, sizeof(obs_t) * OBS_SIZE) == 0);
        assert(memcmp(a->action_mask, b->action_mask, FC_PUFFER_MASK_SIZE) == 0);
        assert(a->rewards[0] == b->rewards[0] && a->terminals[0] == b->terminals[0]);
        if (graphical) {
            puf_render(rendered);
            assert(hash == fc_state_hash(&rendered->state));
            assert(!rendered->viewer->pending_frame);
            if (a->terminals[0]) assert(fc_is_terminal(&rendered->viewer->state));
        }
    }
    assert(episodes > 0);
    puf_reset(env);
    puf_reset(rendered);
    if (graphical) {
        puf_render(rendered);
        assert(fc_state_hash(&rendered->viewer->state) == fc_state_hash(&env->state));
    }
    printf("adapter trace: steps=2048 episodes=%d digest=%08x\n", episodes, trace);
    if (!graphical) {
        free(rendered->viewer);
        rendered->viewer = NULL;
    }
    test_free(rendered);
    test_free(env);
    return 0;
}
