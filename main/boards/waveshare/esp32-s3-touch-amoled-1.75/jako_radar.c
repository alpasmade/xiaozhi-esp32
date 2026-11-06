// Copyright 2026 Alpas Made. All rights reserved.
/* The flight radar on LVGL 9.5: a port of the Scope in
   mockup/radar-concepts/scope.js, with the drawing, text and block layout
   helpers of shared.js that it uses. Its rules, constants and order of
   drawing are the mock's; where the board must differ it says so here.

   All geometry is integer: angles in tenths of a degree from a quarter
   sine table in Q14, block layout in 1/256 px, so the Mac preview and the
   board compute the same pixels. No float anywhere.

   Two styles, picked by each update's "ln" (jako_radar_style_t): the ring
   (the default), a light of six stepped segments running round the 15 mile
   ring with marks at the newest update's places, and the line, which turns
   out to the edge of the radar with its wedge and repaints each plane as it
   passes. The widget keeps the style it drew; an update of the other style
   redraws the whole radar once.

   Each tick the widget works out what it would draw (jr_geom_t), compares
   it with what it drew last, and invalidates only the parts that changed:
   the line and the wedge as short chains of boxes along each moving edge,
   the ring's light as one chain of boxes along the ring from its old tail
   to its new head, a mark only when its record or fade step changes, text
   only when it changes. The boxes are merged before they reach LVGL, at most
   JAKO_RADAR_INV_MAX a tick, so the face's ring tiles always fit in
   LVGL's 32 slot list. The draw callback only draws; it never
   invalidates, and it skips everything outside the area being drawn.

   Where the board differs from the mock, on purpose:
   - Wedge edges are whole degrees (LVGL arcs take whole degrees); the line
     itself is at the exact 2.4 degree step and covers the half degree.
   - The ring's light: its segment edges are whole degrees too, rounded like
     the wedge's, with no 0.3 degree overlap between segments, and its
     widths are whole pixels, 2, 2, 3, 3, 4, 4 from the tail (the mock's 2,
     2, 2.5, 3, 3.5, 4), each centred on the 168 px ring as near as whole
     pixels allow (the 3 px ones sit half a pixel out).
   - The line and its wedge end at 191 px (JR_LINE_R), so nothing they
     light is past 193 px, where the text plates end, and not at 197. On
     the Mac the two light 192.8 px at most over a turn; a wedge of 192 px
     lit antialiased pixels just past 193 at some bearings, so the wedge
     ends with the line. The mock's Edge reach uses the same 191.
   - The grid is a crisp 1 px column or row; the page's canvas spreads it
     over two half-lit pixels.
   - Text is 20 and 22 px (22.4 on the page), whole-pixel advances, no
     kerning.
   - Knockouts (the black plates under text) stop 193 px from the centre,
     inside the state ring's glow, because the ring is under the radar
     here while the page draws it on top.
   - Round-capped strokes under full strength (the chevrons, the crosshair,
     the ticks) are drawn opaque in Light premixed with black, so their caps
     and the chevron's nose do not double up; where one crosses a fainter
     guide it covers it instead of adding to it. */
#include "jako_radar.h"

#include <stdio.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_log.h"
#define JR_LOGI(fmt, ...) ESP_LOGI("jako_radar", fmt, ##__VA_ARGS__)
#else
#define JR_LOGI(fmt, ...) ((void)0)
#endif

LV_FONT_DECLARE(font_inter_600_20_4)
LV_FONT_DECLARE(font_inter_600_22_4)

/* ---- the mock's numbers (scope.js and shared.js), in px at 466 ---- */
#define JR_MAGIC 0x52414452u
#define JR_TURN_MS 5000          /* TURN_S: one turn per 5 s, read from the clock */
#define JR_STEPS 150             /* STEP 2.4 degrees: 150 steps a turn */
#define JR_STEP_TENTHS 24
#define JR_FULL 3600             /* a turn in tenths of a degree */
#define JR_PX_PER_MI_X10 112     /* PX_PER_MILE 11.2: d (tenths of a mile) x 112 / 100 */
#define JR_OUTER 168             /* the 15 mile ring: the crosshair, the grid and the ring's light */
#define JR_LINE_R 191            /* the line's end; with its round cap and the wedge, lit to 192.8 at most (plates: 193) */
#define JR_WEDGE_R JR_LINE_R     /* the wedge's outer radius follows the line's end */
#define JR_RIM 182               /* the rim dot lane */
#define JR_NORTH_R 188           /* "N" at twelve */
#define JR_R_IN 15               /* the guides and the line stop short of You */
#define JR_LINE_W 3
#define JR_SECTORS 6             /* WEDGE: 45 degrees in six sectors of 7.5 */
#define JR_SECTOR_TENTHS 75
#define JR_GRID_PITCH 28         /* GRID: every 2.5 miles, 1 px, at .10 */
#define JR_GRID_LINES 5
#define JR_RING_W 2
#define JR_CROSS_W 2
#define JR_MARK 15               /* T.MARK */
#define JR_MARK_FOCUS 20         /* T.MARK_FOCUS */
#define JR_RIM_DOT 10            /* T.RIM_DOT */
#define JR_RIM_DOT_FOCUS 14
#define JR_YOU_R 10              /* You: r 8 with a 3 px stroke, as an arc of outer radius 10 */
#define JR_YOU_W 3
#define JR_GAP_MS 250            /* GAP_MS: longer than this between ticks, it was not drawing */
#define JR_CAPTION 22            /* T.CAPTION 22.4, the nearest baked size */
#define JR_SMALL 20              /* T.TEXT_MIN */
#define JR_CAPTION_BASE 396      /* T.CAPTION_BASE, 0.85 of 466 */
#define JR_CONTENT_R JAKO_RADAR_CONTENT_R
#define JR_KNOCKOUT_R 193        /* the ring's glow begins 194 px from the centre */
#define JR_TEXT_SOFT 158         /* T.TEXT_SOFT .62 */
#define JR_TEXT_FULL 255
#define JR_BLOCK_GAP 16          /* the focus block's gap from its mark */
#define JR_BLOCK_PAD 6
#define JR_CHAIN 4               /* boxes per moving edge of the line style (5 and 6 measured no better) */
#define JR_BAND_LINKS 4          /* boxes along the band the ring's light sweeps in a step */
#define JR_BAND_IN 164           /* the band the ring's light can light, px */
#define JR_BAND_OUT 172
#define JR_RTICK_IN 172          /* the ring style's three cardinal ticks at east, south and west */
#define JR_RTICK_OUT 180
#define JR_RTICK_W 2
#define JR_LIGHT_SEGS 6          /* SWEEP: six segments of 4 degrees, 24 in all, ending at the head */
#define JR_LIGHT_SEG_TENTHS 40
#define JR_BOX_CAP 96
#define JR_TEXT_MAX 40
#define JR_NONE_MS 0xFFFFFFFFu   /* a record painted quietly, at .62 with no glow */

/* alphas in thousandths, as the mock writes them */
#define JR_GUIDE_MILLI 160       /* T.GUIDE */
#define JR_GRID_MILLI 100        /* GRID.alpha */
#define JR_STALE_MILLI 500       /* T.STALE */
#define JR_SOFT_MILLI 620
static const int32_t JR_WEDGE_MILLI[JR_SECTORS] = {320, 240, 180, 130, 90, 60}; /* from the line back */
static const int32_t JR_TRAIL_MILLI[3] = {420, 220, 160};                       /* T.TRAIL */
static const int32_t JR_TRAIL_DOT[3] = {6, 5, 4};                               /* T.TRAIL_DOT */
static const int32_t JR_GLOW_GROW[2] = {6, 3};                                  /* T.GLOW */
static const int32_t JR_GLOW_MILLI[2] = {100, 170};
/* PAINT: [ms after the pass, mark alpha, glow k], then .62 and no glow until the next pass */
static const int32_t JR_PAINT[3][3] = {{330, 1000, 1000}, {660, 820, 600}, {1000, 700, 300}};
/* The ring style. PING: [ms after the light passed, glow k], then no glow; the mark stays at .62 throughout.
   The light from the head back: alphas (the mock's SWEEP.alphas reversed) and whole-pixel widths. */
static const int32_t JR_PING[3][2] = {{330, 1000}, {660, 600}, {1000, 300}};
static const int32_t JR_LIGHT_MILLI[JR_LIGHT_SEGS] = {620, 420, 300, 220, 160, 90};
static const int32_t JR_LIGHT_W[JR_LIGHT_SEGS] = {4, 4, 3, 3, 2, 2};
_Static_assert(JR_LIGHT_SEGS == JR_SECTORS, "the light's edges share the wedge's edge array");
/* The loading pulse, T.LOADING .06 to .16 in 8 steps every 2.4 s, in millionths:
   .06 + .10 x (0.5 - 0.5 cos(k x 45 degrees)); reduced motion holds .11 */
static const int32_t JR_PULSE_MICRO[8] = {60000, 74645, 110000, 145355, 160000, 145355, 110000, 74645};
#define JR_PULSE_STEP_MS 300
#define JR_PULSE_REDUCED_MICRO 110000

/* sin of whole degrees 0..90 in Q14 */
static const int16_t JR_SIN_Q14[91] = {
    0,     286,   572,   857,   1143,  1428,  1713,  1997,  2280,  2563,  2845,  3126,  3406,  3686,
    3964,  4240,  4516,  4790,  5063,  5334,  5604,  5872,  6138,  6402,  6664,  6924,  7182,  7438,
    7692,  7943,  8192,  8438,  8682,  8923,  9162,  9397,  9630,  9860,  10087, 10311, 10531, 10749,
    10963, 11174, 11381, 11585, 11786, 11982, 12176, 12365, 12551, 12733, 12911, 13085, 13255, 13421,
    13583, 13741, 13894, 14044, 14189, 14330, 14466, 14598, 14726, 14849, 14968, 15082, 15191, 15296,
    15396, 15491, 15582, 15668, 15749, 15826, 15897, 15964, 16026, 16083, 16135, 16182, 16225, 16262,
    16294, 16322, 16344, 16362, 16374, 16382, 16384,
};

/* -------------------------------------------------------------- types */

typedef enum { JR_GEOM_NONE = 0, JR_GEOM_READY } jr_geom_state_t;
typedef enum { JR_MK_NONE = 0, JR_MK_PLANE, JR_MK_HOVER, JR_MK_RIM } jr_mark_kind_t;
typedef enum { JR_YOU_RING = 0, JR_YOU_DASHES } jr_you_t;

/* What the line painted for one plane (the mock's record), frozen until the next pass. */
typedef struct {
    uint16_t id;
    uint8_t in_range, hover;
    int16_t track;
    int16_t x, y;
    uint8_t trail_n;
    lv_point_t trail[JAKO_RADAR_MAX_TRAIL];
    uint32_t at; /* radar clock of the pass, or JR_NONE_MS */
    char line1[JAKO_RADAR_NAME_MAX + 1];
    char line2[24];
} jr_record_t;

/* Integer geometry, all int32 (and char arrays of multiples of 4) so structs compare with memcmp. */
typedef struct {
    int32_t kind, id, focus, order;
    lv_point_t c;
    int32_t opa, size, stroke;
    lv_point_t nose, wl, wr;
    int32_t glow_n;
    int32_t glow_r[2];
    int32_t glow_opa[2][2];
    int32_t trail_n;
    lv_point_t trail[JAKO_RADAR_MAX_TRAIL];
    int32_t trail_opa[JAKO_RADAR_MAX_TRAIL];
    lv_area_t bounds;
} jr_mark_geom_t;

typedef struct {
    int32_t shown, plate_shown;
    lv_area_t plate;
    int32_t font, opa;
    lv_point_t pos; /* top left of the label's line box */
    int32_t w;
    char text[JR_TEXT_MAX];
    lv_area_t bounds;
} jr_text_geom_t;

