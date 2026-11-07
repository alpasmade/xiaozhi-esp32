// Copyright 2026 Alpas Made. All rights reserved.
/* A scene on LVGL 9.5. See jako_scene.h for what it draws and when.

   All geometry is integer and worked out when an update is applied: a box
   for every shape (the pixels it can light, plus one), and for every text
   its line box from the font's advances joined with its glyphs' ink. The
   draw callback only draws, and skips every shape and text outside the
   area being drawn.

   Each tick the widget works out a small frame (the view, the pulse step,
   the half alpha of stale, the status line, the scene's version) and
   compares it with the frame it drew: a new scene or a change of view
   redraws the scene's boxes, a pulse step the loading ring's eight tiles,
   a new status text its old and new line boxes. The boxes are merged
   before they reach LVGL, at most JAKO_SCENE_INV_MAX a tick, so the face's
   ring tiles always fit in LVGL's 32 slot list. Apply works out the frame
   at once, so a refresh between an apply and the next tick never draws
   the new scene with the old frame. */
#include "jako_scene.h"

#include <stdio.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_log.h"
#define JS_LOGI(fmt, ...) ESP_LOGI("jako_scene", fmt, ##__VA_ARGS__)
#else
#define JS_LOGI(fmt, ...) ((void)0)
#endif

LV_FONT_DECLARE(font_inter_600_20_4)
LV_FONT_DECLARE(font_inter_600_22_4)

#define JS_MAGIC 0x53434E45u
#define JS_SOFT 158          /* .62 */
#define JS_FULL 255
#define JS_STATUS_PX 20
#define JS_TEXT_BUF 36       /* a text of 32, or a status line of 24, and its NUL, in multiples of 4 */
#define JS_BOX_CAP 40
#define JS_TILES 8           /* the loading ring's tiles */
#define JS_PULSE_STEP_MS 300
#define JS_PULSE_REDUCED_MICRO 110000
/* The loading pulse, .06 to .16 in 8 steps every 2.4 s, in millionths: .06 + .10 x (0.5 - 0.5 cos(k x 45 degrees)),
   the radar's T.LOADING */
static const int32_t JS_PULSE_MICRO[8] = {60000, 74645, 110000, 145355, 160000, 145355, 110000, 74645};
/* sin of k x 22.5 degrees in Q14, k = 0..15 */
static const int32_t JS_SIN16[16] = {0,     6270,   11585,  15137,  16384,  15137,  11585,  6270,
                                     0,     -6270,  -11585, -15137, -16384, -15137, -11585, -6270};

/* --------------------------------------------------------------- types */

/* A text as drawn. All int32 and a char array of a multiple of 4, so frames compare with memcmp. */
typedef struct {
    int32_t shown, font, opa;
    lv_point_t pos; /* the top left of its line box */
    int32_t w;      /* the sum of its advances */
    lv_area_t box;  /* its line box */
    lv_area_t lit;  /* its line box joined with its glyphs' ink */
    char text[JS_TEXT_BUF];
} js_text_t;

typedef struct {
    int32_t version; /* SHOWN applies: a new scene */
    int32_t view;
    int32_t half;    /* 1: shapes at half alpha */
    int32_t pulse_opa;
    lv_area_t content;
    js_text_t status;
} js_frame_t;

typedef struct {
    uint32_t magic;
    lv_obj_t* obj;
    lv_timer_t* timer;
    uint32_t last_tick;
    jako_scene_expire_cb_t expire_cb;
    void* expire_user;
    jako_scene_hold_t hold;
    bool reduced;

    uint32_t clock;       /* ms since create */
    uint32_t received_ms; /* clock at the last SHOWN apply: ages count from here */
    uint32_t held_ms;     /* clock at the last SHOWN or SAME apply: the hold counts from here */
    uint32_t updates, sames;
    int32_t ox, oy;       /* the object's top left on the screen */

    jako_scene_update_t u;
    jako_scene_box_t shape_box[JAKO_SCENE_MAX_SHAPES];
    js_text_t text[JAKO_SCENE_MAX_TEXTS];
    lv_area_t scene_box; /* every shape and text of u, joined */
    lv_area_t ring_box;  /* the loading ring */
    lv_area_t tile[JS_TILES];

    jako_scene_view_t view;
    int32_t age_s;
    char status[JS_TEXT_BUF];
    int32_t frame_ready;
    js_frame_t frame, next;
    lv_area_t boxes[JS_BOX_CAP];
    int32_t box_n;
    bool box_full;
    uint32_t inval_areas, inval_px;
} js_t;

/* ------------------------------------------------------------- helpers */

static int32_t js_min(int32_t a, int32_t b) { return a < b ? a : b; }
static int32_t js_max(int32_t a, int32_t b) { return a > b ? a : b; }
static int32_t js_abs(int32_t a) { return a < 0 ? -a : a; }
static int32_t js_rdiv(int64_t n, int64_t d) { return (int32_t)(n >= 0 ? (n + d / 2) / d : -((-n + d / 2) / d)); }

static uint32_t js_isqrt(uint64_t v)
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

static void js_area_empty(lv_area_t* a)
{
    a->x1 = 0;
    a->y1 = 0;
    a->x2 = -1;
    a->y2 = -1;
}

static bool js_area_is_empty(const lv_area_t* a) { return a->x2 < a->x1 || a->y2 < a->y1; }

static void js_area_join(lv_area_t* acc, const lv_area_t* b)
{
    if (js_area_is_empty(b)) return;
    if (js_area_is_empty(acc)) {
        *acc = *b;
        return;
    }
    acc->x1 = js_min(acc->x1, b->x1);
    acc->y1 = js_min(acc->y1, b->y1);
    acc->x2 = js_max(acc->x2, b->x2);
    acc->y2 = js_max(acc->y2, b->y2);
}

