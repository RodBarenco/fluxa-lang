/* tests/tools/handover_no_clock.c — fail-closed check for the platform clock.
 *
 * Builds handover.c the way a bare-metal target does: FLUXA_EMBEDDED set, no
 * FLUXA_HAS_POSIX_CLOCK, and no SDK overriding the weak hooks. That is exactly
 * the state of an integration where someone forgot to wire
 * fluxa_platform_ms_now().
 *
 * What must happen: step 4 refuses the switchover with HANDOVER_ERR_SAFE_POINT
 * and leaves Runtime A untouched. Forgetting the hook costs an upgrade, never
 * the running service.
 *
 * Two cases, because "refuses" alone is not enough — a runtime that refuses
 * even when the clock IS present would be just as broken:
 *
 *   1. no hook          → refused, A intact
 *   2. hook provided    → step 4 proceeds past the clock check
 *
 * Case 2 supplies the hook the way pico-sdk would, by defining the strong
 * symbol that overrides the weak default.
 *
 *   cc -std=c99 -DFLUXA_EMBEDDED=1 -DFLUXA_IPC_NONE=1 -DFLUXA_HAS_FFI=0 \
 *      -Isrc tests/tools/handover_no_clock.c src/handover.c ... -o t && ./t
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "handover.h"

/* Case 2 installs a real clock through this switch. The weak default in
 * handover.c stays in force while it is 0. */
static int   g_clock_enabled = 0;
static long  g_fake_ms       = 1000;

long fluxa_platform_ms_now(void) {
    if (!g_clock_enabled) return -1;      /* same answer as the weak default */
    return g_fake_ms;
}
void fluxa_platform_sleep_us(long us) { (void)us; g_fake_ms += (us / 1000) + 1; }

static int failures = 0;

static void check(int cond, const char *name, const char *detail) {
    if (cond) {
        printf("  PASS  handover_clock/%s\n", name);
    } else {
        printf("  FAIL  handover_clock/%s — %s\n", name, detail);
        failures++;
    }
}

int main(void) {
    printf("── handover: platform clock is fail-closed ──────────────────────\n");

    /* Two runtimes, both at a safe point (call_depth and danger_depth zero),
     * so nothing but the clock can decide the outcome. rt_b is heap-allocated
     * because a refused step 4 runs the normal rollback, which destroys the
     * candidate runtime — that destruction is part of what we are checking. */
    static Runtime rt_a;
    memset(&rt_a, 0, sizeof(rt_a));
    rt_a.cycle_count = 7;
    Runtime *rt_b = (Runtime *)calloc(1, sizeof(Runtime));
    if (!rt_b) { printf("  FAIL  handover_clock/setup — out of memory\n"); return 1; }

    HandoverCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.rt_a                  = &rt_a;
    ctx.rt_b                  = rt_b;
    ctx.state                 = HANDOVER_STATE_DRY_RUN;
    ctx.safe_point_timeout_ms = 5000;
    ctx.grace_period_ms       = 0;

    /* ── Case 1: no clock ───────────────────────────────────────────────── */
    g_clock_enabled = 0;
    long cycle_before = rt_a.cycle_count;

    HandoverResult r = handover_step4_switchover(&ctx);

    check(r == HANDOVER_ERR_SAFE_POINT,
          "no_clock_refuses_switchover",
          "step 4 should return HANDOVER_ERR_SAFE_POINT");
    check(rt_a.cycle_count == cycle_before,
          "no_clock_leaves_runtime_a_intact",
          "Runtime A must not be modified by a refused handover");
    check(ctx.pool_after.entries == NULL,
          "no_clock_performs_no_swap",
          "no pool may be transferred when the switchover is refused");
    check(strstr(ctx.error_msg, "clock") != NULL,
          "no_clock_reports_the_cause",
          "the error message should name the missing clock");

    /* ── Case 2: clock provided by the integration ──────────────────────── */
    rt_b = (Runtime *)calloc(1, sizeof(Runtime));   /* rollback freed the first */
    if (!rt_b) { printf("  FAIL  handover_clock/setup2 — out of memory\n"); return 1; }
    memset(&ctx, 0, sizeof(ctx));
    ctx.rt_a                  = &rt_a;
    ctx.rt_b                  = rt_b;
    ctx.state                 = HANDOVER_STATE_DRY_RUN;
    ctx.safe_point_timeout_ms = 5000;
    ctx.grace_period_ms       = 0;

    g_clock_enabled = 1;
    r = handover_step4_switchover(&ctx);

    check(r == HANDOVER_OK,
          "with_clock_switchover_proceeds",
          "step 4 should complete once the platform clock is wired");
    check(ctx.rt_a_cycle_at_swap == rt_a.cycle_count,
          "with_clock_records_swap_cycle",
          "the swap must record Runtime A's cycle count");

    printf("  → handover_clock: %s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
