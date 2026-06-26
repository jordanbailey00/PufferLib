#include "fc_api.h"
#include "fc_combat.h"
#include "fc_prayer.h"
#include "fc_pathfinding.h"
#include <math.h>
#include "fc_npc.h"
#include "fc_wave.h"
#include "fc_contracts.h"

/*
 * fc_tick.c — Main tick loop for Fight Caves simulation.
 *
 * Processing order (adapted from PufferLib PvP two-phase execution):
 *
 *   1. Clear per-tick event flags
 *   2. Process player actions:
 *      a. Prayer toggle (instant)
 *      b. Eat food / drink potion (if timer ready)
 *      c. Movement (walk/run via directional head)
 *      d. Attack initiation (if target valid and timer ready)
 *   3. Decrement player timers (attack, food, potion, combo)
 *   4. Prayer drain (only if prayer stayed active across the tick boundary)
 *   5. NPC AI tick (movement + attack) for all active NPCs
 *   6. Resolve pending hits (NPC → player, player → NPC)
 *   7. Check terminal conditions
 *   8. Increment tick
 */

/* ======================================================================== */
/* Clear per-tick flags                                                      */
/* ======================================================================== */

static void clear_per_tick_flags(FcState* state) {
    state->damage_dealt_this_tick = 0;
    state->hits_landed_this_tick = 0;
    state->damage_taken_this_tick = 0;
    state->npcs_killed_this_tick = 0;
    state->wave_just_cleared = 0;
    state->jad_damage_this_tick = 0;
    state->jad_killed = 0;
    state->correct_jad_prayer = 0;
    state->wrong_jad_prayer = 0;
    state->correct_danger_prayer = 0;
    state->wrong_danger_prayer = 0;
    state->attack_attempt_this_tick = 0;
    state->safespot_attack_this_tick = 0;
    state->invalid_action_this_tick = 0;
    state->movement_this_tick = 0;
    state->idle_this_tick = 0;
    state->food_used_this_tick = 0;
    state->prayer_potion_used_this_tick = 0;
    state->pre_eat_hp = 0;
    state->pre_drink_prayer = 0;
    state->jad_heal_procs_this_tick = 0;

    FcPlayer* p = &state->player;
    p->damage_taken_this_tick = 0;
    p->hit_style_this_tick = 0;
    p->hit_source_npc_type = 0;
    p->hit_locked_prayer_this_tick = 0;
    p->hit_blocked_this_tick = 0;
    p->hit_landed_this_tick = 0;
    p->food_eaten_this_tick = 0;
    p->potion_used_this_tick = 0;
    p->prayer_changed_this_tick = 0;

    for (int i = 0; i < FC_MAX_NPCS; i++) {
        state->npcs[i].damage_taken_this_tick = 0;
        state->npcs[i].died_this_tick = 0;
    }
}

/* ======================================================================== */
/* Resolve NPC visible-slot index to NPC array index                         */
/* ======================================================================== */

/* Same ordering as observation writer — must be identical for consistency */
static int npc_slot_to_index(const FcState* state, int slot) {
    int active_indices[FC_MAX_NPCS];
    int active_count = 0;
    for (int i = 0; i < FC_MAX_NPCS; i++) {
        if (state->npcs[i].active && !state->npcs[i].is_dead) {
            active_indices[active_count++] = i;
        }
    }

    /* Sort by (distance, spawn_index) — same as obs writer */
    for (int i = 1; i < active_count; i++) {
        int key = active_indices[i];
        int dk = fc_distance_to_npc(state->player.x, state->player.y, &state->npcs[key]);
        int j = i - 1;
        while (j >= 0) {
            int dj = fc_distance_to_npc(state->player.x, state->player.y,
                                        &state->npcs[active_indices[j]]);
            if (dj < dk || (dj == dk &&
                state->npcs[active_indices[j]].spawn_index < state->npcs[key].spawn_index)) {
                break;
            }
            active_indices[j + 1] = active_indices[j];
            j--;
        }
        active_indices[j + 1] = key;
    }

    if (slot < 0 || slot >= active_count || slot >= FC_VISIBLE_NPCS) return -1;
    return active_indices[slot];
}

/* ======================================================================== */
/* Process player actions                                                    */
/* ======================================================================== */

