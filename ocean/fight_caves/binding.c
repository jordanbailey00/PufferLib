/*
 * binding.c — PufferLib 4.0 binding for Fight Caves environment.
 *
 * Defines the macros PufferLib needs, includes vecenv.h, and implements
 * the my_init/my_log hooks for config parsing and stat logging.
 */

#include "fight_caves.h"
#include "fc_reward.h"

#define OBS_SIZE FC_PUFFER_OBS_SIZE
#define OBS_TENSOR_T FloatTensor
#define NUM_ATNS FC_PUFFER_NUM_ATNS
#define ACT_SIZES {17, 9, 5, 3, 2}
#define OBS_TYPE FLOAT
#define ACT_TYPE DOUBLE

#define Env FightCaves
#include "vecenv.h"

void my_init(Env* env, Dict* kwargs) {
    env->num_agents = 1;  /* Fight Caves is single-agent */
    FcRewardParams defaults = fc_reward_default_params();

    /* Reward shaping weights (from config/fight_caves.ini [env] section) */
    DictItem* item;
    item = dict_get_unsafe(kwargs, "w_damage_dealt");
    env->w_damage_dealt = item ? (float)item->value : defaults.w_damage_dealt;
    item = dict_get_unsafe(kwargs, "w_damage_taken");
    env->w_damage_taken = item ? (float)item->value : defaults.w_damage_taken;
    item = dict_get_unsafe(kwargs, "w_npc_kill");
    env->w_npc_kill = item ? (float)item->value : defaults.w_npc_kill;
    item = dict_get_unsafe(kwargs, "w_wave_clear");
    env->w_wave_clear = item ? (float)item->value : defaults.w_wave_clear;
    item = dict_get_unsafe(kwargs, "w_jad_kill");
    env->w_jad_kill = item ? (float)item->value : defaults.w_jad_kill;
    item = dict_get_unsafe(kwargs, "w_player_death");
    env->w_player_death = item ? (float)item->value : defaults.w_player_death;
    item = dict_get_unsafe(kwargs, "w_correct_jad_prayer");
    env->w_correct_jad_prayer = item ? (float)item->value : defaults.w_correct_jad_prayer;
    item = dict_get_unsafe(kwargs, "w_correct_danger_prayer");
    env->w_correct_danger_prayer = item ? (float)item->value : defaults.w_correct_danger_prayer;
    item = dict_get_unsafe(kwargs, "w_invalid_action");
    env->w_invalid_action = item ? (float)item->value : defaults.w_invalid_action;
    item = dict_get_unsafe(kwargs, "w_tick_penalty");
    env->w_tick_penalty = item ? (float)item->value : defaults.w_tick_penalty;

    /* Configurable shaping terms */
    item = dict_get_unsafe(kwargs, "shape_food_waste_scale");
    env->shape_food_waste_scale = item ? (float)item->value : defaults.shape_food_waste_scale;
    item = dict_get_unsafe(kwargs, "shape_pot_waste_scale");
    env->shape_pot_waste_scale = item ? (float)item->value : defaults.shape_pot_waste_scale;
    item = dict_get_unsafe(kwargs, "shape_wrong_prayer_penalty");
    env->shape_wrong_prayer_penalty = item ? (float)item->value : defaults.shape_wrong_prayer_penalty;
    item = dict_get_unsafe(kwargs, "shape_npc_melee_penalty");
    env->shape_npc_melee_penalty = item ? (float)item->value : defaults.shape_npc_melee_penalty;
    item = dict_get_unsafe(kwargs, "shape_wasted_attack_penalty");
    env->shape_wasted_attack_penalty = item ? (float)item->value : defaults.shape_wasted_attack_penalty;
    item = dict_get_unsafe(kwargs, "shape_kiting_reward");
    env->shape_kiting_reward = item ? (float)item->value : defaults.shape_kiting_reward;
    item = dict_get_unsafe(kwargs, "shape_safespot_attack_reward");
    env->shape_safespot_attack_reward = item ? (float)item->value : defaults.shape_safespot_attack_reward;
    item = dict_get_unsafe(kwargs, "shape_unnecessary_prayer_penalty");
    env->shape_unnecessary_prayer_penalty = item ? (float)item->value : defaults.shape_unnecessary_prayer_penalty;
    item = dict_get_unsafe(kwargs, "shape_wave_stall_base_penalty");
    env->shape_wave_stall_base_penalty = item ? (float)item->value : defaults.shape_wave_stall_base_penalty;
    item = dict_get_unsafe(kwargs, "shape_wave_stall_cap");
    env->shape_wave_stall_cap = item ? (float)item->value : defaults.shape_wave_stall_cap;
    item = dict_get_unsafe(kwargs, "shape_jad_heal_penalty");
    env->shape_jad_heal_penalty = item ? (float)item->value : defaults.shape_jad_heal_penalty;
    item = dict_get_unsafe(kwargs, "shape_resource_threat_window");
    env->shape_resource_threat_window = item ? (int)item->value : defaults.shape_resource_threat_window;
    item = dict_get_unsafe(kwargs, "shape_kiting_min_dist");
    env->shape_kiting_min_dist = item ? (int)item->value : defaults.shape_kiting_min_dist;
    item = dict_get_unsafe(kwargs, "shape_kiting_max_dist");
    env->shape_kiting_max_dist = item ? (int)item->value : defaults.shape_kiting_max_dist;
    item = dict_get_unsafe(kwargs, "shape_wave_stall_start");
    env->shape_wave_stall_start = item ? (int)item->value : defaults.shape_wave_stall_start;
    item = dict_get_unsafe(kwargs, "shape_wave_stall_ramp_interval");
    env->shape_wave_stall_ramp_interval = item ? (int)item->value : defaults.shape_wave_stall_ramp_interval;
    item = dict_get_unsafe(kwargs, "loadout");
    env->loadout_id = fc_loadout_id_or_default(item ? (int)item->value : FC_DEFAULT_LOADOUT);
    item = dict_get_unsafe(kwargs, "initial_sharks");
    env->initial_sharks = item ? (int)item->value : FC_MAX_SHARKS;
    item = dict_get_unsafe(kwargs, "initial_prayer_doses");
    env->initial_prayer_doses = item ? (int)item->value : FC_MAX_PRAYER_DOSES;

    /* Obs ablation flags (default 0 — i.e. no ablation, full obs).
     * See fc_apply_obs_ablation in fc-core/src/fc_state.c for what each zeroes. */
    item = dict_get_unsafe(kwargs, "obs_ablate_npc_distance");
    env->obs_ablate_npc_distance = item ? (int)item->value : 0;
    item = dict_get_unsafe(kwargs, "obs_ablate_incoming_aggregates");
    env->obs_ablate_incoming_aggregates = item ? (int)item->value : 0;
    item = dict_get_unsafe(kwargs, "obs_ablate_npc_valid");
    env->obs_ablate_npc_valid = item ? (int)item->value : 0;

    /* Initialize game state. PufferLib sets env->rng before my_init. */
    env->seed_counter = fc_seed_counter_from_env_rng(env->rng);
    fc_init(&env->state);
}

