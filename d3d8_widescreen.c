#include <windows.h>

/*
 * d3d8_widescreen.c  -  v16-lite-r8
 *
 * PSO PC V2 widescreen proxy + Dragon bug diagnostics.
 *
 * CURRENT STATE:
 *
 * 1. WIDESCREEN / HUD CORRECTION
 *    XYZRHW correction for 14:9 native and 16:9 letterbox modes.
 *    Full-screen elements (x_range > 580): 14:9 scale + edge snapping.
 *    HUD elements (x_range <= 580): 4:3-centred scale.
 *
 * 2. PARTICLE NEAR-Z SUPPRESSION  (Bug A mitigation)
 *    Dragon fire/smoke particles hit view-space Z~=0 in the game software
 *    particle projector.  Perspective divide blows up -> huge XYZRHW quads.
 *    Proxy collapses outlier quads to zero-area/alpha=0 in hook_VBUnlock.
 *    Two tiers, both suppress:
 *      [SUPPRESS]      |x| or |y| > 5000  ->  hard collapse.
 *      [SUPPRESS-NEAR] bbox beyond near-outlier bounds (x<-1024, x>1664,
 *                      y<-1024, y>1504, or w/h>1500) -> also collapsed.
 *    [NEAR_OUTLIER] log-only tier no longer exists; promoted to
 *    SUPPRESS-NEAR in v15.
 *
 * 3. DRAGON BODY BUG  (Bug B - partially mitigated in pso.exe)
 *    Dragon alternates two bone matrices every frame:
 *      Matrix A - valid rotation, correct world position.
 *      Matrix B - zero rotation rows, valid translation.
 *    Matrix inverse guard at 0x61EA00 patched (je->jbe, identity fallback,
 *    zeroscale NOPs) to collapse Matrix B to a degenerate point.
 *    Matrix A still renders the full Dragon body.
 *    Dragon body (caller=0x00639F2A, fvf=0x0142, opaque) persists during
 *    clear screen until Dragon despawns -- remaining source of texture bleed.
 *    Suppression of 0x00639F2A in the death window is the planned next fix,
 *    not yet applied in this build.
 *
 * 4. DEATH WINDOW DIAGNOSTICS  (v16-lite series)
 *    State machine detects fight->death via suppress texture diversity
 *    (fight: >=3 unique tex0/frame; dying: <=1 tex0 for 30 frames).
 *    Draw signatures accumulated during window, dumped on Dragon despawn.
 *
 * 5. WALL-CLOCK TIMESTAMPS  (v16-lite-r3 onward)
 *    [TIME] lines log both UTC and LOCAL at startup, every 60 frames, and
 *    on DWIN events.  Align with newserv chat phase markers (PRE, TRIGGER,
 *    DEAD, CLEAR, DONE).  No chat reading in proxy -- manual alignment only.
 *
 * 6. BUFFERED LOGGING
 *    128 KB in-memory buffer, flushed once per Present.
 *    No per-call file I/O on hot paths.
 *
 * Build:
 *   i686-w64-mingw32-gcc -shared -O2 -o D3D8.dll d3d8_widescreen.c d3d8.def
 *     -lkernel32 -Wl,--enable-stdcall-fixup
 */
/* ---- D3D8 constants ---- */
#define D3DTS_PROJECTION    3
#define D3DFVF_XYZRHW       0x004
#define D3DLOCK_READONLY    0x0010
#define D3DLOCK_NOOVERWRITE 0x1000   /* D3D8 actual value, not 0x0800 */
#define D3DLOCK_DISCARD     0x2000

#define D3DPT_POINTLIST     1
#define D3DPT_LINELIST      2
#define D3DPT_LINESTRIP     3
#define D3DPT_TRIANGLELIST  4
#define D3DPT_TRIANGLESTRIP 5
#define D3DPT_TRIANGLEFAN   6

/* ---- types ---- */
typedef struct { float m[4][4]; } D3DMATRIX;

typedef struct {
    DWORD X, Y, Width, Height;
    float MinZ, MaxZ;
} D3DVIEWPORT8;

typedef struct {
    UINT  BackBufferWidth, BackBufferHeight;
    DWORD BackBufferFormat, BackBufferCount, MultiSampleType, SwapEffect;
    HWND  hDeviceWindow;
    BOOL  Windowed, EnableAutoDepthStencil;
    DWORD AutoDepthStencilFormat, Flags;
    UINT  FullScreen_RefreshRateInHz, FullScreen_PresentationInterval;
} D3DPRESENT_PARAMETERS;

typedef void*   (WINAPI *Direct3DCreate8_t)       (UINT);
typedef HRESULT (WINAPI *Clear_t)(void*, DWORD, const void*, DWORD, DWORD, float, DWORD);
typedef HRESULT (WINAPI *Present_t)(void*, const RECT*, const RECT*, HWND, const void*);
typedef HRESULT (WINAPI *CreateDevice_t)           (void*, UINT, DWORD, HWND, DWORD,
                                                    D3DPRESENT_PARAMETERS*, void**);
typedef HRESULT (WINAPI *SetTransform_t)           (void*, DWORD, const D3DMATRIX*);
typedef HRESULT (WINAPI *SetViewport_t)            (void*, const D3DVIEWPORT8*);
typedef HRESULT (WINAPI *GetViewport_t)            (void*, D3DVIEWPORT8*);
typedef HRESULT (WINAPI *SetVertexShader_t)        (void*, DWORD);
typedef HRESULT (WINAPI *SetStreamSource_t)        (void*, UINT, void*, UINT);
typedef HRESULT (WINAPI *CreateVertexBuffer_t)     (void*, UINT, DWORD, DWORD, DWORD, void**);
typedef HRESULT (WINAPI *DrawPrimitive_t)          (void*, DWORD, UINT, UINT);
typedef HRESULT (WINAPI *SetRenderState_t)         (void*, DWORD, DWORD);
typedef HRESULT (WINAPI *DrawIndexedPrimitive_t)   (void*, DWORD, UINT, UINT, UINT, UINT);
typedef HRESULT (WINAPI *DrawPrimitiveUP_t)        (void*, DWORD, UINT, const void*, UINT);
typedef HRESULT (WINAPI *VBLock_t)                 (void*, UINT, UINT, BYTE**, DWORD);
typedef HRESULT (WINAPI *VBUnlock_t)               (void*);

/* ---- real pointers ---- */
static HMODULE           real_d3d8;
static Direct3DCreate8_t real_Direct3DCreate8;
static CreateDevice_t    real_CreateDevice;
static Clear_t           real_Clear;
static Present_t         real_Present;
static SetTransform_t    real_SetTransform;
static SetViewport_t     real_SetViewport;
static GetViewport_t     real_GetViewport;
static SetVertexShader_t real_SetVertexShader;
static SetStreamSource_t real_SetStreamSource;
static CreateVertexBuffer_t real_CreateVertexBuffer;
static DrawPrimitive_t   real_DrawPrimitive;
static DrawIndexedPrimitive_t real_DrawIndexedPrimitive;
static DrawPrimitiveUP_t real_DrawPrimitiveUP;
static SetRenderState_t  real_SetRenderState;
typedef HRESULT (WINAPI *SetTexture_t)(void*, DWORD, void*);
static SetTexture_t      real_SetTexture;
static VBLock_t          real_VBLock;
static VBUnlock_t        real_VBUnlock;
static int               hooked       = 0;
static int               g_vb_patched = 0;

/* ---- correction constants ---- */
static const float k_persp = 0.75f;
static const float k_ortho = 0.75f;

/*
 * Full-screen XYZRHW threshold (PSO space, 0-640).
 * x_range > this -> 14:9 formula (fades, letterbox bars).
 * x_range <= this -> 4:3 centred formula (HUD elements).
 */
#define FULLWIDTH_THRESH 580.0f

/* ---- world matrix corruption ---- */
#define D3DTS_WORLD             256
#define D3DTS_VIEW              2

/*
 * v16-lite-r2 diagnostics
 *
 * Death window -- automatic multi-signal state machine, no user input:
 *
 *   IDLE       waiting for fight to start
 *   FIGHT_SEEN >=3 unique tex0 in one suppress frame -> Dragon fight confirmed
 *   DYING      after fight, suppress count <=4/frame AND mono-tex (or zero)
 *              for DEATH_SETTLE_FRAMES consecutive frames
 *   OPEN       collecting draw signatures
 *
 * Mid-fight manual trigger: 100+ suppresses/frame, 7 distinct tex0 -> stays FIGHT_SEEN
 * Dragon death -> clear screen: 2-4 suppresses/frame, single tex0, then silence -> DYING->OPEN
 *
 * Auto-close: Dragon bone hook (caller_ret==0x0061745A) gone >=120 frames.
 */
int   g_dragon_active       = 0;   /* extern'd in bone_hook.h */
int   g_dragon_active_frame = -1;
void* g_dragon_ptr          = NULL; /* captured by bone_hook; NULL until dragon entity fires */

/* Per-frame suppress texture diversity tracker */
static int  g_suppress_this_frame  = 0;
#define     SUPPRESS_TEX_CAP 16
static void* g_suppress_tex_this[SUPPRESS_TEX_CAP];
static int  g_suppress_tex_n       = 0;

/* State machine */
#define DWIN_IDLE        0
#define DWIN_FIGHT_SEEN  1
#define DWIN_DYING       2
#define DWIN_OPEN        3
/* Minimum frames before fight-confirmed can fire.
 * ~90s at 30fps covers ship + lobby + room creation + cutscene. */
#define FIGHT_MIN_FRAME  2700
static int g_dwin_state        = DWIN_IDLE;
static int g_dwin_dying_frames = 0;
#define DEATH_SETTLE_FRAMES 30

static int g_death_window      = 0;
static int g_death_window_start = -1;

/*
 * Draw-signature table -- populated during g_death_window only.
 * Accumulates silently; dumped as [DWIN-SIG] lines when window closes.
 * No per-draw file I/O.  first_frame recorded for post-hoc filtering.
 */
typedef struct {
    DWORD  caller;
    BYTE   draw_type;   /* 0=DP 1=DIP 2=UP */
    DWORD  fvf;
    DWORD  pt;
    UINT   pc_bucket;   /* pc >> 3 */
    UINT   stride;
    void*  tex0;
    DWORD  alphablendenable, srcblend, destblend;
    DWORD  zenable, zwriteenable, lighting, cullmode;
    UINT   count;
    int    first_frame; /* frame this sig was first seen */
    int    last_frame;  /* frame this sig was most recently seen */
} DrawSig16;
#define MAX_DRAW_SIGS 128
static DrawSig16 g_draw_sigs[MAX_DRAW_SIGS];
static int       g_draw_sig_n = 0;

/* UP scan caps -- prevent log explosion even in death window */
static int g_up_logs_this_frame   = 0;
static int g_up_logs_this_session = 0;
#define UP_LOG_CAP_FRAME   5
#define UP_LOG_CAP_SESSION 100

/* INI-driven config -- loaded in hook_CreateDevice */
static int g_cfg_death_draw_sigs = 1;  /* [DragonDiag] EnableDeathDrawSigs=1 */
static int g_cfg_up_scan         = 1;  /* [DragonDiag] EnableUPScan=1 */
static int g_cfg_max_up_frame    = 5;  /* [DragonDiag] MaxUPLogsPerFrame=5 */

/* VIEW matrix for view-space Z -- stored by hook_SetTransform */
static float g_mat_view[16];
static int   g_mat_view_valid = 0;

