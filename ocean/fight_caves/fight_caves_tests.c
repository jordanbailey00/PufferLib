#define _POSIX_C_SOURCE 200809L

/*
 * Fight Caves regression harness.
 *
 * Build from the PufferLib repo root:
 *   cc -std=c11 -Wall -Wextra -I ocean/fight_caves \
 *      ocean/fight_caves/fight_caves_tests.c -o /tmp/fight_caves_tests -lm
 *
 * Run after downloading resources/fight_caves/fightcaves.collision:
 *   FC_COLLISION_PATH=resources/fight_caves/fightcaves.collision /tmp/fight_caves_tests
 */

#include "fight_caves.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

static int g_failures = 0;

#define EXPECT_TRUE(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        g_failures++; \
    } \
} while (0)

#define EXPECT_EQ_INT(actual, expected) do { \
    int _actual = (int)(actual); \
    int _expected = (int)(expected); \
    if (_actual != _expected) { \
        fprintf(stderr, "FAIL %s:%d: %s == %d, expected %d\n", \
                __FILE__, __LINE__, #actual, _actual, _expected); \
        g_failures++; \
    } \
} while (0)

static const char* test_collision_path(void) {
    const char* path = getenv("FC_COLLISION_PATH");
    return (path && path[0]) ? path : "resources/fight_caves/fightcaves.collision";
}

static void apply_default_reward_config(FightCaves* env) {
    FcRewardParams defaults = fc_reward_default_params();
    env->w_damage_dealt = defaults.w_damage_dealt;
    env->w_damage_taken = defaults.w_damage_taken;
    env->w_npc_kill = defaults.w_npc_kill;
    env->w_wave_clear = defaults.w_wave_clear;
    env->w_jad_kill = defaults.w_jad_kill;
    env->w_player_death = defaults.w_player_death;
    env->w_correct_jad_prayer = defaults.w_correct_jad_prayer;
    env->w_correct_danger_prayer = defaults.w_correct_danger_prayer;
    env->w_invalid_action = defaults.w_invalid_action;
    env->w_tick_penalty = defaults.w_tick_penalty;
    env->shape_food_waste_scale = defaults.shape_food_waste_scale;
    env->shape_pot_waste_scale = defaults.shape_pot_waste_scale;
    env->shape_wrong_prayer_penalty = defaults.shape_wrong_prayer_penalty;
    env->shape_npc_melee_penalty = defaults.shape_npc_melee_penalty;
    env->shape_wasted_attack_penalty = defaults.shape_wasted_attack_penalty;
    env->shape_kiting_reward = defaults.shape_kiting_reward;
    env->shape_safespot_attack_reward = defaults.shape_safespot_attack_reward;
    env->shape_unnecessary_prayer_penalty = defaults.shape_unnecessary_prayer_penalty;
    env->shape_wave_stall_base_penalty = defaults.shape_wave_stall_base_penalty;
    env->shape_wave_stall_cap = defaults.shape_wave_stall_cap;
    env->shape_jad_heal_penalty = defaults.shape_jad_heal_penalty;
    env->shape_resource_threat_window = defaults.shape_resource_threat_window;
    env->shape_kiting_min_dist = defaults.shape_kiting_min_dist;
    env->shape_kiting_max_dist = defaults.shape_kiting_max_dist;
    env->shape_wave_stall_start = defaults.shape_wave_stall_start;
    env->shape_wave_stall_ramp_interval = defaults.shape_wave_stall_ramp_interval;
}

static void init_test_env(FightCaves* env,
                          int rng,
                          int loadout_id,
                          int initial_sharks,
                          int initial_prayer_doses) {
    memset(env, 0, sizeof(*env));
    env->num_agents = 1;
    env->rng = rng;
    env->observations = (float*)calloc(FC_PUFFER_OBS_SIZE + 1, sizeof(float));
    env->actions = (float*)calloc(FC_PUFFER_NUM_ATNS, sizeof(float));
    env->rewards = (float*)calloc(1, sizeof(float));
    env->terminals = (float*)calloc(1, sizeof(float));
    EXPECT_TRUE(env->observations != NULL);
    EXPECT_TRUE(env->actions != NULL);
    EXPECT_TRUE(env->rewards != NULL);
    EXPECT_TRUE(env->terminals != NULL);

    apply_default_reward_config(env);
    env->loadout_id = loadout_id;
    env->initial_sharks = initial_sharks;
    env->initial_prayer_doses = initial_prayer_doses;
    env->seed_counter = fc_seed_counter_from_env_rng(env->rng);
    fc_init(&env->state);
}

