#ifndef FC_TYPES_H
#define FC_TYPES_H

#include <stdint.h>

/*
 * fc_types.h — Core data structures for Fight Caves simulation.
 *
 * Design rules (from PufferLib OSRS PvP reference):
 *   - All state is flat fields on structs. No nested pointers, no heap alloc per tick.
 *   - Enables cache locality, fast memset reset, linear observation generation.
 *   - All structs are zeroed on reset via memset; zero must be a safe default for every field.
 *
 * PR 2 combat note — PvM prayer semantics:
 *   Protection prayers in Fight Caves BLOCK 100% of the matching NPC attack style.
 *   This is standard OSRS PvM behavior, NOT the PvP 60% reduction from osrs_pvp.
 *   Exceptions (e.g. TzTok-Jad hits through wrong prayer) must be explicit per NPC/attack.
 *   See fc_combat.c (PR 2) for implementation.
 */

/* ======================================================================== */
/* Enums                                                                     */
/* ======================================================================== */

typedef enum {
    ENTITY_PLAYER = 0,
    ENTITY_NPC    = 1
} FcEntityType;

/* NPC type codes — map to OSRS Fight Caves monsters */
typedef enum {
    NPC_NONE       = 0,
    NPC_TZ_KIH     = 1,  /* Lv 22, melee, drains prayer on hit */
    NPC_TZ_KEK     = 2,  /* Lv 45, melee, splits into 2 small on death */
    NPC_TZ_KEK_SM  = 3,  /* Lv 22, melee, small Tz-Kek spawn (from split) */
    NPC_TOK_XIL    = 4,  /* Lv 90, ranged */
    NPC_YT_MEJKOT  = 5,  /* Lv 180, melee + heals nearby NPCs */
    NPC_KET_ZEK    = 6,  /* Lv 360, magic (primary) + melee */
    NPC_TZTOK_JAD  = 7,  /* Lv 702, magic + ranged, prayer switching */
    NPC_YT_HURKOT  = 8,  /* Jad healer, heals Jad if not distracted */
    NPC_TYPE_COUNT  = 9
} FcNpcType;

/* Attack styles */
typedef enum {
    ATTACK_NONE   = 0,
    ATTACK_MELEE  = 1,
    ATTACK_RANGED = 2,
    ATTACK_MAGIC  = 3
} FcAttackStyle;

/* Protection prayers */
typedef enum {
    PRAYER_NONE          = 0,
    PRAYER_PROTECT_MELEE = 1,
    PRAYER_PROTECT_RANGE = 2,
    PRAYER_PROTECT_MAGIC = 3
} FcPrayer;

/* Terminal state codes */
typedef enum {
    TERMINAL_NONE          = 0,
    TERMINAL_PLAYER_DEATH  = 1,
    TERMINAL_CAVE_COMPLETE = 2,
    TERMINAL_TICK_CAP      = 3
} FcTerminalCode;

/* NPC spawn direction for wave rotations */
typedef enum {
    SPAWN_SOUTH      = 0,
    SPAWN_SOUTH_WEST = 1,
    SPAWN_NORTH_WEST = 2,
    SPAWN_SOUTH_EAST = 3,
    SPAWN_CENTER     = 4
} FcSpawnDir;

/* ======================================================================== */
/* Constants                                                                 */
/* ======================================================================== */

/* Arena */
#define FC_ARENA_WIDTH   64
#define FC_ARENA_HEIGHT  64

/* Entity limits */
#define FC_MAX_NPCS      16   /* max simultaneous NPCs in the arena */
#define FC_VISIBLE_NPCS   8   /* max NPCs in observation (see fc_contracts.h) */
#define FC_MAX_PENDING_HITS 8 /* per entity pending hit queue */

/* Waves */
#define FC_NUM_WAVES     63
#define FC_NUM_ROTATIONS 15

/* Consumables (standard Fight Caves loadout) */
#define FC_MAX_SHARKS            20
#define FC_MAX_PRAYER_DOSES      32  /* 8 potions × 4 doses */