/*
 * Viewport aspect ratio mode.
 * INI key [Resolution] ViewportMode=N
 *   0 = auto-detect from backbuffer AR (default)
 *   1 = 14:9  letterbox  (1.555...) -- PSO native widescreen, bars on 16:9
 *   2 = 16:9  full-screen (1.777...) -- fills a 16:9 monitor completely
 *   3 = 16:10 full-screen (1.600)    -- fills a 16:10 monitor completely
 *
 * Auto-detection bands (from backbuffer AR):
 *   >=1.74 and <=1.82  -> 16:9
 *   >=1.57 and <=1.63  -> 16:10
 *   otherwise          -> 14:9
 *
 * SecondCull sin/cos are derived from the 14:9 calibrated values
 * (sin=0.358419, cos=0.933561) scaled by vp_ar/(14/9).
 * Precomputed for the three standard ARs:
 *   14:9  sin=0.358419 cos=0.933561  (calibrated, origin of scale)
 *   16:9  sin=0.401797 cos=0.915731  (scale * 8/7)
 *   16:10 sin=0.367300 cos=0.930110  (scale * 36/35)
 */
static int   g_vp_mode = 0;   /* resolved from INI + bb_ar in CreateDevice */
static float g_vp_ar   = 14.0f/9.0f; /* actual viewport AR used this session */

/* forward declaration -- defined after g_ctx_* globals */
static void record_draw_sig(BYTE draw_type, DWORD caller,
                             DWORD fvf, DWORD pt, UINT pc, UINT stride);

/* ---- 2D correction state ---- */
static DWORD g_fvf        = 0;
static float g_xrhw_scale = 1.0f;
static float g_xrhw_cx    = 320.0f;

static void* g_s0_vb     = NULL;
static UINT  g_s0_stride = 0;

static UINT g_bb_w = 640;
static UINT g_bb_h = 480;

static UINT  g_bar_w  = 0;
static UINT  g_vp_w   = 640;
static float g_k_proj = 0.75f;

/*
 * 4:3-centred HUD layout (native resolution mode).
 * g_hud_sy = bb_h / 480  (e.g. 4.5 at 4K)
 * g_hud_x0 = (bb_w - 640 * g_hud_sy) / 2  (e.g. 480 at 4K)
 * With sx == sy the HUD maintains 4:3 pixel proportions, matching
 * the 16:9 mode where sx == sy == 4.5 (0.75 compress x 6 stretch).
 */
static float g_hud_x0 = 0.0f;
static float g_hud_sy = 1.0f;

/* ---- frame counter (incremented per Present) ---- */
static int g_frame = 0;

/* =========================================================
 * Shadow VB table
 * ========================================================= */
#define MAX_SHADOW_VBS 128

typedef struct {
    void*  vb;
    BYTE*  shadow;
    UINT   shadow_cap;
    UINT   lock_off;
    UINT   lock_sz;
    BYTE*  real_ptr;
    UINT   stride;
    UINT   vb_size;
    DWORD  lock_count;      /* DIAG: total lock+unlock cycles on this VB */
    DWORD  vert_total;      /* DIAG: total vertices corrected on this VB */
    DWORD  lock_fvf;        /* g_fvf captured at most recent Lock call */
    DWORD  lock_caller_ret;  /* __builtin_return_address(0) -- dgvoodoo thunk */
    DWORD  lock_caller_ret2; /* __builtin_return_address(1) -- hopefully PSO code */
} VBShadow;

static VBShadow g_svb[MAX_SHADOW_VBS];
static int      g_svb_n = 0;

static VBShadow* svb_find(void* vb) {
    int i;
    for (i = 0; i < g_svb_n; i++)
        if (g_svb[i].vb == vb) return &g_svb[i];
    return NULL;
}

static VBShadow* svb_get(void* vb, UINT stride) {
    VBShadow* e = svb_find(vb);
    if (e) {
        if (e->stride == 0 && stride > 0) e->stride = stride;
        return e;
    }
    if (g_svb_n >= MAX_SHADOW_VBS) return NULL;
    e = &g_svb[g_svb_n++];
    e->vb         = vb;
    e->shadow     = NULL;
    e->shadow_cap = 0;
    e->lock_off   = 0;
    e->lock_sz    = 0;
    e->real_ptr   = NULL;
    e->stride     = stride;
    e->vb_size    = 0;
    e->lock_count      = 0;
    e->lock_caller_ret  = 0;
    e->lock_caller_ret2 = 0;
    e->vert_total = 0;
    return e;
}

/* =========================================================
 * Scratch buffer
 * ========================================================= */
#define SCRATCH_SIZE 65536
static BYTE g_scratch[SCRATCH_SIZE];

static BYTE* get_buf(UINT bytes) {
    if (bytes <= SCRATCH_SIZE) return g_scratch;
    return (BYTE*)HeapAlloc(GetProcessHeap(), 0, bytes);
}
static void free_buf(BYTE* p, UINT bytes) {
    if (bytes > SCRATCH_SIZE) HeapFree(GetProcessHeap(), 0, p);
}

/* =========================================================
 * Logging -- buffered, flushed once per Present
 *
 * All log_line/log_f calls write into a 128 KB ring buffer.
 * log_flush() (called in hook_Present) does the single file
 * write per frame.  This keeps file I/O off every hot path.
 *
 * Exception: log_flush() itself is also called directly for
 * any startup/shutdown messages written before the first Present.
 * ========================================================= */
#define LOG_BUF_CAP 131072
static char g_log_buf[LOG_BUF_CAP];
static int  g_log_pos = 0;

static void log_flush(void) {
    if (g_log_pos <= 0) return;
    {
        HANDLE h = CreateFileA("pso-peeps-d3d8-wsh.log",
            FILE_APPEND_DATA, FILE_SHARE_READ|FILE_SHARE_WRITE,
            NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD w = 0;
            WriteFile(h, g_log_buf, (DWORD)g_log_pos, &w, NULL);
            CloseHandle(h);
        }
    }
    g_log_pos = 0;
}

static void log_line(const char* msg) {
    int len = lstrlenA(msg);
    if (g_log_pos + len + 2 >= LOG_BUF_CAP) log_flush();
    if (len < LOG_BUF_CAP - 2) {
        CopyMemory(g_log_buf + g_log_pos, msg, len);
        g_log_pos += len;
        g_log_buf[g_log_pos++] = '\r';
        g_log_buf[g_log_pos++] = '\n';
    }
}

/* log with frame prefix */
static void log_f(const char* msg) {
    char b[288];
    wsprintfA(b, "[F%05u] %s", g_frame, msg);
    log_line(b);
}

/* Wall-clock timestamps - both UTC and LOCAL so correlation with newserv
 * chat logs works regardless of timezone.  Bruce is EDT (UTC-4).
 * Format: [TIME] tag              F##### UTC=HH:MM:SS.mmm LOCAL=HH:MM:SS.mmm */
static void log_timestamp(const char* tag) {
    SYSTEMTIME utc, loc;
    GetSystemTime(&utc);
    GetLocalTime(&loc);
    char b[192];
    wsprintfA(b, "[TIME] %-16s F%05u UTC=%02u:%02u:%02u.%03u LOCAL=%02u:%02u:%02u.%03u",
        tag, g_frame,
        utc.wHour, utc.wMinute, utc.wSecond, utc.wMilliseconds,
        loc.wHour, loc.wMinute, loc.wSecond, loc.wMilliseconds);
    log_line(b);
}

#include "bone_hook.h"

static void log_uint(const char* pfx, UINT v) {
    char b[128]; wsprintfA(b, "%s%u", pfx, v); log_line(b);
}

/* =========================================================
 * Dedup tables for hot-path logging
 * ========================================================= */

/* Unique viewports seen */
typedef struct { DWORD X, Y, W, H; } VP4;
#define MAX_UNIQUE_VP 64
static VP4 g_vp_seen[MAX_UNIQUE_VP];
static int g_vp_seen_n = 0;

static int vp_is_new(DWORD x, DWORD y, DWORD w, DWORD h) {
    int i;
    for (i = 0; i < g_vp_seen_n; i++)
        if (g_vp_seen[i].X==x && g_vp_seen[i].Y==y &&
            g_vp_seen[i].W==w && g_vp_seen[i].H==h)
            return 0;
    if (g_vp_seen_n < MAX_UNIQUE_VP) {
        g_vp_seen[g_vp_seen_n].X = x;
        g_vp_seen[g_vp_seen_n].Y = y;
        g_vp_seen[g_vp_seen_n].W = w;
        g_vp_seen[g_vp_seen_n].H = h;
        g_vp_seen_n++;
        return 1;
    }
    return 0; /* table full - don't log duplicates */
}

/* Unique FVF values seen at SetStreamSource */
#define MAX_UNIQUE_FVF 32
static DWORD g_fvf_seen[MAX_UNIQUE_FVF];
static int   g_fvf_seen_n = 0;

static int fvf_is_new(DWORD fvf) {
    int i;
    for (i = 0; i < g_fvf_seen_n; i++)
        if (g_fvf_seen[i] == fvf) return 0;
    if (g_fvf_seen_n < MAX_UNIQUE_FVF) {
        g_fvf_seen[g_fvf_seen_n++] = fvf;
        return 1;
    }
    return 0;
}

/* Unique DrawPrimitiveUP call patterns */
typedef struct { DWORD fvf, pt; UINT pc, stride; } UP4;
#define MAX_UNIQUE_UP 64
static UP4 g_up_seen[MAX_UNIQUE_UP];
static int g_up_seen_n = 0;

static int up_is_new(DWORD fvf, DWORD pt, UINT pc, UINT stride) {
    int i;
    for (i = 0; i < g_up_seen_n; i++)
        if (g_up_seen[i].fvf==fvf && g_up_seen[i].pt==pt &&
            g_up_seen[i].pc==pc   && g_up_seen[i].stride==stride)
            return 0;
    if (g_up_seen_n < MAX_UNIQUE_UP) {
        g_up_seen[g_up_seen_n].fvf    = fvf;
        g_up_seen[g_up_seen_n].pt     = pt;
        g_up_seen[g_up_seen_n].pc     = pc;
        g_up_seen[g_up_seen_n].stride = stride;
        g_up_seen_n++;
        return 1;
    }
    return 0;
}

/* Unique projection m[0][0] values (stored as int * 10000) */
#define MAX_UNIQUE_PROJ 16
static int g_proj_seen[MAX_UNIQUE_PROJ];
static int g_proj_seen_n = 0;

static int proj_is_new(int v) {
    int i;
    for (i = 0; i < g_proj_seen_n; i++)
        if (g_proj_seen[i] == v) return 0;
    if (g_proj_seen_n < MAX_UNIQUE_PROJ) {
        g_proj_seen[g_proj_seen_n++] = v;
        return 1;
    }
    return 0;
}

/* =========================================================
 * Vtable patching
 * ========================================================= */
static void patch_slot(void** slot, void* rep, void** orig) {
    DWORD old = 0, tmp = 0;
    if (orig && !*orig) *orig = *slot;
    if (*slot == rep) return;
    VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &old);
    *slot = rep;
    VirtualProtect(slot, sizeof(void*), old, &tmp);
    FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void*));
}

/* =========================================================
 * Vertex count
 * ========================================================= */
static UINT vcount(DWORD pt, UINT pc) {
    switch (pt) {
        case D3DPT_POINTLIST:     return pc;
        case D3DPT_LINELIST:      return pc * 2;
        case D3DPT_LINESTRIP:     return pc + 1;
        case D3DPT_TRIANGLELIST:  return pc * 3;
        case D3DPT_TRIANGLESTRIP: return pc + 2;
        case D3DPT_TRIANGLEFAN:   return pc + 2;
        default:                  return pc * 3;
    }
}

/* =========================================================
 * X/Y correction
 * ========================================================= */