static bool js_area_hits(const lv_area_t* a, const lv_area_t* b)
{
    return !js_area_is_empty(a) && !js_area_is_empty(b) && a->x1 <= b->x2 && b->x1 <= a->x2 && a->y1 <= b->y2 &&
           b->y1 <= a->y2;
}

static int32_t js_area_px(const lv_area_t* a)
{
    return js_area_is_empty(a) ? 0 : (a->x2 - a->x1 + 1) * (a->y2 - a->y1 + 1);
}

static lv_area_t js_area(int32_t x1, int32_t y1, int32_t x2, int32_t y2)
{
    lv_area_t a = {x1, y1, x2, y2};
    return a;
}

static jako_scene_box_t js_box16(const lv_area_t* a)
{
    jako_scene_box_t b;
    if (js_area_is_empty(a)) {
        b.x1 = 0;
        b.y1 = 0;
        b.x2 = -1;
        b.y2 = -1;
        return b;
    }
    b.x1 = (int16_t)a->x1;
    b.y1 = (int16_t)a->y1;
    b.x2 = (int16_t)a->x2;
    b.y2 = (int16_t)a->y2;
    return b;
}

static lv_area_t js_from16(const jako_scene_box_t* b) { return js_area(b->x1, b->y1, b->x2, b->y2); }

static js_t* js_get(lv_obj_t* obj)
{
    if (obj == NULL) return NULL;
    js_t* s = (js_t*)lv_obj_get_user_data(obj);
    return (s != NULL && s->magic == JS_MAGIC) ? s : NULL;
}

static const lv_font_t* js_font(int32_t px) { return px >= 22 ? &font_inter_600_22_4 : &font_inter_600_20_4; }

static int32_t js_text_w(int32_t px, const char* s)
{
    const lv_font_t* f = js_font(px);
    int32_t w = 0;
    for (const char* p = s; *p != '\0'; p++) w += lv_font_get_glyph_width(f, (uint8_t)p[0], (uint8_t)p[1]);
    return w;
}

/* A line of text with its line box's top left at (x, top), in screen px: its box from the advances, and its
   lit box, the box joined with every glyph's ink as the label draws it. */
static void js_text_at(js_text_t* t, const char* s, int32_t px, int32_t opa, int32_t x, int32_t top)
{
    memset(t, 0, sizeof(*t));
    const lv_font_t* f = js_font(px);
    t->shown = 1;
    t->font = px;
    t->opa = opa;
    snprintf(t->text, sizeof(t->text), "%s", s);
    t->w = js_text_w(px, t->text);
    t->pos.x = x;
    t->pos.y = top;
    t->box = js_area(x, top, x + t->w - 1, top + f->line_height - 1);
    t->lit = t->box;
    int32_t pen = x;
    for (const char* p = t->text; *p != '\0'; p++) {
        lv_font_glyph_dsc_t g;
        memset(&g, 0, sizeof(g));
        if (lv_font_get_glyph_dsc(f, &g, (uint8_t)p[0], (uint8_t)p[1]) && g.box_w > 0 && g.box_h > 0) {
            const int32_t gy = top + (f->line_height - f->base_line) - g.box_h - g.ofs_y;
            const lv_area_t ink = js_area(pen + g.ofs_x, gy, pen + g.ofs_x + g.box_w - 1, gy + g.box_h - 1);
            js_area_join(&t->lit, &ink);
        }
        pen += g.adv_w;
    }
}

/* A text of tx placed by its align and centre line, in screen px. */
static void js_scene_text(js_text_t* t, const jako_scene_text_t* in)
{
    const lv_font_t* f = js_font(in->size);
    const int32_t w = js_text_w(in->size, in->text);
    int32_t x = in->x;
    if (in->align == JAKO_SCENE_ALIGN_CENTRE) x -= w / 2;
    if (in->align == JAKO_SCENE_ALIGN_RIGHT) x -= w;
    js_text_at(t, in->text, in->size, in->opa, x, in->y - f->line_height / 2);
}

/* Is a lit box (screen px) inside the edge? Its four corner pixels are the anchors, reach 0. */
static jako_scene_apply_t js_text_fits(const lv_area_t* a)
{
    const int32_t xs[2] = {a->x1, a->x2}, ys[2] = {a->y1, a->y2};
    const int32_t lim = JAKO_SCENE_EDGE_R * JAKO_SCENE_EDGE_R;
    for (int i = 0; i < 2; i++)
        for (int k = 0; k < 2; k++) {
            const int32_t dx = xs[i] - JAKO_SCENE_CENTRE, dy = ys[k] - JAKO_SCENE_CENTRE;
            if (dx * dx + dy * dy > lim) return JAKO_SCENE_APPLY_PAST_EDGE;
        }
    return JAKO_SCENE_APPLY_SHOWN;
}

/* --------------------------------------------------------- the update */

static bool js_kind_ok(uint8_t k)
{
    return k == JAKO_SCENE_KIND_CIRCLE || k == JAKO_SCENE_KIND_ARC || k == JAKO_SCENE_KIND_DISC ||
           k == JAKO_SCENE_KIND_LINE || k == JAKO_SCENE_KIND_POLYLINE || k == JAKO_SCENE_KIND_TRIANGLES ||
           k == JAKO_SCENE_KIND_RECT || k == JAKO_SCENE_KIND_PLATE;
}

