/*
 * bone_hook.h -- Stateful instrumentation hook for pso.exe 0x61EA00 (v3)
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
} BoneTracker;

static BoneLogEntry g_bone_log[BONE_LOG_CAP];
static volatile int g_bone_log_head  = 0;

static BoneTracker  g_bone_trackers[BONE_TRACK_CAP];
static int          g_bone_tracker_n = 0;

static int  g_bone_hook_active = 0;
static int  g_bone_stat_frame  = 0;   /* frame of last stat flush */

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
        return t;
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
static DWORD __attribute__((thiscall)) hook_bone_fn(void* self) {
    DWORD caller_ret = (DWORD)__builtin_return_address(0);
    void* actual = (self != NULL) ? self : *(void**)BONE_GLOBAL_PTR;

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

    /* Periodic diagonal check for known dragon entity (caller=0x0061745A).
     * Verifies whether zeroscale patch produces sane diagonals or heap garbage.
     * Emits [BONE-DIAG] once per 60 frames for that call site. */
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
    log_f("[BONE] hook v3 installed -- delta+abs tracking active");
}