static void correct_xyzrhw(BYTE* data, UINT vc, UINT stride) {
    UINT i; BYTE* p = data;

    if (g_bb_w > 640) {
        /*
         * Native resolution mode -- two formulas:
         *
         * FULL-SCREEN (x_range > FULLWIDTH_THRESH):
         *   Maps PSO [0,640] -> [g_bar_w, g_bar_w+g_vp_w].
         *   Used for fades, letterbox bars -- must align with side bars.
         *
         * HUD ELEMENT (x_range <= FULLWIDTH_THRESH):
         *   Maps using sx == sy == g_hud_sy, centred at g_hud_x0.
         *   Preserves 4:3 pixel proportions (matches 16:9 mode behaviour).
         */
        float min_x =  1e9f, max_x = -1e9f;
        p = data;
        for (i = 0; i < vc; i++, p += stride) {
            float x = *(float*)p;
            if (x < min_x) min_x = x;
            if (x > max_x) max_x = x;
        }

        if ((max_x - min_x) > FULLWIDTH_THRESH) {
            /*
             * Full-screen element -> 14:9 scale with edge snapping.
             *
             * PSO was designed for CRT overscan: the top/bottom letterbox
             * bars start at PSO y~=8 (not y=0), assuming ~8 scan lines would
             * be hidden by overscan on real hardware. At 4K with no overscan,
             * that 8-unit gap becomes 36px of visible 3D scene above the bar.
             *
             * Edge snap: after scaling, any corrected coordinate within
             * EDGE_SNAP physical pixels of a screen boundary is extended to
             * that boundary. This eliminates the overscan gap AND the 2-3px
             * D3D8 half-pixel-offset gap at the sides without affecting any
             * non-edge content.
             *
             * EDGE_SNAP = 48px at 4K -> 48/4.5 ~= 10.7 PSO units of margin.
             * Safe because no legitimate full-screen interior content exists
             * within 10 PSO units of any screen edge.
             */
            float sx    = (float)g_vp_w / 640.0f;
            float sy    = (float)g_bb_h / 480.0f;
            float x_lo  = (float)g_bar_w;
            float x_hi  = (float)(g_bar_w + g_vp_w);
            float y_hi  = (float)g_bb_h;
#define EDGE_SNAP 48.0f
            p = data;
            for (i = 0; i < vc; i++, p += stride) {
                float* x = (float*)p;
                float* y = (float*)(p + 4);
                float cx = x_lo + *x * sx;
                float cy = *y * sy;
                /* snap X to bar edges */
                if (cx < x_lo + EDGE_SNAP) cx = x_lo;
                if (cx > x_hi - EDGE_SNAP) cx = x_hi;
                /* snap Y to screen top/bottom */
                if (cy < EDGE_SNAP) cy = 0.0f;
                if (cy > y_hi - EDGE_SNAP) cy = y_hi;
                *x = cx;
                *y = cy;
            }
#undef EDGE_SNAP
        } else {
            /* HUD element -> 4:3 centred scale */
            float sy = g_hud_sy;
            float x0 = g_hud_x0;
            p = data;
            for (i = 0; i < vc; i++, p += stride) {
                float* x = (float*)p;
                float* y = (float*)(p + 4);
                *x = x0 + *x * sy;
                *y = *y * sy;
            }
        }
    } else {
        /* 640x480 backbuffer mode: horizontal widescreen correction */
        for (i = 0; i < vc; i++, p += stride) {
            float* x = (float*)p;
            *x = g_xrhw_cx + (*x - g_xrhw_cx) * g_xrhw_scale;
        }
    }
}

/* =========================================================
 * VB hooks
 * ========================================================= */
static HRESULT WINAPI hook_CreateVertexBuffer(
    void* self, UINT length, DWORD usage, DWORD fvf, DWORD pool, void** ppVB)
{
    HRESULT hr = real_CreateVertexBuffer(self, length, usage, fvf, pool, ppVB);
    if (hr == 0 && ppVB && *ppVB) {
        VBShadow* e = svb_find(*ppVB);
        if (e) {
            e->vb_size = length;
            {
                char b[128];
                wsprintfA(b, "[VB] CreateVB size=%u fvf=0x%04X usage=0x%04X ptr=%p",
                    length, fvf, (UINT)usage, *ppVB);
                log_f(b);
            }
        }
    }
    return hr;
}

static HRESULT WINAPI hook_VBLock(
    void* self, UINT off, UINT sz, BYTE** ppData, DWORD flags)
{
    HRESULT hr = real_VBLock(self, off, sz, ppData, flags);
    if (hr == 0 && ppData && *ppData
        && !(flags & D3DLOCK_READONLY))
    {
        VBShadow* e = svb_find(self);
        if (e && e->stride >= 4) {
            UINT effective_sz = sz;
            if (effective_sz == 0 && e->vb_size > off)
                effective_sz = e->vb_size - off;
            if (effective_sz == 0) goto vblock_done;
            {
            UINT need = off + effective_sz;

            /* DIAG: log first 8 locks on this VB, then every 500th */
            if (e->lock_count < 8 || (e->lock_count % 500) == 0) {
                char b[192];
                wsprintfA(b,
                    "[VB] Lock #%u ptr=%p off=%u sz=%u(eff=%u) flags=0x%X stride=%u cap=%u",
                    e->lock_count, self, off, sz, effective_sz,
                    (UINT)flags, e->stride, e->shadow_cap);
                log_f(b);
                if (sz == 0) {
                    char b2[64];
                    wsprintfA(b2, "[VB]  ^ sz=0 expanded from vb_size=%u", e->vb_size);
                    log_f(b2);
                }
            }

            if (need > e->shadow_cap) {
                BYTE* ns = (BYTE*)HeapAlloc(GetProcessHeap(), 0, need);
                if (ns) {
                    if (e->shadow) {
                        CopyMemory(ns, e->shadow, e->shadow_cap);
                        HeapFree(GetProcessHeap(), 0, e->shadow);
                    }
                    e->shadow     = ns;
                    e->shadow_cap = need;
                }
            }
            if (e->shadow) {
                e->lock_off         = off;
                e->lock_sz          = effective_sz;
                e->real_ptr         = *ppData;
                e->lock_fvf         = g_fvf;
                /* level 0 = dgvoodoo adjustor thunk (not PSO code) */
                e->lock_caller_ret  = (DWORD)__builtin_return_address(0);
                /* level 1 removed: dgvoodoo thunk has no EBP frame,
                 * walking it crashes. Use dp_caller in [DRAWCTX] instead. */
                e->lock_caller_ret2 = 0;
                *ppData     = e->shadow + off;
            }
            }
        }
    }
    vblock_done:
    return hr;
}

/* ---- VB outlier dump -------------------------------------------- */
/* Fired pre-correction when any vertex |x| or |y| > VB_OUTLIER_THRESH.
 * Dumps full XYZRHW fields per-vertex and recent bone log context.
 * Rate-limited to 3 dumps per frame so a sustained corruption event
 * doesn't flood the log. */

#define VB_OUTLIER_THRESH 5000.0f
#define VB_OUTLIER_MAX_PER_FRAME 3

/* Forward declarations -- defined in draw-context section below */
static int g_outlier_pending;
static int g_outlier_pending_frame;

/* Forward declaration -- defined in draw-context section below hook_VBUnlock */
static void* g_ctx_tex[4];

/* ATOC constants (also used by hook_SetRenderState) */
#define ATOC_ENABLE   0x314D3241
#define ATOC_DISABLE  0x304D3241

static int g_outlier_frame     = -1;
static int g_outlier_frame_n   = 0;

static void bone_dump_context_for_vb(int frame) {
    /* Pull last up to 8 bone events within +/-3 frames */
    int start = g_bone_log_head - BONE_LOG_CAP;
    if (start < 0) start = 0;
    for (int i = start; i < g_bone_log_head; i++) {
        BoneLogEntry* e = &g_bone_log[i % BONE_LOG_CAP];
        if (e->frame < frame - 3 || e->frame > frame + 3) continue;
        char b[256];
        wsprintfA(b,
            "[OUTLIER]  bone F%05d ret=0x%08X ptr=%p absmax=%d zero_rot=%d eax=%u reason=%s",
            e->frame, e->caller_ret, e->actual_ptr,
            (int)e->out_absmax, e->zero_rot, e->fn_eax, e->reason);
        log_f(b);
    }
}

static void dump_vb_outlier(BYTE* src, UINT vc, UINT stride, DWORD lock_count,
                             DWORD fvf, DWORD caller_ret, DWORD caller_ret2) {
    /* Rate limit */
    if (g_frame == g_outlier_frame) {
        if (g_outlier_frame_n >= VB_OUTLIER_MAX_PER_FRAME) return;
        g_outlier_frame_n++;
    } else {
        g_outlier_frame   = g_frame;
        g_outlier_frame_n = 1;
    }

    /* FVF field offsets (XYZRHW assumed: x,y,z,rhw at 0,4,8,12) */
    int has_diffuse  = (fvf & 0x40) ? 1 : 0;
    int has_specular = (fvf & 0x80) ? 1 : 0;
    int tex_count    = (fvf >> 8) & 0xF;
    int diff_off     = 16;
    int spec_off     = diff_off + (has_diffuse  ? 4 : 0);
    int uv_off       = spec_off + (has_specular ? 4 : 0);

    char b[384];
    wsprintfA(b,
        "[OUTLIER] F%05d lock#%u vc=%u stride=%u fvf=0x%04X"
        " caller0=0x%08X caller1=0x%08X diff=%d spec=%d uvsets=%d",
        g_frame, lock_count, vc, stride, (UINT)fvf,
        caller_ret, caller_ret2,
        has_diffuse, has_specular, tex_count);
    log_f(b);

    /* Per-vertex dump -- only bad vertices */
    for (UINT i = 0; i < vc; i++) {
        float* vp  = (float*)(src + i * stride);
        float  x   = vp[0], y = vp[1], z = vp[2], rhw = vp[3];
        float  ax  = x < 0.0f ? -x : x;
        float  ay  = y < 0.0f ? -y : y;
        int    bad = (ax > VB_OUTLIER_THRESH || ay > VB_OUTLIER_THRESH
                   || rhw < 0.0f || (rhw == rhw && rhw < 1e-6f && rhw != 0.0f)
                   || rhw != rhw);   /* NaN check */
        if (!bad) continue;

        char flag[16] = "";
        if (rhw != rhw)   wsprintfA(flag, "rhw=NaN");
        else if (rhw < 0) wsprintfA(flag, "rhw<0");
        else if (rhw < 1e-6f && rhw != 0.0f) wsprintfA(flag, "rhw~0");

        if (has_diffuse && (uv_off + 7 <= (int)stride)) {
            DWORD diff = *(DWORD*)(src + i*stride + diff_off);
            float u = *(float*)(src + i*stride + uv_off);
            float v = *(float*)(src + i*stride + uv_off + 4);
            wsprintfA(b,
                "[OUTLIER]   v[%u] x=%d y=%d z=%d rhw_e4=%d diff=0x%08X u=%d v=%d %s",
                i, (int)x, (int)y, (int)z,
                (int)(rhw * 10000.0f),
                diff, (int)(u*1000), (int)(v*1000), flag);
        } else {
            wsprintfA(b,
                "[OUTLIER]   v[%u] x=%d y=%d z=%d rhw_e4=%d %s",
                i, (int)x, (int)y, (int)z,
                (int)(rhw * 10000.0f), flag);
        }
        log_f(b);
    }

    /* Bone context: nearest events within +/-3 frames */
    bone_dump_context_for_vb(g_frame);

    /* Flag: next DrawPrimitive that uses this VB should dump its draw context */
    g_outlier_pending       = 1;
    g_outlier_pending_frame = g_frame;
}

