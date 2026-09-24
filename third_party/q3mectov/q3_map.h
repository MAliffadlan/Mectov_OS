/* q3_map.h — MCTBSP1 map loader for the Mectov Q3 client (v38.104, phase 4).
 *
 * Phase 3 drew a world hardcoded in q3cl_render.c (arena constants shared
 * with the client's clamp) and spawned the player at fixed coordinates.
 * Phase 4 moves world geometry, the player start AND the bots into a data
 * file the engine's own filesystem loads: a small text format, MCTBSP1 —
 * brush-based like Quake's map sources, but AABB brushes with per-face
 * colours instead of planes/textures, because this renderer has no lightmap
 * or texture machinery and the port ships no pak0.
 *
 * Data flow (see q3_client.c):
 *   q3play_start()  stages /ext2/mectov1.map (or the embedded fallback) into
 *                   the tmpfs homepath as baseq3/maps/mectov1.map
 *   CL_StartHunkUsers()  ->  q3map_load("maps/mectov1.map")
 *                        ->  FS_ReadFile (the ENGINE's loader — the same
 *                            entry cm_load.c uses for real .bsp files)
 *                        ->  parse + validate -> q3map_set_current
 *                        ->  q3ref_set_map(q3map_current())   [renderer]
 *                        ->  player state + movement clamps from the map
 *
 * Everything is static: the map is small (tens of brushes), the client task
 * already owns its hunk/zone, and a kmalloc'd world would only add a free
 * path to get wrong at shutdown.
 */
#ifndef Q3_MAP_H
#define Q3_MAP_H

#include "../q3/code/qcommon/q_shared.h"

#define Q3MAP_MAGIC       "MCTBSP1"
#define Q3MAP_VERSION     1
#define Q3MAP_MAX_BRUSHES 64
#define Q3MAP_MAX_BOTS    8
/* The engine's search path cannot find this until Com_Init has run, so the
 * staging step must be able to write the map without the loader: identical
 * text to assets/maps/mectov1.map (keep in sync — it is the same document). */
extern const char Q3MAP_EMBEDDED_MECTOV1[];

typedef struct q3map_s q3map_t;   /* named so q3cl_render.h can fwd-declare */

struct q3map_s {
    char         name[32];
    float        bounds_half;  /* horizontal backstop clamp for movement   */
    float        spawn[5];     /* x y z yaw pitch — the player start state */
    int          n_brushes;
    int          n_bots;
    struct {
        float mins[3], maxs[3];
        float rgb[3];
    }            brushes[Q3MAP_MAX_BRUSHES];
    struct {
        float x, y, z;          /* base elevation (sits on the floor at z=0) */
        float w, d, h;
        float rgb[3];
    }            bots[Q3MAP_MAX_BOTS];
};

/* Load `qpath` through the engine FS. Returns 0 and installs the map as
 * current on success; -1 when the file is missing, unparsable or fails
 * validation (the current map is then left untouched / empty). */
int q3map_load(const char *qpath);

/* 0 = no map loaded (renderer + client fall back to phase-3 behaviour). */
const q3map_t *q3map_current(void);

#endif /* Q3_MAP_H */