static void process_player_actions(FcState* state, const int actions[FC_NUM_ACTION_HEADS]) {
    FcPlayer* p = &state->player;
    int was_attack_ready = (p->attack_timer <= 0 && state->npcs_remaining > 0);

    int act_move     = actions[0];
    int act_attack   = actions[1];
    int act_prayer   = actions[2];
    int act_eat      = actions[3];
    int act_drink    = actions[4];
    int act_target_x = actions[5];
    int act_target_y = actions[6];

    state->invalid_action_this_tick = fc_action_attempt_is_invalid(state, actions);

    /* ---- Prayer (instant, processed first) ---- */
    if (act_prayer > 0) {
        int old_prayer = p->prayer;
        fc_prayer_apply_action(p, act_prayer);
        if (p->prayer != old_prayer) {
            p->prayer_changed_this_tick = 1;
        }
    }

    /* ---- Eat food ---- */
    if (act_eat == FC_EAT_SHARK && p->food_timer <= 0 &&
        p->sharks_remaining > 0 && p->current_hp < p->max_hp) {
        int heal = 200;  /* shark heals 20 HP = 200 tenths */
        state->pre_eat_hp = p->current_hp;
        state->ep_food_pre_hp_sum += state->pre_eat_hp;
        int hp_missing = p->max_hp - p->current_hp;
        if (heal > hp_missing) state->ep_food_overhealed++;
        state->ep_food_eaten++;
        p->total_food_eaten++;
        p->current_hp += heal;
        if (p->current_hp > p->max_hp) p->current_hp = p->max_hp;
        p->sharks_remaining--;
        p->food_timer = FC_FOOD_COOLDOWN_TICKS;
        p->food_eaten_this_tick = 1;
        state->food_used_this_tick = 1;
    }
    /* Combo eat: simplified — treat as another shark for now */
    if (act_eat == FC_EAT_COMBO && p->combo_timer <= 0 &&
        p->sharks_remaining > 0 && p->current_hp < p->max_hp) {
        int heal = 180;  /* karambwan heals 18 HP = 180 tenths */
        state->pre_eat_hp = p->current_hp;
        state->ep_food_pre_hp_sum += state->pre_eat_hp;
        int hp_missing = p->max_hp - p->current_hp;
        if (heal > hp_missing) state->ep_food_overhealed++;
        state->ep_food_eaten++;
        p->total_food_eaten++;
        p->current_hp += heal;
        if (p->current_hp > p->max_hp) p->current_hp = p->max_hp;
        p->sharks_remaining--;
        p->combo_timer = FC_COMBO_EAT_TICKS;
        p->food_eaten_this_tick = 1;
        state->food_used_this_tick = 1;
    }

    /* ---- Drink prayer potion ---- */
    if (act_drink == FC_DRINK_PRAYER_POT && p->potion_timer <= 0 &&
        p->prayer_doses_remaining > 0 && p->current_prayer < p->max_prayer) {
        state->pre_drink_prayer = p->current_prayer;
        state->ep_pot_pre_prayer_sum += state->pre_drink_prayer;
        int prayer_missing = p->max_prayer - p->current_prayer;
        state->ep_pots_used++;
        if (p->current_prayer > p->max_prayer / 5)
            state->ep_pots_wasted++;
        p->total_potions_used++;
        int restore = fc_prayer_potion_restore(p->prayer_level);
        if (restore > prayer_missing) state->ep_pots_overrestored++;
        p->current_prayer += restore;
        if (p->current_prayer > p->max_prayer) p->current_prayer = p->max_prayer;
        p->prayer_doses_remaining--;
        p->potion_timer = FC_POTION_COOLDOWN_TICKS;
        p->potion_used_this_tick = 1;
        state->prayer_potion_used_this_tick = 1;
    }

    /* ---- Walk-to-tile (high-level pathfinding, heads 5+6) ---- */
    /* When both target_x and target_y are non-zero, BFS pathfind to that tile.
     * This is identical to a human clicking a tile in the viewer. The route is
     * consumed one step per tick by the movement code below. */
    if (act_target_x > 0 && act_target_y > 0) {
        int tx = act_target_x - 1;  /* 1-64 → 0-63 */
        int ty = act_target_y - 1;
        if (tx >= 0 && tx < FC_ARENA_WIDTH && ty >= 0 && ty < FC_ARENA_HEIGHT &&
            state->walkable[tx][ty]) {
            int steps = fc_pathfind_bfs(p->x, p->y, tx, ty, state->walkable,
                                        p->route_x, p->route_y, FC_MAX_ROUTE);
            p->route_len = steps;
            p->route_idx = 0;
            /* Clear attack target — walking to tile cancels combat approach */
            p->attack_target_idx = -1;
            p->approach_target = 0;
        }
    }

    /* ---- Movement ---- */
    /* Three modes (priority order):
     * 1. Route-based (from walk-to-tile action or viewer click): consume steps
     * 2. Directional (RL action head 0): immediate step in direction
     * 3. Idle
     * Route takes priority. If route active, directional input is ignored. */
    if (p->route_idx < p->route_len) {
        /* Consume steps from the route: 1 if walking, 2 if running */
        int steps = p->is_running ? 2 : 1;
        for (int s = 0; s < steps && p->route_idx < p->route_len; s++) {
            int nx = p->route_x[p->route_idx];
            int ny = p->route_y[p->route_idx];
            if (fc_tile_walkable(nx, ny, state->walkable)) {
                /* Update facing based on movement direction */
                int dx = nx - p->x;
                int dy = ny - p->y;
                if (dx != 0 || dy != 0) {
                    /* atan2 of world X delta and negated world Y delta (for Raylib Z) */
                    p->facing_angle = atan2f((float)dx, (float)(-dy)) * (180.0f / 3.14159f);
                }
                p->x = nx;
                p->y = ny;
                state->movement_this_tick = 1;
            }
            p->route_idx++;
        }
    } else if (act_move == FC_MOVE_IDLE) {
        state->idle_this_tick = 1;
    } else if (act_move >= FC_MOVE_WALK_N && act_move <= FC_MOVE_RUN_NW) {
        /* Directional movement (for RL agents) */
        int dx = FC_MOVE_DX[act_move];
        int dy = FC_MOVE_DY[act_move];
        int max_steps = (act_move >= FC_MOVE_RUN_N) ? 2 : 1;
        int moved = fc_move_toward(&p->x, &p->y, dx, dy, max_steps, state->walkable);
        if (moved > 0) {
            state->movement_this_tick = 1;
            p->is_running = (moved >= 2) ? 1 : 0;
        }
    }

    /* ---- Attack target selection ---- */
    if (act_attack > FC_ATTACK_NONE) {
        int slot = act_attack - 1;
        int npc_idx = npc_slot_to_index(state, slot);
        if (npc_idx >= 0) {
            p->attack_target_idx = npc_idx;
            p->approach_target = 1;  /* auto-walk into range, same as human click */
        }
    }

    /* ---- Auto-attack current target ---- */
    /* Like Void CombatMovement: if target set, walk toward it until in range,
     * then attack on cooldown. Player stays still once in range. */
    if (p->attack_target_idx >= 0 && (!p->weapon_uses_ammo || p->ammo_count > 0)) {
        FcNpc* target = &state->npcs[p->attack_target_idx];
        if (!target->active || target->is_dead) {
            p->attack_target_idx = -1;  /* target died, clear */
            p->approach_target = 0;
        } else {
            int dist = fc_distance_to_npc(p->x, p->y, target);
            int weapon_range = p->weapon_range;
            int has_los = fc_has_los_to_npc(
                p->x, p->y, target->x, target->y, target->size, state->walkable);

            if ((dist > weapon_range || !has_los) &&
                p->approach_target && p->route_idx >= p->route_len) {
                /* Walk toward the NPC center. The greedy pathfinder will head
                 * straight toward the target. The route consumer in the movement
                 * section will stop once we're in range with LOS (checked next tick). */
                int npc_cx = target->x + target->size / 2;
                int npc_cy = target->y + target->size / 2;
                p->route_len = fc_pathfind_bfs(p->x, p->y, npc_cx, npc_cy,
                                                state->walkable, p->route_x, p->route_y, FC_MAX_ROUTE);
                /* Trim the route to the first tile that can actually fire. */
                for (int ri = 0; ri < p->route_len; ri++) {
                    int rx = p->route_x[ri], ry = p->route_y[ri];
                    /* Chebyshev distance to nearest NPC footprint tile */
                    int nx = (rx < target->x) ? target->x : (rx > target->x + target->size - 1) ? target->x + target->size - 1 : rx;
                    int ny = (ry < target->y) ? target->y : (ry > target->y + target->size - 1) ? target->y + target->size - 1 : ry;
                    int rdx = (rx > nx) ? rx - nx : nx - rx;
                    int rdy = (ry > ny) ? ry - ny : ny - ry;
                    int rdist = (rdx > rdy) ? rdx : rdy;
                    if (rdist <= weapon_range &&
                        fc_has_los_to_npc(rx, ry, target->x, target->y,
                                          target->size, state->walkable)) {
                        p->route_len = ri + 1;  /* stop here */
                        break;
                    }
                }
                p->route_idx = 0;
            }

            /* Face the attack target */
            {
                float tx = (float)target->x + (float)target->size*0.5f;
                float ty = (float)target->y + (float)target->size*0.5f;
                float dx = tx - ((float)p->x + 0.5f);
                float dy = ty - ((float)p->y + 0.5f);
                if (dx != 0 || dy != 0) {
                    p->facing_angle = atan2f(dx, -dy) * (180.0f / 3.14159f);
                }
            }

            if (dist <= weapon_range && has_los && p->attack_timer <= 0) {
                /* In range — fire attack */
                int att_roll = fc_player_ranged_attack_roll(p, target);
                const FcNpcStats* tstats = fc_npc_get_stats(target->npc_type);
                int def_roll = fc_npc_def_roll(tstats->def_level, tstats->def_bonus);
                float chance = fc_hit_chance(att_roll, def_roll);

                int hit = (fc_rng_float(state) < chance) ? 1 : 0;
                int max_hit = fc_player_ranged_max_hit(p, target);
                int damage = hit ? fc_rng_int(state, max_hit + 1) : 0;

                int delay = fc_ranged_hit_delay(dist);
                fc_queue_pending_hit(target->pending_hits, &target->num_pending_hits,
                                     FC_MAX_PENDING_HITS,
                                     damage, delay, ATTACK_RANGED, -1, 0);

                state->attack_attempt_this_tick = 1;
                p->attack_timer = p->weapon_speed;
                if (p->weapon_uses_ammo && p->ammo_count > 0)
                    p->ammo_count--;
                p->hit_landed_this_tick = 1;  /* flag for viewer hitsplat */

                /* Safespot: attacked with no NPC adjacent (dist <= 1) */
                int any_adjacent = 0;
                for (int j = 0; j < FC_MAX_NPCS; j++) {
                    FcNpc *n2 = &state->npcs[j];
                    if (n2->active && !n2->is_dead) {
                        int d = fc_distance_to_npc(p->x, p->y, n2);
                        if (d <= 1) { any_adjacent = 1; break; }
                    }
                }
                if (!any_adjacent) state->safespot_attack_this_tick = 1;
            }
        }
    }

    if (was_attack_ready) {
        state->ep_attack_ready_ticks++;
        if (state->attack_attempt_this_tick) {
            state->ep_attack_attempt_ticks++;
        }
    }
}