jako_scene_apply_t jako_scene_check(const jako_scene_update_t* u)
{
    if (u == NULL || u->shape_n > JAKO_SCENE_MAX_SHAPES || u->point_n > JAKO_SCENE_MAX_POINTS ||
        u->text_n > JAKO_SCENE_MAX_TEXTS || memchr(u->id, '\0', sizeof(u->id)) == NULL ||
        memchr(u->st, '\0', sizeof(u->st)) == NULL)
        return JAKO_SCENE_APPLY_BAD;
    for (uint16_t i = 0; i < u->shape_n; i++) {
        const jako_scene_shape_t* s = &u->shapes[i];
        if (!js_kind_ok(s->kind)) return JAKO_SCENE_APPLY_BAD;
        if (s->kind == JAKO_SCENE_KIND_POLYLINE || s->kind == JAKO_SCENE_KIND_TRIANGLES) {
            const bool poly = s->kind == JAKO_SCENE_KIND_POLYLINE;
            if ((uint32_t)s->pt + s->pn > u->point_n || (poly ? s->pn < 2 : (s->pn < 3 || s->pn % 3 != 0)))
                return JAKO_SCENE_APPLY_BAD;
        }
    }
    if (u->state == JAKO_SCENE_STATE_LOADING) return JAKO_SCENE_APPLY_SHOWN; /* its texts are not drawn */
    for (uint8_t i = 0; i < u->text_n; i++) {
        const jako_scene_text_t* in = &u->texts[i];
        if ((in->size != 20 && in->size != 22) || in->align > JAKO_SCENE_ALIGN_RIGHT ||
            memchr(in->text, '\0', sizeof(in->text)) == NULL)
            return JAKO_SCENE_APPLY_BAD;
        js_text_t t;
        js_scene_text(&t, in);
        const jako_scene_apply_t r = js_text_fits(&t.lit);
        if (r != JAKO_SCENE_APPLY_SHOWN) return r;
    }
    return JAKO_SCENE_APPLY_SHOWN;
}

/* The pixels a shape can light, plus one, in screen px. */
static lv_area_t js_shape_area(const jako_scene_update_t* u, const jako_scene_shape_t* s)
{
    lv_area_t a;
    js_area_empty(&a);
    switch (s->kind) {
    case JAKO_SCENE_KIND_CIRCLE:
    case JAKO_SCENE_KIND_ARC:
    case JAKO_SCENE_KIND_DISC: return js_area(s->x - s->r - 1, s->y - s->r - 1, s->x + s->r, s->y + s->r);
    case JAKO_SCENE_KIND_LINE: {
        const int32_t m = (s->w + 1) / 2 + 2;
        a = js_area(js_min(s->x, s->x2) - m, js_min(s->y, s->y2) - m, js_max(s->x, s->x2) + m, js_max(s->y, s->y2) + m);
        return a;
    }
    case JAKO_SCENE_KIND_POLYLINE:
    case JAKO_SCENE_KIND_TRIANGLES: {
        const int32_t m = s->kind == JAKO_SCENE_KIND_POLYLINE ? (s->w + 1) / 2 + 2 : 2;
        for (uint16_t i = 0; i < s->pn; i++) {
            const jako_scene_point_t* p = &u->points[s->pt + i];
            const lv_area_t b = js_area(p->x - m, p->y - m, p->x + m, p->y + m);
            js_area_join(&a, &b);
        }
        return a;
    }
    case JAKO_SCENE_KIND_RECT:
    case JAKO_SCENE_KIND_PLATE: return js_area(s->x, s->y, s->x + s->x2 - 1, s->y + s->y2 - 1);
    default: return a;
    }
}

/* ------------------------------------------------------ state from data */

static void js_ages(js_t* s) { s->age_s = s->u.age + (int32_t)((s->clock - s->received_ms) / 1000u); }

static jako_scene_view_t js_view(const js_t* s)
{
    switch (s->u.state) {
    case JAKO_SCENE_STATE_LOADING: return JAKO_SCENE_VIEW_LOADING;
    case JAKO_SCENE_STATE_ERROR: return JAKO_SCENE_VIEW_ERROR;
    default: break;
    }
    if (s->u.stale > 0 && s->age_s >= s->u.stale) return JAKO_SCENE_VIEW_STALE;
    return s->u.state == JAKO_SCENE_STATE_EMPTY ? JAKO_SCENE_VIEW_EMPTY : JAKO_SCENE_VIEW_READY;
}

/* The status line, first that applies. opa 0: nothing. */
static int32_t js_status(const js_t* s, char* out, size_t n)
{
    out[0] = '\0';
    switch (s->view) {
    case JAKO_SCENE_VIEW_LOADING: snprintf(out, n, "%s", s->u.st[0] ? s->u.st : "Checking"); return JS_SOFT;
    case JAKO_SCENE_VIEW_ERROR: snprintf(out, n, "%s", s->u.st[0] ? s->u.st : "No data"); return JS_FULL;
    default: break;
    }
    if (s->u.src == JAKO_SCENE_SRC_SAMPLE) {
        snprintf(out, n, "Sample data"); /* invented figures say so, always */
        return JS_SOFT;
    }
    if (s->view == JAKO_SCENE_VIEW_STALE) {
        if (s->age_s < 3600)
            snprintf(out, n, "%d min old", (int)(s->age_s / 60));
        else
            snprintf(out, n, "%d h old", (int)(s->age_s / 3600));
        return JS_SOFT;
    }
    if (s->u.st[0] != '\0') {
        snprintf(out, n, "%s", s->u.st);
        return JS_SOFT;
    }
    return 0;
}

/* The status line in the slot: 20 px, centred, baseline 396, at most 200 px: words drop from the end, then
   characters. */