void my_log(Log* log, Dict* out) {
    dict_set(out, "episode_length", log->episode_length);
    dict_set(out, "wave_reached", log->wave_reached);
    dict_set(out, "loadout_id", log->loadout_id);
    dict_set(out, "max_wave", (float)atomic_load_explicit(&g_max_wave_ever, memory_order_relaxed));
    dict_set(out, "most_npcs_slayed", (float)atomic_load_explicit(&g_most_npcs_ever, memory_order_relaxed));
    dict_set(out, "prayer_uptime_melee", log->prayer_uptime_melee);
    dict_set(out, "prayer_uptime_range", log->prayer_uptime_range);
    dict_set(out, "prayer_uptime_magic", log->prayer_uptime_magic);
    dict_set(out, "correct_prayer", log->correct_prayer);
    dict_set(out, "wrong_prayer_hits", log->wrong_prayer_hits);
    dict_set(out, "no_prayer_hits", log->no_prayer_hits);
    dict_set(out, "prayer_switches", log->prayer_switches);
    dict_set(out, "damage_blocked", log->damage_blocked);
    dict_set(out, "dmg_taken_avg", log->dmg_taken_avg);
    dict_set(out, "attack_when_ready_rate", log->attack_when_ready_rate);
    dict_set(out, "pots_used", log->pots_used);
    dict_set(out, "avg_prayer_on_pot", log->avg_prayer_on_pot);
    dict_set(out, "food_eaten", log->food_eaten);
    dict_set(out, "avg_hp_on_food", log->avg_hp_on_food);
    dict_set(out, "food_wasted", log->food_wasted);
    dict_set(out, "pots_wasted", log->pots_wasted);
    dict_set(out, "tokxil_melee_ticks", log->tokxil_melee_ticks);
    dict_set(out, "ketzek_melee_ticks", log->ketzek_melee_ticks);
    dict_set(out, "max_wave_ticks", log->max_wave_ticks);
    dict_set(out, "max_wave_ticks_wave", log->max_wave_ticks_wave);
    dict_set(out, "reached_wave_63", log->reached_wave_63);
    dict_set(out, "jad_kill_rate", log->jad_kill_rate);

    /* Keys must use static storage because dict_set stores the pointer. */
    static char rwd_keys_total[FC_CH_COUNT][48];
    static char rwd_keys_fires[FC_CH_COUNT][48];
    static int rwd_keys_built = 0;
    if (!rwd_keys_built) {
        for (int i = 0; i < FC_CH_COUNT; i++) {
            snprintf(rwd_keys_total[i], 48, "rwd_%s_total", FC_CH_NAMES[i]);
            snprintf(rwd_keys_fires[i], 48, "rwd_%s_fires", FC_CH_NAMES[i]);
        }
        rwd_keys_built = 1;
    }
    for (int i = 0; i < FC_CH_COUNT; i++) {
        dict_set(out, rwd_keys_total[i], log->rwd_total[i]);
        dict_set(out, rwd_keys_fires[i], log->rwd_fires[i]);
    }
}
