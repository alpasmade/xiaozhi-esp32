// Copyright 2026 Alpas Made. All rights reserved.
/* The dial of today on LVGL 9.5. See jako_dial.h for what it shows; the
   geometry is the plan's section 2.3 with the reviewer's amendments 3
   (digits only, no hour tick under a numeral), 13 (at most 10 areas a
   tick, the bead skipped on a turn tick), 19 (the bottom line at most
   114 px) and 23 (an update that changes only t, age or ask redraws only
   the clock text).

   All geometry is integer: angles in tenths of a degree from a quarter sine
   table in Q14 (the radar's), so the Mac preview and the board compute the
   same pixels. No float anywhere. Every Light shape is drawn opaque, as
   Light mixed with black at its alpha (as jako_radar.c does), so round caps
   never double where shapes overlap and a brighter layer covers a fainter
   one instead of adding to it: that is how the edge and fill of an event,
   and the stepped glow of the event on now and of the now mark, are made.

   What moves, and what it costs:
   - the seconds bead: one 6 degree step a second, its old and new boxes;
   - the clock text: once a minute;
   - the turn: 1 degree every 2 minutes as a cut, the annulus in 8 tiles;
   - the loading pulse: 8 steps in 2.4 s, the track's band in 8 tiles.
   The annulus is recomputed only when its key changes (the reference, the
   screen, the data, the dims, the pulse), so a tick that changes nothing
   costs a few comparisons. */
#include "jako_dial.h"

#include <stdio.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_log.h"
#define JD_LOGI(fmt, ...) ESP_LOGI("jako_dial", fmt, ##__VA_ARGS__)
#else
#define JD_LOGI(fmt, ...) ((void)0)
#endif

LV_FONT_DECLARE(font_inter_600_20_4)
LV_FONT_DECLARE(font_inter_600_22_4)

#define JD_MAGIC 0x4449414Cu

/* ---- the plan's section 2.3, in px at 466 from the centre ---- */
#define JD_PLATE_R 104      /* a 1 px circle at .16 */
#define JD_BEAD_R 104       /* the seconds bead rides the plate */
#define JD_BEAD_D 5         /* a 5 px disc */
#define JD_TRACK_R 132      /* outer radius; r 108 to 132 */
#define JD_TRACK_W 24
#define JD_EV_R 130         /* an event: r 110 to 130 */
#define JD_EV_W 20
#define JD_FILL_R 128       /* a future event's fill: r 112 to 128 */
#define JD_FILL_W 16
#define JD_GLOW_OUT_R 134   /* the event on now's glow, outer step */
#define JD_GLOW_OUT_W 28
#define JD_GLOW_IN_R 132    /* inner step */
#define JD_GLOW_IN_W 24
#define JD_NUM_R 157        /* numerals, centred on this radius */
#define JD_HOUR_IN 184      /* ticks: hour from 184, quarter from 189, both to 195 (the plan's 196 lit */
#define JD_QUARTER_IN 189   /* 198.0 px with the round cap's edge; 195 is the radar's own tick end) */
#define JD_TICK_OUT 195
#define JD_TICK_W 2
#define JD_SUN_R 179        /* the sunset disc, 4 px radius */
#define JD_SUN_D 9
#define JD_SEAM_IN 104      /* the seam: a 3 px black cut at ten o'clock */
#define JD_SEAM_OUT 136
#define JD_SEAM_W 3
#define JD_SEAM_DEG 300
#define JD_MARK_IN 100      /* the now mark at twelve: r 100 to 140 */
#define JD_MARK_OUT 140
#define JD_TIME_DY (-76)    /* the now time's line box centre, from the centre */
#define JD_BOTTOM_DY 74     /* the bottom line's */
#define JD_BOTTOM_MAX_W 114 /* amendment 19: its box corner then stays inside the plate */
/* the error status: the plan's "No calendar" is 116 px at 20 px, past the 114 px cap */
static const char JD_TEXT_ERROR[] = "No agenda";
#define JD_PAST_MIN 120     /* the window: the reference minus 120 min to plus 600 */
#define JD_FUTURE_MIN 600
#define JD_SECTOR_DEG 15    /* the past band's four sectors */
#define JD_NUM_SEAM_DEG 12  /* no numeral this close to the seam */
#define JD_MIN_ARC_DEG 3    /* a future event is at least this long */
#define JD_NIGHT_FROM 1380  /* 23:00 to 05:00: arcs and ticks at 7/10 */
#define JD_NIGHT_TO 300
#define JD_SKIP_R 99        /* the draw callback skips areas inside this radius that miss the text */
#define JD_TILE_IN 99       /* a turn redraws r 99 to 198 (bead to tick caps, with their edges) */
#define JD_TILE_OUT 198
#define JD_BAND_IN 104      /* a loading pulse redraws the track's band */
#define JD_BAND_OUT 136
#define JD_TILES 8
#define JD_ARC_MAX 264      /* the track, 8 arcs for each of 32 events (4 past, an edge and a fill, each split
                               at the seam) and 2 more for the glow of the event on now */
#define JD_TICK_MAX 48      /* 720 min of quarter hours */
#define JD_NUM_MAX 6        /* 720 min of even hours */
#define JD_SUN_MAX 2
#define JD_BOX_CAP 32
#define JD_TEXT_MAX 24
#define JD_SINCE_MAX 0x7FFFFFFFu
#define JD_PULSE_STEP_MS 300
#define JD_PULSE_REDUCED_MICRO 110000

/* alphas of 255 */
#define JD_OPA_FULL 255
#define JD_OPA_TEXT_SOFT 158 /* .62 */
#define JD_OPA_PLATE 41      /* .16 */
#define JD_OPA_TRACK 15      /* .06 */
#define JD_OPA_EDGE 224      /* .88 */
#define JD_OPA_FILL 56       /* .22 */
#define JD_OPA_GLOW_OUT 26   /* .10 */
#define JD_OPA_GLOW_IN 43    /* .17 */
#define JD_OPA_BEAD 191      /* .75 */
#define JD_OPA_SUN 158       /* .62 */
#define JD_OPA_NUM 158       /* .62 */
#define JD_OPA_HOUR 107      /* .42 */
#define JD_OPA_QUARTER 51    /* .20 */
#define JD_OPA_HOUR_PAST 51  /* .20 */
#define JD_OPA_QUARTER_PAST 23 /* .09 */
/* the past band, seam to now: other events .06 .11 .15 .20, the event on now .09 .24 .47 .75 */
static const int32_t JD_PAST_OPA[4] = {15, 28, 38, 51};
static const int32_t JD_PAST_NOW_OPA[4] = {23, 61, 120, 191};
/* the loading pulse, .06 to .16 in 8 steps every 2.4 s, in millionths (the radar's) */
static const int32_t JD_PULSE_MICRO[8] = {60000, 74645, 110000, 145355, 160000, 145355, 110000, 74645};

/* sin of whole degrees 0..90 in Q14 (the radar's table) */
static const int16_t JD_SIN_Q14[91] = {
    0,     286,   572,   857,   1143,  1428,  1713,  1997,  2280,  2563,  2845,  3126,  3406,  3686,
    3964,  4240,  4516,  4790,  5063,  5334,  5604,  5872,  6138,  6402,  6664,  6924,  7182,  7438,
    7692,  7943,  8192,  8438,  8682,  8923,  9162,  9397,  9630,  9860,  10087, 10311, 10531, 10749,
    10963, 11174, 11381, 11585, 11786, 11982, 12176, 12365, 12551, 12733, 12911, 13085, 13255, 13421,
    13583, 13741, 13894, 14044, 14189, 14330, 14466, 14598, 14726, 14849, 14968, 15082, 15191, 15296,
    15396, 15491, 15582, 15668, 15749, 15826, 15897, 15964, 16026, 16083, 16135, 16182, 16225, 16262,
    16294, 16322, 16344, 16362, 16374, 16382, 16384,
};

/* -------------------------------------------------------------- types */