static HRESULT WINAPI hook_VBUnlock(void* self) {
    VBShadow* e = svb_find(self);
    if (e && e->real_ptr && e->lock_sz > 0 && e->stride >= 4) {
        BYTE* src = e->shadow + e->lock_off;
        UINT  vc  = e->lock_sz / e->stride;

        /* Pre-correction outlier scan + Dragon-particle near-z suppression */
        if (e->lock_fvf & D3DFVF_XYZRHW) {

            /* --- Phase 1: scan for outliers (logging only, no modification) --- */
            float max_xy = 0.0f;
            for (UINT i = 0; i < vc; i++) {
                float* vp = (float*)(src + i * e->stride);
                float ax = vp[0] < 0.0f ? -vp[0] : vp[0];
                float ay = vp[1] < 0.0f ? -vp[1] : vp[1];
                if (ax > max_xy) max_xy = ax;
                if (ay > max_xy) max_xy = ay;
            }
            if (max_xy > VB_OUTLIER_THRESH)
                dump_vb_outlier(src, vc, e->stride, e->lock_count,
                                e->lock_fvf, e->lock_caller_ret, e->lock_caller_ret2);

            /*
             * --- Phase 2: Dragon-particle suppression ---
             *
             * Gated on confirmed Dragon-particle signature only:
             *   FVF == 0x0144, stride == 28
             *
             * For each 4-vertex quad, bbox is computed first, then one of:
             *   a) quad_max_xy > VB_OUTLIER_THRESH -> [SUPPRESS] hard collapse
             *      (collapse to (0,0) + alpha=0; clearly impossible coords)
             *   b) bbox extends beyond near-outlier bounds (but below threshold)
             *      -> [SUPPRESS-NEAR] also collapsed and logged separately.
             *      (Was log-only [NEAR_OUTLIER] in earlier builds; promoted
             *       to full suppression in v15.)
             *
             * Near-outlier bounds:
             *   x < -1024 or x > 1664  (~=2.5x screen width from centre)
             *   y < -1024 or y > 1504  (~=3x screen height from centre)
             *   OR bbox width or height > 1500 px
             */
            if (e->lock_fvf == 0x0144 && e->stride == 28) {
                const int diff_off    = 16;
                UINT chunks           = vc / 4;
                UINT quads_suppressed = 0;
                UINT verts_suppressed = 0;
                float suppress_max_xy = 0.0f;

                for (UINT ci = 0; ci < chunks; ci++) {
                    UINT base = ci * 4;

                    /* Compute bbox and abs-max for this quad in one pass */
                    float quad_max_xy = 0.0f;
                    float min_x =  1e9f, max_x = -1e9f;
                    float min_y =  1e9f, max_y = -1e9f;

                    for (UINT j = 0; j < 4; j++) {
                        float* vp = (float*)(src + (base + j) * 28);
                        float ax = vp[0] < 0.0f ? -vp[0] : vp[0];
                        float ay = vp[1] < 0.0f ? -vp[1] : vp[1];
                        if (ax > quad_max_xy) quad_max_xy = ax;
                        if (ay > quad_max_xy) quad_max_xy = ay;
                        if (vp[0] < min_x) min_x = vp[0];
                        if (vp[0] > max_x) max_x = vp[0];
                        if (vp[1] < min_y) min_y = vp[1];
                        if (vp[1] > max_y) max_y = vp[1];
                    }

                    float w = max_x - min_x;
                    float h = max_y - min_y;

                    if (quad_max_xy > VB_OUTLIER_THRESH) {
                        /* Hard suppress: clearly impossible coordinates */
                        if (quad_max_xy > suppress_max_xy)
                            suppress_max_xy = quad_max_xy;
                        for (UINT j = 0; j < 4; j++) {
                            UINT   vi  = base + j;
                            float* vp  = (float*)(src + vi * 28);
                            vp[0] = 0.0f;
                            vp[1] = 0.0f;
                            DWORD* diff = (DWORD*)(src + vi * 28 + diff_off);
                            *diff &= 0x00FFFFFF;
                        }
                        quads_suppressed++;
                        verts_suppressed += 4;

                    } else if (min_x < -1024.0f || max_x > 1664.0f ||
                               min_y < -1024.0f || max_y > 1504.0f ||
                               w > 1500.0f       || h > 1500.0f) {
                        /* Near-outlier suppress: quad below hard threshold but
                         * bbox confirms off-screen extent. Collapse and log
                         * as SUPPRESS-NEAR to distinguish from hard path. */
                        if (quad_max_xy > suppress_max_xy)
                            suppress_max_xy = quad_max_xy;
                        for (UINT j = 0; j < 4; j++) {
                            UINT   vi  = base + j;
                            float* vp  = (float*)(src + vi * 28);
                            vp[0] = 0.0f;
                            vp[1] = 0.0f;
                            DWORD* diff = (DWORD*)(src + vi * 28 + diff_off);
                            *diff &= 0x00FFFFFF;
                        }
                        quads_suppressed++;
                        verts_suppressed += 4;
                        {
                            char b[320];
                            wsprintfA(b,
                                "[SUPPRESS-NEAR] F%05d lock#%u quad=%u"
                                " bbox=(%d,%d)-(%d,%d) w=%d h=%d tex0=%p",
                                g_frame, e->lock_count, ci,
                                (int)min_x, (int)min_y,
                                (int)max_x, (int)max_y,
                                (int)w, (int)h, g_ctx_tex[0]);
                            log_f(b);
                        }
                    }
                }
                /* Remainder vertices (vc % 4 != 0): not processed */

                if (quads_suppressed > 0) {
                    char b[320];
                    /* Track per-frame suppress stats for death-window state machine */
                    g_suppress_this_frame++;
                    {
                        int ti, found = 0;
                        for (ti = 0; ti < g_suppress_tex_n; ti++)
                            if (g_suppress_tex_this[ti] == g_ctx_tex[0]) { found=1; break; }
                        if (!found && g_suppress_tex_n < SUPPRESS_TEX_CAP)
                            g_suppress_tex_this[g_suppress_tex_n++] = g_ctx_tex[0];
                    }
                    wsprintfA(b,
                        "[SUPPRESS] F%05d lock#%u vc=%u stride=%u fvf=0x%04X"
                        " max_xy=%d quads=%u verts=%u"
                        " caller0=0x%08X caller1=0x%08X tex0=%p",
                        g_frame, e->lock_count, vc, e->stride, (UINT)e->lock_fvf,
                        (int)suppress_max_xy, quads_suppressed, verts_suppressed,
                        e->lock_caller_ret, e->lock_caller_ret2, g_ctx_tex[0]);
                    log_f(b);
                }
            }
        }

        /* DIAG: log sample X values before and after correction */
        if (e->lock_count < 8 || (e->lock_count % 500) == 0) {
            float x0_before = (vc > 0) ? *(float*)(src) : 0.0f;
            float y0_before = (vc > 0) ? *(float*)(src+4) : 0.0f;
            float xN_before = (vc > 1) ? *(float*)(src + (vc-1)*e->stride) : x0_before;

            correct_xyzrhw(src, vc, e->stride);

            float x0_after  = (vc > 0) ? *(float*)(src) : 0.0f;
            float y0_after  = (vc > 0) ? *(float*)(src+4) : 0.0f;
            float xN_after  = (vc > 1) ? *(float*)(src + (vc-1)*e->stride) : x0_after;

            char b[256];
            wsprintfA(b,
                "[VB] Unlock #%u ptr=%p vc=%u | x0: %d->%d  xN: %d->%d  y0: %d->%d",
                e->lock_count, self, vc,
                (int)x0_before, (int)x0_after,
                (int)xN_before, (int)xN_after,
                (int)y0_before, (int)y0_after);
            log_f(b);
        } else {
            correct_xyzrhw(src, vc, e->stride);
        }

        CopyMemory(e->real_ptr, src, e->lock_sz);
        e->real_ptr = NULL;
        e->lock_sz  = 0;
        e->lock_count++;
        e->vert_total += vc;
    }
    return real_VBUnlock(self);
}

/* =========================================================
 * Device hooks
 * ========================================================= */
static HRESULT WINAPI hook_SetTransform(void* self, DWORD st, const D3DMATRIX* m) {
    D3DMATRIX w;
    if (!m) return real_SetTransform(self, st, m);

    /* === high_y signature detector (v3.3) ==========================
     * If PSO ever submits the high_y bone matrix to D3D8 (in any
     * transform state, but typically D3DTS_WORLD=256), we'll see it
     * here BEFORE the call reaches the GPU. Diagnostic:
     *   - >0 hits: matrix-mutation path is reachable; bug not fixed
     *     means high_y matrix isn't responsible for the visible
     *     artifact (H1).
     *   - 0 hits across an entire Dragon arena run: PSO never sends
     *     this matrix to D3D8 -- it's consumed by software skinning
     *     (H2). Matrix mutation in the bone hook cannot affect the
     *     visible render.
     * First 5 in detail with their state code, then silent count. */
    {
        const float* fm = (const float*)m;
        float ty_dev = fm[13] - 2000.0f; if (ty_dev < 0) ty_dev = -ty_dev;
        float r0_dev = fm[0]  - 1.0f;    if (r0_dev < 0) r0_dev = -r0_dev;
        float r5_abs = fm[5];            if (r5_abs < 0) r5_abs = -r5_abs;
        float r10_abs= fm[10];           if (r10_abs < 0) r10_abs = -r10_abs;
        if (ty_dev < 0.5f && r0_dev < 0.01f && r5_abs < 0.01f && r10_abs < 0.01f) {
            if (g_setxform_highy_seen < 5) {
                char b[256];
                wsprintfA(b,
                    "[SETXFORM-HIGHY] F%05d state=%u "
                    "tx=%d/1000 ty=%d/1000 tz=%d/1000",
                    g_frame, (UINT)st,
                    (int)(fm[12]*1000.0f),
                    (int)(fm[13]*1000.0f),
                    (int)(fm[14]*1000.0f));
                log_f(b);
            }
            g_setxform_highy_seen++;
        }
    }

    /* Capture VIEW matrix for use in UP-scan view-space Z (death window only) */
    if (st == D3DTS_VIEW) {
        int k;
        for (k = 0; k < 16; k++) g_mat_view[k] = ((const float*)m)[k];
        g_mat_view_valid = 1;
    }

    if (st == D3DTS_PROJECTION) {
        int m00_i = (int)(m->m[0][0] * 10000.0f);
        if (proj_is_new(m00_i)) {
            char b[128];
            wsprintfA(b,
                "[TX] PROJECTION new m[0][0]=%d/10000 after_k_proj=%d/10000  k_proj=%d/10000",
                m00_i,
                (int)(m->m[0][0] * g_k_proj * 10000.0f),
                (int)(g_k_proj * 10000.0f));
            log_f(b);
        }
        w = *m;
        w.m[0][0] *= g_k_proj;
        return real_SetTransform(self, st, &w);
    }

    return real_SetTransform(self, st, m);
}

/* Last viewport as PSO set it (640x480 space) */
static D3DVIEWPORT8 g_pso_vp = {0, 0, 640, 480, 0.0f, 1.0f};

static HRESULT WINAPI hook_GetViewport(void* self, D3DVIEWPORT8* vp) {
    if (vp && g_bb_w > 640) {
        *vp = g_pso_vp;
        return 0;
    }
    return real_GetViewport(self, vp);
}

