/*
 * bone_hook.h -- Stateful instrumentation hook for pso.exe 0x61EA00 (v3.3)
 *
 * KEY FINDINGS FROM PREVIOUS RUN:
 *   - All 4506 events: same call site 0x617455, same ptr 01301910, absmax=2000 every frame
 *   - degen=0, eax=1 always -> detector was checking wrong condition (see below)
 *   - 2000.0f is the Dragon's world-space translation (legitimate)
 *   - F04488 had XYZRHW corruption with NO bone events nearby -> some corruption
 *     does NOT go through 0x61EA00 at all
 *
 * THIS VERSION:
 *   - Tracks previous matrix per ptr/caller pair (delta detection)
 *   - Logs on: NaN/Inf, absmax>10000, eax=0, is_identity=1, zero_rot=1,
 *     OR sudden delta vs previous frame (amplification)
 *   - Per-caller hit counters (not just suspicious events)
 *   - Full 16-float matrix dump on first call per ptr and on any suspicious event
 *   - Per-caller stat summary flushed every 300 frames
 */

#pragma once
#include <windows.h>
#include <math.h>

#define BONE_GLOBAL_PTR  0x007BB7E0
#define BONE_LOG_CAP     128
#define BONE_TRACK_CAP   16    /* max unique ptr/caller pairs to track */
#define BONE_DELTA_THRESH 500.0f  /* flag if any element jumps by this much */
#define BONE_ABS_THRESH  10000.0f /* flag if any element exceeds this */

/* v3.2: high-Y ghost-transform suppression toggle.
 * Default on. Build with -DBONE_SUPPRESS_HIGH_Y=0 to disable for A/B test. */
#ifndef BONE_SUPPRESS_HIGH_Y
#define BONE_SUPPRESS_HIGH_Y 1
#endif

/* ---- per-event log entry ---- */
typedef struct {
    int    frame;
    DWORD  caller_ret;
    void*  actual_ptr;
    float  out_absmax;
    float  delta_max;     /* max change vs previous matrix for same ptr/caller */
    int    has_nan, has_inf;
    int    is_identity; /* mat[0,5,10]==1.0 (was "degen" -- actually the healthy case) */
    int    zero_rot;    /* mat[0..2,4..6,8..10] all 0.0 -- the Matrix B pattern */
    int    degenerate;  /* kept for log format compat; same as zero_rot now */
    DWORD  fn_eax;
    float  mat[16];       /* full matrix snapshot */
    char   reason[32];    /* why it was logged */
} BoneLogEntry;

/* ---- per ptr/caller state (delta tracking) ---- */
typedef struct {
    void*  ptr;
    DWORD  caller_ret;
    float  prev_mat[16];
    int    seen;          /* 1 = has a previous snapshot */
    DWORD  hit_count;     /* total calls for this pair */
    int    last_xyz_frame; /* throttle [BONE-XYZ] per (ptr,caller); -big = never */
} BoneTracker;

static BoneLogEntry g_bone_log[BONE_LOG_CAP];
static volatile int g_bone_log_head  = 0;

static BoneTracker  g_bone_trackers[BONE_TRACK_CAP];
static int          g_bone_tracker_n = 0;

static int  g_bone_hook_active = 0;
static int  g_bone_stat_frame  = 0;   /* frame of last stat flush */
static int  g_bone_xyz_frame   = -999999; /* throttle [BONE-XYZ] translation logs */
static int  g_bone_pre_frame   = -999999; /* v3.3 throttle [BONE-PRE] logs */
static unsigned int g_high_y_suppressed = 0; /* v3.2 counter */
static unsigned int g_setxform_highy_seen = 0; /* v3.3 setxform high_y match counter */

typedef DWORD (__attribute__((thiscall)) *BoneFn)(void* self);
static BoneFn g_real_bone_fn = (BoneFn)0x61EA00;

extern int g_frame;
static void log_f(const char* msg);
static void log_line(const char* msg);