/* Tick timing */
#define FC_FOOD_COOLDOWN_TICKS    3  /* food_delay: 3 ticks */
#define FC_POTION_COOLDOWN_TICKS  2  /* drink_delay: 2 ticks (NOT 3 — separate clock from food) */
#define FC_COMBO_EAT_TICKS        1  /* karambwan combo delay after food */
#define FC_MAX_EPISODE_TICKS   200000 /* ~33 hours at 0.6s/tick — force prayer drain */
#define FC_HP_REGEN_INTERVAL     10  /* HP regen: 1 HP every 10 ticks (6 seconds) */

/* Player base stats — defined in fc_player_init.h */
#include "fc_player_init.h"

/* ======================================================================== */
/* Pending Hit (projectile in flight or delayed melee)                       */
/* ======================================================================== */

/*
 * Attacks queue a PendingHit with a tick delay before damage applies.
 * Prayer blocking is locked into the pending hit before impact:
 *   - normal Fight Caves NPCs snapshot prayer on the attack tick
 *   - Jad special-cases ranged/magic to snapshot shortly after the tell
 * The projectile/hitsplat can still land later, but prayer no longer re-checks
 * the live player prayer on impact.
 *
 * This models OSRS projectile flight:
 *   Melee:  0 tick delay (instant)
 *   Ranged: 1 + floor((3 + distance) / 6) ticks
 *   Magic:  1 + floor((1 + distance) / 3) ticks
 *
 * PR 2 note: Protection prayer blocks 100% damage if correct style match.
 * Unlike PvP (60% reduction), PvM prayer fully blocks the hit.
 * Exception: Jad attacks always deal damage if WRONG prayer is active.
 */
typedef struct {
    int active;           /* 1 if this slot is in use */
    int damage;           /* pre-prayer damage roll (0 = miss) */
    int ticks_remaining;  /* ticks until hit resolves */
    int attack_style;     /* FcAttackStyle of the incoming hit */
    int source_npc_idx;   /* index into FcState.npcs[] of the attacker */
    int prayer_drain;     /* prayer points drained on hit (Tz-Kih) */
    int prayer_snapshot;  /* prayer locked for this hit; -1 = snapshot pending */
    int prayer_lock_tick; /* first tick on which prayer_snapshot should be filled */
} FcPendingHit;

/* ======================================================================== */
/* Player                                                                    */
/* ======================================================================== */

typedef struct {
    /* Position */
    int x, y;

    /* Vitals (in tenths for precision: 700 = 70.0 HP) */
    int current_hp, max_hp;
    int current_prayer, max_prayer;

    /* Active prayer */
    int prayer;  /* FcPrayer enum */

    /* Prayer drain counter (OSRS counter-based system from PrayerDrain.kt):
     * Each tick: counter += active prayer drain rate (12 for protect prayers).
     * When counter > resistance (60 + 2*prayer_bonus): drain 1 point, counter -= resistance.
     * This field is an integer accumulator, NOT in tenths. */
    int prayer_drain_counter;

    /* Consumables */
    int sharks_remaining;
    int prayer_doses_remaining;

    /* Timers (tick countdown, 0 = ready) */
    int attack_timer;
    int food_timer;
    int potion_timer;
    int combo_timer;    /* karambwan combo eat delay */

    /* Run */
    int run_energy;     /* 0-10000 (100.00%) */
    int is_running;     /* 1 if run mode active */

    /* Combat stats (from FightCaveEpisodeInitializer.kt) */
    int attack_level;
    int strength_level;
    int defence_level;
    int ranged_level;
    int prayer_level;
    int magic_level;
    int weapon_kind;
    int weapon_uses_ammo;
    int weapon_speed;
    int weapon_range;

    /* Equipment bonuses (exact values from Void 634 cache item definitions) */
    int ranged_attack_bonus;
    int ranged_strength_bonus;
    int defence_stab, defence_slash, defence_crush;
    int defence_magic, defence_ranged;
    int prayer_bonus;

    /* Ammo */
    int ammo_count;

    /* HP regen counter (ticks since last regen) */
    int hp_regen_counter;

    /* Click-to-move route (like RSMod RouteDestination / Void walkTo).
     * Set once on click, consumed one step per tick until empty.
     * When route_len == route_idx, player stands still. */
    #define FC_MAX_ROUTE 64
    int route_x[FC_MAX_ROUTE];
    int route_y[FC_MAX_ROUTE];
    int route_len;      /* total steps in current route */
    int route_idx;      /* next step to consume (0..route_len-1) */

    /* Facing direction — angle in degrees, set each movement step */
    float facing_angle;

    /* Attack target — NPC array index, or -1 for none. */
    int attack_target_idx;
    /* 1 = player explicitly clicked this NPC (approach + attack).
     * 0 = auto-retaliate set target (attack in place only, no approach). */
    int approach_target;

    /* Pending hits (from NPC attacks in flight) */
    FcPendingHit pending_hits[FC_MAX_PENDING_HITS];
    int num_pending_hits;

    /* Per-tick event flags (cleared each tick, used for obs/reward/hitsplats) */
    int damage_taken_this_tick;
    int hit_style_this_tick;    /* FcAttackStyle of the last hit that resolved this tick */
    int hit_source_npc_type;    /* FcNpcType of the NPC that landed the last hit this tick */
    int hit_locked_prayer_this_tick; /* FcPrayer snapshot used for the last resolved hit */
    int hit_blocked_this_tick;  /* 1 if the last resolved hit this tick was prayer-blocked */
    int hit_landed_this_tick;
    int food_eaten_this_tick;
    int potion_used_this_tick;
    int prayer_changed_this_tick;

    /* Cumulative stats (for reward/logging) */
    int total_damage_taken;
    int total_food_eaten;
    int total_potions_used;
} FcPlayer;

