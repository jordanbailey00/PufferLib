#ifndef FC_PATHFINDING_H
#define FC_PATHFINDING_H

#include "fc_types.h"

/* ======================================================================== */
/* Tile queries                                                              */
/* ======================================================================== */

/* Check if a single tile is walkable (bounds + collision). */
int fc_tile_walkable(int x, int y,
                     const uint8_t walkable[FC_ARENA_WIDTH][FC_ARENA_HEIGHT]);

/* Check if an entity of given size can stand at (x,y).
 * All tiles in the [x..x+size-1, y..y+size-1] footprint must be walkable. */
int fc_footprint_walkable(int x, int y, int size,
                          const uint8_t walkable[FC_ARENA_WIDTH][FC_ARENA_HEIGHT]);

/* ======================================================================== */
/* Movement                                                                  */
/* ======================================================================== */

/* Move a size-1 entity from (x,y) toward offset (dx,dy) for up to max_steps.
 * Diagonal-first fallback. Returns number of tiles moved. Updates *x,*y. */
int fc_move_toward(int* x, int* y, int dx, int dy, int max_steps,
                   const uint8_t walkable[FC_ARENA_WIDTH][FC_ARENA_HEIGHT]);

/* Move an NPC of given size one tile closer to (target_x, target_y).
 * Checks that the full NPC footprint fits at the destination.
 * Diagonal-first, then cardinal fallback.
 * Returns 1 if moved, 0 if blocked. */
int fc_npc_step_toward_sized(int* x, int* y, int target_x, int target_y,
                             int size,
                             const uint8_t walkable[FC_ARENA_WIDTH][FC_ARENA_HEIGHT]);

/* Legacy size-1 wrapper (used by existing code). */
int fc_npc_step_toward(int* x, int* y, int target_x, int target_y,
                       const uint8_t walkable[FC_ARENA_WIDTH][FC_ARENA_HEIGHT]);

/* ======================================================================== */
/* Line of sight                                                             */
/* ======================================================================== */

/* Bresenham LOS check between two points.
 * Returns 1 if all tiles along the line are walkable (no wall blocks LOS).
 * Uses the simple walkability grid — walls block LOS. */
int fc_has_line_of_sight(int x0, int y0, int x1, int y1,
                         const uint8_t walkable[FC_ARENA_WIDTH][FC_ARENA_HEIGHT]);

/* LOS check from a size-1 point to the nearest tile of a multi-tile NPC.
 * Finds the closest footprint tile and checks LOS to it. */
int fc_has_los_to_npc(int px, int py, int npc_x, int npc_y, int npc_size,
                      const uint8_t walkable[FC_ARENA_WIDTH][FC_ARENA_HEIGHT]);

/* Returns 1 if the player is in actual melee contact with any tile of the NPC
 * footprint. Diagonal contact is only valid when the corner is open, matching
 * the same corner-cut rules used by movement/pathing. */
int fc_npc_can_melee_player(int player_x, int player_y,
                            int npc_x, int npc_y, int npc_size,
                            const uint8_t walkable[FC_ARENA_WIDTH][FC_ARENA_HEIGHT]);

/* ======================================================================== */
/* BFS pathfinding (for click-to-move)                                       */
/* ======================================================================== */

/* Find a path from (sx,sy) to (dx,dy) using BFS on the walkable grid.
 * Stores the path (sequence of tile coordinates) in out_x[], out_y[].
 * Returns the number of steps (0 if no path or already there).
 * max_steps limits output array size. Path does NOT include the start tile. */
int fc_pathfind_bfs(int sx, int sy, int dx, int dy,
                    const uint8_t walkable[FC_ARENA_WIDTH][FC_ARENA_HEIGHT],
                    int out_x[], int out_y[], int max_steps);

#endif /* FC_PATHFINDING_H */
