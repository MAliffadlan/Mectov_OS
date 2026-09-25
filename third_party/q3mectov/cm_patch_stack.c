/* cm_patch_stack.c — id's cm_patch.c, compiled with its big grids off the stack.
 *
 * Why this file exists (v38.109, Q3 phase 8)
 * ------------------------------------------
 * cm_patch.c builds a patch mesh's collision form out of two locals sized for
 * the format's maximum grid, MAX_GRID_SIZE == 129:
 *
 *     MAC_STATIC cGrid_t grid;                    129*129*3 floats  = 199,692 B
 *     MAC_STATIC int gridPlanes[129][129][2];     129*129*2 ints    = 133,128 B
 *
 * Both are spelled MAC_STATIC, and in id's own q_shared.h MAC_STATIC is defined
 * as EMPTY on every platform this release supports. The comment on the Linux
 * branch says what was intended — "the mac compiler can't handle >32k of locals,
 * so we just waste space and make big arrays static" — and then the #define is
 * empty anyway. On Linux and Windows that is harmless: the process stack is 8 MB,
 * so 333 KB of locals is invisible.
 *
 * In this kernel it is fatal. Task stacks here are measured in tens of KB, and
 * the first .bsp that contains a patch mesh takes the whole machine down with a
 * double fault inside CM_LoadMap — measured, not guessed: the port's own
 * -fstack-usage build reports CM_GeneratePatchCollide at 332,976 bytes of frame,
 * against a 64 KB kernel stack. Every arena before this release was built from
 * boxes, so no patch was ever loaded and the hazard stayed hidden.
 *
 * The fix is what id's comment says it meant to do, applied where the port can
 * apply it without editing id's tree — third_party/q3a/ stays verbatim (see
 * scripts/vendor_q3a.sh), because "the source we ported" is worth more than a
 * one-line patch that would make every future diff of that tree ambiguous. So
 * the port compiles the file THROUGH this one:
 *
 *   1. q_shared.h is the one header that defines MAC_STATIC, and it carries a
 *      normal include guard. Reading it first here (cm_local.h includes it too,
 *      and that second inclusion then does nothing) is what makes the next step
 *      possible.
 *   2. MAC_STATIC is redefined to `static`.
 *   3. Only then is cm_patch.c included, so its two array declarations pick up
 *      the port's definition and land in .bss instead of in a frame.
 *
 * The order matters: cm_local.h and cm_polylib.h both LACK include guards, so
 * this file must not include them itself — cm_patch.c's own includes are the
 * only ones that may run.
 *
 * What changes as a result: the two grids become file-scope objects, which is
 * what they are on the platforms where id does define MAC_STATIC that way. The
 * cost is that patch collision generation is no longer re-entrant — it never
 * was, in any build where the arrays are static, and this kernel generates patch
 * collision from one thread during map load and nowhere else.
 *
 * Worst frame after the change, per the same -fstack-usage build:
 * CM_LoadMap's chain peaks at CMod_LoadPatches (12,368 B), and a runtime point
 * trace against a patch peaks at CM_TracePointThroughPatchCollide (16,448 B,
 * which is `frontFacing[MAX_PATCH_PLANES]` + `intersection[MAX_PATCH_PLANES]`).
 * Both fit a 64 KB stack with room; neither is reachable from the other.
 */
#include "../q3a/code/game/q_shared.h"

#undef MAC_STATIC
#define MAC_STATIC static

#include "../q3a/code/qcommon/cm_patch.c"