static void js_status_geom(const js_t* s, js_text_t* t, const char* text, int32_t opa)
{
    memset(t, 0, sizeof(*t));
    js_area_empty(&t->box);
    js_area_empty(&t->lit);
    if (opa <= 0 || text[0] == '\0') return;
    char buf[JS_TEXT_BUF];
    snprintf(buf, sizeof(buf), "%s", text);
    while (js_text_w(JS_STATUS_PX, buf) > JAKO_SCENE_STATUS_W) {
        char* sp = strrchr(buf, ' ');
        if (sp != NULL && sp != buf)
            *sp = '\0';
        else
            buf[strlen(buf) - 1] = '\0';
    }
    const lv_font_t* f = js_font(JS_STATUS_PX);
    const int32_t w = js_text_w(JS_STATUS_PX, buf);
    js_text_at(t, buf, JS_STATUS_PX, opa, s->ox + JAKO_SCENE_CENTRE - w / 2,
               s->oy + JAKO_SCENE_STATUS_BASE - (f->line_height - f->base_line));
}

static void js_layout(js_t* s)
{
    js_area_empty(&s->scene_box);
    for (uint16_t i = 0; i < s->u.shape_n; i++) {
        lv_area_t a = js_shape_area(&s->u, &s->u.shapes[i]);
        a.x1 += s->ox;
        a.x2 += s->ox;
        a.y1 += s->oy;
        a.y2 += s->oy;
        s->shape_box[i] = js_box16(&a);
        js_area_join(&s->scene_box, &a);
    }
    for (uint8_t i = 0; i < s->u.text_n; i++) {
        jako_scene_text_t in = s->u.texts[i];
        in.x = (int16_t)(in.x + s->ox);
        in.y = (int16_t)(in.y + s->oy);
        js_scene_text(&s->text[i], &in);
        js_area_join(&s->scene_box, &s->text[i].lit);
    }
}

/* ------------------------------------------------------------ the frame */

static void js_compute(js_t* s, js_frame_t* f)
{
    memset(f, 0, sizeof(*f));
    f->version = (int32_t)s->updates;
    f->view = s->view;
    f->half = s->view == JAKO_SCENE_VIEW_STALE;
    if (s->view == JAKO_SCENE_VIEW_LOADING) {
        const int64_t micro = s->reduced ? JS_PULSE_REDUCED_MICRO : JS_PULSE_MICRO[(s->clock / JS_PULSE_STEP_MS) % 8];
        f->pulse_opa = js_rdiv(micro * 255, 1000000);
        f->content = s->ring_box;
    } else {
        f->content = s->scene_box;
    }
    char status[JS_TEXT_BUF];
    const int32_t opa = js_status(s, status, sizeof(status));
    snprintf(s->status, sizeof(s->status), "%s", status);
    js_status_geom(s, &f->status, status, opa);
}

static void js_box(js_t* s, const lv_area_t* a)
{
    if (js_area_is_empty(a)) return;
    if (s->box_n >= JS_BOX_CAP) {
        s->box_full = true;
        return;
    }
    lv_area_t g = *a;
    g.x1 -= 1;
    g.y1 -= 1;
    g.x2 += 1;
    g.y2 += 1;
    s->boxes[s->box_n++] = g;
}

static bool js_overlap(const lv_area_t* a, const lv_area_t* b)
{
    return a->x1 <= b->x2 + 1 && b->x1 <= a->x2 + 1 && a->y1 <= b->y2 + 1 && b->y1 <= a->y2 + 1;
}

/* Merge the way LVGL joins its areas, then join the cheapest pairs until at most JAKO_SCENE_INV_MAX are left. */
static void js_flush_boxes(js_t* s)
{
    s->inval_areas = 0;
    s->inval_px = 0;
    lv_area_t screen;
    lv_obj_get_coords(s->obj, &screen);
    if (s->box_full) {
        s->box_n = 0;
        s->box_full = false;
        s->boxes[s->box_n++] = screen;
    }
    int32_t n = 0;
    for (int32_t i = 0; i < s->box_n; i++) {
        lv_area_t a = s->boxes[i];
        a.x1 = js_max(a.x1, screen.x1);
        a.y1 = js_max(a.y1, screen.y1);
        a.x2 = js_min(a.x2, screen.x2);
        a.y2 = js_min(a.y2, screen.y2);
        if (!js_area_is_empty(&a)) s->boxes[n++] = a;
    }
    bool changed = true;
    while (changed) {
        changed = false;
        for (int32_t i = 0; i < n && !changed; i++)
            for (int32_t k = i + 1; k < n && !changed; k++) {
                if (!js_overlap(&s->boxes[i], &s->boxes[k])) continue;
                lv_area_t j = s->boxes[i];
                js_area_join(&j, &s->boxes[k]);
                if (js_area_px(&j) < js_area_px(&s->boxes[i]) + js_area_px(&s->boxes[k])) {
                    s->boxes[i] = j;
                    s->boxes[k] = s->boxes[--n];
                    changed = true;
                }
            }
    }
    while (n > JAKO_SCENE_INV_MAX) {
        int32_t bi = 0, bk = 1, best = INT32_MAX;
        for (int32_t i = 0; i < n; i++)
            for (int32_t k = i + 1; k < n; k++) {
                lv_area_t j = s->boxes[i];
                js_area_join(&j, &s->boxes[k]);
                const int32_t grow = js_area_px(&j) - js_area_px(&s->boxes[i]) - js_area_px(&s->boxes[k]);
                if (grow < best) {
                    best = grow;
                    bi = i;
                    bk = k;
                }
            }
        js_area_join(&s->boxes[bi], &s->boxes[bk]);
        s->boxes[bk] = s->boxes[--n];
    }
    for (int32_t i = 0; i < n; i++) {
        lv_obj_invalidate_area(s->obj, &s->boxes[i]);
        s->inval_px += (uint32_t)js_area_px(&s->boxes[i]);
    }
    s->inval_areas = (uint32_t)n;
    s->box_n = 0;
}