typedef enum { JD_GEOM_NONE = 0, JD_GEOM_READY } jd_geom_state_t;
typedef enum { JD_CACHE_NONE = 0, JD_CACHE_HELD } jd_cache_t;
/* whether the last ready, empty or error update could read the store; ERROR draws "No agenda" */
typedef enum { JD_DATA_OK = 0, JD_DATA_ERROR } jd_data_t;

typedef struct {
    int32_t s, e; /* minutes from the midnight of the update's day */
    char name[JAKO_DIAL_NAME_MAX + 2];
} jd_event_t;

/* All int32 so whole structs compare with memcmp. Angles are bearings in whole degrees, a0 < a1. */
typedef struct {
    int32_t r, w, a0, a1, opa, rounded, rgb;
} jd_arc_t;

typedef struct {
    lv_point_t a, b;
    int32_t opa;
} jd_tick_t;

typedef struct {
    int32_t shown, font, opa, rgb;
    lv_point_t pos; /* top left of the line box */
    int32_t w;
    char text[JD_TEXT_MAX];
    lv_area_t bounds; /* what drawing it can touch */
} jd_text_t;

/* What the annulus depends on: when none of it changes, the annulus is not touched. */
typedef struct {
    int32_t screen, arcs, dim_half, night, track_opa, ref, data_ver;
} jd_key_t;

typedef struct {
    int32_t arc_n;
    jd_arc_t arc[JD_ARC_MAX];
    int32_t tick_n;
    jd_tick_t tick[JD_TICK_MAX];
    int32_t num_n;
    jd_text_t num[JD_NUM_MAX];
    int32_t sun_n;
    lv_point_t sun[JD_SUN_MAX];
} jd_ring_t;

typedef struct {
    jd_text_t time, bottom;
    int32_t bead_shown, bead_deg;
    lv_area_t bead;
} jd_marks_t;

typedef struct {
    lv_area_t a[JD_BOX_CAP];
    int32_t n;
    bool full;
} jd_boxes_t;

typedef struct {
    uint32_t magic;
    lv_obj_t* obj;
    lv_timer_t* timer;
    uint32_t last_tick;
    jako_dial_rest_cb_t rest_cb;
    void* rest_user;
    bool rest_armed; /* showable at rest since the last apply: the callback has not fired */
    bool reduced;
    bool hide_text;

    /* the clock */
    uint32_t clock;     /* ms since create */
    uint32_t since_ms;  /* since the last applied update, saturating */
    int32_t t;          /* the last update's t */
    jako_dial_day_t days[2];
    uint32_t updates;

    /* the cache: the last ready or empty update, its minutes counted from the midnight of
       cache_days[0], so a later loading or error update on another day never moves it */
    jd_cache_t cache;
    jako_dial_day_t cache_days[2];
    int32_t cache_span; /* days the cache covers from its day 0: 2, or 1 once shifted a day */
    jd_data_t data;     /* the last ready, empty or error update: OK, or ERROR */
    jako_dial_src_t src;
    int32_t age, stale;
    uint32_t cache_since_ms;
    uint8_t home;
    int32_t sun[2];     /* minutes from the update's midnight, -1 for none */
    int32_t ev_n;
    jd_event_t ev[JAKO_DIAL_MAX_EVENTS];
    int32_t data_ver;

    /* derived */
    int32_t cx, cy;
    jako_dial_screen_t screen;
    int32_t now_s;
    jd_geom_state_t geom_state;
    jd_key_t key;
    jd_ring_t ring;
    jd_marks_t marks, next;
    lv_area_t mark_box;
    jd_boxes_t boxes;
    uint32_t inval_areas, inval_px, turns;
    bool turned;
} jd_t;

/* ------------------------------------------------------------ helpers */

static int32_t jd_rdiv(int64_t n, int64_t d) { return (int32_t)(n >= 0 ? (n + d / 2) / d : -((-n + d / 2) / d)); }
static int32_t jd_min(int32_t a, int32_t b) { return a < b ? a : b; }
static int32_t jd_max(int32_t a, int32_t b) { return a > b ? a : b; }
static int32_t jd_abs(int32_t a) { return a < 0 ? -a : a; }
static int32_t jd_mod(int32_t a, int32_t m) { return ((a % m) + m) % m; }
static int32_t jd_floor_div(int32_t a, int32_t b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }
static int32_t jd_ceil_div(int32_t a, int32_t b) { return -jd_floor_div(-a, b); }

/* sin of tenths of a degree, Q14, linear between whole degrees */
static int32_t jd_sin(int32_t t)
{
    t = jd_mod(t, 3600);
    int32_t sign = 1;
    if (t >= 1800) {
        t -= 1800;
        sign = -1;
    }
    if (t > 900) t = 1800 - t;
    const int32_t d = t / 10, f = t % 10;
    const int32_t lo = JD_SIN_Q14[d], hi = d < 90 ? JD_SIN_Q14[d + 1] : lo;
    return sign * (lo + jd_rdiv((int64_t)(hi - lo) * f, 10));
}

static int32_t jd_cos(int32_t t) { return jd_sin(t + 900); }

/* A point at bearing t (tenths, clockwise from twelve) and radius r16 (1/16 px). */
static lv_point_t jd_polar(int32_t cx, int32_t cy, int32_t t, int32_t r16)
{
    lv_point_t p;
    p.x = cx + jd_rdiv((int64_t)r16 * jd_sin(t), 16 * 16384);
    p.y = cy - jd_rdiv((int64_t)r16 * jd_cos(t), 16 * 16384);
    return p;
}

static void jd_area_empty(lv_area_t* a)
{
    a->x1 = 0;
    a->y1 = 0;
    a->x2 = -1;
    a->y2 = -1;
}

static bool jd_area_is_empty(const lv_area_t* a) { return a->x2 < a->x1 || a->y2 < a->y1; }

static void jd_area_join(lv_area_t* acc, const lv_area_t* b)
{
    if (jd_area_is_empty(b)) return;
    if (jd_area_is_empty(acc)) {
        *acc = *b;
        return;
    }
    acc->x1 = jd_min(acc->x1, b->x1);
    acc->y1 = jd_min(acc->y1, b->y1);
    acc->x2 = jd_max(acc->x2, b->x2);
    acc->y2 = jd_max(acc->y2, b->y2);
}

static bool jd_area_hits(const lv_area_t* a, const lv_area_t* b)
{
    return !jd_area_is_empty(a) && !jd_area_is_empty(b) && a->x1 <= b->x2 && b->x1 <= a->x2 && a->y1 <= b->y2 &&
           b->y1 <= a->y2;
}

static int32_t jd_area_px(const lv_area_t* a)
{
    return jd_area_is_empty(a) ? 0 : (a->x2 - a->x1 + 1) * (a->y2 - a->y1 + 1);
}

static void jd_area_point(lv_area_t* acc, lv_point_t p, int32_t by)
{
    lv_area_t a = {p.x - by, p.y - by, p.x + by, p.y + by};
    jd_area_join(acc, &a);
}

/* A disc of odd diameter d centred on c. */
static lv_area_t jd_disc_area(lv_point_t c, int32_t d)
{
    lv_area_t a = {c.x - d / 2, c.y - d / 2, c.x - d / 2 + d - 1, c.y - d / 2 + d - 1};
    return a;
}

static int32_t jd_dim(int32_t opa, const jd_key_t* k, bool stale_too)
{
    if (k->night) opa = jd_rdiv((int64_t)opa * 7, 10);
    if (stale_too && k->dim_half) opa = jd_rdiv(opa, 2);
    return opa;
}

static jd_t* jd_get(lv_obj_t* obj)
{
    if (obj == NULL) return NULL;
    jd_t* r = (jd_t*)lv_obj_get_user_data(obj);
    return (r != NULL && r->magic == JD_MAGIC) ? r : NULL;
}

/* Copy src into dst of n bytes, cut to fit and always terminated. */
static void jd_copy(char* dst, size_t n, const char* src)
{
    if (n == 0) return;
    size_t i = 0;
    for (; i + 1 < n && src[i] != '\0'; i++) dst[i] = src[i];
    dst[i] = '\0';
}

