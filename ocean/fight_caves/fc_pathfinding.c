#include "fc_pathfinding.h"

/*
 * fc_pathfinding.c — Grid movement, footprint checks, and LOS for Fight Caves.
 *
 * Key design:
 *   - NPCs have sizes 1-5 (Jad and Ket-Zek are 5x5!). Movement must check
 *     the entire footprint at the destination tile.
 *   - LOS uses Bresenham line algorithm on the walkability grid.
 *   - All functions use the authoritative walkable[64][64] grid, never visual mesh.
 */

/* ======================================================================== */
/* Tile queries                                                              */
/* ======================================================================== */

int fc_tile_walkable(int x, int y,
                     const uint8_t walkable[FC_ARENA_WIDTH][FC_ARENA_HEIGHT]) {
    if (x < 0 || x >= FC_ARENA_WIDTH || y < 0 || y >= FC_ARENA_HEIGHT) return 0;
    return walkable[x][y];
}

int fc_footprint_walkable(int x, int y, int size,
                          const uint8_t walkable[FC_ARENA_WIDTH][FC_ARENA_HEIGHT]) {
    /* Check all tiles in the NPC's [x..x+size-1, y..y+size-1] footprint */
    for (int dx = 0; dx < size; dx++) {
        for (int dy = 0; dy < size; dy++) {
            if (!fc_tile_walkable(x + dx, y + dy, walkable)) return 0;
        }
    }
    return 1;
}

/* ======================================================================== */
/* Size-1 movement (player, small NPCs)                                      */
/* ======================================================================== */

int fc_move_toward(int* x, int* y, int dx, int dy, int max_steps,
                   const uint8_t walkable[FC_ARENA_WIDTH][FC_ARENA_HEIGHT]) {
    int tx = *x + dx;
    int ty = *y + dy;
    int steps = 0;

    for (int step = 0; step < max_steps; step++) {
        if (*x == tx && *y == ty) break;

        int sx = 0, sy = 0;
        if (tx > *x) sx = 1; else if (tx < *x) sx = -1;
        if (ty > *y) sy = 1; else if (ty < *y) sy = -1;

        /* Try diagonal first, then x-only, then y-only */
        if (sx != 0 && sy != 0 && fc_tile_walkable(*x + sx, *y + sy, walkable)) {
            *x += sx; *y += sy; steps++;
        } else if (sx != 0 && fc_tile_walkable(*x + sx, *y, walkable)) {
            *x += sx; steps++;
        } else if (sy != 0 && fc_tile_walkable(*x, *y + sy, walkable)) {
            *y += sy; steps++;
        } else {
            break;
        }
    }
    return steps;
}

/* ======================================================================== */
/* Size-aware NPC movement                                                   */
/* ======================================================================== */

int fc_npc_step_toward_sized(int* x, int* y, int target_x, int target_y,
                             int size,
                             const uint8_t walkable[FC_ARENA_WIDTH][FC_ARENA_HEIGHT]) {
    int dx = 0, dy = 0;
    if (target_x > *x) dx = 1; else if (target_x < *x) dx = -1;
    if (target_y > *y) dy = 1; else if (target_y < *y) dy = -1;

    if (dx == 0 && dy == 0) return 0;

    /* Try diagonal, then x-only, then y-only.
     * Each candidate must have the full footprint walkable. */
    if (dx != 0 && dy != 0 &&
        fc_footprint_walkable(*x + dx, *y + dy, size, walkable)) {
        *x += dx; *y += dy; return 1;
    }
    if (dx != 0 && fc_footprint_walkable(*x + dx, *y, size, walkable)) {
        *x += dx; return 1;
    }
    if (dy != 0 && fc_footprint_walkable(*x, *y + dy, size, walkable)) {
        *y += dy; return 1;
    }
    return 0;
}

/* Legacy size-1 wrapper */
int fc_npc_step_toward(int* x, int* y, int target_x, int target_y,
                       const uint8_t walkable[FC_ARENA_WIDTH][FC_ARENA_HEIGHT]) {
    return fc_npc_step_toward_sized(x, y, target_x, target_y, 1, walkable);
}

/* ======================================================================== */
/* Line of sight — Bresenham                                                 */
/* ======================================================================== */