typedef struct {
    int32_t shown, line_n, you_again;
    lv_area_t plate;
    jr_text_geom_t line[2];
    lv_area_t bounds;
} jr_block_geom_t;

typedef struct {
    int32_t style; /* jako_radar_style_t: a change redraws the whole radar */
    int32_t grid_opa, ring_opa, guide_opa, you;
    int32_t wedge, head; /* wedge: the sweep is shown (the line and its wedge, or the ring's light) */
    int32_t edge[JR_SECTORS + 1]; /* whole degrees, bearing: the wedge's sectors, or the light's segments */
    lv_point_t line_a, line_b;
    jr_text_geom_t numeral[3];
    jr_text_geom_t north;
    jr_text_geom_t status;
    int32_t mark_n;
    jr_mark_geom_t mark[JAKO_RADAR_MAX_PLANES];
    jr_block_geom_t block;
} jr_geom_t;

typedef struct {
    lv_area_t a[JR_BOX_CAP];
    int32_t n;
    bool full;
} jr_boxes_t;

/* A rect for the block layout, in 1/256 px, with the mock's weight. */
typedef struct {
    int32_t x, y, w, h, weight;
} jr_rect_t;

typedef struct {
    uint32_t magic;
    lv_obj_t* obj;
    lv_timer_t* timer;
    uint32_t last_tick;
    jako_radar_expire_cb_t expire_cb;
    void* expire_user;
    bool expired;

    /* inputs */
    jako_radar_update_t u;
    uint32_t updates;      /* applied */
    uint32_t received_ms;  /* radar clock at the last apply */
    jako_face_state_t face;
    bool reduced;
    jako_radar_style_t style; /* the style drawn: the last applied update's */
    uint16_t pick;         /* long-press pick, 0 for none */
    uint32_t pick_ms;

    /* the clock and the line */
    uint32_t clock;        /* ms since create */
    int32_t angle_ms;      /* the line's angle as a phase of the turn, ms; -1 before the first tick */
    jako_radar_line_t mode;
    uint32_t last_fix;     /* updates seen by the last tick */

    jr_record_t rec[JAKO_RADAR_MAX_PLANES]; /* the line style's records */
    uint8_t rec_n;
    /* the ring style: when the light last passed each listed plane, while its glow lasts */
    uint16_t ping_id[JAKO_RADAR_MAX_PLANES];
    uint32_t ping_at[JAKO_RADAR_MAX_PLANES];
    uint8_t ping_n;
    jr_record_t snap[JAKO_RADAR_MAX_PLANES]; /* scratch: the marks while the line does not turn */
    uint16_t side_id;      /* the side the focus block took last tick, tried first */
    int32_t side_ux, side_uy;

    /* derived for the debug read-out */
    jako_radar_view_t view;
    int32_t flights_age_s, position_age_s;
    uint16_t focus_id;
    char status[JR_TEXT_MAX];
    uint32_t inval_areas, inval_px;

    /* the guides never move: worked out once */
    lv_point_t tick_a[35], tick_b[35];
    int32_t tick_w[35];
    lv_point_t rtick_a[3], rtick_b[3];
    lv_point_t cross_a[4], cross_b[4];
    lv_area_t grid[JR_GRID_LINES * 4];
    int32_t cx, cy;

    jr_geom_state_t geom_state;
    jr_geom_t geom, next;
    jr_boxes_t boxes;
} jr_t;

/* ------------------------------------------------------------ helpers */

static int32_t jr_rdiv(int64_t n, int64_t d) { return (int32_t)(n >= 0 ? (n + d / 2) / d : -((-n + d / 2) / d)); }
static int32_t jr_min(int32_t a, int32_t b) { return a < b ? a : b; }
static int32_t jr_max(int32_t a, int32_t b) { return a > b ? a : b; }
static int32_t jr_abs(int32_t a) { return a < 0 ? -a : a; }
static int32_t jr_norm(int32_t t) { return ((t % JR_FULL) + JR_FULL) % JR_FULL; }

static uint32_t jr_isqrt(uint64_t v)
{
    uint64_t r = 0, bit = (uint64_t)1 << 62;
    while (bit > v) bit >>= 2;
    while (bit != 0) {
        if (v >= r + bit) {
            v -= r + bit;
            r = (r >> 1) + bit;
        } else {
            r >>= 1;
        }
        bit >>= 2;
    }
    return (uint32_t)r;
}

/* sin of tenths of a degree, Q14, linear between whole degrees */
static int32_t jr_sin(int32_t t)
{
    t = jr_norm(t);
    int32_t sign = 1;
    if (t >= 1800) {
        t -= 1800;
        sign = -1;
    }
    if (t > 900) t = 1800 - t;
    const int32_t d = t / 10, f = t % 10;
    const int32_t lo = JR_SIN_Q14[d], hi = d < 90 ? JR_SIN_Q14[d + 1] : lo;
    return sign * (lo + jr_rdiv((int64_t)(hi - lo) * f, 10));
}

static int32_t jr_cos(int32_t t) { return jr_sin(t + 900); }

/* A point at bearing t (tenths, clockwise from north) and radius r16 (1/16 px) from (cx, cy). */
static lv_point_t jr_polar(int32_t cx, int32_t cy, int32_t t, int32_t r16)
{
    lv_point_t p;
    p.x = cx + jr_rdiv((int64_t)r16 * jr_sin(t), 16 * 16384);
    p.y = cy - jr_rdiv((int64_t)r16 * jr_cos(t), 16 * 16384);
    return p;
}

static int32_t jr_opa(int64_t milli) { return jr_rdiv(milli * 255, 1000); }

static void jr_area_empty(lv_area_t* a)
{
    a->x1 = 0;
    a->y1 = 0;
    a->x2 = -1;
    a->y2 = -1;
}

static bool jr_area_is_empty(const lv_area_t* a) { return a->x2 < a->x1 || a->y2 < a->y1; }

static void jr_area_join(lv_area_t* acc, const lv_area_t* b)
{
    if (jr_area_is_empty(b)) return;
    if (jr_area_is_empty(acc)) {
        *acc = *b;
        return;
    }
    acc->x1 = jr_min(acc->x1, b->x1);
    acc->y1 = jr_min(acc->y1, b->y1);
    acc->x2 = jr_max(acc->x2, b->x2);
    acc->y2 = jr_max(acc->y2, b->y2);
}

static void jr_area_grow(lv_area_t* a, int32_t by)
{
    a->x1 -= by;
    a->y1 -= by;
    a->x2 += by;
    a->y2 += by;
}

static bool jr_area_hits(const lv_area_t* a, const lv_area_t* b)
{
    return !jr_area_is_empty(a) && !jr_area_is_empty(b) && a->x1 <= b->x2 && b->x1 <= a->x2 && a->y1 <= b->y2 &&
           b->y1 <= a->y2;
}

static int32_t jr_area_px(const lv_area_t* a)
{
    return jr_area_is_empty(a) ? 0 : (a->x2 - a->x1 + 1) * (a->y2 - a->y1 + 1);
}

static void jr_area_point(lv_area_t* acc, lv_point_t p, int32_t by)
{
    lv_area_t a = {p.x - by, p.y - by, p.x + by, p.y + by};
    jr_area_join(acc, &a);
}

/* A disc of diameter d centred on (x, y): the box the mock's dot(x, y, d) covers. */
static lv_area_t jr_disc_area(lv_point_t c, int32_t d)
{
    lv_area_t a;
    a.x1 = c.x - d / 2;
    a.y1 = c.y - d / 2;
    a.x2 = a.x1 + d - 1;
    a.y2 = a.y1 + d - 1;
    return a;
}

static jr_t* jr_get(lv_obj_t* obj)
{
    if (obj == NULL) return NULL;
    jr_t* r = (jr_t*)lv_obj_get_user_data(obj);
    return (r != NULL && r->magic == JR_MAGIC) ? r : NULL;
}

static const lv_font_t* jr_font(int32_t px) { return px >= JR_CAPTION ? &font_inter_600_22_4 : &font_inter_600_20_4; }

static int32_t jr_text_w(int32_t px, const char* s)
{
    const lv_font_t* f = jr_font(px);
    int32_t w = 0;
    for (const char* p = s; *p != '\0'; p++) w += lv_font_get_glyph_width(f, (uint8_t)p[0], (uint8_t)p[1]);
    return w;
}

/* A text line with its left edge at x and its baseline at y. */
static void jr_text_at(jr_text_geom_t* t, const char* s, int32_t px, int32_t opa, int32_t x, int32_t baseline)
{
    const lv_font_t* f = jr_font(px);
    t->shown = 1;
    t->font = px;
    t->opa = opa;
    snprintf(t->text, sizeof(t->text), "%s", s);
    t->w = jr_text_w(px, t->text);
    t->pos.x = x;
    t->pos.y = baseline - (f->line_height - f->base_line);
    lv_area_t box = {x - 1, t->pos.y, x + t->w + 1, t->pos.y + f->line_height - 1};
    jr_area_join(&t->bounds, &box);
}

/* ----------------------------------------------------- state from data */

static int32_t jr_since_s(const jr_t* r) { return (int32_t)((r->clock - r->received_ms) / 1000u); }

static void jr_ages(jr_t* r)
{
    const int32_t since = jr_since_s(r);
    r->flights_age_s = r->u.age + since;
    r->position_age_s = r->u.pos_age + since;
}

static bool jr_flights_stale(const jr_t* r)
{
    return (r->u.state == JAKO_RADAR_STATE_LIVE || r->u.state == JAKO_RADAR_STATE_EMPTY) &&
           r->flights_age_s >= JAKO_RADAR_FLIGHTS_STALE_S;
}

static bool jr_position_stale(const jr_t* r) { return r->position_age_s >= JAKO_RADAR_POSITION_STALE_S; }

static jako_radar_view_t jr_view(const jr_t* r)
{
    switch (r->u.state) {
    case JAKO_RADAR_STATE_LOADING: return JAKO_RADAR_VIEW_LOADING;
    case JAKO_RADAR_STATE_ERROR: return JAKO_RADAR_VIEW_ERROR;
    case JAKO_RADAR_STATE_EMPTY: return JAKO_RADAR_VIEW_EMPTY;
    default: break;
    }
    const bool fs = jr_flights_stale(r), ps = jr_position_stale(r);
    if (fs && ps) return JAKO_RADAR_VIEW_STALE_BOTH;
    if (fs) return JAKO_RADAR_VIEW_STALE_FLIGHTS;
    if (ps) return JAKO_RADAR_VIEW_STALE_POSITION;
    return JAKO_RADAR_VIEW_LIVE;
}

static void jr_age_text(char* out, size_t n, const char* what, int32_t s)
{
    if (s < 60)
        snprintf(out, n, "%s %d s old", what, (int)s);
    else
        snprintf(out, n, "%s %d min old", what, (int)(s / 60));
}

/* The status slot, first match wins (the contract's order). opa 0: nothing. */
static int32_t jr_status(const jr_t* r, char* out, size_t n)
{
    out[0] = '\0';
    switch (r->u.state) {
    case JAKO_RADAR_STATE_LOADING: snprintf(out, n, "Checking the sky"); return JR_TEXT_SOFT;
    case JAKO_RADAR_STATE_EMPTY: snprintf(out, n, "Clear sky"); return JR_TEXT_SOFT;
    case JAKO_RADAR_STATE_ERROR: snprintf(out, n, "No flight data"); return JR_TEXT_FULL;
    default: break;
    }
    if (jr_position_stale(r)) {
        jr_age_text(out, n, "Position", r->position_age_s);
        return JR_TEXT_SOFT;
    }
    if (jr_flights_stale(r)) {
        jr_age_text(out, n, "Flights", r->flights_age_s);
        return JR_TEXT_SOFT;
    }
    if (r->u.src == JAKO_RADAR_SRC_SAMPLE) {
        snprintf(out, n, "Sample sky");
        return JR_TEXT_SOFT;
    }
    return 0;
}