static void destroy_test_env(FightCaves* env) {
    c_close(env);
    free(env->observations);
    free(env->actions);
    free(env->rewards);
    free(env->terminals);
}

static int count_walkable_tiles(const FcState* state) {
    int count = 0;
    for (int x = 0; x < FC_ARENA_WIDTH; x++) {
        for (int y = 0; y < FC_ARENA_HEIGHT; y++) {
            count += state->walkable[x][y] ? 1 : 0;
        }
    }
    return count;
}

static int count_alive_healers(const FcState* state) {
    int count = 0;
    for (int i = 0; i < FC_MAX_NPCS; i++) {
        const FcNpc* npc = &state->npcs[i];
        if (npc->active && !npc->is_dead && npc->npc_type == NPC_YT_HURKOT) {
            count++;
        }
    }
    return count;
}

static void clear_healers(FcState* state) {
    for (int i = 0; i < FC_MAX_NPCS; i++) {
        if (state->npcs[i].npc_type == NPC_YT_HURKOT) {
            memset(&state->npcs[i], 0, sizeof(state->npcs[i]));
        }
    }
}

static void test_collision_missing_fails(void) {
    pid_t pid = fork();
    EXPECT_TRUE(pid >= 0);
    if (pid < 0) return;

    if (pid == 0) {
        char tmpdir[] = "/tmp/fc_collision_missing_XXXXXX";
        if (!mkdtemp(tmpdir)) _exit(101);
        if (chdir(tmpdir) != 0) _exit(102);
        freopen("/dev/null", "w", stderr);
        setenv("FC_COLLISION_PATH", "/tmp/fc_missing_collision_file.bin", 1);

        FcState state;
        fc_init(&state);
        fc_reset(&state, 1);
        _exit(0);
    }

    int status = 0;
    EXPECT_TRUE(waitpid(pid, &status, 0) == pid);
    EXPECT_TRUE(WIFSIGNALED(status));
    EXPECT_EQ_INT(WTERMSIG(status), SIGABRT);
}

static void test_collision_success(void) {
    setenv("FC_COLLISION_PATH", test_collision_path(), 1);

    FcState state;
    fc_init(&state);
    fc_reset(&state, 1);

    EXPECT_EQ_INT(count_walkable_tiles(&state), 2156);
    EXPECT_EQ_INT(state.current_wave, 1);
    EXPECT_TRUE(state.npcs_remaining > 0);
}

static void test_observation_and_mask_contract(void) {
    FightCaves env;
    init_test_env(&env, 0, FC_DEFAULT_LOADOUT, FC_MAX_SHARKS, FC_MAX_PRAYER_DOSES);

    env.observations[FC_PUFFER_OBS_SIZE] = 12345.0f;
    c_reset(&env);

    EXPECT_EQ_INT(FC_PUFFER_NUM_ATNS, 5);
    EXPECT_EQ_INT(FC_PUFFER_OBS_SIZE, FC_POLICY_OBS_SIZE + FC_MASK_TARGET_X_START);
    EXPECT_EQ_INT(FC_PUFFER_OBS_SIZE, 158);
    EXPECT_TRUE(env.observations[FC_PUFFER_OBS_SIZE] == 12345.0f);

    for (int i = 0; i < FC_PUFFER_OBS_SIZE; i++) {
        EXPECT_TRUE(isfinite(env.observations[i]));
    }
    for (int i = 0; i < FC_MASK_TARGET_X_START; i++) {
        float value = env.observations[FC_POLICY_OBS_SIZE + i];
        EXPECT_TRUE(value == 0.0f || value == 1.0f);
    }

    destroy_test_env(&env);
}