/* ======================================================================== */
/* Decrement player timers                                                   */
/* ======================================================================== */

static void decrement_player_timers(FcPlayer* p) {
    if (p->attack_timer > 0) p->attack_timer--;
    if (p->food_timer > 0) p->food_timer--;
    if (p->potion_timer > 0) p->potion_timer--;
    if (p->combo_timer > 0) p->combo_timer--;
}

/* ======================================================================== */
/* Check terminal conditions                                                 */
/* ======================================================================== */

/* ======================================================================== */
/* Jad healer auto-spawn                                                     */
/* ======================================================================== */

/*
 * Jad healer spawn (from TzhaarFightCave.kt npcLevelChanged handler):
 *   Trigger: Jad HP drops below 150 HP.
 *   Spawns up to 4 Yt-HurKot (fills missing slots: 4 - currently_alive).
 *   Respawn: Only after healers restore Jad to full HP and he crosses the
 *   threshold again. Crossing back above the threshold is not enough to re-arm.
 */
static void check_jad_healers(FcState* state) {
    if (state->current_wave != FC_NUM_WAVES) return;  /* only on wave 63 */

    /* Find Jad */
    for (int i = 0; i < FC_MAX_NPCS; i++) {
        FcNpc* jad = &state->npcs[i];
        if (jad->npc_type != NPC_TZTOK_JAD || !jad->active || jad->is_dead) continue;

        /* Re-arm respawns only after Jad has been healed all the way to full. */
        if (jad->current_hp >= jad->max_hp) {
            state->jad_healers_spawned = 0;
            return;
        }

        if (jad->current_hp >= FC_JAD_HEALER_THRESHOLD_HP_TENTHS) return;

        /* Below threshold — spawn healers if not already spawned this cycle */
        if (state->jad_healers_spawned) return;

        /* Count currently alive healers */
        int alive_healers = 0;
        for (int h = 0; h < FC_MAX_NPCS; h++) {
            if (state->npcs[h].active && !state->npcs[h].is_dead &&
                state->npcs[h].npc_type == NPC_YT_HURKOT) {
                alive_healers++;
            }
        }

        /* Spawn up to 4 total (fill missing slots) */
        int to_spawn = FC_JAD_NUM_HEALERS - alive_healers;
        int offsets[4][2] = {{-2, 0}, {2, 0}, {0, -2}, {0, 2}};
        for (int h = 0; h < to_spawn; h++) {
            int hx = jad->x + offsets[(alive_healers + h) % 4][0];
            int hy = jad->y + offsets[(alive_healers + h) % 4][1];
            /* Clamp to arena */
            if (hx < 1) hx = 1;
            if (hy < 1) hy = 1;
            if (hx >= FC_ARENA_WIDTH - 1) hx = FC_ARENA_WIDTH - 2;
            if (hy >= FC_ARENA_HEIGHT - 1) hy = FC_ARENA_HEIGHT - 2;

            for (int slot = 0; slot < FC_MAX_NPCS; slot++) {
                if (!state->npcs[slot].active) {
                    fc_npc_spawn(&state->npcs[slot], NPC_YT_HURKOT, hx, hy,
                                 state->next_spawn_index++);
                    state->npcs_remaining++;
                    break;
                }
            }
        }
        state->jad_healers_spawned = 1;
        return;
    }
}