static int32_t jr_find_plane(const jr_t* r, uint16_t id)
{
    if (id == 0) return -1;
    for (int32_t i = 0; i < r->u.plane_n; i++)
        if (r->u.planes[i].id == id) return i;
    return -1;
}

/* Focus shown: the pick while younger than 20 s and listed, else f when listed, else the first. */
static uint16_t jr_focus(jr_t* r)
{
    if (r->pick != 0 && (r->clock - r->pick_ms >= JAKO_RADAR_PICK_MS || jr_find_plane(r, r->pick) < 0)) r->pick = 0;
    if (r->pick != 0) return r->pick;
    if (jr_find_plane(r, r->u.focus) >= 0) return r->u.focus;
    return r->u.plane_n > 0 ? r->u.planes[0].id : 0;
}

/* ------------------------------------------------------------- records */

static void jr_alt(char* out, size_t n, int32_t ft)
{
    /* Math.round(ft / 100) * 100, and a thousands comma from 1,000 (geo.fmtAlt) */
    const int32_t r = (ft + 50 + 100000) / 100 * 100 - 100000;
    if (r < 1000)
        snprintf(out, n, "%d ft", (int)r);
    else
        snprintf(out, n, "%d,%03d ft", (int)(r / 1000), (int)(r % 1000));
}

static int32_t jr_mi_px16(int32_t d) { return jr_rdiv((int64_t)d * JR_PX_PER_MI_X10 * 16, 100); }

/* record(f, at) in scope.js */
static void jr_record(const jr_t* r, const jako_radar_plane_t* p, uint32_t at, jr_record_t* out)
{
    memset(out, 0, sizeof(*out));
    out->id = p->id;
    out->in_range = p->d <= JAKO_RADAR_IN_RANGE_D;
    out->hover = p->t < 0;
    out->track = p->t;
    const lv_point_t c = jr_polar(r->cx, r->cy, p->b * 10, out->in_range ? jr_mi_px16(p->d) : JR_RIM * 16);
    out->x = (int16_t)c.x;
    out->y = (int16_t)c.y;
    if (out->in_range)
        for (uint8_t i = 0; i < p->trail_n && i < JAKO_RADAR_MAX_TRAIL; i++) {
            if (p->trail[i].d > JAKO_RADAR_IN_RANGE_D) continue;
            out->trail[out->trail_n++] = jr_polar(r->cx, r->cy, p->trail[i].b * 10, jr_mi_px16(p->trail[i].d));
        }
    out->at = at;
    snprintf(out->line1, sizeof(out->line1), "%s", p->s);
    char alt[16];
    jr_alt(alt, sizeof(alt), p->a);
    if (p->c[0] != '\0')
        snprintf(out->line2, sizeof(out->line2), "%s, %s", p->c, alt);
    else
        snprintf(out->line2, sizeof(out->line2), "%s", alt);
}

static jr_record_t* jr_rec_find(jr_t* r, uint16_t id)
{
    for (uint8_t i = 0; i < r->rec_n; i++)
        if (r->rec[i].id == id) return &r->rec[i];
    return NULL;
}

static void jr_rec_set(jr_t* r, const jako_radar_plane_t* p, uint32_t at)
{
    jr_record_t* e = jr_rec_find(r, p->id);
    if (e == NULL) {
        if (r->rec_n >= JAKO_RADAR_MAX_PLANES) return;
        e = &r->rec[r->rec_n++];
    }
    jr_record(r, p, at, e);
}

static void jr_repaint_all(jr_t* r)
{
    r->rec_n = 0;
    for (uint8_t i = 0; i < r->u.plane_n; i++) jr_rec_set(r, &r->u.planes[i], JR_NONE_MS);
}

/* Did the committed head pass bearing brg (tenths), clockwise from a to b? */
static bool jr_passed(int32_t a, int32_t b, int32_t brg)
{
    const int32_t d = jr_norm(b - a), e = jr_norm(brg - a);
    return d > 0 && d < 1800 && e > 0 && e <= d;
}

/* The ring style's passes: the light lights a plane's glow as its committed head passes the plane's
   bearing while it turns. Its first frame, a catch-up after a hold and a gap light nothing, as the line
   repaints quietly then. A ping lasts as long as its glow, or until its plane is no longer listed. */
static void jr_pings(jr_t* r, jako_radar_line_t next, int32_t before, int32_t head, bool gap)
{
    if (next == JAKO_RADAR_LINE_STOPPED) {
        r->ping_n = 0;
        return;
    }
    uint8_t k = 0;
    for (uint8_t i = 0; i < r->ping_n; i++)
        if (r->clock - r->ping_at[i] < (uint32_t)JR_PING[2][0] && jr_find_plane(r, r->ping_id[i]) >= 0) {
            r->ping_id[k] = r->ping_id[i];
            r->ping_at[k] = r->ping_at[i];
            k++;
        }
    r->ping_n = k;
    if (next != JAKO_RADAR_LINE_MOVING || r->mode != JAKO_RADAR_LINE_MOVING || gap) return;
    for (uint8_t i = 0; i < r->u.plane_n; i++) {
        const jako_radar_plane_t* p = &r->u.planes[i];
        if (!jr_passed(before, head, p->b * 10)) continue;
        uint8_t at = 0;
        while (at < r->ping_n && r->ping_id[at] != p->id) at++;
        if (at == r->ping_n) {
            if (r->ping_n >= JAKO_RADAR_MAX_PLANES) continue;
            r->ping_n++;
        }
        r->ping_id[at] = p->id;
        r->ping_at[at] = r->clock;
    }
}

static uint32_t jr_ping_at(const jr_t* r, uint16_t id)
{
    for (uint8_t i = 0; i < r->ping_n; i++)
        if (r->ping_id[i] == id) return r->ping_at[i];
    return JR_NONE_MS;
}

/* The sweep, frame() in scope.js up to the marks: the line and its records, or the ring's light and its
   passes. Returns the committed head. */
static int32_t jr_sweep(jr_t* r, bool gap)
{
    const bool turns = r->view == JAKO_RADAR_VIEW_LIVE || r->view == JAKO_RADAR_VIEW_EMPTY ||
                       r->view == JAKO_RADAR_VIEW_STALE_POSITION;
    const bool shown = !r->reduced && turns;
    const jako_radar_line_t next = !shown                                    ? JAKO_RADAR_LINE_STOPPED
                                   : r->face == JAKO_FACE_STATE_THINKING ? JAKO_RADAR_LINE_HELD
                                                                             : JAKO_RADAR_LINE_MOVING;
    const int32_t clock_phase = (int32_t)(r->clock % JR_TURN_MS);
    /* While it turns the line reads the clock; after a hold it catches up in one step. */
    if (r->angle_ms < 0 || gap) r->angle_ms = clock_phase;
    const int32_t before = r->angle_ms * JR_STEPS / JR_TURN_MS * JR_STEP_TENTHS;
    if (next == JAKO_RADAR_LINE_MOVING) r->angle_ms = clock_phase;
    const int32_t head = r->angle_ms * JR_STEPS / JR_TURN_MS * JR_STEP_TENTHS;
    const bool new_fix = r->updates != r->last_fix;

    if (r->style == JAKO_RADAR_STYLE_RING) {
        /* the marks come from the newest update; only the passes are kept */
        jr_pings(r, next, before, head, gap);
        r->rec_n = 0;
    } else if (next != JAKO_RADAR_LINE_STOPPED) {
        r->ping_n = 0;
        const bool repaint = r->mode == JAKO_RADAR_LINE_STOPPED ||
                             (next == JAKO_RADAR_LINE_MOVING && r->mode == JAKO_RADAR_LINE_HELD) || gap ||
                             (next == JAKO_RADAR_LINE_HELD && new_fix);
        if (repaint) {
            jr_repaint_all(r);
        } else {
            for (uint8_t i = 0; i < r->u.plane_n; i++) {
                const jako_radar_plane_t* p = &r->u.planes[i];
                if (next == JAKO_RADAR_LINE_MOVING && jr_passed(before, head, p->b * 10))
                    jr_rec_set(r, p, r->clock);
                else if (jr_rec_find(r, p->id) == NULL)
                    jr_rec_set(r, p, JR_NONE_MS); /* a new plane, at once */
            }
            /* gone, at once */
            uint8_t k = 0;
            for (uint8_t i = 0; i < r->rec_n; i++)
                if (jr_find_plane(r, r->rec[i].id) >= 0) r->rec[k++] = r->rec[i];
            r->rec_n = k;
        }
    } else {
        r->rec_n = 0;
        r->ping_n = 0;
    }
    r->mode = next;
    r->last_fix = r->updates;
    return head;
}

/* ------------------------------------------------------- block layout */

#define JR_Q 256 /* 1/256 px */
#define JR_U 16384

static bool jr_q_inside(const jr_t* r, int32_t x, int32_t y, int32_t w, int32_t h)
{
    /* text.inside: all four corners within 197.01 px */
    const int64_t lim = (int64_t)(JR_CONTENT_R * JR_Q + 3) * (JR_CONTENT_R * JR_Q + 3);
    const int32_t cx = r->cx * JR_Q, cy = r->cy * JR_Q;
    const int32_t xs[2] = {x, x + w}, ys[2] = {y, y + h};
    for (int i = 0; i < 2; i++)
        for (int k = 0; k < 2; k++) {
            const int64_t dx = xs[i] - cx, dy = ys[k] - cy;
            if (dx * dx + dy * dy > lim) return false;
        }
    return true;
}

/* text.covers: does box b cover rect a, with pad px round it? */
static bool jr_q_covers(const jr_rect_t* b, const jr_rect_t* a, int32_t pad)
{
    const int32_t p = pad * JR_Q;
    return b->x < a->x + a->w + p && b->x + b->w > a->x - p && b->y < a->y + a->h + p && b->y + b->h > a->y - p;
}

typedef struct {
    bool ok;
    jr_rect_t box;
    int32_t ux, uy, cost, line_n;
} jr_place_t;

/* place() in shared.js for this block: lines are (22 px, 20 px); layout with n lines */
static jr_place_t jr_layout(const jr_t* r, int32_t ax, int32_t ay, const int32_t* dirs, int32_t dir_n,
                            const int32_t* text_w, int32_t n, const jr_rect_t* avoid, int32_t avoid_n)
{
    jr_place_t best = {0};
    static const int32_t sizes[2] = {JR_CAPTION, JR_SMALL};
    int32_t w = 0, h10 = 0;
    for (int32_t i = 0; i < n; i++) {
        w = jr_max(w, text_w[i]);
        h10 += sizes[i] * 12;
    }
    h10 += JR_BLOCK_PAD * 2 * 10 - sizes[n - 1] * 2;
    const int32_t wq = (w + JR_BLOCK_PAD * 2) * JR_Q, hq = h10 * JR_Q / 10;
    const int32_t cxq = r->cx * JR_Q, cyq = r->cy * JR_Q;
    for (int32_t k = 0; k < dir_n; k++) {
        const int32_t ux = dirs[k * 2], uy = dirs[k * 2 + 1];
        const int64_t extent = ((int64_t)jr_abs(ux) * wq / 2 + (int64_t)jr_abs(uy) * hq / 2) / JR_U;
        const int64_t reach = JR_BLOCK_GAP * JR_Q + extent;
        int32_t x = ax + (int32_t)(ux * reach / JR_U) - wq / 2, y = ay + (int32_t)(uy * reach / JR_U) - hq / 2;
        for (int i = 0; i < 240 && !jr_q_inside(r, x, y, wq, hq); i++) {
            const int64_t ex = cxq - (x + wq / 2), ey = cyq - (y + hq / 2);
            int64_t e = (int64_t)jr_isqrt((uint64_t)(ex * ex + ey * ey));
            if (e == 0) e = 1;
            x += (int32_t)(ex * 2 * JR_Q / e);
            y += (int32_t)(ey * 2 * JR_Q / e);
        }
        if (!jr_q_inside(r, x, y, wq, hq)) continue;
        const jr_rect_t box = {x, y, wq, hq, 0};
        int32_t cost = 0;
        for (int32_t a = 0; a < avoid_n; a++)
            if (jr_q_covers(&box, &avoid[a], 4)) cost += avoid[a].weight;
        if (!best.ok || cost < best.cost) {
            best.ok = true;
            best.box = box;
            best.ux = ux;
            best.uy = uy;
            best.cost = cost;
            best.line_n = n;
        }
        if (cost == 0) break;
    }
    return best;
}

