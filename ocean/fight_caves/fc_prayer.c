#include "fc_api.h"

/*
 * fc_prayer.c — Prayer activation, drain, and potion restore.
 *
 * OSRS prayer drain (from PrayerDrain.kt):
 *   Each tick: prayerDrainCounter += totalDrainEffect (sum of active prayer drains)
 *   prayerDrainResistance = 60 + (prayerBonus * 2)
 *   While counter > resistance: drain 1 prayer point, counter -= resistance
 *
 *   Protection prayers all cost drain=12 per tick (from prayers.toml).
 *   Prayer points are in tenths (430 = 43 prayer points).
 *   Each "drain 1 point" = subtract 10 tenths.
 *
 * Prayer potion restore:
 *   floor(prayer_level * 0.25) + 7 points per dose.
 *   For level 43: floor(10.75) + 7 = 17 points → 170 in tenths.
 */

#define PRAYER_OVERHEAD_DRAIN_RATE 12

void fc_prayer_drain_tick(FcPlayer* p, int prayer_active_at_tick_start) {
    /* Perfect 1-tick flicks should be free: only accrue drain when some prayer
     * was active both before and after this tick's action processing. */
    if (!prayer_active_at_tick_start || p->prayer == PRAYER_NONE) return;
    if (p->current_prayer <= 0) {
        /* Auto-deactivate if prayer points depleted */
        p->prayer = PRAYER_NONE;
        p->prayer_drain_counter = 0;
        return;
    }

    /* Counter-based drain matching OSRS PrayerDrain.kt exactly:
     * Accumulate drain rate each tick, drain 1 point when counter exceeds resistance. */
    int drain_rate = PRAYER_OVERHEAD_DRAIN_RATE;  /* all 3 protect prayers = 12 */
    int resistance = 60 + 2 * p->prayer_bonus;

    p->prayer_drain_counter += drain_rate;

    while (p->prayer_drain_counter > resistance) {
        p->current_prayer -= 10;  /* drain 1 prayer point (10 tenths) */
        p->prayer_drain_counter -= resistance;

        if (p->current_prayer <= 0) {
            p->current_prayer = 0;
            p->prayer = PRAYER_NONE;
            p->prayer_drain_counter = 0;
            return;
        }
    }
}

void fc_prayer_apply_action(FcPlayer* p, int prayer_action) {
    switch (prayer_action) {
        case 0: /* FC_PRAYER_NO_CHANGE */ break;
        case 1: /* FC_PRAYER_OFF */
            p->prayer = PRAYER_NONE;
            break;
        case 2: /* FC_PRAYER_MAGIC */
            p->prayer = (p->current_prayer > 0) ? PRAYER_PROTECT_MAGIC : PRAYER_NONE;
            break;
        case 3: /* FC_PRAYER_RANGE */
            p->prayer = (p->current_prayer > 0) ? PRAYER_PROTECT_RANGE : PRAYER_NONE;
            break;
        case 4: /* FC_PRAYER_MELEE */
            p->prayer = (p->current_prayer > 0) ? PRAYER_PROTECT_MELEE : PRAYER_NONE;
            break;
    }
}

int fc_prayer_potion_restore(int prayer_level) {
    /* floor(level * 0.25) + 7 points → in tenths */
    return (prayer_level / 4 + 7) * 10;
}