static void js_refresh(js_t* s)
{
    js_ages(s);
    s->view = js_view(s);
    js_compute(s, &s->next);
    const js_frame_t* o = &s->frame;
    const js_frame_t* n = &s->next;
    if (!s->frame_ready) {
        js_box(s, &n->content);
        js_box(s, &n->status.lit);
    } else {
        if (o->version != n->version || o->view != n->view || o->half != n->half) {
            js_box(s, &o->content);
            js_box(s, &n->content);
        } else if (o->pulse_opa != n->pulse_opa) {
            for (int i = 0; i < JS_TILES; i++) js_box(s, &s->tile[i]);
        }
        if (memcmp(&o->status, &n->status, sizeof(o->status)) != 0) {
            js_box(s, &o->status.lit);
            js_box(s, &n->status.lit);
        }
    }
    s->frame = s->next;
    s->frame_ready = 1;
    js_flush_boxes(s);
}

/* ------------------------------------------------------------------ draw */

static bool js_visible(const lv_layer_t* layer, const lv_area_t* a) { return js_area_hits(&layer->_clip_area, a); }

/* Does the clip area reach the ring between r_in and r_out round (cx, cy)? */
static bool js_annulus_visible(const lv_layer_t* layer, int32_t cx, int32_t cy, int32_t r_in, int32_t r_out)
{
    const lv_area_t* c = &layer->_clip_area;
    const int32_t nx = cx < c->x1 ? c->x1 : cx > c->x2 ? c->x2 : cx;
    const int32_t ny = cy < c->y1 ? c->y1 : cy > c->y2 ? c->y2 : cy;
    const int64_t near = (int64_t)(nx - cx) * (nx - cx) + (int64_t)(ny - cy) * (ny - cy);
    const int32_t fx = js_max(js_abs(c->x1 - cx), js_abs(c->x2 - cx)), fy = js_max(js_abs(c->y1 - cy), js_abs(c->y2 - cy));
    const int64_t far = (int64_t)fx * fx + (int64_t)fy * fy;
    return near <= (int64_t)(r_out + 2) * (r_out + 2) && far >= (int64_t)js_max(0, r_in - 2) * js_max(0, r_in - 2);
}

/* Light already mixed with black at opa: drawn opaque, so overlaps never add up. */
static lv_color_t js_mix(int32_t opa)
{
    return lv_color_mix(lv_color_hex(JAKO_SCENE_LIGHT), lv_color_black(), (uint8_t)(opa > 255 ? 255 : opa));
}

static void js_fill(lv_layer_t* layer, lv_obj_t* obj, const lv_area_t* a, int32_t radius, lv_color_t color)
{
    if (!js_visible(layer, a)) return;
    lv_draw_fill_dsc_t d;
    lv_draw_fill_dsc_init(&d);
    d.base.layer = layer;
    d.base.obj = obj;
    d.color = color;
    d.opa = LV_OPA_COVER;
    d.radius = radius;
    lv_draw_fill(layer, &d, a);
}

/* An arc by the scene's angles (from twelve, clockwise); a span of 360 is the whole circle. */
static void js_arc(lv_layer_t* layer, lv_obj_t* obj, int32_t cx, int32_t cy, int32_t radius, int32_t width, int32_t a0,
                   int32_t a1, bool rounded, int32_t opa)
{
    if (!js_annulus_visible(layer, cx, cy, radius - width, radius)) return;
    lv_draw_arc_dsc_t d;
    lv_draw_arc_dsc_init(&d);
    d.base.layer = layer;
    d.base.obj = obj;
    d.color = js_mix(opa);
    d.opa = LV_OPA_COVER;
    d.width = width;
    d.center.x = cx;
    d.center.y = cy;
    d.radius = (uint16_t)radius;
    if (a1 - a0 >= 360) {
        d.start_angle = 0;
        d.end_angle = 360;
    } else {
        d.start_angle = (a0 + 270) % 360;
        d.end_angle = (a1 + 270) % 360;
    }
    d.rounded = rounded ? 1 : 0;
    lv_draw_arc(layer, &d);
}

static void js_line(lv_layer_t* layer, lv_obj_t* obj, int32_t x1, int32_t y1, int32_t x2, int32_t y2, int32_t width,
                    bool round_start, bool round_end, int32_t opa)
{
    const int32_t m = (width + 1) / 2 + 2;
    const lv_area_t box = js_area(js_min(x1, x2) - m, js_min(y1, y2) - m, js_max(x1, x2) + m, js_max(y1, y2) + m);
    if (!js_visible(layer, &box)) return;
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.base.layer = layer;
    d.base.obj = obj;
    d.color = js_mix(opa);
    d.opa = LV_OPA_COVER;
    d.width = width;
    d.p1.x = x1;
    d.p1.y = y1;
    d.p2.x = x2;
    d.p2.y = y2;
    d.round_start = round_start ? 1 : 0;
    d.round_end = round_end ? 1 : 0;
    lv_draw_line(layer, &d);
}