static jr_place_t jr_place(jr_t* r, uint16_t id, int32_t ax, int32_t ay, const int32_t* text_w,
                           const jr_rect_t* avoid, int32_t avoid_n)
{
    /* toward the centre; sideways when the plane is within 64 px of it */
    int64_t dx = (int64_t)(r->cx * JR_Q - ax), dy = (int64_t)(r->cy * JR_Q - ay);
    const int64_t d = (int64_t)jr_isqrt((uint64_t)(dx * dx + dy * dy));
    if (d < 64 * JR_Q) {
        dx = ax >= r->cx * JR_Q ? 1 : -1;
        dy = 0;
    }
    int64_t dl = (int64_t)jr_isqrt((uint64_t)(dx * dx + dy * dy));
    if (dl == 0) {
        dx = 1;
        dy = 0;
        dl = 1;
    }
    const int32_t bx = (int32_t)(dx * JR_U / dl), by = (int32_t)(dy * JR_U / dl);
    static const int32_t turns[12] = {0, 300, -300, 600, -600, 900, -900, 1200, -1200, 1500, -1500, 1800};
    int32_t dirs[26];
    int32_t n = 0;
    if (r->side_id == id && id != 0) {
        dirs[n++] = r->side_ux;
        dirs[n++] = r->side_uy;
    }
    for (int i = 0; i < 12; i++) {
        const int32_t c = jr_cos(turns[i]), s = jr_sin(turns[i]);
        /* rotate (bx, by) by the turn: the mock's (dx cos - dy sin, dx sin + dy cos) */
        dirs[n++] = (int32_t)(((int64_t)bx * c - (int64_t)by * s) / JR_U);
        dirs[n++] = (int32_t)(((int64_t)bx * s + (int64_t)by * c) / JR_U);
    }
    jr_place_t best = jr_layout(r, ax, ay, dirs, n / 2, text_w, 2, avoid, avoid_n);
    /* when even the best side covers a hard rect, the last line goes before that is accepted */
    if (!best.ok || best.cost >= 100) {
        const jr_place_t one = jr_layout(r, ax, ay, dirs, n / 2, text_w, 1, avoid, avoid_n);
        if (one.ok && (!best.ok || one.cost < best.cost)) best = one;
    }
    if (best.ok) {
        r->side_id = id;
        r->side_ux = best.ux;
        r->side_uy = best.uy;
    } else {
        r->side_id = 0;
    }
    return best;
}

static jr_rect_t jr_rect_px(int32_t x, int32_t y, int32_t w, int32_t h, int32_t weight)
{
    jr_rect_t q = {x * JR_Q, y * JR_Q, w * JR_Q, h * JR_Q, weight};
    return q;
}

static lv_area_t jr_q_area(const jr_rect_t* q)
{
    lv_area_t a;
    a.x1 = jr_rdiv(q->x, JR_Q);
    a.y1 = jr_rdiv(q->y, JR_Q);
    a.x2 = jr_rdiv((int64_t)q->x + q->w, JR_Q) - 1;
    a.y2 = jr_rdiv((int64_t)q->y + q->h, JR_Q) - 1;
    return a;
}

/* ------------------------------------------------------------- geometry */

/* opacity of a plane mark and its glow k (thousandths) now. The line: stepped down from the pass, then .62.
   The ring: always .62, with the glow stepped down from the light's pass. */
static void jr_fade(const jr_t* r, const jr_record_t* e, int32_t* a, int32_t* k)
{
    *a = JR_SOFT_MILLI;
    *k = 0;
    if (e->at == JR_NONE_MS) return;
    const uint32_t since = r->clock - e->at;
    if (r->style == JAKO_RADAR_STYLE_RING) {
        for (int i = 0; i < 3; i++)
            if (since < (uint32_t)JR_PING[i][0]) {
                *k = JR_PING[i][1];
                return;
            }
        return;
    }
    for (int i = 0; i < 3; i++)
        if (since < (uint32_t)JR_PAINT[i][0]) {
            *a = JR_PAINT[i][1];
            *k = JR_PAINT[i][2];
            return;
        }
}

static void jr_mark_glow(jr_mark_geom_t* m, int32_t radius, int32_t k_milli, int32_t dim_milli)
{
    if (m->glow_n >= 2 || k_milli <= 0) return;
    const int32_t g = m->glow_n++;
    m->glow_r[g] = radius;
    for (int i = 0; i < 2; i++) m->glow_opa[g][i] = jr_opa((int64_t)JR_GLOW_MILLI[i] * k_milli * dim_milli / 1000000);
    for (int i = 0; i < 2; i++) {
        lv_area_t a = jr_disc_area(m->c, (radius + JR_GLOW_GROW[i]) * 2);
        jr_area_join(&m->bounds, &a);
    }
}

/* draw.plane: a chevron of two round-capped strokes meeting at the nose, or a round dot under 40 kt */
static void jr_mark_plane(const jr_t* r, jr_mark_geom_t* m, const jr_record_t* e, int32_t size, int32_t opa)
{
    (void)r;
    m->opa = opa;
    m->size = size;
    if (e->hover) {
        m->kind = JR_MK_HOVER;
        m->size = size * 8 / 10;
        lv_area_t a = jr_disc_area(m->c, m->size);
        jr_area_join(&m->bounds, &a);
        return;
    }
    m->kind = JR_MK_PLANE;
    m->stroke = jr_max(3, jr_rdiv((int64_t)size * 2, 10)); /* max(2.5, size x .2) in whole px: 3 and 4 */
    const int32_t t = e->track * 10;
    const int32_t fx = jr_sin(t), fy = -jr_cos(t), rx = jr_cos(t), ry = jr_sin(t);
    const int32_t nose = size * 8, back = size * 16 * 38 / 100, wing = size * 16 * 46 / 100; /* 1/16 px */
    const int64_t D = 16 * 16384;
    m->nose.x = m->c.x + jr_rdiv((int64_t)fx * nose, D);
    m->nose.y = m->c.y + jr_rdiv((int64_t)fy * nose, D);
    m->wl.x = m->c.x + jr_rdiv(-(int64_t)fx * back - (int64_t)rx * wing, D);
    m->wl.y = m->c.y + jr_rdiv(-(int64_t)fy * back - (int64_t)ry * wing, D);
    m->wr.x = m->c.x + jr_rdiv(-(int64_t)fx * back + (int64_t)rx * wing, D);
    m->wr.y = m->c.y + jr_rdiv(-(int64_t)fy * back + (int64_t)ry * wing, D);
    const int32_t by = m->stroke / 2 + 2;
    jr_area_point(&m->bounds, m->nose, by);
    jr_area_point(&m->bounds, m->wl, by);
    jr_area_point(&m->bounds, m->wr, by);
}

static void jr_mark_dot(jr_mark_geom_t* m, int32_t d, int32_t opa)
{
    m->kind = JR_MK_RIM;
    m->size = d;
    m->opa = opa;
    lv_area_t a = jr_disc_area(m->c, d);
    jr_area_join(&m->bounds, &a);
}

static void jr_mark_trail(jr_mark_geom_t* m, const jr_record_t* e, int32_t dim_milli)
{
    m->trail_n = e->trail_n;
    for (int32_t i = 0; i < e->trail_n; i++) {
        m->trail[i] = e->trail[i];
        m->trail_opa[i] = jr_opa((int64_t)JR_TRAIL_MILLI[i] * dim_milli / 1000);
        lv_area_t a = jr_disc_area(e->trail[i], JR_TRAIL_DOT[i]);
        jr_area_join(&m->bounds, &a);
    }
}

static void jr_mark(const jr_t* r, jr_mark_geom_t* m, const jr_record_t* e, bool focus, int32_t dim_milli)
{
    memset(m, 0, sizeof(*m));
    jr_area_empty(&m->bounds);
    m->id = e->id;
    m->focus = focus ? 1 : 0;
    m->c.x = e->x;
    m->c.y = e->y;
    int32_t a, k;
    jr_fade(r, e, &a, &k);
    if (!focus) {
        if (e->in_range) {
            jr_mark_trail(m, e, dim_milli);
            jr_mark_glow(m, 7, k, dim_milli);
            jr_mark_plane(r, m, e, JR_MARK, jr_opa((int64_t)a * dim_milli / 1000));
        } else {
            jr_mark_glow(m, JR_RIM_DOT / 2, k, dim_milli);
            jr_mark_dot(m, JR_RIM_DOT, jr_opa((int64_t)a * dim_milli / 1000));
        }
    } else {
        /* the plane in focus: larger, at full strength, with its own glow. On the rim the glows are 7, not
           the mock's 8: an even disc's box puts its centre half a pixel up and left of the mark, so 182 + 8 + 6
           lit a pixel or two just past 197 px at some north-west bearings; 182 + 7 + 6 stays inside. */
        const int32_t rim_glow = 7;
        if (e->in_range) jr_mark_trail(m, e, dim_milli);
        jr_mark_glow(m, e->in_range ? 8 : rim_glow, 1000, dim_milli);
        jr_mark_glow(m, e->in_range ? 10 : rim_glow, k, dim_milli);
        if (e->in_range)
            jr_mark_plane(r, m, e, JR_MARK_FOCUS, jr_opa(dim_milli));
        else
            jr_mark_dot(m, JR_RIM_DOT_FOCUS, jr_opa(dim_milli));
    }
    jr_area_grow(&m->bounds, 1);
}

/* A plate (knockout) with a centred line of text in it; the mock's numerals and N. */
static void jr_plate_text(const jr_t* r, jr_text_geom_t* t, const char* s, int32_t baseline, int32_t pad_x,
                          int32_t plate_y, int32_t plate_h)
{
    memset(t, 0, sizeof(*t));
    jr_area_empty(&t->bounds);
    const int32_t w = jr_text_w(JR_SMALL, s);
    jr_text_at(t, s, JR_SMALL, JR_TEXT_SOFT, r->cx - w / 2, baseline);
    t->plate_shown = 1;
    t->plate.x1 = r->cx - w / 2 - pad_x;
    t->plate.y1 = plate_y;
    t->plate.x2 = t->plate.x1 + w + pad_x * 2 - 1;
    t->plate.y2 = plate_y + plate_h - 1;
    jr_area_join(&t->bounds, &t->plate);
}

/* The chord a centred line may use at its baseline: text.chord in shared.js, in 1/256 px. */
static int32_t jr_chord_q(int32_t baseline, int32_t size)
{
    const int32_t half = 233; /* the screen's radius, T.PX / 2 */
    const int32_t top_q = baseline * JR_Q - size * JR_Q * 3 / 4, bottom_q = baseline * JR_Q + size * JR_Q / 5;
    int64_t most = INT64_MAX;
    const int32_t ys[2] = {top_q, bottom_q};
    for (int i = 0; i < 2; i++) {
        const int64_t dy = ys[i] - half * JR_Q, rr = (int64_t)half * JR_Q * half * JR_Q - dy * dy;
        const int64_t hc = rr > 0 ? (int64_t)jr_isqrt((uint64_t)rr) : 0;
        if (hc < most) most = hc;
    }
    return (int32_t)(2 * most - 466 * JR_Q * 16 / 100);
}