static const lv_font_t* jd_font(int32_t px) { return px >= 22 ? &font_inter_600_22_4 : &font_inter_600_20_4; }

static int32_t jd_text_w(int32_t px, const char* s)
{
    const lv_font_t* f = jd_font(px);
    int32_t w = 0;
    for (const char* p = s; *p != '\0'; p++) w += lv_font_get_glyph_width(f, (uint8_t)p[0], (uint8_t)p[1]);
    return w;
}

/* A text line whose line box is centred on (x, y). */
static void jd_text_centred(jd_text_t* t, const char* s, int32_t px, int32_t opa, int32_t rgb, int32_t x, int32_t y)
{
    const lv_font_t* f = jd_font(px);
    memset(t, 0, sizeof(*t));
    jd_area_empty(&t->bounds);
    if (s[0] == '\0') return;
    t->shown = 1;
    t->font = px;
    t->opa = opa;
    t->rgb = rgb;
    jd_copy(t->text, sizeof(t->text), s);
    t->w = jd_text_w(px, t->text);
    t->pos.x = x - t->w / 2;
    t->pos.y = y - f->line_height / 2;
    lv_area_t box = {t->pos.x - 2, t->pos.y, t->pos.x + t->w + 2, t->pos.y + f->line_height - 1};
    t->bounds = box;
}

static lv_area_t jd_text_box(const jd_text_t* t)
{
    const lv_font_t* f = jd_font(t->font);
    lv_area_t a = {t->pos.x, t->pos.y, t->pos.x + t->w - 1, t->pos.y + f->line_height - 1};
    return a;
}

/* 12 hour clock, no AM or PM */
static void jd_hm(char* out, size_t n, int32_t min)
{
    min = jd_mod(min, JAKO_DIAL_DAY_MIN);
    const int32_t h = min / 60 % 12;
    snprintf(out, n, "%d:%02d", (int)(h == 0 ? 12 : h), (int)(min % 60));
}

/* ----------------------------------------------------- state from data */

static int32_t jd_now_s(const jd_t* r) { return r->t + (int32_t)(r->since_ms / 1000u); }
static int32_t jd_age_s(const jd_t* r) { return r->age + (int32_t)(r->cache_since_ms / 1000u); }
static int32_t jd_day(int32_t now_s) { return jd_min(now_s / JAKO_DIAL_DAY_S, 2); }
static bool jd_same_day(jako_dial_day_t a, jako_dial_day_t b) { return a.weekday == b.weekday && a.mday == b.mday; }

static jako_dial_rest_t jd_rest(const jd_t* r)
{
    if (r->cache != JD_CACHE_HELD) return JAKO_DIAL_REST_NO_CACHE;
    if (r->home != 1) return JAKO_DIAL_REST_HOME_OFF;
    if (jd_now_s(r) >= r->cache_span * JAKO_DIAL_DAY_S) return JAKO_DIAL_REST_PAST_DAYS;
    if (jd_age_s(r) >= JAKO_DIAL_REST_AGE_S) return JAKO_DIAL_REST_TOO_OLD;
    return JAKO_DIAL_REST_SHOWABLE;
}

/* Does any cached event touch today (the day the clock is in)? */
static bool jd_events_today(const jd_t* r, int32_t day)
{
    const int32_t a = day * JAKO_DIAL_DAY_MIN, b = a + JAKO_DIAL_DAY_MIN;
    for (int32_t i = 0; i < r->ev_n; i++)
        if (r->ev[i].s < b && r->ev[i].e > a) return true;
    return false;
}

static jako_dial_screen_t jd_screen(const jd_t* r, int32_t now_s)
{
    const int32_t day = jd_day(now_s);
    if (r->cache == JD_CACHE_HELD && day >= r->cache_span) return JAKO_DIAL_SCREEN_PAST;
    if (r->data == JD_DATA_ERROR) return r->cache == JD_CACHE_HELD ? JAKO_DIAL_SCREEN_ERROR_CACHED : JAKO_DIAL_SCREEN_ERROR;
    if (r->cache != JD_CACHE_HELD) return JAKO_DIAL_SCREEN_LOADING;
    if (r->src == JAKO_DIAL_SRC_SAMPLE) return JAKO_DIAL_SCREEN_SAMPLE;
    if (jd_age_s(r) > r->stale) return JAKO_DIAL_SCREEN_STALE;
    return jd_events_today(r, day) ? JAKO_DIAL_SCREEN_READY : JAKO_DIAL_SCREEN_EMPTY;
}

/* The event on now at the reference (the minute under the mark): the latest start, then the earliest end. */
static int32_t jd_event_on_now(const jd_t* r, int32_t ref)
{
    int32_t best = -1;
    for (int32_t i = 0; i < r->ev_n; i++) {
        const jd_event_t* e = &r->ev[i];
        if (e->s > ref || e->e <= ref) continue;
        if (best < 0 || e->s > r->ev[best].s || (e->s == r->ev[best].s && e->e < r->ev[best].e)) best = i;
    }
    return best;
}

/* The first event to start after the reference, inside the window. */
static int32_t jd_event_next(const jd_t* r, int32_t ref)
{
    int32_t best = -1;
    for (int32_t i = 0; i < r->ev_n; i++) {
        const jd_event_t* e = &r->ev[i];
        if (e->s <= ref || e->s >= ref + JD_FUTURE_MIN) continue;
        if (best < 0 || e->s < r->ev[best].s) best = i;
    }
    return best;
}

/* -------------------------------------------------------- the annulus */

static void jd_key(const jd_t* r, int32_t now_s, jd_key_t* k)
{
    memset(k, 0, sizeof(*k));
    const int32_t now_min = now_s / 60;
    const int32_t today_min = now_min % JAKO_DIAL_DAY_MIN;
    k->screen = r->screen;
    k->arcs = r->cache == JD_CACHE_HELD && r->screen != JAKO_DIAL_SCREEN_PAST;
    k->dim_half = r->screen == JAKO_DIAL_SCREEN_STALE || r->screen == JAKO_DIAL_SCREEN_ERROR_CACHED ||
                  (r->screen == JAKO_DIAL_SCREEN_SAMPLE && jd_age_s(r) > r->stale);
    k->night = today_min >= JD_NIGHT_FROM || today_min < JD_NIGHT_TO;
    int32_t micro = 60000; /* .06 */
    if (r->screen == JAKO_DIAL_SCREEN_LOADING)
        micro = r->reduced ? JD_PULSE_REDUCED_MICRO : JD_PULSE_MICRO[(r->clock / JD_PULSE_STEP_MS) % 8];
    k->track_opa = jd_dim(jd_rdiv((int64_t)micro * 255, 1000000), k, false);
    k->ref = now_min - jd_mod(now_min, 2);
    k->data_ver = r->data_ver;
}

static void jd_add_arc(jd_ring_t* g, int32_t radius, int32_t w, int32_t a0, int32_t a1, int32_t opa, bool rounded,
                       int32_t rgb)
{
    if (g->arc_n >= JD_ARC_MAX || a1 <= a0 || opa <= LV_OPA_MIN) return;
    jd_arc_t* a = &g->arc[g->arc_n++];
    a->r = radius;
    a->w = w;
    a->a0 = a0;
    a->a1 = a1;
    a->opa = opa;
    a->rounded = rounded ? 1 : 0;
    a->rgb = rgb;
}