/* ======================================================================== */
/* NPC                                                                       */
/* ======================================================================== */

typedef struct {
    /* Identity */
    int active;       /* 1 if alive and in the arena */
    int npc_type;     /* FcNpcType */
    int spawn_index;  /* unique index across the episode, stable for NPC slot ordering */

    /* Position */
    int x, y;
    int size;         /* tile size: 1 for most, 2+ for larger NPCs */

    /* Vitals */
    int current_hp, max_hp;
    int is_dead;
    int death_timer;      /* ticks remaining before despawn (0 = despawn immediately) */

    /* Combat */
    int attack_style;       /* FcAttackStyle: what this NPC attacks with */
    int attack_timer;       /* tick countdown to next attack */
    int attack_speed;       /* ticks between attacks */
    int attack_range;       /* tile distance for ranged/magic, 1 for melee */
    int max_hit;            /* max damage this NPC can roll */

    /* AI */
    int movement_speed;     /* 1 = walk, 2 = run */

    /* Yt-MejKot healing */
    int heal_timer;         /* ticks until next heal attempt */
    int heal_amount;        /* HP healed per proc */

    /* Yt-HurKot (Jad healer) */
    int healer_distracted;  /* 1 if player has attacked this healer */
    int heal_target_idx;    /* NPC index of the entity being healed (Jad) */

    /* Per-tick event flags */
    int damage_taken_this_tick;
    int died_this_tick;

    /* Pending hits (player attacks in flight toward this NPC) */
    FcPendingHit pending_hits[FC_MAX_PENDING_HITS];
    int num_pending_hits;
} FcNpc;

/* ======================================================================== */
/* Wave entry (spawn table row)                                              */
/* ======================================================================== */

#define FC_MAX_SPAWNS_PER_WAVE 6

typedef struct {
    int npc_types[FC_MAX_SPAWNS_PER_WAVE];  /* FcNpcType per spawn */
    int num_spawns;
} FcWaveEntry;

/* ======================================================================== */
/* Render entity (value type for viewer — filled by fc_fill_render_entities) */
/* ======================================================================== */

/*
 * The viewer never reads FcPlayer/FcNpc directly. It receives an array of
 * these render entities via the fc_fill_render_entities callback.
 * This decouples rendering from simulation internals.
 */
typedef struct {
    int entity_type;    /* FcEntityType */
    int npc_type;       /* FcNpcType (0 for player) */
    int x, y;
    int size;           /* tile size */
    int current_hp, max_hp;
    int attack_style;   /* FcAttackStyle: current/last attack style */
    int prayer;         /* FcPrayer: active prayer (player only) */
    int is_dead;

    /* Per-tick events for hitsplat/animation rendering */
    int damage_taken_this_tick;
    int hit_landed_this_tick;
    int died_this_tick;

    int npc_slot;       /* NPC array index (for stable interpolation lookup) */
} FcRenderEntity;