/* The status line in the caption slot: text.centred with a knockout. It shrinks to 20 px, then drops words. */
static void jr_status_geom(const jr_t* r, jr_text_geom_t* t, const char* s, int32_t opa)
{
    memset(t, 0, sizeof(*t));
    jr_area_empty(&t->bounds);
    if (opa <= 0 || s[0] == '\0') return;
    char buf[JR_TEXT_MAX];
    snprintf(buf, sizeof(buf), "%s", s);
    int32_t size = JR_CAPTION;
    if (jr_text_w(size, buf) * JR_Q > jr_chord_q(JR_CAPTION_BASE, size)) size = JR_SMALL;
    while (jr_text_w(size, buf) * JR_Q > jr_chord_q(JR_CAPTION_BASE, size)) {
        char* sp = strrchr(buf, ' ');
        if (sp == NULL) break;
        *sp = '\0';
    }
    const int32_t w = jr_text_w(size, buf);
    jr_text_at(t, buf, size, opa, r->cx - w / 2, JR_CAPTION_BASE);
    t->plate_shown = 1;
    t->plate.x1 = r->cx - w / 2 - 12;
    t->plate.y1 = JR_CAPTION_BASE - jr_rdiv((int64_t)size * 98, 100);
    t->plate.x2 = t->plate.x1 + w + 24 - 1;
    t->plate.y2 = t->plate.y1 + jr_rdiv((int64_t)size * 136, 100) - 1;
    jr_area_join(&t->bounds, &t->plate);
}

static void jr_compute(jr_t* r, jr_geom_t* g, int32_t head)
{
    memset(g, 0, sizeof(*g));
    g->style = r->style;
    for (int i = 0; i < 3; i++) jr_area_empty(&g->numeral[i].bounds);
    jr_area_empty(&g->north.bounds);
    jr_area_empty(&g->status.bounds);
    jr_area_empty(&g->block.bounds);
    const bool pos_stale = jr_position_stale(r);
    const bool flights_stale = r->view == JAKO_RADAR_VIEW_STALE_FLIGHTS || r->view == JAKO_RADAR_VIEW_STALE_BOTH;
    const int32_t dim = flights_stale ? JR_STALE_MILLI : 1000; /* snap.markAlpha */
    const int32_t stale = pos_stale ? JR_STALE_MILLI : 1000;

    /* the guides: .16, or the loading pulse */
    int64_t gmicro = (int64_t)JR_GUIDE_MILLI * 1000;
    if (r->view == JAKO_RADAR_VIEW_LOADING)
        gmicro = r->reduced ? JR_PULSE_REDUCED_MICRO : JR_PULSE_MICRO[(r->clock / JR_PULSE_STEP_MS) % 8];
    g->guide_opa = jr_rdiv(gmicro * 255, 1000000);
    g->ring_opa = jr_rdiv(gmicro * 255 * stale, 1000000000LL);
    g->grid_opa = jr_rdiv(gmicro * 255 * JR_GRID_MILLI * stale, (int64_t)JR_GUIDE_MILLI * 1000000000LL);
    g->you = pos_stale ? JR_YOU_DASHES : JR_YOU_RING;

    /* the sweep: the wedge and the line, or the ring's light */
    g->wedge = r->mode != JAKO_RADAR_LINE_STOPPED;
    if (g->wedge) {
        const int32_t span = r->style == JAKO_RADAR_STYLE_RING ? JR_LIGHT_SEG_TENTHS : JR_SECTOR_TENTHS;
        g->head = head;
        for (int k = 0; k <= JR_SECTORS; k++) g->edge[k] = jr_rdiv(head - k * span, 10);
        if (r->style == JAKO_RADAR_STYLE_LINE) {
            g->line_a = jr_polar(r->cx, r->cy, head, JR_R_IN * 16);
            g->line_b = jr_polar(r->cx, r->cy, head, JR_LINE_R * 16);
        }
    }

    /* the marks: from the line's records, or from the newest update in the ring style and while the line
       does not turn (the ring style with the light's passes) */
    jr_record_t* snap = r->snap;
    const jr_record_t* recs[JAKO_RADAR_MAX_PLANES];
    int32_t rn = 0;
    for (uint8_t i = 0; i < r->u.plane_n; i++) {
        if (r->style == JAKO_RADAR_STYLE_RING || r->mode == JAKO_RADAR_LINE_STOPPED) {
            jr_record(r, &r->u.planes[i], jr_ping_at(r, r->u.planes[i].id), &snap[rn]);
            recs[rn] = &snap[rn];
            rn++;
        } else {
            const jr_record_t* e = NULL;
            for (uint8_t k = 0; k < r->rec_n && e == NULL; k++)
                if (r->rec[k].id == r->u.planes[i].id) e = &r->rec[k];
            if (e != NULL) recs[rn++] = e;
        }
    }
    const jr_record_t* fr = NULL;
    jr_rect_t avoid[3 + 1 + 1 + 1 + 1 + JAKO_RADAR_MAX_PLANES];
    int32_t an = 0;
    jr_rect_t others[JAKO_RADAR_MAX_PLANES];
    int32_t on = 0;
    for (int32_t i = 0; i < rn; i++) {
        if (recs[i]->id == r->focus_id) {
            fr = recs[i];
            continue;
        }
        others[on++] = recs[i]->in_range ? jr_rect_px(recs[i]->x - 9, recs[i]->y - 9, 18, 18, 1)
                                         : jr_rect_px(recs[i]->x - 6, recs[i]->y - 6, 12, 12, 1);
    }

    /* the numerals on each ring at twelve, N and You, laid out before the block */
    static const char* labels[3] = {"5", "10", "15 mi"};
    jr_rect_t numeral_rect[3];
    if (!pos_stale)
        for (int i = 0; i < 3; i++) {
            const int32_t y = r->cy - (i + 1) * 56;
            const int32_t w = jr_text_w(JR_SMALL, labels[i]);
            numeral_rect[i] = jr_rect_px(0, 0, 0, 0, 100);
            numeral_rect[i].x = r->cx * JR_Q - w * JR_Q / 2 - 6 * JR_Q;
            numeral_rect[i].y = (y - 12) * JR_Q;
            numeral_rect[i].w = (w + 12) * JR_Q;
            numeral_rect[i].h = 24 * JR_Q;
            avoid[an++] = numeral_rect[i];
        }
    const int32_t nw = jr_text_w(JR_SMALL, "N");
    jr_rect_t n_rect = {r->cx * JR_Q - nw * JR_Q / 2 - 4 * JR_Q, (r->cy - JR_NORTH_R - 9) * JR_Q, (nw + 8) * JR_Q,
                        20 * JR_Q, 100};
    avoid[an++] = n_rect;
    const jr_rect_t you_rect = jr_rect_px(r->cx - 11, r->cy - 11, 22, 22, 10000);
    avoid[an++] = you_rect;

    /* the status line first, so its knockout can be avoided */
    char status[JR_TEXT_MAX];
    const int32_t status_opa = jr_status(r, status, sizeof(status));
    snprintf(r->status, sizeof(r->status), "%s", status);
    jr_status_geom(r, &g->status, status, status_opa);
    if (g->status.shown) {
        jr_rect_t slot = {g->status.plate.x1 * JR_Q, g->status.plate.y1 * JR_Q,
                          (g->status.plate.x2 - g->status.plate.x1 + 1) * JR_Q,
                          (g->status.plate.y2 - g->status.plate.y1 + 1) * JR_Q, 100};
        avoid[an++] = slot;
    }

    /* the focus block */
    jr_place_t blk = {0};
    if (fr != NULL) {
        avoid[an++] = jr_rect_px(fr->x - 10, fr->y - 10, 20, 20, 100); /* its own mark */
        for (int32_t i = 0; i < on; i++) avoid[an++] = others[i];
        const int32_t tw[2] = {jr_text_w(JR_CAPTION, fr->line1), jr_text_w(JR_SMALL, fr->line2)};
        blk = jr_place(r, fr->id, fr->x * JR_Q, fr->y * JR_Q, tw, avoid, an);
    } else {
        r->side_id = 0;
    }

    /* numerals: a numeral the block would cover is left out whole */
    if (!pos_stale)
        for (int i = 0; i < 3; i++) {
            if (blk.ok && jr_q_covers(&blk.box, &numeral_rect[i], 4)) {
                jr_area_empty(&g->numeral[i].bounds);
                continue;
            }
            const int32_t y = r->cy - (i + 1) * 56;
            jr_plate_text(r, &g->numeral[i], labels[i], y + 7, 6, y - 12, 24);
        }
    if (!(blk.ok && jr_q_covers(&blk.box, &n_rect, 4)))
        jr_plate_text(r, &g->north, "N", r->cy - JR_NORTH_R + 7, 4, r->cy - JR_NORTH_R - 9, 20);

    /* the marks in list order (nearest first, as the page draws them), then the focus. When two planes
       swap places in the list their order fields change, so the change check redraws both marks whole. */
    for (int32_t i = 0; i < rn; i++)
        if (recs[i] != fr) jr_mark(r, &g->mark[g->mark_n++], recs[i], false, dim);
    if (fr != NULL) jr_mark(r, &g->mark[g->mark_n++], fr, true, dim);
    for (int32_t i = 0; i < g->mark_n; i++) g->mark[i].order = i;

    /* the block's text: drawPlaced */
    if (blk.ok) {
        jr_block_geom_t* b = &g->block;
        b->shown = 1;
        b->line_n = blk.line_n;
        b->plate = jr_q_area(&blk.box);
        jr_area_join(&b->bounds, &b->plate);
        const int32_t align = blk.ux > 5734 ? 1 : blk.ux < -5734 ? -1 : 0; /* 0.35 in Q14 */
        static const int32_t sizes[2] = {JR_CAPTION, JR_SMALL};
        static const int32_t opas[2] = {JR_TEXT_FULL, JR_TEXT_SOFT};
        int32_t yq = blk.box.y + JR_BLOCK_PAD * JR_Q;
        for (int32_t i = 0; i < blk.line_n; i++) {
            const char* s = i == 0 ? fr->line1 : fr->line2;
            jr_area_empty(&b->line[i].bounds);
            yq += sizes[i] * JR_Q * 94 / 100;
            const int32_t w = jr_text_w(sizes[i], s);
            int32_t xq;
            if (align > 0)
                xq = blk.box.x + JR_BLOCK_PAD * JR_Q;
            else if (align < 0)
                xq = blk.box.x + blk.box.w - JR_BLOCK_PAD * JR_Q - w * JR_Q;
            else
                xq = blk.box.x + blk.box.w / 2 - w * JR_Q / 2;
            jr_text_at(&b->line[i], s, sizes[i], opas[i], jr_rdiv(xq, JR_Q), jr_rdiv(yq, JR_Q));
            jr_area_join(&b->bounds, &b->line[i].bounds);
            yq += sizes[i] * JR_Q * 26 / 100;
        }
        /* the last resort: You is never knocked out */
        b->you_again = jr_q_covers(&blk.box, &you_rect, 0) ? 1 : 0;
        if (b->you_again) {
            lv_area_t you = {r->cx - JR_YOU_R - 2, r->cy - JR_YOU_R - 2, r->cx + JR_YOU_R + 2, r->cy + JR_YOU_R + 2};
            jr_area_join(&b->bounds, &you);
        }
        jr_area_grow(&b->bounds, 1);
    }
}

/* ---------------------------------------------------------- invalidation */