/* The past part of an event, from the seam to the mark, in four square-ended sectors. */
static void jd_past(jd_ring_t* g, const jd_event_t* e, int32_t ref, bool on_now, const jd_key_t* k)
{
    const int32_t ps = jd_max(e->s, ref - JD_PAST_MIN), pe = jd_min(e->e, ref);
    if (pe <= ps) return;
    const int32_t a0 = jd_floor_div(ps - ref, 2), a1 = jd_min(0, jd_ceil_div(pe - ref, 2));
    for (int32_t s = 0; s < 4; s++) {
        const int32_t lo = jd_max(a0, -60 + s * JD_SECTOR_DEG), hi = jd_min(a1, -60 + (s + 1) * JD_SECTOR_DEG);
        const int32_t opa = jd_dim(on_now ? JD_PAST_NOW_OPA[s] : JD_PAST_OPA[s], k, true);
        jd_add_arc(g, JD_EV_R, JD_EV_W, lo, hi, opa, false, on_now ? JAKO_DIAL_NOW_EVENT_RGB : JAKO_DIAL_LIGHT);
    }
}

/* How far a round cap reaches past its arc's end, in whole degrees: half the width at the middle
   radius, ceil(w / 2 * 180 / (pi * rmid)) with pi as 22 / 7. 5 for an event (r 130, w 20). */
static int32_t jd_cap_deg(int32_t radius, int32_t w)
{
    const int32_t rmid = radius - w / 2;
    return rmid <= 0 ? 0 : jd_ceil_div(630 * w, 22 * rmid);
}

/* An arc with a round start and, when square_end, a square end exactly at a1: a rounded arc that stops a
   cap short of a1, whose cap the square arc covers to a1 (every shape is opaque, so they join without a seam). */
static void jd_add_event_arc(jd_ring_t* g, int32_t radius, int32_t w, int32_t a0, int32_t a1, int32_t opa,
                             bool square_end, int32_t rgb)
{
    if (!square_end) {
        jd_add_arc(g, radius, w, a0, a1, opa, true, rgb);
        return;
    }
    const int32_t mid = a1 - jd_cap_deg(radius, w);
    if (mid > a0) {
        jd_add_arc(g, radius, w, a0, mid, opa, true, rgb);
        jd_add_arc(g, radius, w, mid, a1, opa, false, rgb);
    } else {
        jd_add_arc(g, radius, w, a0, a1, opa, false, rgb);
    }
}

/* The future part: a rounded edge and a fill pulled in a degree at each end, or for the event on now the
   stepped glow and a full arc. Whole degrees, start floor and end ceil, at least 3 degrees long.
   At the window's end (the seam, ten o'clock) a round cap would cross the seam's black cut, so an event
   that runs past the window, or whose cap would reach past the seam, ends square there; one whose part
   inside the window is under the 3 degree minimum is not drawn (the minimum never stretches it). */
static void jd_future(jd_ring_t* g, const jd_event_t* e, int32_t ref, bool on_now, const jd_key_t* k)
{
    const int32_t fs = jd_max(e->s, ref), fe = jd_min(e->e, ref + JD_FUTURE_MIN);
    if (fe <= fs) return;
    const bool clipped = e->e > ref + JD_FUTURE_MIN;
    int32_t a0 = jd_floor_div(fs - ref, 2), a1 = jd_ceil_div(fe - ref, 2);
    if (a1 - a0 < JD_MIN_ARC_DEG) {
        if (clipped) return;
        a1 = a0 + JD_MIN_ARC_DEG;
    }
    if (a1 > JD_SEAM_DEG) {
        a1 = JD_SEAM_DEG;
        a0 = jd_min(a0, a1 - JD_MIN_ARC_DEG);
    }
    const int32_t cap = on_now ? jd_cap_deg(JD_GLOW_OUT_R, JD_GLOW_OUT_W) : jd_cap_deg(JD_EV_R, JD_EV_W);
    const bool square_end = clipped || a1 + cap > JD_SEAM_DEG;
    if (on_now) {
        const int32_t rgb = JAKO_DIAL_NOW_EVENT_RGB;
        jd_add_event_arc(g, JD_GLOW_OUT_R, JD_GLOW_OUT_W, a0, a1, jd_dim(JD_OPA_GLOW_OUT, k, true), square_end, rgb);
        jd_add_event_arc(g, JD_GLOW_IN_R, JD_GLOW_IN_W, a0, a1, jd_dim(JD_OPA_GLOW_IN, k, true), square_end, rgb);
        jd_add_event_arc(g, JD_EV_R, JD_EV_W, a0, a1, jd_dim(JD_OPA_FULL, k, true), square_end, rgb);
        return;
    }
    jd_add_event_arc(g, JD_EV_R, JD_EV_W, a0, a1, jd_dim(JD_OPA_EDGE, k, true), square_end, JAKO_DIAL_LIGHT);
    /* the fill runs to the square end, so the event reads as going on past the seam */
    jd_add_event_arc(g, JD_FILL_R, JD_FILL_W, a0 + 1, square_end ? a1 : a1 - 1, jd_dim(JD_OPA_FILL, k, true),
                     square_end, JAKO_DIAL_LIGHT);
}

static bool jd_in_window(int32_t m, int32_t ref) { return m >= ref - JD_PAST_MIN && m < ref + JD_FUTURE_MIN; }

static void jd_ring_compute(jd_t* r, const jd_key_t* k)
{
    jd_ring_t* g = &r->ring;
    memset(g, 0, sizeof(*g));
    const int32_t ref = k->ref;
    jd_add_arc(g, JD_TRACK_R, JD_TRACK_W, 0, 360, k->track_opa, false, JAKO_DIAL_LIGHT);
    if (k->arcs) {
        const int32_t now = jd_event_on_now(r, ref);
        for (int32_t i = 0; i < r->ev_n; i++) jd_past(g, &r->ev[i], ref, i == now, k);
        for (int32_t i = 0; i < r->ev_n; i++)
            if (i != now) jd_future(g, &r->ev[i], ref, false, k);
        if (now >= 0) jd_future(g, &r->ev[now], ref, true, k);
    }

    /* numerals on even hours, digits only, and not near the seam or the now mark */
    const int32_t first = ref - JD_PAST_MIN + jd_mod(-(ref - JD_PAST_MIN), 15);
    int32_t numeral_at[JD_NUM_MAX];
    int32_t numeral_n = 0;
    for (int32_t m = first; m < ref + JD_FUTURE_MIN && g->num_n < JD_NUM_MAX; m += 15) {
        const int32_t day_min = jd_mod(m, JAKO_DIAL_DAY_MIN);
        if (day_min % 120 != 0) continue;
        const int32_t deg2 = m - ref; /* half degrees */
        if (deg2 <= -2 * (60 - JD_NUM_SEAM_DEG) || deg2 >= 2 * (JD_SEAM_DEG - JD_NUM_SEAM_DEG)) continue;
        const int32_t h = day_min / 60 % 12;
        char s[4];
        snprintf(s, sizeof(s), "%d", (int)(h == 0 ? 12 : h));
        const lv_point_t c = jd_polar(r->cx, r->cy, deg2 * 5, JD_NUM_R * 16);
        jd_text_t* t = &g->num[g->num_n];
        jd_text_centred(t, s, 20, JD_OPA_NUM, JAKO_DIAL_LIGHT, c.x, c.y);
        lv_area_t near = r->mark_box;
        near.x1 -= 2;
        near.y1 -= 2;
        near.x2 += 2;
        near.y2 += 2;
        const lv_area_t box = jd_text_box(t);
        if (jd_area_hits(&box, &near)) {
            memset(t, 0, sizeof(*t));
            continue;
        }
        g->num_n++;
        numeral_at[numeral_n++] = m;
    }

    /* ticks every 15 minutes; no hour tick under a numeral */
    for (int32_t m = first; m < ref + JD_FUTURE_MIN && g->tick_n < JD_TICK_MAX; m += 15) {
        const bool hour = jd_mod(m, 60) == 0;
        bool under = false;
        for (int32_t i = 0; i < numeral_n; i++) under = under || numeral_at[i] == m;
        if (under) continue;
        const bool past = m < ref;
        const int32_t opa = hour ? (past ? JD_OPA_HOUR_PAST : JD_OPA_HOUR) : (past ? JD_OPA_QUARTER_PAST : JD_OPA_QUARTER);
        jd_tick_t* t = &g->tick[g->tick_n++];
        t->a = jd_polar(r->cx, r->cy, (m - ref) * 5, (hour ? JD_HOUR_IN : JD_QUARTER_IN) * 16);
        t->b = jd_polar(r->cx, r->cy, (m - ref) * 5, JD_TICK_OUT * 16);
        t->opa = jd_dim(opa, k, false);
    }

    /* sunset, today's or tomorrow's, inside the window */
    if (k->arcs)
        for (int32_t i = 0; i < 2 && g->sun_n < JD_SUN_MAX; i++) {
            if (r->sun[i] < 0) continue;
            const int32_t m = i * JAKO_DIAL_DAY_MIN + r->sun[i];
            if (!jd_in_window(m, ref)) continue;
            g->sun[g->sun_n++] = jd_polar(r->cx, r->cy, (m - ref) * 5, JD_SUN_R * 16);
        }
}