/* ======================================================================== */
/* Check terminal conditions                                                 */
/* ======================================================================== */

static void check_terminal(FcState* state) {
    if (state->terminal != TERMINAL_NONE) return;

    /* Player death */
    if (state->player.current_hp <= 0) {
        state->terminal = TERMINAL_PLAYER_DEATH;
        return;
    }

    /* Wave advancement (handles wave-clear and cave-complete) */
    fc_wave_check_advance(state);

    /* Jad healer spawn check */
    check_jad_healers(state);

    /* Tick cap */
    if (state->tick >= FC_MAX_EPISODE_TICKS) {
        state->terminal = TERMINAL_TICK_CAP;
    }
}

/* ======================================================================== */
/* Main tick entry point                                                     */
/* ======================================================================== */

void fc_tick(FcState* state, const int actions[FC_NUM_ACTION_HEADS]) {
    int prayer_active_at_tick_start = (state->player.prayer != PRAYER_NONE);

    /* 1. Clear per-tick flags */
    clear_per_tick_flags(state);

    /* 2. Process player actions */
    process_player_actions(state, actions);

    /* 3. Decrement player timers */
    decrement_player_timers(&state->player);

    /* 4. Prayer drain */
    fc_prayer_drain_tick(&state->player, prayer_active_at_tick_start);

    /* 4b. HP regen (1 HP = 10 tenths every FC_HP_REGEN_INTERVAL ticks) */
    if (state->player.current_hp > 0 && state->player.current_hp < state->player.max_hp) {
        state->player.hp_regen_counter++;
        if (state->player.hp_regen_counter >= FC_HP_REGEN_INTERVAL) {
            state->player.hp_regen_counter = 0;
            state->player.current_hp += 10;  /* 1 HP in tenths */
            if (state->player.current_hp > state->player.max_hp) {
                state->player.current_hp = state->player.max_hp;
            }
        }
    }

    /* 5. NPC AI tick */
    for (int i = 0; i < FC_MAX_NPCS; i++) {
        fc_npc_tick(state, i);
    }

    /* 6. Resolve pending hits */
    fc_resolve_player_pending_hits(state);
    for (int i = 0; i < FC_MAX_NPCS; i++) {
        if (state->npcs[i].active) {
            fc_resolve_npc_pending_hits(state, i);
        }
    }

    /* 6b. Process death timers — dead NPCs stay visible briefly */
    for (int i = 0; i < FC_MAX_NPCS; i++) {
        FcNpc* n = &state->npcs[i];
        if (n->is_dead && n->active) {
            if (n->death_timer > 0) {
                n->death_timer--;
            } else {
                n->active = 0;  /* fully despawn */
            }
        }
    }

    /* 7. Check terminal */
    check_terminal(state);

    /* 8. Episode analytics */
    if (state->player.prayer == PRAYER_PROTECT_MELEE)
        state->ep_ticks_pray_melee++;
    if (state->player.prayer == PRAYER_PROTECT_RANGE)
        state->ep_ticks_pray_range++;
    if (state->player.prayer == PRAYER_PROTECT_MAGIC)
        state->ep_ticks_pray_magic++;
    if (state->player.prayer_changed_this_tick)
        state->ep_prayer_switches++;
    if (state->current_wave >= 63)
        state->ep_reached_wave_63 = 1;

    /* 9. Increment tick */
    state->tick++;
}