static void jr_box(jr_boxes_t* b, const lv_area_t* a)
{
    if (jr_area_is_empty(a)) return;
    if (b->n >= JR_BOX_CAP) {
        b->full = true;
        return;
    }
    b->a[b->n++] = *a;
}

/* The thin band an edge sweeps from bearing a0 to a1 (tenths, clockwise), r0 to r1 px, as a chain of boxes. */
static void jr_chain(jr_t* r, int32_t a0, int32_t a1, int32_t r0, int32_t r1, int32_t margin)
{
    const int32_t mid = a0 + jr_norm(a1 - a0) / 2;
    for (int32_t j = 0; j < JR_CHAIN; j++) {
        const int32_t ra = (r0 + (r1 - r0) * j / JR_CHAIN) * 16, rb = (r0 + (r1 - r0) * (j + 1) / JR_CHAIN) * 16;
        lv_area_t box;
        jr_area_empty(&box);
        const lv_point_t pts[5] = {jr_polar(r->cx, r->cy, a0, ra), jr_polar(r->cx, r->cy, a0, rb),
                                   jr_polar(r->cx, r->cy, a1, ra), jr_polar(r->cx, r->cy, a1, rb),
                                   jr_polar(r->cx, r->cy, mid, rb)};
        for (int i = 0; i < 5; i++) jr_area_point(&box, pts[i], margin);
        jr_box(&r->boxes, &box);
    }
}

/* The band the ring's light sweeps from bearing a0 to a1 (tenths, clockwise), r0 to r1 px, as a chain of
   boxes along it: each link spans a few degrees, so its box hugs the curve. */
static void jr_band(jr_t* r, int32_t a0, int32_t a1, int32_t r0, int32_t r1, int32_t margin)
{
    const int32_t span = jr_norm(a1 - a0);
    for (int32_t j = 0; j < JR_BAND_LINKS; j++) {
        const int32_t b0 = a0 + span * j / JR_BAND_LINKS, b1 = a0 + span * (j + 1) / JR_BAND_LINKS;
        lv_area_t box;
        jr_area_empty(&box);
        const lv_point_t pts[5] = {jr_polar(r->cx, r->cy, b0, r0 * 16), jr_polar(r->cx, r->cy, b0, r1 * 16),
                                   jr_polar(r->cx, r->cy, b1, r0 * 16), jr_polar(r->cx, r->cy, b1, r1 * 16),
                                   jr_polar(r->cx, r->cy, (b0 + b1) / 2, r1 * 16)};
        for (int i = 0; i < 5; i++) jr_area_point(&box, pts[i], margin);
        jr_box(&r->boxes, &box);
    }
}

static void jr_disc_box(jr_t* r)
{
    lv_area_t a = {r->cx - JR_CONTENT_R - 2, r->cy - JR_CONTENT_R - 2, r->cx + JR_CONTENT_R + 2,
                   r->cy + JR_CONTENT_R + 2};
    jr_box(&r->boxes, &a);
}

static void jr_diff_text(jr_t* r, const jr_text_geom_t* o, const jr_text_geom_t* n)
{
    if (memcmp(o, n, sizeof(*o)) == 0) return;
    jr_box(&r->boxes, &o->bounds);
    jr_box(&r->boxes, &n->bounds);
}

static void jr_invalidate_changes(jr_t* r, const jr_geom_t* o, const jr_geom_t* n)
{
    if (o->style != n->style || o->grid_opa != n->grid_opa || o->ring_opa != n->ring_opa ||
        o->guide_opa != n->guide_opa || o->you != n->you || o->wedge != n->wedge) {
        jr_disc_box(r);
        return;
    }
    if (n->wedge && o->head != n->head && n->style == JAKO_RADAR_STYLE_RING) {
        /* every segment of the light changes when the head steps: one band from the old tail to the new head,
           a degree wider each side for the whole-degree edges. After a hold the light catches up in one
           step: then the old light and the new one, each its own band. */
        if (jr_norm(n->head - o->head) <= 150) {
            jr_band(r, o->edge[JR_LIGHT_SEGS] * 10 - 10, n->edge[0] * 10 + 10, JR_BAND_IN, JR_BAND_OUT, 2);
        } else {
            jr_band(r, o->edge[JR_LIGHT_SEGS] * 10 - 10, o->edge[0] * 10 + 10, JR_BAND_IN, JR_BAND_OUT, 2);
            jr_band(r, n->edge[JR_LIGHT_SEGS] * 10 - 10, n->edge[0] * 10 + 10, JR_BAND_IN, JR_BAND_OUT, 2);
        }
    } else if (n->wedge && o->head != n->head) {
        if (jr_norm(n->head - o->head) > 150) {
            jr_disc_box(r); /* a jump after a hold or a stall: rare */
            return;
        }
        /* the line moves clockwise; each sector edge sweeps a thin band from its old whole degree to its new
           one, and the leading edge also covers the line at its exact steps (within half a degree of it) */
        for (int k = 0; k <= JR_SECTORS; k++) {
            const int32_t a = k == 0 ? o->head - 5 : o->edge[k] * 10, b = k == 0 ? n->head + 5 : n->edge[k] * 10;
            jr_chain(r, a - 5, b + 5, JR_R_IN - 3, JR_WEDGE_R + 3, 3);
        }
    }
    for (int i = 0; i < 3; i++) jr_diff_text(r, &o->numeral[i], &n->numeral[i]);
    jr_diff_text(r, &o->north, &n->north);
    jr_diff_text(r, &o->status, &n->status);
    if (memcmp(&o->block, &n->block, sizeof(o->block)) != 0) {
        jr_box(&r->boxes, &o->block.bounds);
        jr_box(&r->boxes, &n->block.bounds);
    }
    /* marks by id: a change, a plane gone, a plane new */
    for (int32_t i = 0; i < o->mark_n; i++) {
        const jr_mark_geom_t* om = &o->mark[i];
        const jr_mark_geom_t* nm = NULL;
        for (int32_t k = 0; k < n->mark_n && nm == NULL; k++)
            if (n->mark[k].id == om->id) nm = &n->mark[k];
        if (nm == NULL) {
            jr_box(&r->boxes, &om->bounds);
        } else if (memcmp(om, nm, sizeof(*om)) != 0) {
            jr_box(&r->boxes, &om->bounds);
            jr_box(&r->boxes, &nm->bounds);
        }
    }
    for (int32_t k = 0; k < n->mark_n; k++) {
        bool seen = false;
        for (int32_t i = 0; i < o->mark_n && !seen; i++) seen = o->mark[i].id == n->mark[k].id;
        if (!seen) jr_box(&r->boxes, &n->mark[k].bounds);
    }
    /* a mark whose place in the drawing order changed differs in its order field, so it is redrawn above:
       marks that swap places are always redrawn together, in their new order */
}

static bool jr_overlap(const lv_area_t* a, const lv_area_t* b)
{
    return a->x1 <= b->x2 + 1 && b->x1 <= a->x2 + 1 && a->y1 <= b->y2 + 1 && b->y1 <= a->y2 + 1;
}

/* Merge the way LVGL joins its areas, then join the cheapest pairs until at most JAKO_RADAR_INV_MAX are left. */
static void jr_flush_boxes(jr_t* r)
{
    jr_boxes_t* b = &r->boxes;
    r->inval_areas = 0;
    r->inval_px = 0;
    lv_area_t screen;
    lv_obj_get_coords(r->obj, &screen);
    if (b->full) {
        b->n = 0;
        b->full = false;
        lv_area_t a = {r->cx - JR_CONTENT_R - 2, r->cy - JR_CONTENT_R - 2, r->cx + JR_CONTENT_R + 2,
                       r->cy + JR_CONTENT_R + 2};
        jr_box(b, &a);
    }
    int32_t n = 0;
    for (int32_t i = 0; i < b->n; i++) {
        lv_area_t a = b->a[i];
        a.x1 = jr_max(a.x1, screen.x1);
        a.y1 = jr_max(a.y1, screen.y1);
        a.x2 = jr_min(a.x2, screen.x2);
        a.y2 = jr_min(a.y2, screen.y2);
        if (!jr_area_is_empty(&a)) b->a[n++] = a;
    }
    bool changed = true;
    while (changed) {
        changed = false;
        for (int32_t i = 0; i < n && !changed; i++)
            for (int32_t k = i + 1; k < n && !changed; k++) {
                if (!jr_overlap(&b->a[i], &b->a[k])) continue;
                lv_area_t j = b->a[i];
                jr_area_join(&j, &b->a[k]);
                if (jr_area_px(&j) < jr_area_px(&b->a[i]) + jr_area_px(&b->a[k])) {
                    b->a[i] = j;
                    b->a[k] = b->a[--n];
                    changed = true;
                }
            }
    }
    while (n > JAKO_RADAR_INV_MAX) {
        int32_t bi = 0, bk = 1, best = INT32_MAX;
        for (int32_t i = 0; i < n; i++)
            for (int32_t k = i + 1; k < n; k++) {
                lv_area_t j = b->a[i];
                jr_area_join(&j, &b->a[k]);
                const int32_t grow = jr_area_px(&j) - jr_area_px(&b->a[i]) - jr_area_px(&b->a[k]);
                if (grow < best) {
                    best = grow;
                    bi = i;
                    bk = k;
                }
            }
        jr_area_join(&b->a[bi], &b->a[bk]);
        b->a[bk] = b->a[--n];
    }
    for (int32_t i = 0; i < n; i++) {
        lv_obj_invalidate_area(r->obj, &b->a[i]);
        r->inval_px += (uint32_t)jr_area_px(&b->a[i]);
    }
    r->inval_areas = (uint32_t)n;
    b->n = 0;
}

static void jr_refresh(jr_t* r, int32_t head)
{
    jr_compute(r, &r->next, head);
    if (r->geom_state != JR_GEOM_READY) {
        r->geom = r->next;
        r->geom_state = JR_GEOM_READY;
        r->boxes.n = 0;
        jr_disc_box(r);
        jr_flush_boxes(r);
        return;
    }
    jr_invalidate_changes(r, &r->geom, &r->next);
    r->geom = r->next;
    jr_flush_boxes(r);
}

/* ------------------------------------------------------------------ draw */

static bool jr_visible(const lv_layer_t* layer, const lv_area_t* a) { return jr_area_hits(&layer->_clip_area, a); }

/* Does the clip area reach the ring between r_in and r_out round (cx, cy)? */
static bool jr_annulus_visible(const lv_layer_t* layer, int32_t cx, int32_t cy, int32_t r_in, int32_t r_out)
{
    const lv_area_t* c = &layer->_clip_area;
    const int32_t nx = cx < c->x1 ? c->x1 : cx > c->x2 ? c->x2 : cx;
    const int32_t ny = cy < c->y1 ? c->y1 : cy > c->y2 ? c->y2 : cy;
    const int64_t near = (int64_t)(nx - cx) * (nx - cx) + (int64_t)(ny - cy) * (ny - cy);
    const int32_t fx = jr_max(jr_abs(c->x1 - cx), jr_abs(c->x2 - cx)), fy = jr_max(jr_abs(c->y1 - cy), jr_abs(c->y2 - cy));
    const int64_t far = (int64_t)fx * fx + (int64_t)fy * fy;
    return near <= (int64_t)(r_out + 2) * (r_out + 2) && far >= (int64_t)jr_max(0, r_in - 2) * jr_max(0, r_in - 2);
}

static void jr_fill(lv_layer_t* layer, lv_obj_t* obj, const lv_area_t* a, int32_t radius, lv_color_t color,
                    int32_t opa)
{
    if (opa <= LV_OPA_MIN || !jr_visible(layer, a)) return;
    lv_draw_fill_dsc_t d;
    lv_draw_fill_dsc_init(&d);
    d.base.layer = layer;
    d.base.obj = obj;
    d.color = color;
    d.opa = (lv_opa_t)(opa > 255 ? 255 : opa);
    d.radius = radius;
    lv_draw_fill(layer, &d, a);
}