/* ----------------------------------------------------- the plate's marks */

static void jd_bottom_text(const jd_t* r, int32_t ref, char* out, size_t n, int32_t* opa)
{
    out[0] = '\0';
    *opa = JD_OPA_TEXT_SOFT;
    const int32_t day = jd_day(r->now_s);
    switch (r->screen) {
    case JAKO_DIAL_SCREEN_LOADING: snprintf(out, n, "Checking"); return;
    case JAKO_DIAL_SCREEN_ERROR:
    case JAKO_DIAL_SCREEN_ERROR_CACHED:
        jd_copy(out, n, JD_TEXT_ERROR);
        *opa = JD_OPA_FULL;
        return;
    case JAKO_DIAL_SCREEN_SAMPLE: snprintf(out, n, "Sample day"); return;
    case JAKO_DIAL_SCREEN_STALE: {
        const int32_t age = jd_age_s(r);
        if (age < 3600)
            snprintf(out, n, "%d min old", (int)(age / 60));
        else
            snprintf(out, n, "%d h old", (int)(age / 3600));
        return;
    }
    case JAKO_DIAL_SCREEN_PAST: return;
    default: break;
    }
    if (r->screen == JAKO_DIAL_SCREEN_READY) {
        /* the event on now "Reel to 12:30", or the next "Call 2:00"; past 114 px: "Reel 12:30", "Reel",
           then "To 12:30" or "At 2:00", which still says what the time is */
        const int32_t now = jd_event_on_now(r, ref);
        const int32_t next = now >= 0 ? -1 : jd_event_next(r, ref);
        if (now >= 0 || next >= 0) {
            const jd_event_t* e = &r->ev[now >= 0 ? now : next];
            char tm[8];
            jd_hm(tm, sizeof(tm), now >= 0 ? e->e : e->s);
            if (e->name[0] == '\0') {
                snprintf(out, n, "%s %s", now >= 0 ? "To" : "At", tm);
                return;
            }
            char c[4][JD_TEXT_MAX];
            snprintf(c[0], sizeof(c[0]), now >= 0 ? "%s to %s" : "%s %s", e->name, tm);
            snprintf(c[1], sizeof(c[1]), "%s %s", e->name, tm);
            jd_copy(c[2], sizeof(c[2]), e->name);
            snprintf(c[3], sizeof(c[3]), "%s %s", now >= 0 ? "To" : "At", tm); /* never a bare time */
            for (int i = 0; i < 4; i++)
                if (jd_text_w(20, c[i]) <= JD_BOTTOM_MAX_W || i == 3) {
                    jd_copy(out, n, c[i]);
                    return;
                }
        }
    }
    static const char* WEEKDAY[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    const jako_dial_day_t* d = &r->days[jd_min(day, 1)];
    if (day < 2 && d->weekday >= 0 && d->weekday <= 6 && d->mday >= 1) snprintf(out, n, "%s %d", WEEKDAY[d->weekday], d->mday);
}

static void jd_marks_compute(const jd_t* r, const jd_key_t* k, jd_marks_t* m)
{
    memset(m, 0, sizeof(*m));
    const int32_t now_min = r->now_s / 60;
    char s[JD_TEXT_MAX];
    jd_hm(s, sizeof(s), now_min);
    jd_text_centred(&m->time, s, 22, JD_OPA_FULL, JAKO_DIAL_NOW_TIME_RGB, r->cx, r->cy + JD_TIME_DY);
    int32_t opa;
    jd_bottom_text(r, k->ref, s, sizeof(s), &opa);
    jd_text_centred(&m->bottom, s, 20, opa, JAKO_DIAL_LIGHT, r->cx, r->cy + JD_BOTTOM_DY);
    m->bead_shown = 1;
    m->bead_deg = jd_mod(r->now_s, 60) * 6;
    m->bead = jd_disc_area(jd_polar(r->cx, r->cy, m->bead_deg * 10, JD_BEAD_R * 16), JD_BEAD_D);
}

/* ---------------------------------------------------------- invalidation */

static void jd_box(jd_boxes_t* b, const lv_area_t* a)
{
    if (jd_area_is_empty(a)) return;
    if (b->n >= JD_BOX_CAP) {
        b->full = true;
        return;
    }
    b->a[b->n++] = *a;
}

static void jd_disc_box(jd_t* r)
{
    lv_area_t a = {r->cx - JD_TILE_OUT - 2, r->cy - JD_TILE_OUT - 2, r->cx + JD_TILE_OUT + 2, r->cy + JD_TILE_OUT + 2};
    jd_box(&r->boxes, &a);
}

/* The band r_in to r_out as eight 45 degree tiles, each the box of its corners (the axes fall on tile edges). */
static void jd_tiles(jd_t* r, int32_t r_in, int32_t r_out)
{
    for (int32_t k = 0; k < JD_TILES; k++) {
        lv_area_t box;
        jd_area_empty(&box);
        for (int32_t e = 0; e < 2; e++) {
            const int32_t t = (k + e) * 450;
            jd_area_point(&box, jd_polar(r->cx, r->cy, t, r_in * 16), 2);
            jd_area_point(&box, jd_polar(r->cx, r->cy, t, r_out * 16), 2);
        }
        jd_box(&r->boxes, &box);
    }
}

static bool jd_overlap(const lv_area_t* a, const lv_area_t* b)
{
    return a->x1 <= b->x2 + 1 && b->x1 <= a->x2 + 1 && a->y1 <= b->y2 + 1 && b->y1 <= a->y2 + 1;
}

/* Merge the way LVGL joins its areas, then join the cheapest pairs until at most JAKO_DIAL_INV_MAX are left. */
static void jd_flush_boxes(jd_t* r)
{
    jd_boxes_t* b = &r->boxes;
    r->inval_areas = 0;
    r->inval_px = 0;
    lv_area_t screen;
    lv_obj_get_coords(r->obj, &screen);
    if (b->full) {
        b->n = 0;
        b->full = false;
        jd_disc_box(r);
    }
    int32_t n = 0;
    for (int32_t i = 0; i < b->n; i++) {
        lv_area_t a = b->a[i];
        a.x1 = jd_max(a.x1, screen.x1);
        a.y1 = jd_max(a.y1, screen.y1);
        a.x2 = jd_min(a.x2, screen.x2);
        a.y2 = jd_min(a.y2, screen.y2);
        if (!jd_area_is_empty(&a)) b->a[n++] = a;
    }
    bool changed = true;
    while (changed) {
        changed = false;
        for (int32_t i = 0; i < n && !changed; i++)
            for (int32_t k = i + 1; k < n && !changed; k++) {
                if (!jd_overlap(&b->a[i], &b->a[k])) continue;
                lv_area_t j = b->a[i];
                jd_area_join(&j, &b->a[k]);
                if (jd_area_px(&j) <= jd_area_px(&b->a[i]) + jd_area_px(&b->a[k])) {
                    b->a[i] = j;
                    b->a[k] = b->a[--n];
                    changed = true;
                }
            }
    }
    while (n > JAKO_DIAL_INV_MAX) {
        int32_t bi = 0, bk = 1, best = INT32_MAX;
        for (int32_t i = 0; i < n; i++)
            for (int32_t k = i + 1; k < n; k++) {
                lv_area_t j = b->a[i];
                jd_area_join(&j, &b->a[k]);
                const int32_t grow = jd_area_px(&j) - jd_area_px(&b->a[i]) - jd_area_px(&b->a[k]);
                if (grow < best) {
                    best = grow;
                    bi = i;
                    bk = k;
                }
            }
        jd_area_join(&b->a[bi], &b->a[bk]);
        b->a[bk] = b->a[--n];
    }
    for (int32_t i = 0; i < n; i++) {
        lv_obj_invalidate_area(r->obj, &b->a[i]);
        r->inval_px += (uint32_t)jd_area_px(&b->a[i]);
    }
    r->inval_areas = (uint32_t)n;
    b->n = 0;
}

static void jd_refresh(jd_t* r)
{
    r->now_s = jd_now_s(r);
    r->screen = jd_screen(r, r->now_s);
    jd_key_t k;
    jd_key(r, r->now_s, &k);
    jd_marks_compute(r, &k, &r->next);
    r->turned = false;
    r->boxes.n = 0;
    r->boxes.full = false;
    if (r->geom_state != JD_GEOM_READY) {
        r->key = k;
        jd_ring_compute(r, &k);
        r->marks = r->next;
        r->geom_state = JD_GEOM_READY;
        r->turned = true;
        r->turns++;
        jd_disc_box(r);
        jd_flush_boxes(r);
        return;
    }
    if (memcmp(&k, &r->key, sizeof(k)) != 0) {
        jd_key_t a = k, b = r->key;
        a.track_opa = b.track_opa = 0;
        if (memcmp(&a, &b, sizeof(a)) == 0) {
            jd_tiles(r, JD_BAND_IN, JD_BAND_OUT); /* the loading pulse: only the track's band */
        } else {
            jd_tiles(r, JD_TILE_IN, JD_TILE_OUT); /* a turn, or new data: the annulus, the bead with it */
            r->turned = true;
            r->turns++;
        }
        r->key = k;
        jd_ring_compute(r, &k);
    }
    if (memcmp(&r->marks.time, &r->next.time, sizeof(r->next.time)) != 0) {
        jd_box(&r->boxes, &r->marks.time.bounds);
        jd_box(&r->boxes, &r->next.time.bounds);
    }
    if (memcmp(&r->marks.bottom, &r->next.bottom, sizeof(r->next.bottom)) != 0) {
        jd_box(&r->boxes, &r->marks.bottom.bounds);
        jd_box(&r->boxes, &r->next.bottom.bounds);
    }
    if (!r->turned && (r->marks.bead_deg != r->next.bead_deg || r->marks.bead_shown != r->next.bead_shown)) {
        lv_area_t o = r->marks.bead, nb = r->next.bead;
        o.x1 -= 2;
        o.y1 -= 2;
        o.x2 += 2;
        o.y2 += 2;
        nb.x1 -= 2;
        nb.y1 -= 2;
        nb.x2 += 2;
        nb.y2 += 2;
        jd_box(&r->boxes, &o);
        jd_box(&r->boxes, &nb);
    }
    r->marks = r->next;
    jd_flush_boxes(r);
}

/* ------------------------------------------------------------------ draw */

static bool jd_visible(const lv_layer_t* layer, const lv_area_t* a) { return jd_area_hits(&layer->_clip_area, a); }

/* Does the clip area reach the ring between r_in and r_out round (cx, cy)? */
static bool jd_annulus_visible(const lv_layer_t* layer, int32_t cx, int32_t cy, int32_t r_in, int32_t r_out)
{
    const lv_area_t* c = &layer->_clip_area;
    const int32_t nx = cx < c->x1 ? c->x1 : cx > c->x2 ? c->x2 : cx;
    const int32_t ny = cy < c->y1 ? c->y1 : cy > c->y2 ? c->y2 : cy;
    const int64_t near = (int64_t)(nx - cx) * (nx - cx) + (int64_t)(ny - cy) * (ny - cy);
    const int32_t fx = jd_max(jd_abs(c->x1 - cx), jd_abs(c->x2 - cx)), fy = jd_max(jd_abs(c->y1 - cy), jd_abs(c->y2 - cy));
    const int64_t far = (int64_t)fx * fx + (int64_t)fy * fy;
    return near <= (int64_t)(r_out + 2) * (r_out + 2) && far >= (int64_t)jd_max(0, r_in - 2) * jd_max(0, r_in - 2);
}

static lv_color_t jd_mix(int32_t rgb, int32_t opa)
{
    return lv_color_mix(lv_color_hex((uint32_t)rgb), lv_color_black(), (uint8_t)(opa > 255 ? 255 : opa));
}

static void jd_fill(lv_layer_t* layer, lv_obj_t* obj, const lv_area_t* a, int32_t radius, lv_color_t color)
{
    if (!jd_visible(layer, a)) return;
    lv_draw_fill_dsc_t d;
    lv_draw_fill_dsc_init(&d);
    d.base.layer = layer;
    d.base.obj = obj;
    d.color = color;
    d.opa = LV_OPA_COVER;
    d.radius = radius;
    lv_draw_fill(layer, &d, a);
}

static int32_t jd_lv_angle(int32_t bearing_deg) { return jd_mod(bearing_deg + 270, 360); }

static void jd_draw_arc(lv_layer_t* layer, lv_obj_t* obj, const jd_t* r, const jd_arc_t* a)
{
    if (a->opa <= LV_OPA_MIN || !jd_annulus_visible(layer, r->cx, r->cy, a->r - a->w, a->r + (a->rounded ? 1 : 0)))
        return;
    lv_draw_arc_dsc_t d;
    lv_draw_arc_dsc_init(&d);
    d.base.layer = layer;
    d.base.obj = obj;
    d.color = jd_mix(a->rgb, a->opa);
    d.opa = LV_OPA_COVER;
    d.width = a->w;
    d.center.x = r->cx;
    d.center.y = r->cy;
    d.radius = (uint16_t)a->r;
    if (a->a1 - a->a0 >= 360) {
        d.start_angle = 0;
        d.end_angle = 360;
    } else {
        d.start_angle = jd_lv_angle(a->a0);
        d.end_angle = jd_lv_angle(a->a1);
    }
    d.rounded = a->rounded ? 1 : 0;
    lv_draw_arc(layer, &d);
}

static void jd_line(lv_layer_t* layer, lv_obj_t* obj, lv_point_t a, lv_point_t b, int32_t width, lv_color_t color,
                    bool round)
{
    lv_area_t box;
    jd_area_empty(&box);
    jd_area_point(&box, a, width / 2 + 2);
    jd_area_point(&box, b, width / 2 + 2);
    if (!jd_visible(layer, &box)) return;
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.base.layer = layer;
    d.base.obj = obj;
    d.color = color;
    d.opa = LV_OPA_COVER;
    d.width = width;
    d.p1.x = a.x;
    d.p1.y = a.y;
    d.p2.x = b.x;
    d.p2.y = b.y;
    d.round_start = round ? 1 : 0;
    d.round_end = round ? 1 : 0;
    lv_draw_line(layer, &d);
}

static void jd_text(lv_layer_t* layer, lv_obj_t* obj, const jd_text_t* t)
{
    if (!t->shown || t->text[0] == '\0' || !jd_visible(layer, &t->bounds)) return;
    const lv_font_t* f = jd_font(t->font);
    lv_draw_label_dsc_t d;
    lv_draw_label_dsc_init(&d);
    d.base.layer = layer;
    d.base.obj = obj;
    d.text = t->text;
    d.font = f;
    d.color = lv_color_hex((uint32_t)t->rgb);
    d.opa = (lv_opa_t)t->opa;
    d.align = LV_TEXT_ALIGN_LEFT;
    d.flag = LV_TEXT_FLAG_EXPAND;
    lv_area_t a = {t->pos.x, t->pos.y, t->pos.x + t->w + 2, t->pos.y + f->line_height - 1};
    lv_draw_label(layer, &d, &a);
}

static void jd_draw_cb(lv_event_t* e)
{
    jd_t* r = (jd_t*)lv_event_get_user_data(e);
    lv_layer_t* layer = lv_event_get_layer(e);
    if (r == NULL || layer == NULL || r->geom_state != JD_GEOM_READY) return;
    lv_obj_t* obj = r->obj;
    const jd_ring_t* g = &r->ring;
    const jd_marks_t* m = &r->marks;

    /* inside the plate and clear of the text: the face's own area, nothing of the dial's */
    const lv_area_t* c = &layer->_clip_area;
    const int32_t fx = jd_max(jd_abs(c->x1 - r->cx), jd_abs(c->x2 - r->cx));
    const int32_t fy = jd_max(jd_abs(c->y1 - r->cy), jd_abs(c->y2 - r->cy));
    const bool in_plate = (int64_t)fx * fx + (int64_t)fy * fy < (int64_t)JD_SKIP_R * JD_SKIP_R;
    if (in_plate && !jd_visible(layer, &m->time.bounds) && !jd_visible(layer, &m->bottom.bounds)) return;

    if (!in_plate) {
        /* the track and the arcs, oldest first, then the seam cut through them */
        for (int32_t i = 0; i < g->arc_n; i++) jd_draw_arc(layer, obj, r, &g->arc[i]);
        /* the seam (the today view, the only view in slice 1) */
        const int32_t seam = JD_SEAM_DEG * 10;
        jd_line(layer, obj, jd_polar(r->cx, r->cy, seam, JD_SEAM_IN * 16), jd_polar(r->cx, r->cy, seam, JD_SEAM_OUT * 16),
                JD_SEAM_W, lv_color_black(), false);
        /* the plate */
        jd_arc_t plate = {JD_PLATE_R, 1, 0, 360, JD_OPA_PLATE, 0, JAKO_DIAL_LIGHT};
        jd_draw_arc(layer, obj, r, &plate);
        /* ticks, numerals, sunset */
        for (int32_t i = 0; i < g->tick_n; i++)
            jd_line(layer, obj, g->tick[i].a, g->tick[i].b, JD_TICK_W, jd_mix(JAKO_DIAL_LIGHT, g->tick[i].opa), true);
        if (!r->hide_text)
            for (int32_t i = 0; i < g->num_n; i++) jd_text(layer, obj, &g->num[i]);
        for (int32_t i = 0; i < g->sun_n; i++) {
            const lv_area_t a = jd_disc_area(g->sun[i], JD_SUN_D);
            jd_fill(layer, obj, &a, LV_RADIUS_CIRCLE, jd_mix(JAKO_DIAL_LIGHT, JD_OPA_SUN));
        }
        /* the seconds bead, then the now mark over it at twelve */
        if (m->bead_shown) jd_fill(layer, obj, &m->bead, LV_RADIUS_CIRCLE, jd_mix(JAKO_DIAL_LIGHT, JD_OPA_BEAD));
        const lv_area_t* mk = &r->mark_box;
        jd_fill(layer, obj, mk, 5, lv_color_black());
        jd_fill(layer, obj, mk, 5, jd_mix(JAKO_DIAL_NOW_RGB, JD_OPA_GLOW_OUT));
        const lv_area_t m38 = {mk->x1 + 1, mk->y1 + 1, mk->x2 - 1, mk->y2 - 1};
        jd_fill(layer, obj, &m38, 4, jd_mix(JAKO_DIAL_NOW_RGB, JD_OPA_GLOW_IN));
        const lv_area_t m36 = {mk->x1 + 2, mk->y1 + 2, mk->x2 - 2, mk->y2 - 2};
        jd_fill(layer, obj, &m36, 3, jd_mix(JAKO_DIAL_NOW_RGB, JD_OPA_FULL));
    }
    if (!r->hide_text) {
        jd_text(layer, obj, &m->time);
        jd_text(layer, obj, &m->bottom);
    }
}

/* ---------------------------------------------------------------- events */

static void jd_setup(jd_t* r)
{
    lv_area_t c;
    lv_obj_get_coords(r->obj, &c);
    r->cx = c.x1 + lv_area_get_width(&c) / 2;
    r->cy = c.y1 + lv_area_get_height(&c) / 2;
    /* the now mark: 10 by 40 at twelve, r 100 to 140 */
    r->mark_box.x1 = r->cx - 5;
    r->mark_box.x2 = r->cx + 4;
    r->mark_box.y1 = r->cy - JD_MARK_OUT;
    r->mark_box.y2 = r->cy - JD_MARK_IN - 1;
}

static void jd_size_cb(lv_event_t* e)
{
    jd_t* r = (jd_t*)lv_event_get_user_data(e);
    if (r == NULL) return;
    jd_setup(r);
    r->geom_state = JD_GEOM_NONE;
    lv_obj_invalidate(r->obj);
}

static void jd_delete_cb(lv_event_t* e)
{
    jd_t* r = (jd_t*)lv_event_get_user_data(e);
    if (r == NULL) return;
    if (r->timer != NULL) lv_timer_delete(r->timer);
    lv_obj_set_user_data(r->obj, NULL);
    r->magic = 0;
    lv_free(r);
}

static void jd_timer_cb(lv_timer_t* t)
{
    jd_t* r = (jd_t*)lv_timer_get_user_data(t);
    const uint32_t elapsed = lv_tick_elaps(r->last_tick);
    r->last_tick = lv_tick_get();
    jako_dial_advance(r->obj, elapsed);
}

static uint32_t jd_sat_add(uint32_t a, uint32_t b) { return a > JD_SINCE_MAX - b ? JD_SINCE_MAX : a + b; }

/* ------------------------------------------------------------- public API */

lv_obj_t* jako_dial_create(lv_obj_t* parent)
{
    jd_t* r = (jd_t*)lv_malloc_zeroed(sizeof(jd_t));
    if (r == NULL) return NULL;
    lv_obj_t* obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj); /* no style: transparent, so the face shows through the plate */
    lv_obj_set_size(obj, lv_pct(100), lv_pct(100));
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_CLICK_FOCUSABLE);
    r->magic = JD_MAGIC;
    r->obj = obj;
    r->cache = JD_CACHE_NONE;
    r->stale = JAKO_DIAL_STALE_DEFAULT_S;
    r->sun[0] = r->sun[1] = -1;
    for (int i = 0; i < 2; i++) {
        r->days[i].weekday = -1;
        r->days[i].mday = 0;
    }
    lv_obj_update_layout(obj);
    jd_setup(r);
    lv_obj_set_user_data(obj, r);
    lv_obj_add_event_cb(obj, jd_draw_cb, LV_EVENT_DRAW_MAIN, r);
    lv_obj_add_event_cb(obj, jd_size_cb, LV_EVENT_SIZE_CHANGED, r);
    lv_obj_add_event_cb(obj, jd_delete_cb, LV_EVENT_DELETE, r);
    r->last_tick = lv_tick_get();
    r->timer = lv_timer_create(jd_timer_cb, JAKO_DIAL_TICK_MS, r);
    return obj;
}