static int viewport_looks_physical(const D3DVIEWPORT8* vp) {
    if (!vp) return 0;
    if (g_bb_w <= 640 || g_bb_h <= 480) return 0;
    if (vp->X == 0 && vp->Y == 0 &&
        vp->Width == g_bb_w && vp->Height == g_bb_h)
        return 1;
    if (vp->X > 640 || vp->Y > 480 ||
        vp->Width > 640 || vp->Height > 480)
        return 1;
    return 0;
}

static HRESULT WINAPI hook_SetViewport(void* self, const D3DVIEWPORT8* vp) {
    if (!vp) return real_SetViewport(self, vp);

    if (g_bb_w > 640 && g_bb_h > 480) {
        int is_phys = viewport_looks_physical(vp);

        /* DIAG: log every unique viewport call with its disposition */
        if (vp_is_new(vp->X, vp->Y, vp->Width, vp->Height)) {
            char b[256];
            const char* disp = is_phys ? "PHYSICAL-PASSTHRU" :
                               (vp->Width == 640 && vp->Height == 480 && vp->X == 0 && vp->Y == 0)
                               ? "FULLSCREEN-14:9" : "SUB-VIEWPORT";
            wsprintfA(b,
                "[VP] NEW vp={%u,%u,%u,%u} disp=%s  pso_vp={%u,%u,%u,%u}",
                vp->X, vp->Y, vp->Width, vp->Height, disp,
                g_pso_vp.X, g_pso_vp.Y, g_pso_vp.Width, g_pso_vp.Height);
            log_f(b);

            if (is_phys) {
                char b2[128];
                wsprintfA(b2,
                    "[VP]  ^ viewport_looks_physical fired! bb=%ux%u bar_w=%u vp_w=%u",
                    g_bb_w, g_bb_h, g_bar_w, g_vp_w);
                log_f(b2);
            }
        }

        if (is_phys)
            return real_SetViewport(self, vp);

        g_pso_vp = *vp;
        {
            D3DVIEWPORT8 vp2 = *vp;
            float sx = (float)g_vp_w / 640.0f;
            float sy = (float)g_bb_h / 480.0f;

            if (vp->X == 0 && vp->Y == 0 &&
                vp->Width == 640 && vp->Height == 480) {
                vp2.X      = g_bar_w;
                vp2.Y      = 0;
                vp2.Width  = g_vp_w;
                vp2.Height = g_bb_h;
            } else {
                vp2.X      = g_bar_w + (DWORD)((float)vp->X * sx + 0.5f);
                vp2.Y      = (DWORD)((float)vp->Y * sy + 0.5f);
                vp2.Width  = (DWORD)((float)vp->Width  * sx + 0.5f);
                vp2.Height = (DWORD)((float)vp->Height * sy + 0.5f);
            }
            return real_SetViewport(self, &vp2);
        }
    }

    /* 640x480 backbuffer mode */
    if (g_xrhw_scale != 1.0f && vp->X != 0) {
        D3DVIEWPORT8 vp2 = *vp;
        vp2.X     = (DWORD)(g_xrhw_cx + ((float)vp->X - g_xrhw_cx) * g_xrhw_scale + 0.5f);
        vp2.Width = (DWORD)(vp->Width * g_xrhw_scale + 0.5f);
        return real_SetViewport(self, &vp2);
    }

    return real_SetViewport(self, vp);
}

static HRESULT WINAPI hook_SetVertexShader(void* self, DWORD h) {
    g_fvf = h;
    /* DIAG: log each unique FVF value */
    if (fvf_is_new(h)) {
        char b[80];
        wsprintfA(b, "[FVF] new FVF=0x%04X (XYZRHW=%s)",
            h, (h & D3DFVF_XYZRHW) ? "YES" : "no");
        log_f(b);
    }
    return real_SetVertexShader(self, h);
}

/* =============================================================
 * Draw context tracking
 * Captures texture, render state, and primitive type at draw
 * time when an OUTLIER vertex buffer was just unlocked.
 * ============================================================= */

/* Current texture per stage, updated by hook_SetTexture */
/* g_ctx_tex[4] forward-declared above hook_VBUnlock; initialized here */
static void* g_ctx_tex[4] = {0, 0, 0, 0};

/* Key render state snapshot */
static DWORD g_ctx_alphablendenable = 0;
static DWORD g_ctx_srcblend        = 2;   /* D3DBLEND_SRCALPHA */
static DWORD g_ctx_destblend       = 5;   /* D3DBLEND_INVSRCALPHA */
static DWORD g_ctx_zenable         = 1;
static DWORD g_ctx_zwriteenable    = 1;
static DWORD g_ctx_cullmode        = 2;   /* D3DCULL_CW */
static DWORD g_ctx_alphatestenable = 0;
static DWORD g_ctx_lighting        = 1;

/* ---- draw-signature recorder ---- */
static void record_draw_sig(BYTE draw_type, DWORD caller,
                             DWORD fvf, DWORD pt, UINT pc, UINT stride)
{
    UINT pc_bucket = pc >> 3;
    int i;
    for (i = 0; i < g_draw_sig_n; i++) {
        DrawSig16* s = &g_draw_sigs[i];
        if (s->caller == caller && s->draw_type == draw_type
            && s->fvf == fvf && s->pt == pt
            && s->pc_bucket == pc_bucket && s->stride == stride
            && s->tex0 == g_ctx_tex[0]
            && s->zwriteenable    == g_ctx_zwriteenable
            && s->alphablendenable == g_ctx_alphablendenable) {
            s->count++;
            s->last_frame = g_frame;
            return;
        }
    }
    if (g_draw_sig_n < MAX_DRAW_SIGS) {
        DrawSig16* s = &g_draw_sigs[g_draw_sig_n++];
        s->caller           = caller;
        s->draw_type        = draw_type;
        s->fvf              = fvf;
        s->pt               = pt;
        s->pc_bucket        = pc_bucket;
        s->stride           = stride;
        s->tex0             = g_ctx_tex[0];
        s->alphablendenable = g_ctx_alphablendenable;
        s->srcblend         = g_ctx_srcblend;
        s->destblend        = g_ctx_destblend;
        s->zenable          = g_ctx_zenable;
        s->zwriteenable     = g_ctx_zwriteenable;
        s->lighting         = g_ctx_lighting;
        s->cullmode         = g_ctx_cullmode;
        s->count            = 1;
        s->first_frame      = g_frame;
        s->last_frame       = g_frame;
        /* No log_f here -- entire table is dumped when death window closes */
    }
}

/* Recent state-change ring buffer -- types: 0xF001=SetTexture, else=SetRenderState */
#define SLOG_CAP 48
typedef struct { int frame; DWORD type; DWORD a; DWORD b; } SLogEntry;
static SLogEntry g_slog[SLOG_CAP];
static int       g_slog_head = 0;

static void slog_push(DWORD type, DWORD a, DWORD b) {
    g_slog[g_slog_head % SLOG_CAP] = (SLogEntry){ g_frame, type, a, b };
    g_slog_head++;
}

/* Set when VBUnlock detects an outlier; cleared after the next DrawPrimitive dumps */
/* (declared at top of file before dump_vb_outlier; not redeclared here) */

/* Query texture info -- intentionally minimal: just log the pointer.
 * Calling GetSurfaceLevel on dgvoodoo-wrapped textures crashes because
 * dgvoodoo's internal vtable layout differs from stock D3D8.
 * The pointer value alone is enough to compare Dragon vs loot-box draws. */

static void dump_draw_context(DWORD prim_type, UINT start_or_minidx, UINT prim_count,
                               DWORD dp_caller) {
    char b[512];

    /* dp_caller = __builtin_return_address(0) from hook_DrawPrimitive.
     * Device vtable is patched directly; PSO calls through it without a
     * dgvoodoo thunk layer, so this should be a real PSO code address. */
    wsprintfA(b, "[DRAWCTX] F%05d prim=%u count=%u fvf=0x%04X svb=%p dp_caller=0x%08X",
              g_frame, prim_type, prim_count, (UINT)g_fvf, g_s0_vb, dp_caller);
    log_f(b);

    wsprintfA(b,
        "[DRAWCTX]  rs: ablend=%u src=%u dst=%u atest=%u zenable=%u zwrite=%u cull=%u lit=%u",
        g_ctx_alphablendenable, g_ctx_srcblend, g_ctx_destblend, g_ctx_alphatestenable,
        g_ctx_zenable, g_ctx_zwriteenable, g_ctx_cullmode, g_ctx_lighting);
    log_f(b);

    /* Log texture pointers for stages 0-3 */
    for (int s = 0; s < 4; s++) {
        void* t = g_ctx_tex[s];
        if (!t) { if (s == 0) log_f("[DRAWCTX]  tex0=NULL"); continue; }
        wsprintfA(b, "[DRAWCTX]  tex%d=%p", s, t);
        log_f(b);
    }

    /* Recent state changes: last 24 entries */
    int start = g_slog_head - 24;
    if (start < 0) start = 0;
    for (int i = start; i < g_slog_head; i++) {
        SLogEntry* e = &g_slog[i % SLOG_CAP];
        if (e->type == 0xF001)
            wsprintfA(b, "[DRAWCTX]  F%05d SetTexture stage=%u ptr=%p",
                      e->frame, e->a, (void*)(DWORD_PTR)e->b);
        else
            wsprintfA(b, "[DRAWCTX]  F%05d SetRS state=%u val=0x%X",
                      e->frame, e->type, e->b);
        log_f(b);
    }
}

static HRESULT WINAPI hook_SetTexture(void* self, DWORD stage, void* tex) {
    if (stage < 4 && g_ctx_tex[stage] != tex) {
        slog_push(0xF001, stage, (DWORD)(DWORD_PTR)tex);
        g_ctx_tex[stage] = tex;
    }
    return real_SetTexture(self, stage, tex);
}

static HRESULT WINAPI hook_SetStreamSource(void* self, UINT stream, void* vb, UINT stride) {
    if (stream == 0) {
        g_s0_vb     = vb;
        g_s0_stride = stride;

        if (vb && !g_vb_patched) {
            void** vvt = *(void***)vb;
            patch_slot(&vvt[11], (void*)hook_VBLock,   (void**)&real_VBLock);
            patch_slot(&vvt[12], (void*)hook_VBUnlock, (void**)&real_VBUnlock);
            g_vb_patched = 1;
            log_line("[VB] VB vtable patched");
        }

        if (vb && (g_fvf & D3DFVF_XYZRHW)) {
            VBShadow* e = svb_get(vb, stride);
            if (e && e->lock_count == 0) {
                /* DIAG: log first registration of this VB */
                char b[128];
                wsprintfA(b,
                    "[VB] Registered XYZRHW VB ptr=%p stride=%u vb_size=%u (svb_n=%d)",
                    vb, stride, e->vb_size, g_svb_n);
                log_f(b);
            }
        }
    }
    return real_SetStreamSource(self, stream, vb, stride);
}

static HRESULT WINAPI hook_DrawPrimitive(void* self, DWORD pt, UINT sv, UINT pc) {
    DWORD caller = (DWORD)__builtin_return_address(0);
    if (g_outlier_pending && g_frame - g_outlier_pending_frame <= 1) {
        dump_draw_context(pt, sv, pc, caller);
        g_outlier_pending = 0;
    }
    /* Death-window draw signature -- no file I/O, accumulate only */
    if (g_death_window && g_cfg_death_draw_sigs)
        record_draw_sig(0, caller, g_fvf, pt, pc, g_s0_stride);
    return real_DrawPrimitive(self, pt, sv, pc);
}