static void test_runtime_loadout_switching(void) {
    FightCaves env;
    init_test_env(&env, 0, FC_LOADOUT_BLACK_DHIDE_RCB, FC_MAX_SHARKS, FC_MAX_PRAYER_DOSES);
    c_reset(&env);
    EXPECT_EQ_INT(env.loadout_id, FC_LOADOUT_BLACK_DHIDE_RCB);
    EXPECT_EQ_INT(env.state.player.max_hp, 700);
    EXPECT_EQ_INT(env.state.player.max_prayer, 430);
    EXPECT_EQ_INT(env.state.player.ranged_attack_bonus, 153);
    EXPECT_EQ_INT(env.state.player.weapon_kind, FC_WEAPON_GENERIC_RANGED);

    env.loadout_id = FC_LOADOUT_MASORI_TBOW;
    c_reset(&env);
    EXPECT_EQ_INT(env.loadout_id, FC_LOADOUT_MASORI_TBOW);
    EXPECT_EQ_INT(env.state.player.max_hp, 990);
    EXPECT_EQ_INT(env.state.player.max_prayer, 990);
    EXPECT_EQ_INT(env.state.player.ranged_attack_bonus, 215);
    EXPECT_EQ_INT(env.state.player.weapon_kind, FC_WEAPON_TWISTED_BOW);

    env.loadout_id = -1;
    c_reset(&env);
    EXPECT_EQ_INT(env.loadout_id, FC_DEFAULT_LOADOUT);
    EXPECT_EQ_INT(env.state.player.weapon_kind, FC_WEAPON_TWISTED_BOW);

    destroy_test_env(&env);
}

static void test_supplies_and_masks(void) {
    FightCaves full;
    init_test_env(&full, 0, FC_DEFAULT_LOADOUT, FC_MAX_SHARKS, FC_MAX_PRAYER_DOSES);
    c_reset(&full);
    EXPECT_EQ_INT(full.state.player.sharks_remaining, FC_MAX_SHARKS);
    EXPECT_EQ_INT(full.state.player.prayer_doses_remaining, FC_MAX_PRAYER_DOSES);
    full.state.player.current_hp -= 100;
    full.state.player.current_prayer -= 100;
    fc_puffer_write_obs(&full);
    EXPECT_TRUE(full.observations[FC_POLICY_OBS_SIZE + FC_MASK_EAT_START + FC_EAT_SHARK] == 1.0f);
    EXPECT_TRUE(full.observations[FC_POLICY_OBS_SIZE + FC_MASK_DRINK_START + FC_DRINK_PRAYER_POT] == 1.0f);
    destroy_test_env(&full);

    FightCaves empty;
    init_test_env(&empty, 0, FC_DEFAULT_LOADOUT, 0, 0);
    c_reset(&empty);
    EXPECT_EQ_INT(empty.state.player.sharks_remaining, 0);
    EXPECT_EQ_INT(empty.state.player.prayer_doses_remaining, 0);
    empty.state.player.current_hp -= 100;
    empty.state.player.current_prayer -= 100;
    fc_puffer_write_obs(&empty);
    EXPECT_TRUE(empty.observations[FC_POLICY_OBS_SIZE + FC_MASK_EAT_START + FC_EAT_SHARK] == 0.0f);
    EXPECT_TRUE(empty.observations[FC_POLICY_OBS_SIZE + FC_MASK_EAT_START + FC_EAT_COMBO] == 0.0f);
    EXPECT_TRUE(empty.observations[FC_POLICY_OBS_SIZE + FC_MASK_DRINK_START + FC_DRINK_PRAYER_POT] == 0.0f);
    destroy_test_env(&empty);

    FightCaves clamped;
    init_test_env(&clamped, 0, FC_DEFAULT_LOADOUT, FC_MAX_SHARKS + 10, FC_MAX_PRAYER_DOSES + 10);
    c_reset(&clamped);
    EXPECT_EQ_INT(clamped.state.player.sharks_remaining, FC_MAX_SHARKS);
    EXPECT_EQ_INT(clamped.state.player.prayer_doses_remaining, FC_MAX_PRAYER_DOSES);
    destroy_test_env(&clamped);
}