/* Dragon-active flag: set each frame when caller_ret==0x0061745A fires.
 * Defined non-static in d3d8_widescreen.c; reset at top of each Present.
 * NOTE: fires for non-dragon lobby entities too -- use only as a loose gate. */
extern int   g_dragon_active;
extern int   g_dragon_active_frame;
extern void* g_dragon_ptr;  /* captured dynamically; NULL until first dragon bone fires */

/* ------------------------------------------------------------------ */
static BoneTracker* bone_get_tracker(void* ptr, DWORD caller_ret) {
    for (int i = 0; i < g_bone_tracker_n; i++)
        if (g_bone_trackers[i].ptr == ptr && g_bone_trackers[i].caller_ret == caller_ret)
            return &g_bone_trackers[i];
    if (g_bone_tracker_n < BONE_TRACK_CAP) {
        BoneTracker* t = &g_bone_trackers[g_bone_tracker_n++];
        t->ptr = ptr; t->caller_ret = caller_ret;
        t->seen = 0; t->hit_count = 0;
        t->last_xyz_frame = -999999;
        return t;
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
static DWORD __attribute__((thiscall)) hook_bone_fn(void* self) {
    DWORD caller_ret = (DWORD)__builtin_return_address(0);
    void* actual = (self != NULL) ? self : *(void**)BONE_GLOBAL_PTR;

    /* === [BONE-PRE] (v3.3) ==========================================
     * Inspect *actual BEFORE invoking the real bone function. This
     * captures whatever state was left in the matrix slot since the
     * previous call. If high_y suppression actually persisted between
     * calls, the matrix here should NOT match the high_y signature
     * (because suppression zeros mat[0..10]; mat[0]==0 fails the gate).
     *
     * Cases this catches:
     *   - matches_pre_high_y=1 -> something restored Matrix 1 between
     *     calls. Either a separate writer, or the bone fn itself
     *     re-stamps *actual on entry, or our suppression is being
     *     undone.
     *   - matches_pre_high_y=0 with mat[0]==0 -> suppression persisted,
     *     i.e. nothing else writes *actual between calls. (Doesn't
     *     prove the renderer used our zeros -- just that the slot
     *     stayed zeroed.)
     *
     * Throttle: 1 per 15 frames + every matches_pre_high_y=1. */
    if (caller_ret == 0x0061745A && actual != NULL && g_dragon_ptr != NULL
        && !IsBadReadPtr(actual, 64)) {
        const float* pre = (const float*)actual;
        float pre_ty   = pre[13] - 2000.0f; if (pre_ty < 0) pre_ty = -pre_ty;
        float pre_r0   = pre[0]  - 1.0f;    if (pre_r0 < 0) pre_r0 = -pre_r0;
        float pre_r5   = pre[5];            if (pre_r5 < 0) pre_r5 = -pre_r5;
        float pre_r10  = pre[10];           if (pre_r10 < 0) pre_r10 = -pre_r10;
        int matches_pre_high_y = (pre_ty < 0.5f && pre_r0 < 0.01f
                                   && pre_r5 < 0.01f && pre_r10 < 0.01f);
        if (matches_pre_high_y || (g_frame - g_bone_pre_frame) >= 15) {
            char b[256];
            wsprintfA(b,
                "[BONE-PRE] F%05d ptr=%p matches_pre_high_y=%d "
                "ty=%d/1000 diag=[%d/1000 %d/1000 %d/1000]",
                g_frame, actual, matches_pre_high_y,
                (int)(pre[13]*1000.0f),
                (int)(pre[0]*1000.0f),
                (int)(pre[5]*1000.0f),
                (int)(pre[10]*1000.0f));
            log_f(b);
            g_bone_pre_frame = g_frame;
        }
    }

    DWORD fn_eax = g_real_bone_fn(self);

    if (!g_bone_hook_active || actual == NULL || IsBadReadPtr(actual, 64))
        return fn_eax;

    float* mat = (float*)actual;

    /* Dynamic Dragon ptr capture: on first 0x0061745A fire with large translation
     * (ty > 1000 singles out the Dragon arena vs ship/lobby/Forest entities),
     * lock this as the Dragon entity ptr for the session.
     * Not hard-coded: heap address varies across launches/restarts. */
    if (caller_ret == 0x0061745A && actual != NULL) {
        if (g_dragon_ptr == NULL) {
            float ty = ((float*)actual)[13];
            if (ty > 1000.0f || ty < -1000.0f) {
                g_dragon_ptr = actual;
                char dbuf[80];
                wsprintfA(dbuf, "[BONE-PTR] Dragon ptr captured: %p ty=%d",
                    actual, (int)ty);
                log_f(dbuf);
            }
        }
        if (actual == g_dragon_ptr) {
            g_dragon_active       = 1;
            g_dragon_active_frame = g_frame;
        }
    }
    BoneTracker* tr = bone_get_tracker(actual, caller_ret);
    if (tr) tr->hit_count++;

    /* Compute current matrix stats */
    float out_absmax = 0.0f, delta_max = 0.0f;
    int has_nan = 0, has_inf = 0;
    for (int i = 0; i < 16; i++) {
        float v = mat[i];
        if (v != v)                       { has_nan = 1; continue; }
        if (v > 3.3e38f || v < -3.3e38f) { has_inf = 1; continue; }
        float a = v < 0.0f ? -v : v;
        if (a > out_absmax) out_absmax = a;
        if (tr && tr->seen) {
            float d = v - tr->prev_mat[i];
            if (d < 0) d = -d;
            if (d > delta_max) delta_max = d;
        }
    }
    /* is_identity: diagonal all 1.0 -- healthy matrix, not suspicious.
     * Previously mislabeled "degen". Kept for reference.
     * zero_rot: rotation 3x3 block all zero -- the actual Matrix B pattern.
     * This is what the old "degen" check should have been detecting. */
    int is_identity = (mat[0] == 1.0f && mat[5] == 1.0f && mat[10] == 1.0f);
    int zero_rot    = (mat[0]==0.0f && mat[1]==0.0f && mat[2]==0.0f &&
                       mat[4]==0.0f && mat[5]==0.0f && mat[6]==0.0f &&
                       mat[8]==0.0f && mat[9]==0.0f && mat[10]==0.0f);
    int degen = zero_rot; /* alias for log compat */

    /* Capture ALL matrices at caller 0x0061745A once Dragon arena is armed
     * (g_dragon_ptr != NULL). v3.1 broadens the gate so a second/duplicate
     * ptr at the same call site -- the suspected Matrix-B / stale head-neck --
     * shows up alongside the locked Dragon body.
     *
     * Throttle is per-tracker: each (ptr, caller) pair logs at most every
     * 15 frames, but zero_rot frames and first-sight of a new ptr always
     * log immediately. match=1 means actual==g_dragon_ptr (the locked
     * Dragon body); match=0 means a different ptr that hit 0x0061745A
     * during the dragon arena.
     *
     * tr is set above when actual!=NULL; if BoneTracker capacity is
     * exhausted (>16 unique pairs), tr is NULL and we log unthrottled
     * rather than drop, so we'd rather see overflow than miss the bug. */
    if (caller_ret == 0x0061745A && actual != NULL && g_dragon_ptr != NULL) {
        int last        = tr ? tr->last_xyz_frame : -999999;
        int first_sight = (tr && !tr->seen);
        if ((g_frame - last) >= 15 || zero_rot || first_sight) {
            float* fm = (float*)actual;
            char xbuf[256];
            wsprintfA(xbuf,
                "[BONE-XYZ] F%05d ptr=%p match=%d zero_rot=%d identity=%d "
                "tx=%d/1000 ty=%d/1000 tz=%d/1000 "
                "diag=[%d/1000 %d/1000 %d/1000]",
                g_frame, actual,
                (actual == g_dragon_ptr) ? 1 : 0,
                zero_rot, is_identity,
                (int)(fm[12] * 1000.0f),
                (int)(fm[13] * 1000.0f),
                (int)(fm[14] * 1000.0f),
                (int)(fm[0] * 1000.0f),
                (int)(fm[5] * 1000.0f),
                (int)(fm[10] * 1000.0f));
            log_f(xbuf);
            if (tr) tr->last_xyz_frame = g_frame;
        }
    }

    if (caller_ret == 0x0061745A && actual != NULL && actual == g_dragon_ptr) {
        static int s_last_diag = -60;
        if (g_frame - s_last_diag >= 60) {
            char dbuf[128];
            s_last_diag = g_frame;
            wsprintfA(dbuf,
                "[BONE-DIAG] F%05d dragon diag=[%d/1000 %d/1000 %d/1000] zero_rot=%d",
                g_frame,
                (int)(mat[0]*1000.0f), (int)(mat[5]*1000.0f), (int)(mat[10]*1000.0f),
                zero_rot);
            log_f(dbuf);
        }
    }

    /* --- Suspicious predicates (not just absmax > 1000 anymore) --- */
    char reason[32] = {0};
    int log_it = 0;

    if (has_nan)           { log_it = 1; wsprintfA(reason, "NAN"); }
    else if (has_inf)      { log_it = 1; wsprintfA(reason, "INF"); }
    else if (fn_eax == 0)  { log_it = 1; wsprintfA(reason, "EAX0"); }
    else if (zero_rot)     { log_it = 1; wsprintfA(reason, "ZERO_ROT"); }  /* Matrix B */
    else if (out_absmax > BONE_ABS_THRESH) { log_it = 1; wsprintfA(reason, "HUGE%.0f", out_absmax); }
    else if (tr && tr->seen && delta_max > BONE_DELTA_THRESH) {
                             log_it = 1; wsprintfA(reason, "JUMPf%d", (int)delta_max); }
    else if (tr && !tr->seen) { log_it = 1; wsprintfA(reason, "FIRST"); }

    if (log_it) {
        int idx = g_bone_log_head % BONE_LOG_CAP;
        BoneLogEntry* e = &g_bone_log[idx];
        e->frame      = g_frame;
        e->caller_ret = caller_ret;
        e->actual_ptr = actual;
        e->out_absmax = out_absmax;
        e->delta_max  = delta_max;
        e->has_nan    = has_nan;
        e->has_inf    = has_inf;
        e->is_identity = is_identity;
        e->zero_rot    = zero_rot;
        e->degenerate  = zero_rot;  /* alias */
        e->fn_eax     = fn_eax;
        for (int i = 0; i < 16; i++) e->mat[i] = mat[i];
        for (int i = 0; i < 31; i++) e->reason[i] = reason[i];
        e->reason[31] = 0;
        g_bone_log_head++;
    }

    /* Update tracker */
    if (tr) {
        for (int i = 0; i < 16; i++) tr->prev_mat[i] = mat[i];
        tr->seen = 1;
    }

    /* === HIGH-Y SUPPRESSION (v3.2) =================================
     * Empirical: bone fn 0x61EA00 alternates two matrices per frame for
     * the same Dragon ptr at caller 0x0061745A. CSV analysis of the v3.1
     * capture (4155 paired BONE events) showed:
     *   - 2074 high_y entries: ty exactly 2000.0,
     *     rotation [1,0,0 | 0,0,1 | 0,-1,0] (one unique signature)
     *   - 2081 normal_y entries: the visible Dragon body
     *   - perfect alternation, exclusively at caller 0x0061745A
     *
     * The high_y transform is a parallel "ghost" Dragon at altitude
     * 2000 whose geometry texture-bleeds into the scene whenever the
     * camera frustum includes that altitude.
     *
     * Suppression: zero the 3x3 rotation/scale block, preserve translation.
     * Every vertex collapses to (tx, ty, tz). Zero-area triangles do not
     * render. Translation preserved so any code reading world position
     * from this matrix sees something sane (no NaN cascades).
     *
     * Tolerances loose enough for fp noise, tight enough that Matrix 2
     * (the visible body, diag ~ (0.737, -0.994, -0.733)) cannot match.
     *
     * First 10 suppressions log in full as [HIGHY-SUP] for verification;
     * after that we just bump g_high_y_suppressed silently. Total dumped
     * alongside [BONE STAT] every 300 frames.
     *
     * To disable: rebuild with -DBONE_SUPPRESS_HIGH_Y=0. */
#if BONE_SUPPRESS_HIGH_Y
    if (caller_ret == 0x0061745A && actual != NULL) {
        float ty_dev  = mat[13] - 2000.0f; if (ty_dev  < 0.0f) ty_dev  = -ty_dev;
        float r0_dev  = mat[0]  - 1.0f;    if (r0_dev  < 0.0f) r0_dev  = -r0_dev;
        float r5_abs  = mat[5];            if (r5_abs  < 0.0f) r5_abs  = -r5_abs;
        float r10_abs = mat[10];           if (r10_abs < 0.0f) r10_abs = -r10_abs;
        if (ty_dev < 0.5f && r0_dev < 0.01f && r5_abs < 0.01f && r10_abs < 0.01f) {
            if (g_high_y_suppressed < 10) {
                char b[256];
                wsprintfA(b,
                    "[HIGHY-SUP] F%05d ptr=%p ty=%d/1000 "
                    "tx=%d/1000 tz=%d/1000 diag=[%d/1000 %d/1000 %d/1000]",
                    g_frame, actual,
                    (int)(mat[13]*1000.0f),
                    (int)(mat[12]*1000.0f),
                    (int)(mat[14]*1000.0f),
                    (int)(mat[0]*1000.0f),
                    (int)(mat[5]*1000.0f),
                    (int)(mat[10]*1000.0f));
                log_f(b);
            }
            mat[0] = 0.0f; mat[1] = 0.0f; mat[2]  = 0.0f;
            mat[4] = 0.0f; mat[5] = 0.0f; mat[6]  = 0.0f;
            mat[8] = 0.0f; mat[9] = 0.0f; mat[10] = 0.0f;
            g_high_y_suppressed++;
            /* === [BONE-POST] (v3.3) post-mutation verification ====
             * Re-read mat[] AFTER the write to confirm it actually
             * landed. mutation_took=1 means our zeros are at *actual
             * right now. mutation_took=0 means something is concurrently
             * writing to this memory and we need to look elsewhere. */
            if (g_high_y_suppressed <= 10) {
                int took = (mat[0] == 0.0f && mat[5] == 0.0f && mat[10] == 0.0f
                            && mat[1] == 0.0f && mat[2] == 0.0f
                            && mat[4] == 0.0f && mat[6] == 0.0f
                            && mat[8] == 0.0f && mat[9] == 0.0f);
                char b[256];
                wsprintfA(b,
                    "[BONE-POST] F%05d ptr=%p mutation_took=%d "
                    "diag=[%d/1000 %d/1000 %d/1000] ty=%d/1000",
                    g_frame, actual, took,
                    (int)(mat[0]*1000.0f),
                    (int)(mat[5]*1000.0f),
                    (int)(mat[10]*1000.0f),
                    (int)(mat[13]*1000.0f));
                log_f(b);
            }
        }
    }
#endif

    return fn_eax;
}

/* ------------------------------------------------------------------ */
static void bone_hook_flush(void) {
    static int last_flushed = 0;

    /* Flush suspicious event log */
    while (last_flushed < g_bone_log_head) {
        int idx = last_flushed % BONE_LOG_CAP;
        BoneLogEntry* e = &g_bone_log[idx];
        char buf[512];
        wsprintfA(buf,
            "[BONE] F%05d ret=0x%08X ptr=%p absmax=%d delta=%d "
            "nan=%d inf=%d zero_rot=%d is_id=%d eax=%u reason=%s",
            e->frame, e->caller_ret, e->actual_ptr,
            (int)e->out_absmax, (int)e->delta_max,
            e->has_nan, e->has_inf, e->zero_rot, e->is_identity,
            e->fn_eax, e->reason);
        log_f(buf);
        /* Always dump full 16 floats for context */
        wsprintfA(buf,
            "[BONE]   mat(x10k): [%d,%d,%d,%d | %d,%d,%d,%d | %d,%d,%d,%d | %d,%d,%d,%d]",
            (int)(e->mat[0]*10000),  (int)(e->mat[1]*10000),
            (int)(e->mat[2]*10000),  (int)(e->mat[3]*10000),
            (int)(e->mat[4]*10000),  (int)(e->mat[5]*10000),
            (int)(e->mat[6]*10000),  (int)(e->mat[7]*10000),
            (int)(e->mat[8]*10000),  (int)(e->mat[9]*10000),
            (int)(e->mat[10]*10000), (int)(e->mat[11]*10000),
            (int)(e->mat[12]*10000), (int)(e->mat[13]*10000),
            (int)(e->mat[14]*10000), (int)(e->mat[15]*10000));
        log_f(buf);
        last_flushed++;
    }

    /* Per-caller stat dump every 300 frames */
    if (g_frame - g_bone_stat_frame >= 300 && g_bone_tracker_n > 0) {
        char buf[256];
        for (int i = 0; i < g_bone_tracker_n; i++) {
            BoneTracker* t = &g_bone_trackers[i];
            wsprintfA(buf, "[BONE STAT] ret=0x%08X ptr=%p hits=%u",
                t->caller_ret, t->ptr, t->hit_count);
            log_line(buf);
        }
#if BONE_SUPPRESS_HIGH_Y
        wsprintfA(buf, "[BONE STAT] high_y_suppressed_total=%u",
            g_high_y_suppressed);
        log_line(buf);
#endif
        wsprintfA(buf, "[BONE STAT] setxform_highy_seen_total=%u",
            g_setxform_highy_seen);
        log_line(buf);
        g_bone_stat_frame = g_frame;
    }
}

/* ------------------------------------------------------------------ */
static void bone_hook_install(void) {
    if (g_bone_hook_active) return;

    static const DWORD call_sites[] = {
        0x004D9043, 0x004D90A1, 0x004E0AD5, 0x004F0C91,
        0x00574C37, 0x00617455, 0x00617535
    };
    void* hook_fn = (void*)hook_bone_fn;

    for (int i = 0; i < 7; i++) {
        DWORD site = call_sites[i];
        DWORD old_prot;
        DWORD new_rel = (DWORD)hook_fn - (site + 5);
        BYTE patch[5] = {
            0xE8,
            (BYTE)(new_rel & 0xFF),
            (BYTE)((new_rel >> 8) & 0xFF),
            (BYTE)((new_rel >> 16) & 0xFF),
            (BYTE)((new_rel >> 24) & 0xFF)
        };
        VirtualProtect((void*)site, 5, PAGE_EXECUTE_READWRITE, &old_prot);
        memcpy((void*)site, patch, 5);
        VirtualProtect((void*)site, 5, old_prot, &old_prot);
        char b[128];
        wsprintfA(b, "[BONE] hooked 0x%08X", site);
        log_f(b);
    }
    FlushInstructionCache(GetCurrentProcess(), NULL, 0);
    g_bone_hook_active = 1;
    log_f("[BONE] hook v3.3 installed -- v3.2 + BONE-PRE/POST + setxform high_y detector");
}
