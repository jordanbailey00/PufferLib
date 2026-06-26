/*
 * fight_caves.c — Standalone entry point for testing without PufferLib.
 *
 * Compiled with: ./build.sh --local (debug) or ./build.sh --fast (optimized)
 * Runs N episodes with random actions and prints stats.
 */

#include "fight_caves.h"
#include <stdio.h>
#include <time.h>

int main(void) {
    FightCaves env = {0};
    env.num_agents = 1;
    env.observations = (float*)calloc(FC_PUFFER_OBS_SIZE, sizeof(float));
    env.actions = (float*)calloc(FC_PUFFER_NUM_ATNS, sizeof(float));
    env.rewards = (float*)calloc(1, sizeof(float));
    env.terminals = (float*)calloc(1, sizeof(float));

    {
        FcRewardParams defaults = fc_reward_default_params();
        env.w_damage_dealt = defaults.w_damage_dealt;
        env.w_damage_taken = defaults.w_damage_taken;
        env.w_npc_kill = defaults.w_npc_kill;
        env.w_wave_clear = defaults.w_wave_clear;
        env.w_jad_kill = defaults.w_jad_kill;
        env.w_player_death = defaults.w_player_death;
        env.w_correct_jad_prayer = defaults.w_correct_jad_prayer;
        env.w_correct_danger_prayer = defaults.w_correct_danger_prayer;
        env.w_invalid_action = defaults.w_invalid_action;
        env.w_tick_penalty = defaults.w_tick_penalty;

        env.shape_food_waste_scale = defaults.shape_food_waste_scale;
        env.shape_pot_waste_scale = defaults.shape_pot_waste_scale;
        env.shape_wrong_prayer_penalty = defaults.shape_wrong_prayer_penalty;
        env.shape_npc_melee_penalty = defaults.shape_npc_melee_penalty;
        env.shape_wasted_attack_penalty = defaults.shape_wasted_attack_penalty;
        env.shape_kiting_reward = defaults.shape_kiting_reward;
        env.shape_safespot_attack_reward = defaults.shape_safespot_attack_reward;
        env.shape_unnecessary_prayer_penalty = defaults.shape_unnecessary_prayer_penalty;
        env.shape_wave_stall_base_penalty = defaults.shape_wave_stall_base_penalty;
        env.shape_wave_stall_cap = defaults.shape_wave_stall_cap;
        env.shape_resource_threat_window = defaults.shape_resource_threat_window;
        env.shape_kiting_min_dist = defaults.shape_kiting_min_dist;
        env.shape_kiting_max_dist = defaults.shape_kiting_max_dist;
        env.shape_wave_stall_start = defaults.shape_wave_stall_start;
        env.shape_wave_stall_ramp_interval = defaults.shape_wave_stall_ramp_interval;
        env.shape_jad_heal_penalty = defaults.shape_jad_heal_penalty;
        env.loadout_id = FC_DEFAULT_LOADOUT;
        env.initial_sharks = FC_MAX_SHARKS;
        env.initial_prayer_doses = FC_MAX_PRAYER_DOSES;

        /* Obs ablation flags default to 0 (no ablation) for the standalone harness. */
        env.obs_ablate_npc_distance = 0;
        env.obs_ablate_incoming_aggregates = 0;
        env.obs_ablate_npc_valid = 0;
    }

    fc_init(&env.state);

    srand((unsigned)time(NULL));
    int episodes = 100;
    int total_ticks = 0;
    float total_reward = 0;
    int max_wave = 0;

    printf("Running %d episodes with random actions...\n", episodes);
    clock_t start = clock();

    for (int ep = 0; ep < episodes; ep++) {
        c_reset(&env);
        int ep_ticks = 0;
        while (!env.terminals[0] && ep_ticks < 30000) {
            for (int h = 0; h < FC_PUFFER_NUM_ATNS; h++)
                env.actions[h] = (float)(rand() % 17);
            env.actions[0] = (rand() % 3 == 0) ? (float)(rand() % 17) : 0.0f;
            env.actions[1] = (rand() % 5 == 0) ? (float)(rand() % 9) : 0.0f;
            env.actions[2] = (rand() % 10 == 0) ? (float)(rand() % 5) : 0.0f;
            env.actions[3] = (rand() % 8 == 0) ? (float)(rand() % 3) : 0.0f;
            env.actions[4] = (rand() % 8 == 0) ? (float)(rand() % 2) : 0.0f;
            c_step(&env);
            total_reward += env.rewards[0];
            ep_ticks++;
        }
        total_ticks += ep_ticks;
        if (env.state.current_wave > max_wave) max_wave = env.state.current_wave;
    }

    clock_t end = clock();
    double elapsed = (double)(end - start) / CLOCKS_PER_SEC;

    printf("Results:\n");
    printf("  Episodes:    %d\n", episodes);
    printf("  Total ticks: %d\n", total_ticks);
    printf("  SPS:         %.0f steps/sec\n", total_ticks / elapsed);
    printf("  Avg reward:  %.2f\n", total_reward / episodes);
    printf("  Max wave:    %d\n", max_wave);
    printf("  Time:        %.2fs\n", elapsed);
    printf("  Log: ep_len=%.1f wave=%.1f n=%.0f\n",
           env.log.episode_length, env.log.wave_reached, env.log.n);

    c_close(&env);
    free(env.observations);
    free(env.actions);
    free(env.rewards);
    free(env.terminals);
    return 0;
}