int fc_has_line_of_sight(int x0, int y0, int x1, int y1,
                         const uint8_t walkable[FC_ARENA_WIDTH][FC_ARENA_HEIGHT]) {
    int dx = x1 - x0;
    int dy = y1 - y0;
    int sx = (dx > 0) ? 1 : (dx < 0) ? -1 : 0;
    int sy = (dy > 0) ? 1 : (dy < 0) ? -1 : 0;

    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;

    int err = dx - dy;
    int cx = x0, cy = y0;

    while (cx != x1 || cy != y1) {
        /* Check intermediate tiles (skip source tile) */
        if (cx != x0 || cy != y0) {
            if (!fc_tile_walkable(cx, cy, walkable)) return 0;
        }

        int e2 = 2 * err;
        if (e2 > -dy) {
            err -= dy;
            cx += sx;
        }
        if (e2 < dx) {
            err += dx;
            cy += sy;
        }
    }

    return 1;  /* all intermediate tiles walkable */
}

int fc_has_los_to_npc(int px, int py, int npc_x, int npc_y, int npc_size,
                      const uint8_t walkable[FC_ARENA_WIDTH][FC_ARENA_HEIGHT]) {
    /* Find the closest tile of the NPC footprint to the player */
    int cx = px, cy = py;
    if (cx < npc_x) cx = npc_x;
    else if (cx > npc_x + npc_size - 1) cx = npc_x + npc_size - 1;
    if (cy < npc_y) cy = npc_y;
    else if (cy > npc_y + npc_size - 1) cy = npc_y + npc_size - 1;

    return fc_has_line_of_sight(px, py, cx, cy, walkable);
}

int fc_npc_can_melee_player(int player_x, int player_y,
                            int npc_x, int npc_y, int npc_size,
                            const uint8_t walkable[FC_ARENA_WIDTH][FC_ARENA_HEIGHT]) {
    for (int dx = 0; dx < npc_size; dx++) {
        for (int dy = 0; dy < npc_size; dy++) {
            int tile_x = npc_x + dx;
            int tile_y = npc_y + dy;
            int step_x = tile_x - player_x;
            int step_y = tile_y - player_y;
            int abs_x = (step_x < 0) ? -step_x : step_x;
            int abs_y = (step_y < 0) ? -step_y : step_y;

            if (abs_x > 1 || abs_y > 1) continue;
            if (abs_x == 0 && abs_y == 0) continue;

            /* Diagonal melee contact is only valid when the corner is open. */
            if (abs_x == 1 && abs_y == 1 &&
                (!fc_tile_walkable(player_x, tile_y, walkable) ||
                 !fc_tile_walkable(tile_x, player_y, walkable))) {
                continue;
            }

            return 1;
        }
    }

    return 0;
}

/* ======================================================================== */
/* BFS pathfinding                                                           */
/* ======================================================================== */

/*
 * Generate a movement route from (sx,sy) to (dx,dy).
 *
 * Uses greedy walk first (OSRS-style direct movement toward target).
 * If greedy gets stuck on an obstacle, falls back to BFS to find a path around it.
 */
static int greedy_walk(int sx, int sy, int dx, int dy,
                       const uint8_t walkable[FC_ARENA_WIDTH][FC_ARENA_HEIGHT],
                       int out_x[], int out_y[], int max_steps) {
    int cx = sx, cy = sy;
    int steps = 0;

    while ((cx != dx || cy != dy) && steps < max_steps) {
        int sdx = 0, sdy = 0;
        if (dx > cx) sdx = 1; else if (dx < cx) sdx = -1;
        if (dy > cy) sdy = 1; else if (dy < cy) sdy = -1;

        int moved = 0;
        /* Try diagonal */
        if (sdx != 0 && sdy != 0 &&
            fc_tile_walkable(cx + sdx, cy + sdy, walkable) &&
            fc_tile_walkable(cx + sdx, cy, walkable) &&
            fc_tile_walkable(cx, cy + sdy, walkable)) {
            cx += sdx; cy += sdy; moved = 1;
        }
        /* Try primary axis first (larger distance), then secondary */
        else {
            int adx = (dx > cx) ? dx - cx : cx - dx;
            int ady = (dy > cy) ? dy - cy : cy - dy;
            if (adx >= ady && sdx != 0 && fc_tile_walkable(cx + sdx, cy, walkable)) {
                cx += sdx; moved = 1;
            } else if (sdy != 0 && fc_tile_walkable(cx, cy + sdy, walkable)) {
                cy += sdy; moved = 1;
            } else if (sdx != 0 && fc_tile_walkable(cx + sdx, cy, walkable)) {
                cx += sdx; moved = 1;
            }
        }

        if (!moved) break;  /* stuck — let BFS take over */
        out_x[steps] = cx;
        out_y[steps] = cy;
        steps++;
    }
    return steps;
}