static void js_triangle(lv_layer_t* layer, lv_obj_t* obj, const lv_point_t* p, int32_t opa)
{
    lv_area_t box = js_area(p[0].x, p[0].y, p[0].x, p[0].y);
    for (int i = 1; i < 3; i++) {
        const lv_area_t b = js_area(p[i].x, p[i].y, p[i].x, p[i].y);
        js_area_join(&box, &b);
    }
    box.x1 -= 2;
    box.y1 -= 2;
    box.x2 += 2;
    box.y2 += 2;
    if (!js_visible(layer, &box)) return;
    lv_draw_triangle_dsc_t d;
    lv_draw_triangle_dsc_init(&d);
    d.base.layer = layer;
    d.base.obj = obj;
    d.color = js_mix(opa);
    d.opa = LV_OPA_COVER;
    for (int i = 0; i < 3; i++) {
        d.p[i].x = p[i].x;
        d.p[i].y = p[i].y;
    }
    lv_draw_triangle(layer, &d);
}

/* A plate: black, but never past JAKO_SCENE_PLATE_R, so the state ring's glow under the scene stays whole. */
static void js_plate(lv_layer_t* layer, lv_obj_t* obj, const js_t* s, const lv_area_t* a)
{
    if (js_area_is_empty(a) || !js_visible(layer, a)) return;
    const int32_t cx = s->ox + JAKO_SCENE_CENTRE, cy = s->oy + JAKO_SCENE_CENTRE;
    const int64_t lim = (int64_t)JAKO_SCENE_PLATE_R * JAKO_SCENE_PLATE_R;
    bool inside = true;
    const int32_t xs[2] = {a->x1, a->x2 + 1}, ys[2] = {a->y1, a->y2 + 1};
    for (int i = 0; i < 2; i++)
        for (int k = 0; k < 2; k++) {
            const int64_t dx = xs[i] - cx, dy = ys[k] - cy;
            inside = inside && dx * dx + dy * dy <= lim;
        }
    if (inside) {
        js_fill(layer, obj, a, 0, lv_color_black());
        return;
    }
    for (int32_t y = a->y1; y <= a->y2; y += 4) {
        const int32_t y2 = js_min(y + 3, a->y2);
        const int32_t dy = js_max(js_abs(y - cy), js_abs(y2 + 1 - cy));
        if (dy >= JAKO_SCENE_PLATE_R) continue;
        const int32_t half = (int32_t)js_isqrt((uint64_t)(lim - (int64_t)dy * dy));
        const lv_area_t band = js_area(js_max(a->x1, cx - half), y, js_min(a->x2, cx + half - 1), y2);
        if (!js_area_is_empty(&band)) js_fill(layer, obj, &band, 0, lv_color_black());
    }
}

static void js_text(lv_layer_t* layer, lv_obj_t* obj, const js_text_t* t)
{
    if (!t->shown || t->text[0] == '\0' || !js_visible(layer, &t->lit)) return;
    lv_draw_label_dsc_t d;
    lv_draw_label_dsc_init(&d);
    d.base.layer = layer;
    d.base.obj = obj;
    d.text = t->text;
    d.font = js_font(t->font);
    d.color = lv_color_hex(JAKO_SCENE_LIGHT);
    d.opa = (lv_opa_t)t->opa;
    d.align = LV_TEXT_ALIGN_LEFT;
    d.flag = LV_TEXT_FLAG_EXPAND;
    /* the area is the lit box, so a glyph whose ink starts left of the pen is inside it; the pen keeps its place */
    d.ofs_x = t->pos.x - t->lit.x1;
    lv_area_t a = {t->lit.x1, t->pos.y, t->lit.x2 + 1, t->lit.y2};
    lv_draw_label(layer, &d, &a);
}

static void js_draw_shape(lv_layer_t* layer, lv_obj_t* obj, const js_t* s, uint16_t i, int32_t half)
{
    const jako_scene_shape_t* sh = &s->u.shapes[i];
    const lv_area_t box = js_from16(&s->shape_box[i]);
    if (!js_visible(layer, &box)) return;
    const int32_t ox = s->ox, oy = s->oy;
    if (sh->kind == JAKO_SCENE_KIND_PLATE) {
        const lv_area_t a = js_area(ox + sh->x, oy + sh->y, ox + sh->x + sh->x2 - 1, oy + sh->y + sh->y2 - 1);
        js_plate(layer, obj, s, &a);
        return;
    }
    const int32_t opa = half ? (sh->opa + 1) / 2 : sh->opa;
    if (opa <= LV_OPA_MIN) return;
    switch (sh->kind) {
    case JAKO_SCENE_KIND_CIRCLE: js_arc(layer, obj, ox + sh->x, oy + sh->y, sh->r, sh->w, 0, 360, false, opa); break;
    case JAKO_SCENE_KIND_ARC:
        js_arc(layer, obj, ox + sh->x, oy + sh->y, sh->r, sh->w, sh->a0, sh->a1, sh->cap != 0, opa);
        break;
    case JAKO_SCENE_KIND_DISC: {
        const lv_area_t a = js_area(ox + sh->x - sh->r, oy + sh->y - sh->r, ox + sh->x + sh->r - 1, oy + sh->y + sh->r - 1);
        js_fill(layer, obj, &a, LV_RADIUS_CIRCLE, js_mix(opa));
        break;
    }
    case JAKO_SCENE_KIND_LINE:
        js_line(layer, obj, ox + sh->x, oy + sh->y, ox + sh->x2, oy + sh->y2, sh->w, sh->cap != 0, sh->cap != 0, opa);
        break;
    case JAKO_SCENE_KIND_POLYLINE:
        /* the joints are round whatever the cap, so a bend never shows a notch; cap is for the two ends */
        for (uint16_t k = 0; k + 1 < sh->pn; k++) {
            const jako_scene_point_t* a = &s->u.points[sh->pt + k];
            const jako_scene_point_t* b = &s->u.points[sh->pt + k + 1];
            js_line(layer, obj, ox + a->x, oy + a->y, ox + b->x, oy + b->y, sh->w, k == 0 ? sh->cap != 0 : true,
                    k + 2 == sh->pn ? sh->cap != 0 : true, opa);
        }
        break;
    case JAKO_SCENE_KIND_TRIANGLES:
        for (uint16_t k = 0; k + 2 < sh->pn; k += 3) {
            lv_point_t p[3];
            for (int j = 0; j < 3; j++) {
                p[j].x = ox + s->u.points[sh->pt + k + j].x;
                p[j].y = oy + s->u.points[sh->pt + k + j].y;
            }
            js_triangle(layer, obj, p, opa);
        }
        break;
    case JAKO_SCENE_KIND_RECT: {
        const lv_area_t a = js_area(ox + sh->x, oy + sh->y, ox + sh->x + sh->x2 - 1, oy + sh->y + sh->y2 - 1);
        js_fill(layer, obj, &a, sh->r, js_mix(opa));
        break;
    }
    default: break;
    }
}