#define FC_MAX_RENDER_ENTITIES (1 + FC_MAX_NPCS)  /* player + NPCs */

/* ======================================================================== */
/* Top-level simulation state                                                */
/* ======================================================================== */

typedef struct {
    FcPlayer player;
    FcNpc npcs[FC_MAX_NPCS];

    /* Wave progression */
    int current_wave;       /* 1-indexed: 1..63. 0 = not started */
    int rotation_id;        /* 0..14, selected at episode start */
    int npcs_remaining;     /* count of active (alive) NPCs in current wave */
    int total_npcs_killed;
    int next_spawn_index;   /* monotonic counter for NPC spawn ordering */

    /* Tick */
    int tick;

    /* Terminal */
    int terminal;           /* FcTerminalCode */

    /* RNG — XORshift32, single state, seeded at reset */
    uint32_t rng_state;
    uint32_t rng_seed;      /* saved for replay */

    /* Arena walkability (1 = walkable, 0 = obstacle) */
    uint8_t walkable[FC_ARENA_WIDTH][FC_ARENA_HEIGHT];

    /* Jad healer state */
    int jad_healers_spawned;  /* 1 until Jad has been healed back to full HP */

    /* Per-tick aggregated event flags (for reward features) */
    int damage_dealt_this_tick;
    int hits_landed_this_tick;   /* count of player pending-hits that resolved on NPCs this tick */
    int damage_taken_this_tick;
    int npcs_killed_this_tick;
    int wave_just_cleared;
    int jad_damage_this_tick;
    int jad_killed;
    int correct_jad_prayer;
    int wrong_jad_prayer;
    int correct_danger_prayer;
    int wrong_danger_prayer;
    int attack_attempt_this_tick;
    int invalid_action_this_tick;
    int movement_this_tick;
    int idle_this_tick;
    int food_used_this_tick;
    int prayer_potion_used_this_tick;
    int pre_eat_hp;                 /* HP before eating (for reward threshold check) */
    int pre_drink_prayer;           /* prayer before drinking (for reward threshold check) */
    int jad_heal_procs_this_tick;   /* number of Yt-HurKot heal procs that restored Jad HP */

    /* Episode-level analytics (cumulative, zeroed on fc_reset via memset) */
    int ep_ticks_pray_melee;    /* ticks with protect melee active */
    int ep_ticks_pray_range;    /* ticks with protect range active */
    int ep_ticks_pray_magic;    /* ticks with protect magic active */
    int ep_correct_blocks;      /* hits correctly blocked by matching prayer */
    int ep_wrong_prayer_hits;   /* hits where prayer active but wrong type */
    int ep_no_prayer_hits;      /* hits where no prayer was active */
    int ep_damage_blocked;      /* total damage prevented by correct prayer */
    int ep_prayer_switches;     /* number of prayer changes */
    int ep_pots_used;           /* prayer pot doses consumed */
    int ep_pots_wasted;         /* doses consumed when prayer was above 20% */
    int ep_pot_pre_prayer_sum;  /* prayer points before each potion use */
    int ep_food_eaten;          /* sharks consumed */
    int ep_food_pre_hp_sum;     /* HP before each food use */
    int ep_food_overhealed;     /* sharks that overhealed (wasted HP) */
    int ep_pots_overrestored;   /* doses that over-restored (wasted prayer) */
    int ep_tokxil_melee_ticks;  /* ticks with any Tok-Xil at melee distance */
    int ep_ketzek_melee_ticks;  /* ticks with any Ket-Zek at melee distance */
    int ep_attack_ready_ticks;  /* ticks where attack cooldown was ready */
    int ep_attack_attempt_ticks;/* ready ticks where a real attack fired */
    int safespot_attack_this_tick; /* 1 if player attacked with no NPC adjacent */
    int ep_reached_wave_63;     /* 1 if episode reached Jad wave */
    int ep_jad_killed;          /* 1 if Jad died at any point this episode */
    int wave_start_tick;        /* tick when current wave was spawned */
    int ep_max_wave_ticks;      /* longest single wave duration in ticks */
    int ep_max_wave_ticks_wave; /* which wave number that was */
} FcState;

#endif /* FC_TYPES_H */