static HRESULT WINAPI hook_DrawIndexedPrimitive(
    void* self, DWORD pt, UINT min_idx, UINT nv, UINT si, UINT pc)
{
    DWORD caller = (DWORD)__builtin_return_address(0);
    if (g_outlier_pending && g_frame - g_outlier_pending_frame <= 1) {
        dump_draw_context(pt, min_idx, pc, caller);
        g_outlier_pending = 0;
    }
    /* Death-window draw signature -- no file I/O, accumulate only */
    if (g_death_window && g_cfg_death_draw_sigs)
        record_draw_sig(1, caller, g_fvf, pt, pc, g_s0_stride);
    return real_DrawIndexedPrimitive(self, pt, min_idx, nv, si, pc);
}

/* D3DRS constants needed for context tracking */
#define D3DRS_ZENABLE           7
#define D3DRS_ZWRITEENABLE     14
#define D3DRS_ALPHATESTENABLE  15
#define D3DRS_SRCBLEND         19
#define D3DRS_DESTBLEND        20
#define D3DRS_CULLMODE         22
#define D3DRS_ALPHABLENDENABLE 27
#define D3DRS_LIGHTING        137
#define D3DRS_POINTSIZE       154

static HRESULT WINAPI hook_SetRenderState(void* self, DWORD state, DWORD value) {
    /* ATOC passthrough (existing logic) */
    if (state == D3DRS_ALPHATESTENABLE) {
        real_SetRenderState(self, D3DRS_POINTSIZE,
            value ? ATOC_ENABLE : ATOC_DISABLE);
    }
    /* Track key states for draw-context dumps */
    switch (state) {
    case D3DRS_ALPHABLENDENABLE: g_ctx_alphablendenable = value; break;
    case D3DRS_SRCBLEND:         g_ctx_srcblend         = value; break;
    case D3DRS_DESTBLEND:        g_ctx_destblend        = value; break;
    case D3DRS_ZENABLE:          g_ctx_zenable          = value; break;
    case D3DRS_ZWRITEENABLE:     g_ctx_zwriteenable     = value; break;
    case D3DRS_CULLMODE:         g_ctx_cullmode         = value; break;
    case D3DRS_ALPHATESTENABLE:  g_ctx_alphatestenable  = value; break;
    case D3DRS_LIGHTING:         g_ctx_lighting         = value; break;
    }
    /* Push to ring buffer for recent-change replay in dump */
    slog_push(state, 0, value);
    return real_SetRenderState(self, state, value);
}

static HRESULT WINAPI hook_DrawPrimitiveUP(
    void* self, DWORD pt, UINT pc, const void* pD, UINT stride)
{
    DWORD caller = (DWORD)__builtin_return_address(0);
    int is_2d = (g_fvf & D3DFVF_XYZRHW) &&
                (g_xrhw_scale != 1.0f || g_bb_w > 640) &&
                pD && stride >= 4;

    /* DIAG: log each unique UP call signature */
    if (up_is_new(g_fvf, pt, pc, stride)) {
        char b[192];
        UINT vc = vcount(pt, pc);
        float x0 = pD ? *(float*)pD : 0.0f;
        float y0 = pD ? *((float*)pD + 1) : 0.0f;
        wsprintfA(b,
            "[UP] NEW DrawPrimUP fvf=0x%04X pt=%u pc=%u stride=%u vc=%u x0=%d y0=%d correcting=%s",
            g_fvf, pt, pc, stride, vc, (int)x0, (int)y0,
            is_2d ? "YES" : "no");
        log_f(b);
    }

    /*
     * v16-lite UP scan -- death window only, non-XYZRHW, plausible stride.
     * Accumulates draw sig; if log cap allows, scans raw XYZ for extremes.
     * View-space Z computed only when raw scan is suspicious and cap permits.
     * No file I/O here -- log_f writes to buffer, flushed in Present.
     */
    if (g_death_window && g_cfg_up_scan
        && !(g_fvf & D3DFVF_XYZRHW)
        && stride >= 24 && stride <= 40
        && pD && pc > 0)
    {
        /* Always accumulate draw sig (no I/O) */
        if (g_cfg_death_draw_sigs)
            record_draw_sig(2, caller, g_fvf, pt, pc, stride);

        /* Raw XYZ scan -- only if log cap not exceeded */
        if (g_up_logs_this_frame  < g_cfg_max_up_frame &&
            g_up_logs_this_session < UP_LOG_CAP_SESSION)
        {
            UINT vc = vcount(pt, pc);
            UINT i;
            const BYTE* p = (const BYTE*)pD;
            float min_x =  1e30f, max_x = -1e30f;
            float min_y =  1e30f, max_y = -1e30f;
            float min_z =  1e30f, max_z = -1e30f;
            int bad = 0;
            for (i = 0; i < vc; i++, p += stride) {
                float wx = *(const float*)(p + 0);
                float wy = *(const float*)(p + 4);
                float wz = *(const float*)(p + 8);
                /* NaN/Inf check */
                if (wx != wx || wy != wy || wz != wz ||
                    wx > 3.3e38f || wx < -3.3e38f) { bad = 1; break; }
                if (wx < min_x) min_x = wx; if (wx > max_x) max_x = wx;
                if (wy < min_y) min_y = wy; if (wy > max_y) max_y = wy;
                if (wz < min_z) min_z = wz; if (wz > max_z) max_z = wz;
            }

            /* Suspicious: extreme world XY or near-zero world Z */
            int extreme = (!bad) && (min_x < -3000.f || max_x > 3000.f
                                  || min_y < -3000.f || max_y > 3000.f);
            int nearz_world = (!bad) && (max_z > -0.5f && max_z < 0.5f
                                      && min_z > -0.5f && min_z < 0.5f);

            /* Optional view-space Z when suspicious and mat available */
            float min_vz = 1e30f, max_vz = -1e30f;
            int vz_done = 0;
            if ((extreme || nearz_world || bad) && g_mat_view_valid) {
                p = (const BYTE*)pD;
                for (i = 0; i < vc; i++, p += stride) {
                    float wx = *(const float*)(p+0);
                    float wy = *(const float*)(p+4);
                    float wz = *(const float*)(p+8);
                    /* col-2 of row-vector VIEW matrix */
                    float vz = g_mat_view[2]*wx + g_mat_view[6]*wy
                             + g_mat_view[10]*wz + g_mat_view[14];
                    if (vz < min_vz) min_vz = vz;
                    if (vz > max_vz) max_vz = vz;
                }
                vz_done = 1;
            }

            if (bad || extreme || nearz_world
                || (vz_done && min_vz > -1.0f && max_vz < 1.0f))
            {
                char b[320];
                wsprintfA(b,
                    "[UP-3D] F%05d caller=0x%08X fvf=0x%04X pt=%u pc=%u "
                    "str=%u vc=%u xyz=[%d..%d,%d..%d,%d..%d] "
                    "tex0=%p ab=%u src=%u dst=%u zw=%u%s%s",
                    g_frame, caller, g_fvf, pt, pc, stride, vc,
                    (int)min_x,(int)max_x,(int)min_y,(int)max_y,
                    (int)min_z,(int)max_z,
                    g_ctx_tex[0],
                    g_ctx_alphablendenable,g_ctx_srcblend,g_ctx_destblend,
                    g_ctx_zwriteenable,
                    bad     ? " *** NAN/INF ***"    : "",
                    extreme ? " *** EXTREME-XY ***"  :
                    (vz_done && min_vz > -1.0f && max_vz < 1.0f)
                            ? " *** NEAR-Z ***" : "");
                log_f(b);
                if (vz_done) {
                    char bz[96];
                    wsprintfA(bz, "[UP-3D]  ^ vz=[%d/1000 .. %d/1000]",
                        (int)(min_vz*1000.f), (int)(max_vz*1000.f));
                    log_f(bz);
                }
                g_up_logs_this_frame++;
                g_up_logs_this_session++;
            }
        }
    }

    if (is_2d) {
        UINT  vc    = vcount(pt, pc);
        UINT  bytes = vc * stride;
        BYTE* buf   = get_buf(bytes);
        if (buf) {
            HRESULT hr;
            CopyMemory(buf, pD, bytes);
            correct_xyzrhw(buf, vc, stride);
            hr = real_DrawPrimitiveUP(self, pt, pc, buf, stride);
            free_buf(buf, bytes);
            return hr;
        }
    }
    return real_DrawPrimitiveUP(self, pt, pc, pD, stride);
}

/* ---- bar clearing ---- */
#define D3DCLEAR_TARGET 0x00000001
typedef struct { LONG x1, y1, x2, y2; } BarRect;

static void clear_black_bars_now(void* self) {
    D3DVIEWPORT8 saved, full;
    BarRect bars[2];
    HRESULT got_vp;

    if (!real_Clear || !real_SetViewport || g_bar_w == 0 || g_bb_w <= 640)
        return;

    bars[0].x1 = 0;              bars[0].y1 = 0;
    bars[0].x2 = (LONG)g_bar_w;  bars[0].y2 = (LONG)g_bb_h;
    bars[1].x1 = (LONG)(g_bar_w + g_vp_w);
    bars[1].y1 = 0;
    bars[1].x2 = (LONG)g_bb_w;   bars[1].y2 = (LONG)g_bb_h;

    got_vp = real_GetViewport ? real_GetViewport(self, &saved) : -1;

    full.X = 0; full.Y = 0;
    full.Width = g_bb_w; full.Height = g_bb_h;
    full.MinZ = 0.0f; full.MaxZ = 1.0f;
    real_SetViewport(self, &full);
    real_Clear(self, 2, bars, D3DCLEAR_TARGET, 0xFF000000, 1.0f, 0);

    if (got_vp == 0)
        real_SetViewport(self, &saved);
}