static void js_draw_cb(lv_event_t* e)
{
    js_t* s = (js_t*)lv_event_get_user_data(e);
    lv_layer_t* layer = lv_event_get_layer(e);
    if (s == NULL || layer == NULL || !s->frame_ready) return;
    lv_obj_t* obj = s->obj;
    const js_frame_t* f = &s->frame;
    if (f->view == JAKO_SCENE_VIEW_LOADING) {
        if (f->pulse_opa > LV_OPA_MIN)
            js_arc(layer, obj, s->ox + JAKO_SCENE_PULSE_X, s->oy + JAKO_SCENE_PULSE_Y, JAKO_SCENE_PULSE_R,
                   JAKO_SCENE_PULSE_W, 0, 360, false, f->pulse_opa);
    } else {
        /* the scene ends above the status slot: its shapes and texts are clipped at y 371 */
        const lv_area_t keep = layer->_clip_area;
        if (layer->_clip_area.y2 > s->oy + JAKO_SCENE_SLOT_Y - 1) layer->_clip_area.y2 = s->oy + JAKO_SCENE_SLOT_Y - 1;
        if (layer->_clip_area.y2 >= layer->_clip_area.y1) {
            for (uint16_t i = 0; i < s->u.shape_n; i++) js_draw_shape(layer, obj, s, i, f->half);
            for (uint8_t i = 0; i < s->u.text_n; i++) js_text(layer, obj, &s->text[i]);
        }
        layer->_clip_area = keep;
    }
    js_text(layer, obj, &f->status);
}

/* ---------------------------------------------------------------- events */

static void js_setup(js_t* s)
{
    lv_area_t c;
    lv_obj_get_coords(s->obj, &c);
    s->ox = c.x1;
    s->oy = c.y1;
    const int32_t cx = s->ox + JAKO_SCENE_PULSE_X, cy = s->oy + JAKO_SCENE_PULSE_Y, r = JAKO_SCENE_PULSE_R;
    s->ring_box = js_area(cx - r - 1, cy - r - 1, cx + r, cy + r);
    /* eight tiles along the ring, each its octant's ends and middle, inner and outer, plus 3 px */
    for (int k = 0; k < JS_TILES; k++) {
        lv_area_t t;
        js_area_empty(&t);
        for (int j = 0; j < 3; j++) {
            const int32_t i = (k * 2 + j) % 16;
            const int32_t sn = JS_SIN16[i], cs = JS_SIN16[(i + 4) % 16];
            const int32_t radii[2] = {r - JAKO_SCENE_PULSE_W - 1, r + 1};
            for (int q = 0; q < 2; q++) {
                const int32_t x = cx + js_rdiv((int64_t)radii[q] * sn, 16384);
                const int32_t y = cy - js_rdiv((int64_t)radii[q] * cs, 16384);
                const lv_area_t p = js_area(x - 3, y - 3, x + 3, y + 3);
                js_area_join(&t, &p);
            }
        }
        s->tile[k] = t;
    }
    js_layout(s);
}

static void js_size_cb(lv_event_t* e)
{
    js_t* s = (js_t*)lv_event_get_user_data(e);
    if (s == NULL) return;
    js_setup(s);
    s->frame_ready = 0;
    lv_obj_invalidate(s->obj);
}

static void js_delete_cb(lv_event_t* e)
{
    js_t* s = (js_t*)lv_event_get_user_data(e);
    if (s == NULL) return;
    if (s->timer != NULL) lv_timer_delete(s->timer);
    lv_obj_set_user_data(s->obj, NULL);
    s->magic = 0;
    lv_free(s);
}

static void js_timer_cb(lv_timer_t* t)
{
    js_t* s = (js_t*)lv_timer_get_user_data(t);
    const uint32_t elapsed = lv_tick_elaps(s->last_tick);
    s->last_tick = lv_tick_get();
    jako_scene_advance(s->obj, elapsed);
}

/* ------------------------------------------------------------- public API */

lv_obj_t* jako_scene_create(lv_obj_t* parent)
{
    js_t* s = (js_t*)lv_malloc_zeroed(sizeof(js_t));
    if (s == NULL) return NULL;
    lv_obj_t* obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj); /* no style: transparent, so the face's ring shows round it */
    lv_obj_set_size(obj, lv_pct(100), lv_pct(100));
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_CLICK_FOCUSABLE);
    s->magic = JS_MAGIC;
    s->obj = obj;
    s->hold = JAKO_SCENE_HOLD_RUNNING;
    s->u.state = JAKO_SCENE_STATE_LOADING;
    s->u.src = JAKO_SCENE_SRC_SAMPLE;
    s->u.hold = JAKO_SCENE_HOLD_DEFAULT;
    lv_obj_update_layout(obj);
    js_setup(s);
    lv_obj_set_user_data(obj, s);
    lv_obj_add_event_cb(obj, js_draw_cb, LV_EVENT_DRAW_MAIN, s);
    lv_obj_add_event_cb(obj, js_size_cb, LV_EVENT_SIZE_CHANGED, s);
    lv_obj_add_event_cb(obj, js_delete_cb, LV_EVENT_DELETE, s);
    s->last_tick = lv_tick_get();
    s->timer = lv_timer_create(js_timer_cb, JAKO_SCENE_TICK_MS, s);
    js_refresh(s);
    return obj;
}