/* A loading or error update carries no events but moves t and days to its own day. The cache keeps
   minutes from its own day 0, so it follows: the same day, nothing to do; the cache's day 1 (the
   update came after midnight), every event and the sunsets move back a day and the cache now covers
   that one day; any other day, the cache is dropped rather than drawn on the wrong day. */
static void jd_rebase_cache(jd_t* r, const jako_dial_update_t* u)
{
    if (r->cache != JD_CACHE_HELD || jd_same_day(u->days[0], r->cache_days[0])) return;
    if (r->cache_span == 2 && jd_same_day(u->days[0], r->cache_days[1])) {
        int32_t n = 0;
        for (int32_t i = 0; i < r->ev_n; i++) {
            jd_event_t e = r->ev[i];
            e.s -= JAKO_DIAL_DAY_MIN;
            e.e -= JAKO_DIAL_DAY_MIN;
            if (e.e <= -JD_PAST_MIN) continue; /* ended before any window of the new day 0 can see it */
            r->ev[n++] = e;
        }
        if (n < r->ev_n) memset(&r->ev[n], 0, sizeof(r->ev[0]) * (size_t)(r->ev_n - n));
        r->ev_n = n;
        r->sun[0] = r->sun[1];
        r->sun[1] = -1;
        r->cache_days[0] = r->cache_days[1];
        r->cache_days[1].weekday = -1;
        r->cache_days[1].mday = 0;
        r->cache_span = 1;
    } else {
        memset(r->ev, 0, sizeof(r->ev));
        r->ev_n = 0;
        r->sun[0] = r->sun[1] = -1;
        r->cache = JD_CACHE_NONE;
        r->cache_span = 0;
    }
    r->data_ver++;
}