static HRESULT WINAPI hook_Present(void* self,
    const RECT* src, const RECT* dst, HWND hwnd, const void* dirty)
{
    /* Wall-clock timestamp every 60 frames (~1 sec) for newserv alignment */
    if ((g_frame % 60) == 0)
        log_timestamp("tick");

    /* Diagnostic: warn once if dragon ptr capture didn't trigger by F06000.
     * Prevents silently empty DWIN logs when ty>1000 heuristic doesn't match. */
    {
        static int s_dragon_ptr_warned = 0;
        if (!s_dragon_ptr_warned && g_dragon_ptr == NULL && g_frame >= 6000) {
            log_f("[BONE-PTR] WARNING: dragon ptr not captured by F06000 -- "
                  "DWIN will not arm. Check ty>1000 heuristic in bone_hook.h.");
            s_dragon_ptr_warned = 1;
        }
    }

    /* DIAG: dump VB table summary every 300 frames */
    if (g_frame > 0 && (g_frame % 300) == 0) {
        char b[128];
        int i;
        wsprintfA(b, "[STAT] frame=%u svb_n=%d vp_seen=%d up_seen=%d fvf_seen=%d",
            g_frame, g_svb_n, g_vp_seen_n, g_up_seen_n, g_fvf_seen_n);
        log_line(b);
        for (i = 0; i < g_svb_n; i++) {
            wsprintfA(b, "[STAT]   svb[%d] ptr=%p stride=%u sz=%u locks=%u verts=%u",
                i, g_svb[i].vb, g_svb[i].stride,
                g_svb[i].vb_size, g_svb[i].lock_count, g_svb[i].vert_total);
            log_line(b);
        }
    }

    /* ---- v16-lite-r2 death window state machine ---- */
    {
        int dragon_this_frame = g_dragon_active;
        if (dragon_this_frame)
            g_dragon_active_frame = g_frame;

        int fsd = g_frame - g_dragon_active_frame;

        /* Evaluate this frame's suppress diversity */
        int sup_count = g_suppress_this_frame;
        int sup_uniq  = g_suppress_tex_n;
        int is_fight_frame = (sup_uniq >= 3);          /* multi-tex = mid-fight */
        int is_dying_frame = (sup_count <= 4           /* few suppresses */
                           && sup_uniq  <= 1);         /* mono-tex or none */

        /* Passive burst marker for log anchoring */
        if (sup_count >= 20) {
            char b[96];
            wsprintfA(b, "[BURST] F%05d sups=%d uniq_tex=%d%s",
                g_frame, sup_count, sup_uniq,
                is_fight_frame ? " (fight burst)" : "");
            log_line(b);
            log_timestamp("burst");
        }

        /* State machine transitions */
        switch (g_dwin_state) {
        case DWIN_IDLE:
            if (is_fight_frame && g_frame >= FIGHT_MIN_FRAME && g_dragon_active) {
                g_dwin_state = DWIN_FIGHT_SEEN;
                log_f("[DWIN] fight confirmed -- watching for death transition");
                log_timestamp("fight_confirmed");
            }
            break;

        case DWIN_FIGHT_SEEN:
            if (is_fight_frame) {
                g_dwin_dying_frames = 0;  /* reset dying counter on any fight frame */
            } else if (is_dying_frame) {
                g_dwin_dying_frames++;
                if (g_dwin_dying_frames >= DEATH_SETTLE_FRAMES) {
                    /* Transition to open */
                    char b[96];
                    g_dwin_state         = DWIN_OPEN;
                    g_death_window       = 1;
                    g_death_window_start = g_frame;
                    g_draw_sig_n         = 0;
                    g_up_logs_this_session = 0;
                    wsprintfA(b,
                        "[DWIN] opened at F%05d after %d settle frames",
                        g_frame, DEATH_SETTLE_FRAMES);
                    log_f(b);
                    log_timestamp("dwin_open");
                }
            } else {
                g_dwin_dying_frames = 0;  /* non-fight, non-dying: reset */
            }
            break;

        case DWIN_OPEN:
            /* Auto-close: Dragon bone hook gone >=120 frames */
            if (fsd >= 120 && (g_frame - g_death_window_start) >= 30) {
                char b[288];
                int i;
                g_dwin_state   = DWIN_IDLE;
                g_death_window = 0;
                log_timestamp("dwin_close");
                wsprintfA(b,
                    "[DWIN] closed F%05d -- Dragon gone, %d sigs, %d UP logs, window=%d fr",
                    g_frame, g_draw_sig_n, g_up_logs_this_session,
                    g_frame - g_death_window_start);
                log_line(b);
                for (i = 0; i < g_draw_sig_n; i++) {
                    DrawSig16* s = &g_draw_sigs[i];
                    static const char* dt[] = {"DP","DIP","UP"};
                    wsprintfA(b,
                        "[DWIN-SIG] %s caller=0x%08X fvf=0x%04X pt=%u pc~%u "
                        "str=%u tex0=%p ab=%u src=%u dst=%u "
                        "ze=%u zw=%u lit=%u cull=%u hits=%u first=F%05d last=F%05d",
                        (s->draw_type < 3) ? dt[s->draw_type] : "??",
                        s->caller, s->fvf, s->pt, s->pc_bucket<<3,
                        s->stride, s->tex0,
                        s->alphablendenable, s->srcblend, s->destblend,
                        s->zenable, s->zwriteenable, s->lighting, s->cullmode,
                        s->count, s->first_frame, s->last_frame);
                    log_line(b);
                }
                g_draw_sig_n          = 0;
                g_up_logs_this_session = 0;
            }
            break;
        }

        /* Reset per-frame counters */
        g_dragon_active       = 0;
        g_up_logs_this_frame  = 0;
        g_suppress_this_frame = 0;
        g_suppress_tex_n      = 0;
    }

    g_frame++;
    bone_hook_flush();
    clear_black_bars_now(self);

    /* Flush log buffer -- single file write per frame */
    log_flush();

    return real_Present(self, src, dst, hwnd, dirty);
}

static HRESULT WINAPI hook_Clear(void* self, DWORD count, const void* pRects,
                                  DWORD flags, DWORD color, float z, DWORD stencil) {
    HRESULT hr = real_Clear(self, count, pRects, flags, color, z, stencil);
    if (hr == 0 && g_bar_w > 0 && (flags & D3DCLEAR_TARGET)) {
        BarRect bars[2];
        bars[0].x1 = 0;               bars[0].y1 = 0;
        bars[0].x2 = (LONG)g_bar_w;   bars[0].y2 = (LONG)g_bb_h;
        bars[1].x1 = (LONG)(g_bar_w + g_vp_w);
        bars[1].y1 = 0;
        bars[1].x2 = (LONG)g_bb_w;   bars[1].y2 = (LONG)g_bb_h;
        real_Clear(self, 2, bars, D3DCLEAR_TARGET, 0xFF000000, z, stencil);
    }
    return hr;
}

/* =========================================================
 * CreateDevice hook
 * ========================================================= */

/*
 * PATCHF with explicit PASS/FAIL logging.
 * Logs address, expected value, actual value, and whether the patch fired.
 */
static void do_patchf(DWORD va, float expected, float val, const char* tag) {
    float* p   = (float*)va;
    float  act = 0.0f;
    float  d;
    char   b[128];
    DWORD  old, tmp;

    if (IsBadReadPtr(p, 4)) {
        wsprintfA(b, "[PATCH] BADPTR addr=0x%08X tag=%s", va, tag);
        log_line(b); return;
    }

    act = *p;
    d   = act - expected; if (d < 0) d = -d;

    if (d < 0.1f) {
        VirtualProtect(p, 4, PAGE_EXECUTE_READWRITE, &old);
        *p = val;
        VirtualProtect(p, 4, old, &tmp);
        wsprintfA(b, "[PATCH] PASS  addr=0x%08X %-20s expect=%d/1000 actual=%d/1000 -> %d/1000",
            va, tag,
            (int)(expected*1000.0f), (int)(act*1000.0f), (int)(val*1000.0f));
    } else {
        wsprintfA(b, "[PATCH] FAIL  addr=0x%08X %-20s expect=%d/1000 actual=%d/1000 (diff=%d/1000)",
            va, tag,
            (int)(expected*1000.0f), (int)(act*1000.0f), (int)(d*1000.0f));
    }
    log_line(b);
}