jako_scene_apply_t jako_scene_apply(lv_obj_t* obj, const jako_scene_update_t* u)
{
    js_t* s = js_get(obj);
    if (s == NULL || u == NULL) return JAKO_SCENE_APPLY_BAD;
    const jako_scene_apply_t r = jako_scene_check(u);
    if (r != JAKO_SCENE_APPLY_SHOWN) {
        JS_LOGI("refused: %s", jako_scene_apply_reason(r));
        return r;
    }
    s->held_ms = s->clock;
    s->hold = JAKO_SCENE_HOLD_RUNNING;
    if (s->updates > 0 && jako_scene_same(&s->u, u)) {
        s->sames++;
        return JAKO_SCENE_APPLY_SAME;
    }
    memcpy(&s->u, u, sizeof(s->u));
    if (s->u.state == JAKO_SCENE_STATE_LOADING) {
        s->u.shape_n = 0;
        s->u.point_n = 0;
        s->u.text_n = 0;
    }
    if (s->u.hold < 1) s->u.hold = JAKO_SCENE_HOLD_DEFAULT;
    s->received_ms = s->clock;
    s->updates++;
    js_layout(s);
    js_refresh(s);
    JS_LOGI("%s: %u shapes, %u texts", s->u.id, (unsigned)s->u.shape_n, (unsigned)s->u.text_n);
    return JAKO_SCENE_APPLY_SHOWN;
}

const char* jako_scene_apply_reason(jako_scene_apply_t r)
{
    switch (r) {
    case JAKO_SCENE_APPLY_SHOWN: return "shown";
    case JAKO_SCENE_APPLY_SAME: return "the same scene";
    case JAKO_SCENE_APPLY_PAST_EDGE: return "past the edge";
    case JAKO_SCENE_APPLY_BAD: return "not a parsed update";
    default: return "unknown";
    }
}

const char* jako_scene_id(lv_obj_t* obj)
{
    js_t* s = js_get(obj);
    return s != NULL ? s->u.id : "";
}

void jako_scene_set_reduced_motion(lv_obj_t* obj, bool on)
{
    js_t* s = js_get(obj);
    if (s != NULL) s->reduced = on;
}

void jako_scene_set_expire_cb(lv_obj_t* obj, jako_scene_expire_cb_t cb, void* user)
{
    js_t* s = js_get(obj);
    if (s == NULL) return;
    s->expire_cb = cb;
    s->expire_user = user;
}

void jako_scene_advance(lv_obj_t* obj, uint32_t ms)
{
    js_t* s = js_get(obj);
    if (s == NULL) return;
    s->clock += ms;
    js_refresh(s);
    if (s->hold == JAKO_SCENE_HOLD_RUNNING && s->clock - s->held_ms >= (uint32_t)s->u.hold * 1000u) {
        s->hold = JAKO_SCENE_HOLD_EXPIRED;
        JS_LOGI("no update for %d s", (int)s->u.hold);
        if (s->expire_cb != NULL) s->expire_cb(obj, s->expire_user); /* may delete the widget: nothing after */
    }
}

void jako_scene_get_debug(lv_obj_t* obj, jako_scene_debug_t* out)
{
    js_t* s = js_get(obj);
    if (s == NULL || out == NULL) return;
    memset(out, 0, sizeof(*out));
    out->clock_ms = s->clock;
    out->since_update_ms = s->clock - s->received_ms;
    out->since_hold_ms = s->clock - s->held_ms;
    out->updates = s->updates;
    out->sames = s->sames;
    out->state = s->u.state;
    out->src = s->u.src;
    out->view = s->view;
    out->hold = s->hold;
    out->age_s = s->age_s;
    out->id = s->u.id;
    out->shape_n = s->u.shape_n;
    out->point_n = s->u.point_n;
    out->text_n = s->u.text_n;
    const js_frame_t* f = &s->frame;
    out->pulse_opa = (uint8_t)f->pulse_opa;
    out->dim_half = (uint8_t)f->half;
    out->status = s->status;
    out->status_opa = (uint8_t)(f->status.shown ? f->status.opa : 0);
    out->status_box = js_box16(&f->status.box);
    out->status_lit = js_box16(&f->status.lit);
    out->content = js_box16(&f->content);
    if (f->status.shown) {
        out->text_px_min = (uint8_t)f->status.font;
        out->text_opa_min = (uint8_t)f->status.opa;
    }
    const bool texts = f->view != JAKO_SCENE_VIEW_LOADING;
    for (uint8_t i = 0; i < s->u.text_n; i++) {
        const js_text_t* t = &s->text[i];
        out->text_w[i] = (int16_t)t->w;
        out->text_box[i] = js_box16(&t->box);
        out->text_lit[i] = js_box16(&t->lit);
        if (!texts) continue;
        if (out->text_px_min == 0 || t->font < out->text_px_min) out->text_px_min = (uint8_t)t->font;
        if (out->text_opa_min == 0 || t->opa < out->text_opa_min) out->text_opa_min = (uint8_t)t->opa;
    }
    out->inval_areas = s->inval_areas;
    out->inval_px = s->inval_px;
}