int32_t jako_dial_apply(lv_obj_t* obj, const jako_dial_update_t* u)
{
    jd_t* r = jd_get(obj);
    if (r == NULL || u == NULL) return 0;
    if (u->state == JAKO_DIAL_STATE_LOADING || u->state == JAKO_DIAL_STATE_ERROR) jd_rebase_cache(r, u);
    r->t = u->t;
    r->days[0] = u->days[0];
    r->days[1] = u->days[1];
    r->since_ms = 0;
    r->updates++;
    if (u->state == JAKO_DIAL_STATE_ERROR) {
        /* the store could not be read: the cache stays, shown under the error status */
        r->data = JD_DATA_ERROR;
        r->home = u->home;
    } else if (u->state == JAKO_DIAL_STATE_READY || u->state == JAKO_DIAL_STATE_EMPTY) {
        jd_event_t ev[JAKO_DIAL_MAX_EVENTS];
        memset(ev, 0, sizeof(ev));
        const int32_t n = u->ev_n > JAKO_DIAL_MAX_EVENTS ? JAKO_DIAL_MAX_EVENTS : u->ev_n;
        for (int32_t i = 0; i < n; i++) {
            ev[i].s = u->ev[i].d * JAKO_DIAL_DAY_MIN + u->ev[i].s;
            ev[i].e = u->ev[i].d * JAKO_DIAL_DAY_MIN + jd_max(u->ev[i].e, u->ev[i].s + 1);
            memcpy(ev[i].name, u->ev[i].name, sizeof(u->ev[i].name));
            ev[i].name[JAKO_DIAL_NAME_MAX] = '\0';
        }
        const int32_t sun[2] = {u->sun[0], u->sun[1]};
        /* amendment 23: the annulus is redrawn only when what it draws changed */
        const bool same = r->cache == JD_CACHE_HELD && r->ev_n == n && memcmp(r->ev, ev, sizeof(ev)) == 0 &&
                          memcmp(r->sun, sun, sizeof(sun)) == 0;
        if (!same) r->data_ver++;
        memcpy(r->ev, ev, sizeof(ev));
        r->ev_n = n;
        r->sun[0] = sun[0];
        r->sun[1] = sun[1];
        r->cache = JD_CACHE_HELD;
        r->cache_days[0] = u->days[0];
        r->cache_days[1] = u->days[1];
        r->cache_span = 2;
        r->data = JD_DATA_OK;
        r->src = u->src == JAKO_DIAL_SRC_SAMPLE ? JAKO_DIAL_SRC_SAMPLE : JAKO_DIAL_SRC_DESK;
        r->age = u->age < 0 ? 0 : u->age;
        r->cache_since_ms = 0;
        r->stale = u->stale;
        r->home = u->home;
    }
    /* loading: amendment 7, never replaces the cache; t and days were taken above */
    jd_refresh(r);
    r->rest_armed = jd_rest(r) == JAKO_DIAL_REST_SHOWABLE;
    return 0;
}