static void jr_dot(lv_layer_t* layer, lv_obj_t* obj, lv_point_t c, int32_t d, int32_t opa)
{
    const lv_area_t a = jr_disc_area(c, d);
    jr_fill(layer, obj, &a, LV_RADIUS_CIRCLE, lv_color_hex(JAKO_RADAR_LIGHT), opa);
}

/* A round-capped stroke. LVGL draws each cap as its own disc over the line, so a stroke under full
   strength would show brighter dots where they overlap; it is drawn instead in Light already mixed
   with the black screen, at full opacity, which on black is the same colour the mock blends. */
static void jr_line(lv_layer_t* layer, lv_obj_t* obj, lv_point_t a, lv_point_t b, int32_t width, int32_t opa)
{
    if (opa <= LV_OPA_MIN) return;
    lv_area_t box;
    jr_area_empty(&box);
    jr_area_point(&box, a, width / 2 + 2);
    jr_area_point(&box, b, width / 2 + 2);
    if (!jr_visible(layer, &box)) return;
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.base.layer = layer;
    d.base.obj = obj;
    d.color = lv_color_mix(lv_color_hex(JAKO_RADAR_LIGHT), lv_color_black(), (uint8_t)(opa > 255 ? 255 : opa));
    d.opa = LV_OPA_COVER;
    d.width = width;
    d.p1.x = a.x;
    d.p1.y = a.y;
    d.p2.x = b.x;
    d.p2.y = b.y;
    d.round_start = 1;
    d.round_end = 1;
    lv_draw_line(layer, &d);
}

/* An arc by LVGL angles (0 at three o'clock, clockwise), outer radius and width. */
static void jr_arc(lv_layer_t* layer, lv_obj_t* obj, int32_t cx, int32_t cy, int32_t radius, int32_t width,
                   int32_t a0, int32_t a1, bool rounded, int32_t opa)
{
    if (opa <= LV_OPA_MIN || !jr_annulus_visible(layer, cx, cy, radius - width, radius)) return;
    lv_draw_arc_dsc_t d;
    lv_draw_arc_dsc_init(&d);
    d.base.layer = layer;
    d.base.obj = obj;
    d.color = lv_color_hex(JAKO_RADAR_LIGHT);
    d.width = width;
    d.center.x = cx;
    d.center.y = cy;
    d.radius = (uint16_t)radius;
    d.start_angle = a0;
    d.end_angle = a1;
    d.opa = (lv_opa_t)(opa > 255 ? 255 : opa);
    d.rounded = rounded ? 1 : 0;
    lv_draw_arc(layer, &d);
}

static int32_t jr_lv_angle(int32_t bearing_deg) { return ((bearing_deg + 270) % 360 + 360) % 360; }

/* A knockout: black, rounded, but never past JR_KNOCKOUT_R, so the state ring's glow under the radar stays whole. */
static void jr_knockout(lv_layer_t* layer, lv_obj_t* obj, const jr_t* r, const lv_area_t* a)
{
    if (jr_area_is_empty(a) || !jr_visible(layer, a)) return;
    const int64_t lim = (int64_t)JR_KNOCKOUT_R * JR_KNOCKOUT_R;
    bool inside = true;
    const int32_t xs[2] = {a->x1, a->x2 + 1}, ys[2] = {a->y1, a->y2 + 1};
    for (int i = 0; i < 2; i++)
        for (int k = 0; k < 2; k++) {
            const int64_t dx = xs[i] - r->cx, dy = ys[k] - r->cy;
            inside = inside && dx * dx + dy * dy <= lim;
        }
    if (inside) {
        const int32_t h = a->y2 - a->y1 + 1;
        jr_fill(layer, obj, a, jr_min(h / 2, 12), lv_color_black(), LV_OPA_COVER);
        return;
    }
    for (int32_t y = a->y1; y <= a->y2; y += 4) {
        const int32_t y2 = jr_min(y + 3, a->y2);
        const int32_t dy = jr_max(jr_abs(y - r->cy), jr_abs(y2 + 1 - r->cy));
        if (dy >= JR_KNOCKOUT_R) continue;
        const int32_t half = (int32_t)jr_isqrt((uint64_t)(lim - (int64_t)dy * dy));
        lv_area_t band = {jr_max(a->x1, r->cx - half), y, jr_min(a->x2, r->cx + half - 1), y2};
        if (!jr_area_is_empty(&band)) jr_fill(layer, obj, &band, 0, lv_color_black(), LV_OPA_COVER);
    }
}

static void jr_text(lv_layer_t* layer, lv_obj_t* obj, const jr_text_geom_t* t)
{
    if (!t->shown || t->text[0] == '\0' || !jr_visible(layer, &t->bounds)) return;
    const lv_font_t* f = jr_font(t->font);
    lv_draw_label_dsc_t d;
    lv_draw_label_dsc_init(&d);
    d.base.layer = layer;
    d.base.obj = obj;
    d.text = t->text;
    d.font = f;
    d.color = lv_color_hex(JAKO_RADAR_LIGHT);
    d.opa = (lv_opa_t)t->opa;
    d.align = LV_TEXT_ALIGN_LEFT;
    d.flag = LV_TEXT_FLAG_EXPAND;
    lv_area_t a = {t->pos.x, t->pos.y, t->pos.x + t->w + 2, t->pos.y + f->line_height - 1};
    lv_draw_label(layer, &d, &a);
}

static void jr_you(lv_layer_t* layer, lv_obj_t* obj, const jr_t* r, int32_t style)
{
    if (style == JR_YOU_DASHES) {
        /* the position is old: six dashes at .62 (0.18 rad, about 10 degrees, trimmed from each end) */
        for (int i = 0; i < 6; i++)
            jr_arc(layer, obj, r->cx, r->cy, JR_YOU_R, JR_YOU_W, i * 60 + 10, i * 60 + 50, true, JR_TEXT_SOFT);
        return;
    }
    jr_arc(layer, obj, r->cx, r->cy, JR_YOU_R, JR_YOU_W, 0, 360, false, LV_OPA_COVER);
}

static void jr_draw_mark(lv_layer_t* layer, lv_obj_t* obj, const jr_mark_geom_t* m)
{
    if (m->kind == JR_MK_NONE || !jr_visible(layer, &m->bounds)) return;
    for (int32_t i = 0; i < m->trail_n; i++) jr_dot(layer, obj, m->trail[i], JR_TRAIL_DOT[i], m->trail_opa[i]);
    for (int32_t g = 0; g < m->glow_n; g++)
        for (int i = 0; i < 2; i++) jr_dot(layer, obj, m->c, (m->glow_r[g] + JR_GLOW_GROW[i]) * 2, m->glow_opa[g][i]);
    switch (m->kind) {
    case JR_MK_PLANE:
        jr_line(layer, obj, m->wl, m->nose, m->stroke, m->opa);
        jr_line(layer, obj, m->nose, m->wr, m->stroke, m->opa);
        break;
    case JR_MK_HOVER:
    case JR_MK_RIM: jr_dot(layer, obj, m->c, m->size, m->opa); break;
    default: break;
    }
}

static void jr_draw_cb(lv_event_t* e)
{
    jr_t* r = (jr_t*)lv_event_get_user_data(e);
    lv_layer_t* layer = lv_event_get_layer(e);
    if (r == NULL || layer == NULL || r->geom_state != JR_GEOM_READY) return;
    lv_obj_t* obj = r->obj;
    const jr_geom_t* g = &r->geom;
    const lv_color_t light = lv_color_hex(JAKO_RADAR_LIGHT);

    /* the guides: the line style's grid, the rings, then the line style's crosshair and ticks or the ring
       style's three cardinal ticks */
    const bool line = g->style == JAKO_RADAR_STYLE_LINE;
    if (line)
        for (int i = 0; i < JR_GRID_LINES * 4; i++) jr_fill(layer, obj, &r->grid[i], 0, light, g->grid_opa);
    for (int i = 1; i <= 3; i++)
        jr_arc(layer, obj, r->cx, r->cy, i * 56 + JR_RING_W / 2, JR_RING_W, 0, 360, false, g->ring_opa);
    if (line) {
        for (int i = 0; i < 4; i++) jr_line(layer, obj, r->cross_a[i], r->cross_b[i], JR_CROSS_W, g->guide_opa);
        for (int i = 0; i < 35; i++) jr_line(layer, obj, r->tick_a[i], r->tick_b[i], r->tick_w[i], g->guide_opa);
    } else {
        for (int i = 0; i < 3; i++) jr_line(layer, obj, r->rtick_a[i], r->rtick_b[i], JR_RTICK_W, g->guide_opa);
    }

    /* the ring's light, tail to head: stepped segments on the 15 mile ring, each centred on it */
    if (g->wedge && !line) {
        for (int k = JR_LIGHT_SEGS - 1; k >= 0; k--) {
            const int32_t w = JR_LIGHT_W[k], outer = JR_OUTER + (w + 1) / 2;
            lv_area_t box;
            jr_area_empty(&box);
            const int32_t t[3] = {g->edge[k + 1] * 10, (g->edge[k + 1] + g->edge[k]) * 5, g->edge[k] * 10};
            for (int i = 0; i < 3; i++) {
                jr_area_point(&box, jr_polar(r->cx, r->cy, t[i], (outer - w) * 16), 3);
                jr_area_point(&box, jr_polar(r->cx, r->cy, t[i], outer * 16), 3);
            }
            if (!jr_visible(layer, &box)) continue;
            jr_arc(layer, obj, r->cx, r->cy, outer, w, jr_lv_angle(g->edge[k + 1]), jr_lv_angle(g->edge[k]), false,
                   jr_opa(JR_LIGHT_MILLI[k]));
        }
    }

    /* the wedge, dim to bright, then the line: stepped sectors, never a gradient */
    if (g->wedge && line) {
        for (int i = JR_SECTORS - 1; i >= 0; i--) {
            /* LVGL masks every row of the whole circle's box for an arc, so a sector the area being drawn
               cannot reach is skipped: its box is its corners and middle, inner and outer, plus 3 px */
            lv_area_t box;
            jr_area_empty(&box);
            const int32_t t[3] = {g->edge[i + 1] * 10, (g->edge[i + 1] + g->edge[i]) * 5, g->edge[i] * 10};
            for (int k = 0; k < 3; k++) {
                jr_area_point(&box, jr_polar(r->cx, r->cy, t[k], JR_R_IN * 16), 3);
                jr_area_point(&box, jr_polar(r->cx, r->cy, t[k], JR_WEDGE_R * 16), 3);
            }
            if (!jr_visible(layer, &box)) continue;
            jr_arc(layer, obj, r->cx, r->cy, JR_WEDGE_R, JR_WEDGE_R - JR_R_IN, jr_lv_angle(g->edge[i + 1]),
                   jr_lv_angle(g->edge[i]), false, jr_opa(JR_WEDGE_MILLI[i]));
        }
        jr_line(layer, obj, g->line_a, g->line_b, JR_LINE_W, LV_OPA_COVER);
    }

    /* the numerals on their plates, over the wedge */
    for (int i = 0; i < 3; i++) {
        if (!g->numeral[i].shown) continue;
        jr_knockout(layer, obj, r, &g->numeral[i].plate);
        jr_text(layer, obj, &g->numeral[i]);
    }
    jr_you(layer, obj, r, g->you); /* under the planes */

    /* planes, then N, then the focus and its block */
    int32_t focus = -1;
    for (int32_t i = 0; i < g->mark_n; i++) {
        if (g->mark[i].focus) {
            focus = i;
            continue;
        }
        jr_draw_mark(layer, obj, &g->mark[i]);
    }
    if (g->north.shown) {
        jr_knockout(layer, obj, r, &g->north.plate);
        jr_text(layer, obj, &g->north);
    }
    if (focus >= 0) jr_draw_mark(layer, obj, &g->mark[focus]);
    if (g->block.shown) {
        jr_knockout(layer, obj, r, &g->block.plate);
        for (int32_t i = 0; i < g->block.line_n; i++) jr_text(layer, obj, &g->block.line[i]);
        if (g->block.you_again) jr_you(layer, obj, r, g->you);
    }

    /* the status line in the caption slot, over everything */
    if (g->status.shown) {
        jr_knockout(layer, obj, r, &g->status.plate);
        jr_text(layer, obj, &g->status);
    }
}

