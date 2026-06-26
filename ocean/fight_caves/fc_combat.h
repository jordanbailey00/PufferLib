#ifndef FC_COMBAT_H
#define FC_COMBAT_H

#include "fc_types.h"

/* OSRS accuracy formula: returns hit probability in [0,1] */
float fc_hit_chance(int att_roll, int def_roll);

/* NPC combat */
int fc_npc_attack_roll(int att_level, int att_bonus);
int fc_npc_melee_max_hit(int str_level, int str_bonus);

/* Player combat */
int fc_player_def_roll(const FcPlayer* p, int attack_style);
int fc_player_ranged_attack_roll(const FcPlayer* p, const FcNpc* target);
int fc_player_ranged_max_hit(const FcPlayer* p, const FcNpc* target);

/* PvM prayer: returns 1 if prayer blocks the attack style (100% block) */
int fc_prayer_blocks_style(int prayer, int attack_style);

/* Distance to multi-tile NPC (Chebyshev) */
int fc_distance_to_npc(int px, int py, const FcNpc* npc);

/* Hit delay */
int fc_melee_hit_delay(void);
int fc_ranged_hit_delay(int distance);  /* player ranged projectile */
int fc_magic_hit_delay(int distance);   /* generic magic fallback */
int fc_npc_hit_delay(int npc_type, int attack_style, int distance);  /* per-NPC exact timing */

/* NPC defence roll (for player accuracy against NPC) */
int fc_npc_def_roll(int def_level, int def_bonus);

/* Pending hit queue */
int fc_queue_pending_hit(FcPendingHit hits[], int* num_hits, int max_hits,
                         int damage, int ticks, int style, int source_idx,
                         int prayer_drain);

/* Resolve pending hits (call each tick) */
void fc_resolve_player_pending_hits(FcState* state);
void fc_resolve_npc_pending_hits(FcState* state, int npc_idx);

#endif /* FC_COMBAT_H */