static HRESULT WINAPI hook_CreateDevice(
    void* self, UINT adapter, DWORD dtype, HWND hwnd, DWORD flags,
    D3DPRESENT_PARAMETERS* params, void** ppDev)
{
    HRESULT hr;
    log_line("hook_CreateDevice called");
    if (params) {
        log_uint("  BackBufferWidth  = ", params->BackBufferWidth);
        log_uint("  BackBufferHeight = ", params->BackBufferHeight);
    }

    if (params && params->BackBufferWidth == 640 && params->BackBufferHeight == 480) {
        char cfg[MAX_PATH] = {0};
        char exe[MAX_PATH] = {0};
        char* p;
        int   tw, th;

        GetModuleFileNameA(NULL, exe, MAX_PATH);
        lstrcpyA(cfg, exe);
        for (p = cfg + lstrlenA(cfg) - 1; p > cfg; p--)
            if (*p == '\\' || *p == '/') { *(p+1) = '\0'; break; }
        lstrcatA(cfg, "widescreen_res.cfg");

        tw = GetPrivateProfileIntA("Resolution", "Width",  640, cfg);
        th = GetPrivateProfileIntA("Resolution", "Height", 480, cfg);

        /* v16-lite diagnostic config -- same .cfg file, [DragonDiag] section
         * EnableDeathDrawSigs=1   -- draw signature table during clear screen
         * EnableUPScan=1          -- scan non-XYZRHW UP calls in death window
         * MaxUPLogsPerFrame=5     -- cap UP scan log lines per frame          */
        g_vp_mode             = GetPrivateProfileIntA("Resolution","ViewportMode",      0,cfg);
        g_cfg_death_draw_sigs = GetPrivateProfileIntA("DragonDiag","EnableDeathDrawSigs",1,cfg);
        g_cfg_up_scan         = GetPrivateProfileIntA("DragonDiag","EnableUPScan",       1,cfg);
        g_cfg_max_up_frame    = GetPrivateProfileIntA("DragonDiag","MaxUPLogsPerFrame",  5,cfg);
        {
            char b[128];
            wsprintfA(b,
                "[DIAG] DragonDiag: draw_sigs=%d up_scan=%d max_up_frame=%d",
                g_cfg_death_draw_sigs, g_cfg_up_scan, g_cfg_max_up_frame);
            log_line(b);
        }

        if (tw < 640)  tw = 640;
        if (tw > 3840) tw = 3840;
        if (th < 480)  th = 480;
        if (th > 2160) th = 2160;

        if (tw != 640 || th != 480) {
            char b[96];
            wsprintfA(b, "  overriding to %dx%d", tw, th);
            log_line(b);
            params->BackBufferWidth  = (UINT)tw;
            params->BackBufferHeight = (UINT)th;
        }
    }

    hr = real_CreateDevice(self, adapter, dtype, hwnd, flags, params, ppDev);
    log_line(hr == 0 ? "CreateDevice succeeded" : "CreateDevice failed");

    if (hr == 0 && ppDev && *ppDev) {
        void** vt = *(void***)(*ppDev);

        if (params && params->BackBufferWidth && params->BackBufferHeight) {
            float bb_ar = (float)params->BackBufferWidth /
                          (float)params->BackBufferHeight;
            g_bb_w       = params->BackBufferWidth;
            g_bb_h       = params->BackBufferHeight;
            g_xrhw_scale = bb_ar / (16.0f / 9.0f);
            g_xrhw_cx    = params->BackBufferWidth * 0.5f;

            if (g_bb_w > 640) {
                /* Resolve viewport AR from config or auto-detect */
                if (g_vp_mode == 2) {
                    g_vp_ar = 16.0f / 9.0f;
                } else if (g_vp_mode == 3) {
                    g_vp_ar = 16.0f / 10.0f;
                } else if (g_vp_mode == 1) {
                    g_vp_ar = 14.0f / 9.0f;
                } else {
                    /* Auto: detect from backbuffer AR */
                    if (bb_ar >= 1.74f && bb_ar <= 1.82f)
                        g_vp_ar = 16.0f / 9.0f;
                    else if (bb_ar >= 1.57f && bb_ar <= 1.63f)
                        g_vp_ar = 16.0f / 10.0f;
                    else
                        g_vp_ar = 14.0f / 9.0f;
                }
                g_vp_w   = (UINT)((float)g_bb_h * g_vp_ar + 0.5f);
                g_bar_w  = (g_bb_w > g_vp_w) ? (g_bb_w - g_vp_w) / 2 : 0;
                g_k_proj = (4.0f / 3.0f) / g_vp_ar;
                g_hud_sy = (float)g_bb_h / 480.0f;
                g_hud_x0 = ((float)g_bb_w - 640.0f * g_hud_sy) * 0.5f;
            } else {
                g_vp_ar  = 16.0f / 9.0f;
                g_vp_w   = g_bb_w;
                g_bar_w  = 0;
                g_k_proj = (4.0f / 3.0f) / (16.0f / 9.0f);
                g_hud_sy = 1.0f;
                g_hud_x0 = 0.0f;
            }

            {
                char b[192];
                wsprintfA(b,
                    "[INIT] bb=%ux%u bar_w=%u vp_w=%u k_proj=%d/10000 hud_x0=%d hud_sy=%d/1000",
                    g_bb_w, g_bb_h, g_bar_w, g_vp_w,
                    (int)(g_k_proj * 10000.0f),
                    (int)g_hud_x0,
                    (int)(g_hud_sy * 1000.0f));
                log_line(b);
            }

            if (g_bb_w != 640 || g_bb_h != 480) {
                float  fw  = (float)g_bb_w, fh = (float)g_bb_h;
                float  fcx = fw * 0.5f,     fcy = fh * 0.5f;

                log_line("[PATCH] --- applying pso.exe patches ---");
                do_patchf(0x006517B4, 640.0f, fw,  "Width-A");
                do_patchf(0x00651848, 640.0f, fw,  "Width-B");
                do_patchf(0x00651864, 640.0f, fw,  "Width-C");
                do_patchf(0x006517B8, 480.0f, fh,  "Height-A");
                do_patchf(0x00671DB8, 480.0f, fh,  "Height-B");
                do_patchf(0x00671DC8, 480.0f, fh,  "Height-C");
                do_patchf(0x006724B4, 480.0f, fh,  "Height-D");
                do_patchf(0x006724CC, 480.0f, fh,  "Height-E");
                do_patchf(0x006724E4, 480.0f, fh,  "Height-F");
                do_patchf(0x0065020C, 320.0f, fcx, "CentreX-A");
                do_patchf(0x00651AC8, 320.0f, fcx, "CentreX-B");
                do_patchf(0x00651EC4, 320.0f, fcx, "CentreX-C");
                do_patchf(0x00651ECC, 320.0f, fcx, "CentreX-D");
                do_patchf(0x00650208, 240.0f, fcy, "CentreY-A");
                do_patchf(0x00651ACC, 240.0f, fcy, "CentreY-B");
                do_patchf(0x00671DC4, 240.0f, fcy, "CentreY-C");
                do_patchf(0x00671DD4, 240.0f, fcy, "CentreY-D");
                /* Frustum -- DllMain already set to 1.4. Expected is 1.4. */
                log_line("[PATCH] --- done ---");
            }

            /* Frustum -- AR-dependent, applies regardless of internal bb size.
             * For dgvoodoo-stretched modes (bb stays 640x480), this is the only
             * way first-path culling matches g_k_proj and SecondCull. */
            {
                float frustum = (1.0f / g_k_proj) * 1.2f;
                do_patchf(0x0064FA70, 1.4f, frustum, "Frustum");
            }

            /*
             * Second (per-mesh) culling path -- computed from viewport AR.
             * Scaled from calibrated 14:9 values (sin=0.358419, cos=0.933561)
             * using vp_ar / (14/9).  Precomputed for three standard ARs;
             * generic path handles any other AR with the same formula.
             *   14:9  sin=0.358419 cos=0.933561
             *   16:9  sin=0.401797 cos=0.915731
             *   16:10 sin=0.367300 cos=0.930110
             */
            {
                float sc_sin, sc_cos;
                float vp = (g_bb_w > 640) ? g_vp_ar : (16.0f/9.0f);
                /* Check for standard ARs first (no sqrt needed) */
                if (vp >= 14.0f/9.0f - 0.01f && vp <= 14.0f/9.0f + 0.01f) {
                    sc_sin = 0.358419f; sc_cos = 0.933561f;  /* 14:9 calibrated */
                } else if (vp >= 16.0f/9.0f - 0.01f && vp <= 16.0f/9.0f + 0.01f) {
                    sc_sin = 0.45f; sc_cos = 0.893f;  /* 16:9 */
                } else if (vp >= 16.0f/10.0f - 0.01f && vp <= 16.0f/10.0f + 0.01f) {
                    sc_sin = 0.367300f; sc_cos = 0.930110f;  /* 16:10 */
                } else {
                    /* Generic: scale tan from 14:9 anchor, approximate 1/sqrt */
                    float t = (0.358419f/0.933561f) * (vp / (14.0f/9.0f));
                    float q = 1.0f + t*t;
                    /* Fast inverse sqrt (Quake III), one Newton step */
                    float x = q * 0.5f;
                    int   i = 0x5F3759DF - (*(int*)&q >> 1);
                    float r = *(float*)&i;
                    r = r * (1.5f - x * r * r);
                    sc_cos = r;
                    sc_sin = t * r;
                }
                do_patchf(0x00441EDF, 0.3f,  sc_sin, "SecondCull-Sin");
                do_patchf(0x00441EE4, 0.96f, sc_cos, "SecondCull-Cos");
            }
        }

        /*
         * Per-bone (or per-something else cone-using) culling override.
         *
         * Overrides runtime cone globals at 0x6F7B4C (cos) and 0x6F7B50 (sin),
         * and NOPs out the 8 instructions that overwrite them during gameplay.
         *
         * Per-bone cone globals use locally recomputed SecondCull values.  Do not
         * reuse the sc_sin/sc_cos variables from the SecondCull patch block above;
         * those variables are intentionally block-scoped and are not visible here.
         *
         * EXPERIMENTAL: addresses confirmed via static analysis, but the exact
         * culling test that reads these globals has not been runtime-verified.
         */
        {
            float sc_sin;
            float sc_cos;
            float vp = (g_bb_w > 640) ? g_vp_ar : (16.0f / 9.0f);
            static const DWORD nop_addrs[] = {
                0x005046F2, 0x005046FB,
                0x00504767, 0x00504773,
                0x00503705, 0x0050370E,
                0x00503A2B, 0x00503A34,
            };
            DWORD old;
            int i;

            if (vp >= 14.0f / 9.0f - 0.01f && vp <= 14.0f / 9.0f + 0.01f) {
                sc_sin = 0.358419f;
                sc_cos = 0.933561f;
            } else if (vp >= 16.0f / 9.0f - 0.01f && vp <= 16.0f / 9.0f + 0.01f) {
                sc_sin = 0.45f;
                sc_cos = 0.893f;
            } else if (vp >= 16.0f / 10.0f - 0.01f && vp <= 16.0f / 10.0f + 0.01f) {
                sc_sin = 0.367300f;
                sc_cos = 0.930110f;
            } else {
                float t = (0.358419f / 0.933561f) * (vp / (14.0f / 9.0f));
                float q = 1.0f + t * t;
                float x = q * 0.5f;
                int bits = 0x5F3759DF - (*(int*)&q >> 1);
                float r = *(float*)&bits;

                r = r * (1.5f - x * r * r);
                sc_cos = r;
                sc_sin = t * r;
            }

            for (i = 0; i < 8; i++) {
                BYTE* p = (BYTE*)nop_addrs[i];

                VirtualProtect(p, 6, PAGE_EXECUTE_READWRITE, &old);
                memset(p, 0x90, 6);
                VirtualProtect(p, 6, old, &old);
            }

            {
                float* p_cos = (float*)0x006F7B4C;
                float* p_sin = (float*)0x006F7B50;

                VirtualProtect(p_cos, 8, PAGE_READWRITE, &old);
                *p_cos = sc_cos;
                *p_sin = sc_sin;
                VirtualProtect(p_cos, 8, old, &old);
            }

            log_line("[PATCH] per-bone cone globals overridden (8 NOPs + 2 writes)");
        }

        log_line("patching device vtable");
        patch_slot(&vt[23], hook_CreateVertexBuffer,   (void**)&real_CreateVertexBuffer);
        patch_slot(&vt[15], hook_Present,              (void**)&real_Present);
        patch_slot(&vt[36], hook_Clear,                (void**)&real_Clear);
        patch_slot(&vt[37], hook_SetTransform,         (void**)&real_SetTransform);
        patch_slot(&vt[40], hook_SetViewport,          (void**)&real_SetViewport);
        patch_slot(&vt[41], hook_GetViewport,          (void**)&real_GetViewport);
        patch_slot(&vt[50], hook_SetRenderState,       (void**)&real_SetRenderState);
        patch_slot(&vt[61], hook_SetTexture,           (void**)&real_SetTexture);
        patch_slot(&vt[70], hook_DrawPrimitive,        (void**)&real_DrawPrimitive);
        patch_slot(&vt[71], hook_DrawIndexedPrimitive, (void**)&real_DrawIndexedPrimitive);
        patch_slot(&vt[72], hook_DrawPrimitiveUP,      (void**)&real_DrawPrimitiveUP);
        patch_slot(&vt[76], hook_SetVertexShader,      (void**)&real_SetVertexShader);
        patch_slot(&vt[83], hook_SetStreamSource,      (void**)&real_SetStreamSource);
        log_line("device vtable patched");
        bone_hook_install();
    }
    return hr;
}

/* =========================================================
 * Direct3DCreate8 export
 * ========================================================= */
__declspec(dllexport)
void* WINAPI Direct3DCreate8(UINT SDKVersion) {
    void* obj;
    log_line("Direct3DCreate8 called");
    if (!real_d3d8) {
        log_line("loading D3D8_dgvoodoo.dll");
        real_d3d8 = LoadLibraryA("D3D8_dgvoodoo.dll");
        if (!real_d3d8) { log_line("ERROR: load failed"); return NULL; }
        real_Direct3DCreate8 = (Direct3DCreate8_t)
            GetProcAddress(real_d3d8, "Direct3DCreate8");
        if (!real_Direct3DCreate8) { log_line("ERROR: GetProcAddress failed"); return NULL; }
        log_line("Direct3DCreate8 resolved");
    }
    obj = real_Direct3DCreate8(SDKVersion);
    if (!obj) { log_line("ERROR: real Direct3DCreate8 returned NULL"); return NULL; }
    log_line("real Direct3DCreate8 succeeded");
    if (!hooked) {
        void** vt = *(void***)obj;
        patch_slot(&vt[15], hook_CreateDevice, (void**)&real_CreateDevice);
        hooked = 1;
        log_line("IDirect3D8::CreateDevice hooked at slot 15");
    }
    return obj;
}

/* ---- required exports ---- */
__declspec(dllexport) void WINAPI DebugSetMute(void)         {}
__declspec(dllexport) void WINAPI ValidatePixelShader(void)  {}
__declspec(dllexport) void WINAPI ValidateVertexShader(void) {}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID r) {
    (void)h; (void)r;
    if (reason == DLL_PROCESS_ATTACH) {
        /* Wipe log file on each launch for clean sessions */
        HANDLE lh = CreateFileA("pso-peeps-d3d8-wsh.log",
            GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (lh != INVALID_HANDLE_VALUE) CloseHandle(lh);

        log_line("d3d8 widescreen proxy v16-lite-r8 loaded");
        log_timestamp("startup");
        log_flush();
        {
            char  path[MAX_PATH] = {0};
            char* fname          = path;
            char* p;
            float cur_val;
            GetModuleFileNameA(NULL, path, MAX_PATH);
            for (p = path; *p; p++)
                if (*p == '\\' || *p == '/') fname = p + 1;

            if (lstrcmpiA(fname, "pso.exe") == 0) {
                float* fp = (float*)0x0064FA70;
                DWORD  old;
                float  target = (7.0f / 6.0f) * 1.2f; /* 1.4 */

                /* Log what value is currently at the frustum address */
                cur_val = IsBadReadPtr(fp, 4) ? -1.0f : *fp;
                {
                    char b[128];
                    wsprintfA(b,
                        "[DLLMAIN] frustum addr=0x0064FA70 current=%d/1000 -> setting %d/1000",
                        (int)(cur_val * 1000.0f), (int)(target * 1000.0f));
                    log_line(b);
                }

                VirtualProtect(fp, sizeof(float), PAGE_EXECUTE_READWRITE, &old);
                *fp = target;
                VirtualProtect(fp, sizeof(float), old, &old);
                log_line("[DLLMAIN] frustum pre-set for 14:9; AR-specific values applied in CreateDevice");
            } else {
                log_line("[DLLMAIN] skipping patches (not pso.exe)");
            }
        }
    }
    return TRUE;
}