/* ---------------------------------------------------------------- events */

static void jr_setup_guides(jr_t* r)
{
    lv_area_t c;
    lv_obj_get_coords(r->obj, &c);
    r->cx = c.x1 + lv_area_get_width(&c) / 2;
    r->cy = c.y1 + lv_area_get_height(&c) / 2;
    int n = 0;
    for (int b = 10; b < 360; b += 10, n++) {
        const bool cardinal = b % 90 == 0;
        /* ticks every 10 degrees in the clear band, none at north: 190 to 196 px, and 188.5 to 196.5 at east,
           south and west (the page's 188 to 197, a px shorter to stay inside 197 with whole-px ends) */
        r->tick_a[n] = jr_polar(r->cx, r->cy, b * 10, (cardinal ? 190 : 191) * 16);
        r->tick_b[n] = jr_polar(r->cx, r->cy, b * 10, 195 * 16);
        r->tick_w[n] = cardinal ? 3 : 2;
    }
    /* the ring style: east, south and west, 172 to 180 px, round-capped (N stands at twelve) */
    for (int i = 0; i < 3; i++) {
        r->rtick_a[i] = jr_polar(r->cx, r->cy, (i + 1) * 900, JR_RTICK_IN * 16);
        r->rtick_b[i] = jr_polar(r->cx, r->cy, (i + 1) * 900, JR_RTICK_OUT * 16);
    }
    for (int i = 0; i < 4; i++) {
        r->cross_a[i] = jr_polar(r->cx, r->cy, i * 900, JR_R_IN * 16);
        r->cross_b[i] = jr_polar(r->cx, r->cy, i * 900, JR_OUTER * 16);
    }
    /* the grid: lines at 28, 56, 84, 112 and 140 px each way, ends on the outer ring */
    n = 0;
    for (int i = 1; i <= JR_GRID_LINES; i++)
        for (int s = -1; s <= 1; s += 2) {
            const int32_t d = s * i * JR_GRID_PITCH;
            const int32_t h = (int32_t)jr_isqrt((uint64_t)(JR_OUTER * JR_OUTER - d * d));
            lv_area_t v = {r->cx + d, r->cy - h, r->cx + d, r->cy + h - 1};
            lv_area_t hz = {r->cx - h, r->cy + d, r->cx + h - 1, r->cy + d};
            r->grid[n++] = v;
            r->grid[n++] = hz;
        }
}

static void jr_size_cb(lv_event_t* e)
{
    jr_t* r = (jr_t*)lv_event_get_user_data(e);
    if (r == NULL) return;
    jr_setup_guides(r);
    r->geom_state = JR_GEOM_NONE;
    lv_obj_invalidate(r->obj);
}

static void jr_delete_cb(lv_event_t* e)
{
    jr_t* r = (jr_t*)lv_event_get_user_data(e);
    if (r == NULL) return;
    if (r->timer != NULL) lv_timer_delete(r->timer);
    lv_obj_set_user_data(r->obj, NULL);
    r->magic = 0;
    lv_free(r);
}

static void jr_timer_cb(lv_timer_t* t)
{
    jr_t* r = (jr_t*)lv_timer_get_user_data(t);
    const uint32_t elapsed = lv_tick_elaps(r->last_tick);
    r->last_tick = lv_tick_get();
    jako_radar_advance(r->obj, elapsed);
}

/* ------------------------------------------------------------- public API */

lv_obj_t* jako_radar_create(lv_obj_t* parent)
{
    jr_t* r = (jr_t*)lv_malloc_zeroed(sizeof(jr_t));
    if (r == NULL) return NULL;
    lv_obj_t* obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj); /* no style: transparent, so the face's ring shows round it */
    lv_obj_set_size(obj, lv_pct(100), lv_pct(100));
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_CLICK_FOCUSABLE);
    r->magic = JR_MAGIC;
    r->obj = obj;
    r->face = JAKO_FACE_STATE_IDLE;
    r->angle_ms = -1;
    r->mode = JAKO_RADAR_LINE_STOPPED;
    r->u.state = JAKO_RADAR_STATE_LOADING;
    r->u.src = JAKO_RADAR_SRC_FEED;
    r->u.style = JAKO_RADAR_STYLE_RING;
    r->style = JAKO_RADAR_STYLE_RING;
    lv_obj_update_layout(obj);
    jr_setup_guides(r);
    lv_obj_set_user_data(obj, r);
    lv_obj_add_event_cb(obj, jr_draw_cb, LV_EVENT_DRAW_MAIN, r);
    lv_obj_add_event_cb(obj, jr_size_cb, LV_EVENT_SIZE_CHANGED, r);
    lv_obj_add_event_cb(obj, jr_delete_cb, LV_EVENT_DELETE, r);
    r->last_tick = lv_tick_get();
    r->timer = lv_timer_create(jr_timer_cb, JAKO_RADAR_TICK_MS, r);
    return obj;
}

uint16_t jako_radar_apply(lv_obj_t* obj, const jako_radar_update_t* u)
{
    jr_t* r = jr_get(obj);
    if (r == NULL || u == NULL) return 0;
    const uint16_t old_f = r->u.focus;
    r->u = *u;
    if (r->u.plane_n > JAKO_RADAR_MAX_PLANES) r->u.plane_n = JAKO_RADAR_MAX_PLANES;
    if (r->u.state != JAKO_RADAR_STATE_LIVE) r->u.plane_n = 0;
    if (r->u.style != JAKO_RADAR_STYLE_LINE) r->u.style = JAKO_RADAR_STYLE_RING;
    if (r->u.focus != old_f) r->pick = 0; /* any change of f clears the local pick */
    if (r->u.style != r->style) {
        /* the other style: every mark quiet at .62, no pass remembered, and the whole radar redrawn once */
        r->style = r->u.style;
        jr_repaint_all(r);
        r->ping_n = 0;
        jr_disc_box(r);
        JR_LOGI("style %s", r->style == JAKO_RADAR_STYLE_LINE ? "line" : "ring");
    }
    r->received_ms = r->clock;
    r->updates++;
    r->expired = false;
    r->focus_id = jr_focus(r);
    return r->focus_id;
}

uint16_t jako_radar_next(lv_obj_t* obj)
{
    jr_t* r = jr_get(obj);
    if (r == NULL || r->u.plane_n == 0) return 0;
    const int32_t i = jr_find_plane(r, jr_focus(r));
    r->pick = r->u.planes[(i + 1) % r->u.plane_n].id;
    r->pick_ms = r->clock;
    r->focus_id = jr_focus(r);
    return r->focus_id;
}

uint16_t jako_radar_focus(lv_obj_t* obj)
{
    jr_t* r = jr_get(obj);
    if (r == NULL) return 0;
    r->focus_id = jr_focus(r);
    return r->focus_id;
}

void jako_radar_set_face_state(lv_obj_t* obj, jako_face_state_t state)
{
    jr_t* r = jr_get(obj);
    if (r != NULL) r->face = state;
}

void jako_radar_set_reduced_motion(lv_obj_t* obj, bool on)
{
    jr_t* r = jr_get(obj);
    if (r != NULL) r->reduced = on;
}

void jako_radar_set_expire_cb(lv_obj_t* obj, jako_radar_expire_cb_t cb, void* user)
{
    jr_t* r = jr_get(obj);
    if (r == NULL) return;
    r->expire_cb = cb;
    r->expire_user = user;
}

void jako_radar_advance(lv_obj_t* obj, uint32_t ms)
{
    jr_t* r = jr_get(obj);
    if (r == NULL) return;
    const bool gap = ms > JR_GAP_MS;
    r->clock += ms;
    jr_ages(r);
    r->view = jr_view(r);
    r->focus_id = jr_focus(r);
    const int32_t head = jr_sweep(r, gap);
    jr_refresh(r, head);
    if (!r->expired && r->clock - r->received_ms >= JAKO_RADAR_EXPIRE_MS) {
        r->expired = true;
        JR_LOGI("no update for %u s", (unsigned)(JAKO_RADAR_EXPIRE_MS / 1000));
        if (r->expire_cb != NULL) r->expire_cb(obj, r->expire_user); /* may delete the radar: nothing after */
    }
}

void jako_radar_get_debug(lv_obj_t* obj, jako_radar_debug_t* out)
{
    jr_t* r = jr_get(obj);
    if (r == NULL || out == NULL) return;
    memset(out, 0, sizeof(*out));
    out->clock_ms = r->clock;
    out->since_update_ms = r->clock - r->received_ms;
    out->updates = r->updates;
    out->state = r->u.state;
    out->src = r->u.src;
    out->style = r->style;
    out->view = r->view;
    out->flights_age_s = r->flights_age_s;
    out->position_age_s = r->position_age_s;
    out->line = r->mode;
    out->head_tenths = r->geom.head;
    out->focus_id = jr_focus(r);
    out->server_focus = r->u.focus;
    out->pick_id = r->pick;
    out->plane_n = r->u.plane_n;
    const jr_geom_t* g = &r->geom;
    for (int32_t i = 0; i < g->mark_n && i < JAKO_RADAR_MAX_PLANES; i++) {
        const jr_mark_geom_t* m = &g->mark[i];
        jako_radar_mark_debug_t* d = &out->marks[out->mark_n++];
        d->id = (uint16_t)m->id;
        d->x = (int16_t)m->c.x;
        d->y = (int16_t)m->c.y;
        d->opa = (uint8_t)m->opa;
        d->in_range = m->kind != JR_MK_RIM;
        d->focus = m->focus != 0;
        d->painted_ms = JR_NONE_MS;
        if (r->style == JAKO_RADAR_STYLE_RING) d->painted_ms = jr_ping_at(r, (uint16_t)m->id);
        for (uint8_t k = 0; k < r->rec_n; k++)
            if (r->rec[k].id == m->id) d->painted_ms = r->rec[k].at;
        jr_record_t e;
        memset(&e, 0, sizeof(e));
        e.at = d->painted_ms;
        int32_t a, k;
        jr_fade(r, &e, &a, &k);
        d->glow_k = (uint16_t)k;
    }
    out->status = r->status;
    out->status_opa = (uint8_t)(g->status.shown ? g->status.opa : 0);
    out->block_lines = (uint8_t)(g->block.shown ? g->block.line_n : 0);
    if (g->block.shown)
        out->block = g->block.plate;
    else
        jr_area_empty(&out->block);
    /* the smallest and faintest text drawn */
    const jr_text_geom_t* texts[8] = {&g->numeral[0], &g->numeral[1], &g->numeral[2], &g->north, &g->status,
                                      &g->block.line[0], &g->block.line[1], NULL};
    for (int i = 0; i < 7; i++) {
        const jr_text_geom_t* t = texts[i];
        if (!t->shown || (i >= 5 && (!g->block.shown || i - 5 >= g->block.line_n))) continue;
        if (out->text_px_min == 0 || t->font < out->text_px_min) out->text_px_min = (uint8_t)t->font;
        if (out->text_opa_min == 0 || t->opa < out->text_opa_min) out->text_opa_min = (uint8_t)t->opa;
    }
    out->inval_areas = r->inval_areas;
    out->inval_px = r->inval_px;
    out->expired = r->expired;
}