jako_dial_screen_t jako_dial_screen(lv_obj_t* obj)
{
    jd_t* r = jd_get(obj);
    return r == NULL ? JAKO_DIAL_SCREEN_LOADING : jd_screen(r, jd_now_s(r));
}

jako_dial_rest_t jako_dial_rest(lv_obj_t* obj)
{
    jd_t* r = jd_get(obj);
    return r == NULL ? JAKO_DIAL_REST_NO_CACHE : jd_rest(r);
}

void jako_dial_set_rest_cb(lv_obj_t* obj, jako_dial_rest_cb_t cb, void* user)
{
    jd_t* r = jd_get(obj);
    if (r == NULL) return;
    r->rest_cb = cb;
    r->rest_user = user;
}

void jako_dial_set_reduced_motion(lv_obj_t* obj, bool on)
{
    jd_t* r = jd_get(obj);
    if (r != NULL) r->reduced = on;
}

void jako_dial_advance(lv_obj_t* obj, uint32_t ms)
{
    jd_t* r = jd_get(obj);
    if (r == NULL) return;
    r->clock += ms;
    r->since_ms = jd_sat_add(r->since_ms, ms);
    r->cache_since_ms = jd_sat_add(r->cache_since_ms, ms);
    jd_refresh(r);
    if (r->rest_armed && jd_rest(r) != JAKO_DIAL_REST_SHOWABLE) {
        r->rest_armed = false;
        JD_LOGI("no longer shown at rest");
        if (r->rest_cb != NULL) r->rest_cb(obj, r->rest_user); /* may delete the dial: nothing after */
    }
}

void jako_dial_debug_set_hide_text(lv_obj_t* obj, bool hide)
{
    jd_t* r = jd_get(obj);
    if (r == NULL || r->hide_text == hide) return;
    r->hide_text = hide;
    lv_obj_invalidate(obj);
}

void jako_dial_get_debug(lv_obj_t* obj, jako_dial_debug_t* out)
{
    jd_t* r = jd_get(obj);
    if (r == NULL || out == NULL) return;
    memset(out, 0, sizeof(*out));
    out->clock_ms = r->clock;
    out->updates = r->updates;
    out->screen = jd_screen(r, jd_now_s(r));
    out->rest = jd_rest(r);
    out->now_s = r->now_s;
    out->day = jd_day(r->now_s);
    out->ref_min = jd_mod(r->key.ref, JAKO_DIAL_DAY_MIN);
    out->age_s = r->cache == JD_CACHE_HELD ? jd_age_s(r) : -1;
    out->night = (uint8_t)r->key.night;
    out->dim_half = (uint8_t)r->key.dim_half;
    jd_copy(out->time_text, sizeof(out->time_text), r->marks.time.text);
    jd_copy(out->bottom_text, sizeof(out->bottom_text), r->marks.bottom.text);
    out->bottom_opa = (uint8_t)(r->marks.bottom.shown ? r->marks.bottom.opa : 0);
    out->bead_deg = r->marks.bead_shown ? r->marks.bead_deg : -1;
    out->arc_n = (uint32_t)r->ring.arc_n;
    out->tick_n = (uint32_t)r->ring.tick_n;
    out->numeral_n = (uint32_t)r->ring.num_n;
    out->sunset_shown = (uint8_t)r->ring.sun_n;
    const jd_text_t* texts[2 + JD_NUM_MAX];
    int n = 0;
    texts[n++] = &r->marks.time;
    texts[n++] = &r->marks.bottom;
    for (int32_t i = 0; i < r->ring.num_n; i++) texts[n++] = &r->ring.num[i];
    for (int i = 0; i < n; i++) {
        const jd_text_t* t = texts[i];
        if (!t->shown) continue;
        if (out->text_px_min == 0 || t->font < out->text_px_min) out->text_px_min = (uint8_t)t->font;
        if (out->text_opa_min == 0 || t->opa < out->text_opa_min) out->text_opa_min = (uint8_t)t->opa;
        if (out->text_n < sizeof(out->text_box) / sizeof(out->text_box[0])) out->text_box[out->text_n++] = jd_text_box(t);
    }
    out->now_mark = r->mark_box;
    out->inval_areas = r->inval_areas;
    out->inval_px = r->inval_px;
    out->turns = r->turns;
    out->turned = r->turned ? 1 : 0;
}