static int bfs_walk(int sx, int sy, int dx, int dy,
                    const uint8_t walkable[FC_ARENA_WIDTH][FC_ARENA_HEIGHT],
                    int out_x[], int out_y[], int max_steps) {
    int8_t pdx[FC_ARENA_WIDTH][FC_ARENA_HEIGHT];
    int8_t pdy[FC_ARENA_WIDTH][FC_ARENA_HEIGHT];
    uint8_t vis[FC_ARENA_WIDTH][FC_ARENA_HEIGHT];
    int qx[FC_ARENA_WIDTH * FC_ARENA_HEIGHT];
    int qy[FC_ARENA_WIDTH * FC_ARENA_HEIGHT];
    int qh = 0, qt = 0;
    static const int DD[8][2] = {{0,1},{1,1},{1,0},{1,-1},{0,-1},{-1,-1},{-1,0},{-1,1}};

    for (int x = 0; x < FC_ARENA_WIDTH; x++)
        for (int y = 0; y < FC_ARENA_HEIGHT; y++) vis[x][y] = 0;

    vis[sx][sy] = 1;
    qx[qt] = sx; qy[qt] = sy; qt++;
    int found = 0;
    while (qh < qt) {
        int cx = qx[qh], cy = qy[qh]; qh++;
        for (int d = 0; d < 8; d++) {
            int nx = cx + DD[d][0], ny = cy + DD[d][1];
            if (nx < 0 || nx >= FC_ARENA_WIDTH || ny < 0 || ny >= FC_ARENA_HEIGHT) continue;
            if (vis[nx][ny] || !walkable[nx][ny]) continue;
            if (DD[d][0] != 0 && DD[d][1] != 0) {
                if (!walkable[cx+DD[d][0]][cy] || !walkable[cx][cy+DD[d][1]]) continue;
            }
            vis[nx][ny] = 1;
            pdx[nx][ny] = (int8_t)(-DD[d][0]);
            pdy[nx][ny] = (int8_t)(-DD[d][1]);
            qx[qt] = nx; qy[qt] = ny; qt++;
            if (nx == dx && ny == dy) { found = 1; break; }
        }
        if (found) break;
    }
    if (!found) return 0;

    int px[FC_ARENA_WIDTH * FC_ARENA_HEIGHT], py[FC_ARENA_WIDTH * FC_ARENA_HEIGHT];
    int plen = 0;
    int bx = dx, by = dy;
    while (bx != sx || by != sy) {
        px[plen] = bx; py[plen] = by; plen++;
        int td = pdx[bx][by], te = pdy[bx][by];
        bx += td; by += te;
    }
    int steps = (plen < max_steps) ? plen : max_steps;
    for (int i = 0; i < steps; i++) {
        out_x[i] = px[plen - 1 - i];
        out_y[i] = py[plen - 1 - i];
    }
    return steps;
}

int fc_pathfind_bfs(int sx, int sy, int dx, int dy,
                    const uint8_t walkable[FC_ARENA_WIDTH][FC_ARENA_HEIGHT],
                    int out_x[], int out_y[], int max_steps) {
    if (sx == dx && sy == dy) return 0;
    if (!fc_tile_walkable(dx, dy, walkable)) return 0;

    /* Try greedy first — gives natural OSRS movement feel */
    int steps = greedy_walk(sx, sy, dx, dy, walkable, out_x, out_y, max_steps);

    /* Check if greedy reached the destination */
    if (steps > 0 && out_x[steps-1] == dx && out_y[steps-1] == dy)
        return steps;

    /* Greedy got stuck — fall back to BFS for full pathfinding around obstacles */
    return bfs_walk(sx, sy, dx, dy, walkable, out_x, out_y, max_steps);
}