static void test_per_env_seed_streams(void) {
    FightCaves env0;
    FightCaves env1;
    FightCaves env0_repeat;
    init_test_env(&env0, 0, FC_DEFAULT_LOADOUT, FC_MAX_SHARKS, FC_MAX_PRAYER_DOSES);
    init_test_env(&env1, 1, FC_DEFAULT_LOADOUT, FC_MAX_SHARKS, FC_MAX_PRAYER_DOSES);
    init_test_env(&env0_repeat, 0, FC_DEFAULT_LOADOUT, FC_MAX_SHARKS, FC_MAX_PRAYER_DOSES);

    c_reset(&env0);
    c_reset(&env1);
    c_reset(&env0_repeat);
    uint32_t env0_seed1 = env0.state.rng_seed;
    uint32_t env1_seed1 = env1.state.rng_seed;
    uint32_t repeat_seed1 = env0_repeat.state.rng_seed;

    c_reset(&env0);
    c_reset(&env1);
    c_reset(&env0_repeat);
    uint32_t env0_seed2 = env0.state.rng_seed;
    uint32_t env1_seed2 = env1.state.rng_seed;
    uint32_t repeat_seed2 = env0_repeat.state.rng_seed;

    EXPECT_TRUE(env0_seed1 != env1_seed1);
    EXPECT_TRUE(env0_seed2 != env1_seed2);
    EXPECT_TRUE(env0_seed1 == repeat_seed1);
    EXPECT_TRUE(env0_seed2 == repeat_seed2);
    EXPECT_TRUE(env0_seed2 == env0_seed1 + 1u);
    EXPECT_TRUE(env1_seed2 == env1_seed1 + 1u);

    destroy_test_env(&env0);
    destroy_test_env(&env1);
    destroy_test_env(&env0_repeat);
}

static void test_jad_healer_threshold_and_rearm(void) {
    FcState state;
    memset(&state, 0, sizeof(state));
    state.current_wave = FC_NUM_WAVES;
    state.next_spawn_index = 1;
    state.npcs_remaining = 1;
    fc_npc_spawn(&state.npcs[0], NPC_TZTOK_JAD, 32, 32, 0);

    state.npcs[0].current_hp = FC_JAD_HEALER_THRESHOLD_HP_TENTHS;
    check_jad_healers(&state);
    EXPECT_EQ_INT(count_alive_healers(&state), 0);
    EXPECT_EQ_INT(state.jad_healers_spawned, 0);

    state.npcs[0].current_hp = FC_JAD_HEALER_THRESHOLD_HP_TENTHS - 1;
    check_jad_healers(&state);
    EXPECT_EQ_INT(count_alive_healers(&state), FC_JAD_NUM_HEALERS);
    EXPECT_EQ_INT(state.jad_healers_spawned, 1);

    clear_healers(&state);
    state.npcs[0].current_hp = FC_JAD_HEALER_THRESHOLD_HP_TENTHS + 100;
    check_jad_healers(&state);
    EXPECT_EQ_INT(count_alive_healers(&state), 0);
    EXPECT_EQ_INT(state.jad_healers_spawned, 1);

    state.npcs[0].current_hp = state.npcs[0].max_hp;
    check_jad_healers(&state);
    EXPECT_EQ_INT(state.jad_healers_spawned, 0);

    state.npcs[0].current_hp = FC_JAD_HEALER_THRESHOLD_HP_TENTHS - 1;
    check_jad_healers(&state);
    EXPECT_EQ_INT(count_alive_healers(&state), FC_JAD_NUM_HEALERS);
    EXPECT_EQ_INT(state.jad_healers_spawned, 1);
}

int main(void) {
    test_collision_missing_fails();
    test_collision_success();
    test_observation_and_mask_contract();
    test_runtime_loadout_switching();
    test_supplies_and_masks();
    test_per_env_seed_streams();
    test_jad_healer_threshold_and_rearm();

    if (g_failures != 0) {
        fprintf(stderr, "%d Fight Caves regression checks failed\n", g_failures);
        return 1;
    }

    puts("Fight Caves regression checks passed");
    return 0;
}
