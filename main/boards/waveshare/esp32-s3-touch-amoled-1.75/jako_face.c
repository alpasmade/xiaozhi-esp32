// Copyright 2026 Alpas Made. All rights reserved.
/* Jako's face on LVGL 9.5. A line-for-line port of update() and draw() in
   brand/jako-face.js, drawn with the means the Firmware budget in
   DESIGN-TRUTH.md allows: rounded rects for eyes and glow, an eyelid cut
   triangle instead of rotation, A8 images for the hearts, arcs for the
   mouths and the ring, a six-segment comet, and a full ring whose opacity
   changes at most every 66 ms. No canvas, no blur, no rotation.

   Each tick the face computes its integer geometry, compares it with what
   was drawn last, and invalidates only the parts that changed. The draw
   callback only draws that geometry; it never invalidates. */
#include "jako_face.h"
#include "jako_face_port.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* ---- numbers from DESIGN-TRUTH.md section 2 and brand/jako-face.js ---- */
#define JF_UNITS 26.0f
#define JF_HALF 13.0f
#define JF_BLINK 0.26f
#define JF_EYE_X 5.0f
#define JF_MOUTH_Y 4.2f
#define JF_RING_R 11.9f
#define JF_LIGHT 0xF1F0EC
#define JF_PI 3.14159265358979f
#define JF_TWO_PI 6.28318530717959f

/* ---- device-side substitutions, recorded in the Firmware budget ---- */
#define JF_GLOW_OUTER_OPA 26 /* 10 percent, grown 0.8 units a side */
#define JF_GLOW_INNER_OPA 43 /* 17 percent over the outer band, about 25 in all, grown 0.4 units a side */
#define JF_COMET_SEGS 6
#define JF_COMET_LEN_DEG 126 /* 0.7 of a half turn */
#define JF_COMET_SEG_DEG 21
#define JF_COMET_OVERLAP_DEG 1
#define JF_RING_TILES 16
#define JF_RING_STEP_S 0.0655f /* 66 ms between full ring changes, less timer jitter */
#define JF_RING_OPA_STEP 8
#define JF_TICK_MS 33
#define JF_DT_MAX 0.05f
#define JF_BODY_DIZZY_S 2.0f
#define JF_HEART_REF_PX 466
#define JF_MOUTH_ATTACK_S 0.030f
#define JF_MOUTH_RELEASE_S 0.080f
#define JF_WARN_MAX 16
#define JF_WARN_LEN 24
#define JF_MAGIC 0x4A414B4Fu
/* Timers fire within 10 us of zero: single-precision sums of dt land just above
   zero where the engine's doubles land on or below it (2.5 s is 75 frames). */
#define JF_TIMER_EPS 1e-5f

enum { JF_NO_BLINK = 1, JF_HEARTS = 2, JF_WOBBLE = 4 };

typedef struct {
    float w, h, r, tilt;
} jf_eye_spec_t;

typedef struct {
    const char* name;
    jf_eye_spec_t l, r;
    float y;
    jako_face_mouth_t mouth;
    uint8_t flags;
} jf_mood_t;

enum {
    M_NEUTRAL = 0, M_HAPPY, M_CURIOUS, M_SLEEPY, M_SURPRISED,
    M_LOVE, M_DIZZY, M_SAD, M_ANGRY, M_WINK, M_COUNT
};

/* MOODS in jako-face.js:24-35 */
static const jf_mood_t MOODS[M_COUNT] = {
    [M_NEUTRAL] = {"neutral", {5.0f, 7.0f, 1.8f, 0.0f}, {5.0f, 7.0f, 1.8f, 0.0f}, -0.5f, JAKO_FACE_MOUTH_SMILE, 0},
    [M_HAPPY] = {"happy", {5.4f, 3.2f, 1.6f, 0.0f}, {5.4f, 3.2f, 1.6f, 0.0f}, -1.2f, JAKO_FACE_MOUTH_GRIN, 0},
    [M_CURIOUS] = {"curious", {5.0f, 8.0f, 1.8f, 0.0f}, {4.2f, 5.6f, 1.6f, 0.0f}, -0.5f, JAKO_FACE_MOUTH_DOT, 0},
    [M_SLEEPY] = {"sleepy", {5.0f, 1.6f, 0.8f, 0.0f}, {5.0f, 1.6f, 0.8f, 0.0f}, 0.5f, JAKO_FACE_MOUTH_FLAT, JF_NO_BLINK},
    [M_SURPRISED] = {"surprised", {5.6f, 8.6f, 2.4f, 0.0f}, {5.6f, 8.6f, 2.4f, 0.0f}, -0.8f, JAKO_FACE_MOUTH_O, 0},
    [M_LOVE] = {"love", {6.4f, 6.4f, 1.8f, 0.0f}, {6.4f, 6.4f, 1.8f, 0.0f}, -0.6f, JAKO_FACE_MOUTH_SMILE, JF_HEARTS | JF_NO_BLINK},
    [M_DIZZY] = {"dizzy", {4.4f, 4.4f, 2.2f, 0.0f}, {4.4f, 4.4f, 2.2f, 0.0f}, -0.5f, JAKO_FACE_MOUTH_O, JF_WOBBLE},
    [M_SAD] = {"sad", {5.0f, 3.0f, 1.4f, -0.3f}, {5.0f, 3.0f, 1.4f, 0.3f}, 0.8f, JAKO_FACE_MOUTH_FROWN, 0},
    [M_ANGRY] = {"angry", {5.0f, 3.6f, 1.2f, 0.3f}, {5.0f, 3.6f, 1.2f, -0.3f}, -0.6f, JAKO_FACE_MOUTH_FLAT, 0},
    [M_WINK] = {"wink", {5.0f, 7.0f, 1.8f, 0.0f}, {5.0f, 1.4f, 0.7f, 0.0f}, -0.5f, JAKO_FACE_MOUTH_SMILE, JF_NO_BLINK},
};

/* ALIASES in jako-face.js:37-42 */
typedef struct {
    const char* name;
    uint8_t mood;
} jf_alias_t;

static const jf_alias_t ALIASES[] = {
    {"delicious", M_HAPPY}, {"confident", M_HAPPY}, {"laughing", M_HAPPY}, {"funny", M_HAPPY}, {"relaxed", M_HAPPY},
    {"winking", M_WINK}, {"crying", M_SAD}, {"loving", M_LOVE}, {"kissy", M_LOVE}, {"shocked", M_SURPRISED},
    {"embarrassed", M_CURIOUS}, {"confused", M_CURIOUS}, {"cool", M_NEUTRAL}, {"silly", M_WINK}, {"thinking", M_NEUTRAL},
    {"warning", M_SAD}, {"cancel", M_SAD}, {"cloud_off", M_SAD}, {"cloud_download", M_NEUTRAL}, {"download", M_NEUTRAL},
    {"link", M_NEUTRAL}, {"robot_2", M_NEUTRAL},
};

/* STATE_COLOR in jako-face.js:17; -1 is no ring */
static const int32_t STATE_COLOR[] = {
    [JAKO_FACE_STATE_IDLE] = -1,
    [JAKO_FACE_STATE_LISTENING] = 0xEEFF75,
    [JAKO_FACE_STATE_THINKING] = 0x4C6BFF,
    [JAKO_FACE_STATE_SPEAKING] = 0xFF6A2B,
    [JAKO_FACE_STATE_ERROR] = 0xFF6A2B,
};

typedef enum { JF_BODY_NONE = 0, JF_BODY_DIZZY } jf_body_t;
typedef enum { JF_GEOM_NONE = 0, JF_GEOM_READY } jf_geom_state_t;
typedef enum { JF_EYE_NONE = 0, JF_EYE_BLOCK, JF_EYE_HEART } jf_eye_kind_t;
typedef enum { JF_MG_NONE = 0, JF_MG_ARC, JF_MG_RECT } jf_mouth_kind_t;

/* Integer geometry, all int32 so whole structs compare with memcmp. */
typedef struct {
    int32_t kind;
    lv_area_t block;
    int32_t radius;
    int32_t cut;
    lv_point_t tri[3];
    int32_t heart_tag;
    lv_area_t heart_area;
    int32_t heart_scale;
    lv_point_t heart_pivot;
    lv_area_t bounds;
} jf_eye_geom_t;

typedef struct {
    int32_t kind;
    lv_point_t center;
    int32_t radius, width, start, end;
    lv_area_t rect;
    int32_t rect_radius;
    lv_area_t bounds;
} jf_mouth_geom_t;

typedef struct {
    int32_t start, end, width, radius, opa;
    lv_area_t bounds;
} jf_seg_geom_t;

typedef struct {
    int32_t mode, color, opa;
    lv_point_t center;
    int32_t radius, width;
    jf_seg_geom_t seg[JF_COMET_SEGS];
    lv_area_t head;
    int32_t head_opa;
    lv_area_t head_bounds;
} jf_ring_geom_t;

typedef struct {
    int32_t g04, g08;
    jf_eye_geom_t eye[2];
    jf_mouth_geom_t mouth;
    jf_ring_geom_t ring;
} jf_geom_t;

typedef struct {
    uint32_t magic;
    lv_obj_t* obj;
    lv_timer_t* timer;
    uint32_t last_tick;
    jako_face_pre_tick_cb_t pre_cb;
    void* pre_user;
    uint32_t rng;

    /* inputs */
    uint8_t mood_set;
    jako_face_state_t state_set;
    bool rest;
    bool reduced;
    jf_body_t body;
    float body_left;

    /* drawn after precedence */
    uint8_t mood;
    jako_face_state_t state;

    /* the engine's moving parts */
    float t;
    float ph_listen, ph_error, ph_wobble, ph_comet;
    jako_face_box_t cur[2], tgt[2];
    jako_face_mouth_t mouth_kind;
    float open, open_target;
    float blink, next_blink;
    float gx, gy, gtx, gty, gnext, think_side;
    int32_t ring_color;
    float ring_alpha, ring_level;

    /* what the full ring shows, changed at most every 66 ms */
    jako_face_ring_mode_t c_mode;
    int32_t c_color, c_opa;
    float ring_age;
    uint32_t ring_commits;
    float ring_commit_time;

    jf_geom_state_t geom_state;
    jf_geom_t geom, next;
} jf_t;

/* ---------------------------------------------------------------- helpers */

static inline int32_t jf_round(float v) { return (int32_t)floorf(v + 0.5f); }
static inline float jf_minf(float a, float b) { return a < b ? a : b; }
static inline float jf_maxf(float a, float b) { return a > b ? a : b; }
static inline float jf_clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static inline float jf_ease(float c, float t, float k) { return c + (t - c) * k; }

static float jf_smoothstep(float x)
{
    x = jf_clampf(x, 0.0f, 1.0f);
    return x * x * (3.0f - 2.0f * x);
}

static float jf_wrap(float ph)
{
    if (ph >= JF_TWO_PI) ph = fmodf(ph, JF_TWO_PI);
    return ph;
}

static int32_t jf_norm_deg(int32_t a) { return ((a % 360) + 360) % 360; }

static void jf_area_empty(lv_area_t* a)
{
    a->x1 = 0;
    a->y1 = 0;
    a->x2 = -1;
    a->y2 = -1;
}

static bool jf_area_is_empty(const lv_area_t* a) { return a->x2 < a->x1 || a->y2 < a->y1; }

static void jf_area_join(lv_area_t* acc, const lv_area_t* b)
{
    if (jf_area_is_empty(b)) return;
    if (jf_area_is_empty(acc)) {
        *acc = *b;
        return;
    }
    if (b->x1 < acc->x1) acc->x1 = b->x1;
    if (b->y1 < acc->y1) acc->y1 = b->y1;
    if (b->x2 > acc->x2) acc->x2 = b->x2;
    if (b->y2 > acc->y2) acc->y2 = b->y2;
}

static void jf_area_grow(lv_area_t* a, int32_t by)
{
    a->x1 -= by;
    a->y1 -= by;
    a->x2 += by;
    a->y2 += by;
}

/* A pixel area for the canvas rect (x, y, w, h) in px. */
static void jf_rect_px(lv_area_t* a, float x, float y, float w, float h)
{
    a->x1 = jf_round(x);
    a->y1 = jf_round(y);
    a->x2 = jf_round(x + w) - 1;
    a->y2 = jf_round(y + h) - 1;
}

/* Bounding box of an arc stroke: outer radius r_out, width, clockwise from a0 to a1 degrees. */
static void jf_arc_bounds(lv_point_t c, int32_t r_out, int32_t width, int32_t a0, int32_t a1, bool rounded,
                          lv_area_t* out)
{
    float ro = (float)r_out, ri = jf_maxf(0.0f, (float)(r_out - width));
    int32_t span = a1 - a0;
    if (span <= 0) span += 360;
    float minx = 1e9f, miny = 1e9f, maxx = -1e9f, maxy = -1e9f;
    int32_t angles[8];
    int n = 0;
    angles[n++] = a0;
    angles[n++] = a0 + span;
    for (int32_t q = (a0 / 90 + 1) * 90; q < a0 + span && n < 8; q += 90) angles[n++] = q;
    for (int i = 0; i < n; i++) {
        float rad = (float)angles[i] * (JF_PI / 180.0f);
        float cs = cosf(rad), sn = sinf(rad);
        for (int k = 0; k < 2; k++) {
            float r = k ? ro : ri;
            float x = (float)c.x + r * cs, y = (float)c.y + r * sn;
            minx = jf_minf(minx, x);
            maxx = jf_maxf(maxx, x);
            miny = jf_minf(miny, y);
            maxy = jf_maxf(maxy, y);
        }
    }
    int32_t m = rounded ? width / 2 + 2 : 2;
    out->x1 = (int32_t)floorf(minx) - m;
    out->y1 = (int32_t)floorf(miny) - m;
    out->x2 = (int32_t)ceilf(maxx) + m;
    out->y2 = (int32_t)ceilf(maxy) + m;
}

static float jf_rand(jf_t* f)
{
    uint32_t x = f->rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    f->rng = x;
    return (float)(x >> 8) / 16777216.0f;
}

static jf_t* jf_get(lv_obj_t* obj)
{
    if (obj == NULL) return NULL;
    jf_t* f = (jf_t*)lv_obj_get_user_data(obj);
    return (f != NULL && f->magic == JF_MAGIC) ? f : NULL;
}

/* ------------------------------------------------------------ the engine */

static int jf_find_mood(const char* name)
{
    for (int i = 0; i < M_COUNT; i++)
        if (strcmp(MOODS[i].name, name) == 0) return i;
    for (size_t i = 0; i < sizeof(ALIASES) / sizeof(ALIASES[0]); i++)
        if (strcmp(ALIASES[i].name, name) == 0) return ALIASES[i].mood;
    return -1;
}

/* Unknown names are logged once each, like the engine's warned set, in a
   fixed table: once it is full, nothing more is logged. */
static char jf_warned[JF_WARN_MAX][JF_WARN_LEN];
static uint8_t jf_warned_count;

static void jf_warn_once(const char* name)
{
    char key[JF_WARN_LEN];
    size_t n = 0;
    while (n < JF_WARN_LEN - 1 && name[n] != '\0') {
        key[n] = name[n];
        n++;
    }
    key[n] = '\0';
    for (uint8_t i = 0; i < jf_warned_count; i++)
        if (strcmp(jf_warned[i], key) == 0) return;
    if (jf_warned_count >= JF_WARN_MAX) return;
    memcpy(jf_warned[jf_warned_count], key, n + 1);
    jf_warned_count++;
    JAKO_FACE_LOGW("unknown emotion %s, using neutral", key);
}

static bool jf_talking(const jf_t* f) { return f->state == JAKO_FACE_STATE_SPEAKING; }

static bool jf_hearts(const jf_t* f)
{
    return (MOODS[f->mood].flags & JF_HEARTS) && f->state != JAKO_FACE_STATE_ERROR;
}

/* apply() in jako-face.js:89-97 */
static void jf_apply(jf_t* f)
{
    const jf_mood_t* m = &MOODS[f->mood];
    for (int i = 0; i < 2; i++) {
        const jf_eye_spec_t* s = i ? &m->r : &m->l;
        jako_face_box_t* e = &f->tgt[i];
        e->x = i ? JF_EYE_X : -JF_EYE_X;
        e->y = m->y;
        e->w = s->w;
        e->h = s->h;
        e->r = s->r;
        e->tilt = s->tilt;
        switch (f->state) {
        case JAKO_FACE_STATE_LISTENING:
            e->h += 0.8f;
            e->w += 0.4f;
            e->y -= 0.3f;
            break;
        case JAKO_FACE_STATE_THINKING:
            e->h *= 0.8f;
            e->r = jf_minf(e->r, 1.4f);
            break;
        case JAKO_FACE_STATE_SPEAKING:
            e->h *= 0.88f;
            break;
        case JAKO_FACE_STATE_ERROR:
            e->w = 3.4f;
            e->h = 2.2f;
            e->r = 0.9f;
            e->y = 0.2f;
            e->tilt = 0.0f;
            break;
        default:
            break;
        }
    }
    f->mouth_kind = m->mouth;
    if (f->state == JAKO_FACE_STATE_THINKING) f->mouth_kind = JAKO_FACE_MOUTH_FLAT;
    if (f->state == JAKO_FACE_STATE_ERROR) f->mouth_kind = JAKO_FACE_MOUTH_FROWN;
}

/* Precedence, highest first: body event, error, session state, the
   firmware's emotion, the rest state. */
static void jf_resolve(jf_t* f)
{
    uint8_t mood = f->mood_set;
    jako_face_state_t state = f->state_set;
    if (f->body == JF_BODY_DIZZY) {
        mood = M_DIZZY;
        if (state == JAKO_FACE_STATE_ERROR) state = JAKO_FACE_STATE_IDLE;
    } else if (f->rest && state == JAKO_FACE_STATE_IDLE && mood == M_NEUTRAL) {
        mood = M_SLEEPY;
    }
    f->mood = mood;
    /* a new reply starts with the mouth closed, not at the last reply's opening */
    if (state == JAKO_FACE_STATE_SPEAKING && f->state != JAKO_FACE_STATE_SPEAKING) f->open = 0.15f;
    f->state = state;
    jf_apply(f);
}

static float jf_blink_amount(const jf_t* f)
{
    float p = f->blink > 0.0f ? 1.0f - f->blink / JF_BLINK : 0.0f;
    if (p <= 0.0f) return 0.0f;
    return p < 0.35f ? jf_smoothstep(p / 0.35f) : 1.0f - jf_smoothstep((p - 0.35f) / 0.65f);
}

/* update() in jako-face.js:111-142, with the device's mouth and timers */
static void jf_update(jf_t* f, float dt)
{
    if (f->body == JF_BODY_DIZZY) {
        f->body_left -= dt;
        if (f->body_left <= JF_TIMER_EPS) {
            f->body = JF_BODY_NONE;
            jf_resolve(f);
        }
    }
    f->t += dt;
    f->ph_listen = jf_wrap(f->ph_listen + 3.0f * dt);
    f->ph_error = jf_wrap(f->ph_error + 2.5f * dt);
    f->ph_wobble = jf_wrap(f->ph_wobble + 7.0f * dt);
    f->ph_comet = jf_wrap(f->ph_comet + 2.2f * dt);

    const float k = f->reduced ? 1.0f : 1.0f - expf(-dt * 10.0f);
    for (int i = 0; i < 2; i++) {
        jako_face_box_t* c = &f->cur[i];
        const jako_face_box_t* t = &f->tgt[i];
        c->x = jf_ease(c->x, t->x, k);
        c->y = jf_ease(c->y, t->y, k);
        c->w = jf_ease(c->w, t->w, k);
        c->h = jf_ease(c->h, t->h, k);
        c->r = jf_ease(c->r, t->r, k);
        c->tilt = jf_ease(c->tilt, t->tilt, k);
    }
    const jf_mood_t* m = &MOODS[f->mood];

    /* blink: the timer keeps counting in noBlink moods and in error, as in the engine */
    f->next_blink -= dt;
    if (f->next_blink <= JF_TIMER_EPS && f->state != JAKO_FACE_STATE_ERROR && !(m->flags & JF_NO_BLINK)) {
        f->blink = JF_BLINK;
        f->next_blink = jf_rand(f) < 0.15f ? 0.4f : 2.5f + jf_rand(f) * 3.5f;
    }
    if (f->blink > 0.0f) f->blink = jf_maxf(0.0f, f->blink - dt);

    /* glance */
    f->gnext -= dt;
    const bool fired = f->gnext <= JF_TIMER_EPS;
    if (fired) {
        f->gtx = (jf_rand(f) - 0.5f) * 1.6f;
        f->gty = (jf_rand(f) - 0.5f) * 0.8f;
        f->gnext = 1.5f + jf_rand(f) * 3.0f;
        if (jf_rand(f) < 0.4f) {
            f->gtx = 0.0f;
            f->gty = 0.0f;
        }
    }
    float tx = 0.0f, ty = 0.0f, rate = 12.0f;
    if (f->state == JAKO_FACE_STATE_IDLE && !f->reduced) {
        tx = f->gtx;
        ty = f->gty;
    }
    if (f->state == JAKO_FACE_STATE_THINKING) {
        if (fired && !f->reduced) {
            f->think_side = -f->think_side;
            f->gnext = 2.5f + jf_rand(f) * 1.5f;
        }
        tx = 1.7f * f->think_side;
        ty = -2.1f;
        rate = 3.0f;
    }
    if ((m->flags & JF_WOBBLE) && f->state != JAKO_FACE_STATE_ERROR && !f->reduced) {
        tx = cosf(f->ph_wobble) * 0.9f;
        ty = sinf(f->ph_wobble) * 0.6f;
        rate = 30.0f;
    }
    const float gk = f->reduced ? 1.0f : 1.0f - expf(-dt * rate);
    f->gx = jf_ease(f->gx, tx, gk);
    f->gy = jf_ease(f->gy, ty, gk);

    /* ring: fades, dips to nothing on a colour change, eased breath */
    const int32_t accent = STATE_COLOR[f->state];
    if (accent >= 0 && f->ring_color >= 0 && accent != f->ring_color && f->ring_alpha > 0.05f) f->ring_alpha = 0.0f;
    if (accent >= 0) f->ring_color = accent;
    f->ring_alpha = jf_ease(f->ring_alpha, accent >= 0 ? 1.0f : 0.0f, f->reduced ? 1.0f : 1.0f - expf(-dt * 6.0f));
    float base = 1.0f;
    if (f->state == JAKO_FACE_STATE_LISTENING) base = f->reduced ? 1.0f : 0.85f + 0.15f * sinf(f->ph_listen);
    /* reads the previous frame's open, as the engine does */
    if (f->state == JAKO_FACE_STATE_SPEAKING) base = f->reduced ? 0.8f : 0.5f + 0.5f * (f->open - 0.15f) / 0.85f;
    if (f->state == JAKO_FACE_STATE_ERROR) base = f->reduced ? 0.6f : 0.6f + 0.15f * sinf(f->ph_error);
    f->ring_level = jf_ease(f->ring_level, base, f->reduced ? 1.0f : 1.0f - expf(-dt * 10.0f));

    /* mouth: the device follows the audio level, attack 30 ms, release 80 ms */
    if (jf_talking(f)) {
        if (f->reduced) {
            f->open = 0.6f;
        } else if (dt > 0.0f) {
            const float tau = f->open_target > f->open ? JF_MOUTH_ATTACK_S : JF_MOUTH_RELEASE_S;
            f->open += (f->open_target - f->open) * (1.0f - expf(-dt / tau));
        }
    }
}

/* The full ring redraws the whole screen edge, so what it shows (mode,
   colour, opacity in steps of 8) changes at most every 66 ms. */
static void jf_ring_commit(jf_t* f, float dt, bool force)
{
    jako_face_ring_mode_t mode = JAKO_FACE_RING_NONE;
    int32_t opa = 0, color = -1;
    if (f->ring_color >= 0 && f->ring_alpha > 0.01f) {
        opa = jf_round(f->ring_level * f->ring_alpha * 255.0f / (float)JF_RING_OPA_STEP) * JF_RING_OPA_STEP;
        if (opa > 255) opa = 255;
        if (opa > LV_OPA_MIN) {
            mode = f->state == JAKO_FACE_STATE_THINKING ? JAKO_FACE_RING_COMET : JAKO_FACE_RING_FULL;
            color = f->ring_color;
        } else {
            opa = 0;
        }
    }
    f->ring_age += dt;
    const bool changed = mode != f->c_mode || opa != f->c_opa || color != f->c_color;
    if (!changed) return;
    if (!force && f->ring_age < JF_RING_STEP_S) return;
    f->c_mode = mode;
    f->c_opa = opa;
    f->c_color = color;
    f->ring_age = 0.0f;
    f->ring_commits++;
    f->ring_commit_time = f->t;
}

/* --------------------------------------------------------------- geometry */

/* Tilt as an eyelid cut: a black triangle over the top corner on the side
   the tilt lowers, along the rotated block's top edge. Computed from the
   unblinked height and scaled with the blink, then extended past the glow. */
static void jf_eye_cut(jf_eye_geom_t* e, const jako_face_box_t* b, float hd, float cx, float cy, float u, float x0,
                       float y0)
{
    const float th = fabsf(b->tilt), side = b->tilt > 0.0f ? 1.0f : -1.0f;
    const float h = b->h, hw = 0.5f * b->w, sc = hd / h;
    const float p1x = 0.5f * h * tanf(0.5f * th);
    if (p1x >= hw) return;
    float p3y = hw * tanf(th) - h / (2.0f * cosf(th));
    if (p3y > 0.5f * h) p3y = 0.5f * h;
    const float top = -0.5f * hd, p3s = p3y * sc;
    const float m = (p3s - top) / (hw - p1x);
    if (m <= 0.0f) return;
    const float ext = 0.8f + 1.0f / u;
    const float yt = top - ext;
    float xt = p1x - ext / m;
    if (xt < -hw - ext) xt = -hw - ext;
    const float xs = hw + ext;
    float ys = p3s + m * ext;
    if (ys > 0.5f * hd + ext) ys = 0.5f * hd + ext;
    const float px[3] = {xt, xs, xs}, py[3] = {yt, yt, ys};
    for (int i = 0; i < 3; i++) {
        e->tri[i].x = jf_round(x0 + (cx + side * px[i]) * u);
        e->tri[i].y = jf_round(y0 + (cy + py[i]) * u);
    }
    e->cut = 1;
    lv_area_t tb = {e->tri[0].x, e->tri[0].y, e->tri[0].x, e->tri[0].y};
    for (int i = 1; i < 3; i++) {
        if (e->tri[i].x < tb.x1) tb.x1 = e->tri[i].x;
        if (e->tri[i].x > tb.x2) tb.x2 = e->tri[i].x;
        if (e->tri[i].y < tb.y1) tb.y1 = e->tri[i].y;
        if (e->tri[i].y > tb.y2) tb.y2 = e->tri[i].y;
    }
    jf_area_grow(&tb, 1);
    jf_area_join(&e->bounds, &tb);
}

static void jf_eye_geom(const jf_t* f, jf_geom_t* g, int i, float u, float x0, float y0, float blink, bool hearts)
{
    const jako_face_box_t* b = &f->cur[i];
    jf_eye_geom_t* e = &g->eye[i];
    const float cx = JF_HALF + b->x + f->gx, cy = JF_HALF + b->y + f->gy;
    if (hearts) {
        /* chosen by the target width, so an easing width never swaps images mid-way */
        const jako_face_heart_t* h = f->tgt[i].w > 6.6f ? &jako_face_heart_68 : &jako_face_heart_64;
        const int32_t ox = jf_round(x0 + cx * u), oy = jf_round(y0 + (cy - 0.2f) * u);
        const int32_t w = (int32_t)h->img.header.w, hh = (int32_t)h->img.header.h;
        e->kind = JF_EYE_HEART;
        e->heart_tag = h->box_w_tenths;
        e->heart_scale = jf_round(256.0f * u * JF_UNITS / (float)JF_HEART_REF_PX);
        e->heart_pivot.x = h->origin_x;
        e->heart_pivot.y = h->origin_y;
        e->heart_area.x1 = ox - h->origin_x;
        e->heart_area.y1 = oy - h->origin_y;
        e->heart_area.x2 = e->heart_area.x1 + w - 1;
        e->heart_area.y2 = e->heart_area.y1 + hh - 1;
        if (e->heart_scale == 256) {
            e->bounds = e->heart_area;
        } else {
            const int32_t s = e->heart_scale;
            e->bounds.x1 = ox - h->origin_x * s / 256 - 2;
            e->bounds.y1 = oy - h->origin_y * s / 256 - 2;
            e->bounds.x2 = ox + (w - h->origin_x) * s / 256 + 2;
            e->bounds.y2 = oy + (hh - h->origin_y) * s / 256 + 2;
        }
        return;
    }
    const float hd = jf_maxf(0.5f, b->h * (1.0f - blink));
    e->kind = JF_EYE_BLOCK;
    jf_rect_px(&e->block, x0 + (cx - 0.5f * b->w) * u, y0 + (cy - 0.5f * hd) * u, b->w * u, hd * u);
    e->radius = jf_round(jf_minf(b->r, jf_minf(0.5f * b->w, 0.5f * hd)) * u);
    e->bounds = e->block;
    jf_area_grow(&e->bounds, g->g08);
    if (fabsf(b->tilt) > 0.01f) jf_eye_cut(e, b, hd, cx, cy, u, x0, y0);
}

static void jf_mouth_geom(const jf_t* f, jf_geom_t* g, float u, float x0, float y0)
{
    jf_mouth_geom_t* m = &g->mouth;
    const float mx = JF_HALF + f->gx * 0.5f, my = JF_HALF + JF_MOUTH_Y + f->gy * 0.5f;
    const jako_face_mouth_t kind = jf_talking(f) ? JAKO_FACE_MOUTH_TALK : f->mouth_kind;
    /* arcs: centre offset, radius, start and end in degrees (fractions of a half turn x 180) */
    float dy = 0.0f, r = 0.0f;
    int32_t a0 = 0, a1 = 0;
    switch (kind) {
    case JAKO_FACE_MOUTH_SMILE: dy = -1.4f; r = 2.2f; a0 = 40; a1 = 140; break;
    case JAKO_FACE_MOUTH_GRIN: dy = -2.0f; r = 3.2f; a0 = 27; a1 = 153; break;
    case JAKO_FACE_MOUTH_FROWN: dy = 1.8f; r = 2.2f; a0 = 220; a1 = 320; break;
    default: break;
    }
    if (r > 0.0f) {
        m->kind = JF_MG_ARC;
        m->center.x = jf_round(x0 + mx * u);
        m->center.y = jf_round(y0 + (my + dy) * u);
        m->width = jf_round(0.9f * u);
        m->radius = jf_round((r + 0.45f) * u);
        m->start = a0;
        m->end = a1;
        jf_arc_bounds(m->center, m->radius + g->g08, m->width + 2 * g->g08, a0, a1, true, &m->bounds);
        return;
    }
    float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f, rr = 0.0f;
    switch (kind) {
    case JAKO_FACE_MOUTH_DOT: x = mx - 0.6f; y = my - 0.5f; w = 1.2f; h = 1.2f; rr = 0.5f; break;
    case JAKO_FACE_MOUTH_O: x = mx - 1.1f; y = my - 1.3f; w = 2.2f; h = 2.6f; rr = 1.1f; break;
    case JAKO_FACE_MOUTH_FLAT: x = mx - 2.05f; y = my - 0.45f; w = 4.1f; h = 0.9f; rr = 0.45f; break;
    default: {
        const float th = 0.9f + jf_clampf(f->open, 0.0f, 1.0f) * 2.4f;
        x = mx - 1.4f; y = my - 0.5f * th; w = 2.8f; h = th; rr = jf_minf(1.2f, 0.5f * th);
        break;
    }
    }
    m->kind = JF_MG_RECT;
    jf_rect_px(&m->rect, x0 + x * u, y0 + y * u, w * u, h * u);
    m->rect_radius = jf_round(rr * u);
    m->bounds = m->rect;
    jf_area_grow(&m->bounds, g->g08);
}

static void jf_ring_geom(const jf_t* f, jf_geom_t* g, float u, float x0, float y0)
{
    jf_ring_geom_t* r = &g->ring;
    r->mode = f->c_mode;
    r->color = f->c_color;
    r->opa = f->c_opa;
    r->center.x = jf_round(x0 + JF_HALF * u);
    r->center.y = jf_round(y0 + JF_HALF * u);
    r->width = jf_round(0.6f * u);
    r->radius = jf_round(JF_RING_R * u + 0.5f * (float)r->width);
    if (f->c_mode != JAKO_FACE_RING_COMET) return;
    int32_t sweep = f->reduced ? 270 : jf_norm_deg(jf_round(f->ph_comet * (180.0f / JF_PI)));
    for (int i = 0; i < JF_COMET_SEGS; i++) {
        jf_seg_geom_t* s = &r->seg[i];
        const float fr = (float)(i + 1) / (float)JF_COMET_SEGS;
        s->width = jf_round(u * (0.2f + 0.45f * fr));
        s->radius = jf_round(JF_RING_R * u + 0.5f * (float)s->width);
        s->start = jf_norm_deg(sweep - JF_COMET_LEN_DEG + JF_COMET_SEG_DEG * i);
        s->end = jf_norm_deg(s->start + JF_COMET_SEG_DEG + JF_COMET_OVERLAP_DEG);
        s->opa = jf_round((float)f->c_opa * fr * fr);
        jf_arc_bounds(r->center, s->radius, s->width, s->start, s->end, false, &s->bounds);
    }
    const float rad = (float)sweep * (JF_PI / 180.0f), hr = 0.325f * u;
    const float hx = x0 + (JF_HALF + JF_RING_R * cosf(rad)) * u, hy = y0 + (JF_HALF + JF_RING_R * sinf(rad)) * u;
    jf_rect_px(&r->head, hx - hr, hy - hr, 2.0f * hr, 2.0f * hr);
    r->head_opa = f->c_opa;
    r->head_bounds = r->head;
    jf_area_grow(&r->head_bounds, 1);
}

static void jf_compute(const jf_t* f, jf_geom_t* g)
{
    memset(g, 0, sizeof(*g));
    for (int i = 0; i < 2; i++) jf_area_empty(&g->eye[i].bounds);
    jf_area_empty(&g->mouth.bounds);
    for (int i = 0; i < JF_COMET_SEGS; i++) jf_area_empty(&g->ring.seg[i].bounds);
    jf_area_empty(&g->ring.head_bounds);
    lv_area_t c;
    lv_obj_get_coords(f->obj, &c);
    const int32_t w = lv_area_get_width(&c);
    if (w <= 0) return;
    const float u = (float)w / JF_UNITS, x0 = (float)c.x1, y0 = (float)c.y1;
    g->g04 = jf_round(0.4f * u);
    g->g08 = jf_round(0.8f * u);
    const float blink = jf_blink_amount(f);
    const bool hearts = jf_hearts(f);
    for (int i = 0; i < 2; i++) jf_eye_geom(f, g, i, u, x0, y0, blink, hearts);
    jf_mouth_geom(f, g, u, x0, y0);
    jf_ring_geom(f, g, u, x0, y0);
}

/* ------------------------------------------------------------ invalidation */

static void jf_inval(jf_t* f, const lv_area_t* a, const lv_area_t* b)
{
    lv_area_t u;
    jf_area_empty(&u);
    jf_area_join(&u, a);
    jf_area_join(&u, b);
    if (jf_area_is_empty(&u)) return;
    jf_area_grow(&u, 2);
    lv_obj_invalidate_area(f->obj, &u);
}

/* The full ring as 16 tiles, each the box of a 22.5 degree band with its glow. */
static void jf_inval_ring_tiles(jf_t* f, const jf_ring_geom_t* r, int32_t g08)
{
    for (int k = 0; k < JF_RING_TILES; k++) {
        lv_area_t t;
        jf_arc_bounds(r->center, r->radius + g08, r->width + 2 * g08, k * 45 / 2, (k + 1) * 45 / 2, false, &t);
        jf_inval(f, &t, &t);
    }
}

static void jf_invalidate_changes(jf_t* f, const jf_geom_t* o, const jf_geom_t* n)
{
    for (int i = 0; i < 2; i++)
        if (memcmp(&o->eye[i], &n->eye[i], sizeof(o->eye[i])) != 0) jf_inval(f, &o->eye[i].bounds, &n->eye[i].bounds);
    if (memcmp(&o->mouth, &n->mouth, sizeof(o->mouth)) != 0) jf_inval(f, &o->mouth.bounds, &n->mouth.bounds);

    const jf_ring_geom_t *ro = &o->ring, *rn = &n->ring;
    const bool look_changed = ro->mode != rn->mode || ro->opa != rn->opa || ro->color != rn->color;
    const bool moved = ro->radius != rn->radius || ro->width != rn->width || ro->center.x != rn->center.x ||
                       ro->center.y != rn->center.y || o->g08 != n->g08;
    const bool full_old = ro->mode == JAKO_FACE_RING_FULL, full_new = rn->mode == JAKO_FACE_RING_FULL;
    if ((full_old || full_new) && (look_changed || moved)) {
        if (full_new) jf_inval_ring_tiles(f, rn, n->g08);
        if (full_old && (!full_new || moved)) jf_inval_ring_tiles(f, ro, o->g08);
    }
    for (int i = 0; i < JF_COMET_SEGS; i++)
        if (memcmp(&ro->seg[i], &rn->seg[i], sizeof(ro->seg[i])) != 0 || (look_changed && ro->color != rn->color))
            jf_inval(f, &ro->seg[i].bounds, &rn->seg[i].bounds);
    if (memcmp(&ro->head, &rn->head, sizeof(ro->head)) != 0 || ro->head_opa != rn->head_opa ||
        (ro->color != rn->color && (ro->mode == JAKO_FACE_RING_COMET || rn->mode == JAKO_FACE_RING_COMET)))
        jf_inval(f, &ro->head_bounds, &rn->head_bounds);
}

static void jf_refresh_geom(jf_t* f)
{
    jf_compute(f, &f->next);
    if (f->geom_state != JF_GEOM_READY) {
        f->geom = f->next;
        f->geom_state = JF_GEOM_READY;
        lv_obj_invalidate(f->obj);
        return;
    }
    jf_invalidate_changes(f, &f->geom, &f->next);
    f->geom = f->next;
}

/* ------------------------------------------------------------------ draw */

static void jf_fill(lv_layer_t* layer, lv_obj_t* obj, const lv_area_t* a, int32_t radius, lv_color_t color, lv_opa_t opa)
{
    if (opa <= LV_OPA_MIN) return;
    lv_draw_fill_dsc_t d;
    lv_draw_fill_dsc_init(&d);
    d.base.layer = layer;
    d.base.obj = obj;
    d.color = color;
    d.opa = opa;
    d.radius = radius;
    lv_draw_fill(layer, &d, a);
}

/* A part with its glow: grown 0.8 units at 10 percent, 0.4 at 17 (about 25 stacked), then itself. */
static void jf_fill_glow(lv_layer_t* layer, lv_obj_t* obj, const lv_area_t* a, int32_t radius, const jf_geom_t* g,
                         lv_color_t color)
{
    lv_area_t o = *a;
    jf_area_grow(&o, g->g08);
    jf_fill(layer, obj, &o, radius + g->g08, color, JF_GLOW_OUTER_OPA);
    o = *a;
    jf_area_grow(&o, g->g04);
    jf_fill(layer, obj, &o, radius + g->g04, color, JF_GLOW_INNER_OPA);
    jf_fill(layer, obj, a, radius, color, LV_OPA_COVER);
}

static void jf_arc(lv_layer_t* layer, lv_obj_t* obj, lv_point_t c, int32_t radius, int32_t width, int32_t a0,
                   int32_t a1, bool rounded, lv_color_t color, int32_t opa)
{
    if (opa <= LV_OPA_MIN || width <= 0 || radius <= 0) return;
    lv_draw_arc_dsc_t d;
    lv_draw_arc_dsc_init(&d);
    d.base.layer = layer;
    d.base.obj = obj;
    d.color = color;
    d.width = width;
    d.center = c;
    d.radius = (uint16_t)radius;
    d.start_angle = a0;
    d.end_angle = a1;
    d.opa = (lv_opa_t)(opa > 255 ? 255 : opa);
    d.rounded = rounded ? 1 : 0;
    lv_draw_arc(layer, &d);
}

static void jf_arc_glow(lv_layer_t* layer, lv_obj_t* obj, lv_point_t c, int32_t radius, int32_t width, int32_t a0,
                        int32_t a1, bool rounded, const jf_geom_t* g, lv_color_t color, int32_t opa)
{
    jf_arc(layer, obj, c, radius + g->g08, width + 2 * g->g08, a0, a1, rounded, color,
           (opa * JF_GLOW_OUTER_OPA + 127) / 255);
    jf_arc(layer, obj, c, radius + g->g04, width + 2 * g->g04, a0, a1, rounded, color,
           (opa * JF_GLOW_INNER_OPA + 127) / 255);
    jf_arc(layer, obj, c, radius, width, a0, a1, rounded, color, opa);
}

static void jf_draw_eye(lv_layer_t* layer, lv_obj_t* obj, const jf_geom_t* g, const jf_eye_geom_t* e)
{
    const lv_color_t light = lv_color_hex(JF_LIGHT);
    if (e->kind == JF_EYE_HEART) {
        const jako_face_heart_t* h = e->heart_tag == 68 ? &jako_face_heart_68 : &jako_face_heart_64;
        lv_draw_image_dsc_t d;
        lv_draw_image_dsc_init(&d);
        d.base.layer = layer;
        d.base.obj = obj;
        d.src = &h->img;
        d.recolor = light;
        d.recolor_opa = LV_OPA_COVER;
        d.opa = LV_OPA_COVER;
        if (e->heart_scale != 256) {
            d.scale_x = e->heart_scale;
            d.scale_y = e->heart_scale;
            d.pivot = e->heart_pivot;
            d.antialias = 1;
        }
        lv_draw_image(layer, &d, &e->heart_area);
        return;
    }
    if (e->kind != JF_EYE_BLOCK) return;
    jf_fill_glow(layer, obj, &e->block, e->radius, g, light);
    if (e->cut) {
        lv_draw_triangle_dsc_t d;
        lv_draw_triangle_dsc_init(&d);
        d.base.layer = layer;
        d.base.obj = obj;
        d.color = lv_color_black();
        d.opa = LV_OPA_COVER;
        for (int i = 0; i < 3; i++) {
            d.p[i].x = e->tri[i].x;
            d.p[i].y = e->tri[i].y;
        }
        lv_draw_triangle(layer, &d);
    }
}

static void jf_draw_ring(lv_layer_t* layer, lv_obj_t* obj, const jf_geom_t* g)
{
    const jf_ring_geom_t* r = &g->ring;
    if (r->mode == JAKO_FACE_RING_NONE || r->color < 0) return;
    const lv_color_t color = lv_color_hex((uint32_t)r->color);
    if (r->mode == JAKO_FACE_RING_FULL) {
        jf_arc_glow(layer, obj, r->center, r->radius, r->width, 0, 360, false, g, color, r->opa);
        return;
    }
    for (int i = 0; i < JF_COMET_SEGS; i++) {
        const jf_seg_geom_t* s = &r->seg[i];
        jf_arc(layer, obj, r->center, s->radius, s->width, s->start, s->end, false, color, s->opa);
    }
    jf_fill(layer, obj, &r->head, LV_RADIUS_CIRCLE, color, (lv_opa_t)r->head_opa);
}

static void jf_draw_mouth(lv_layer_t* layer, lv_obj_t* obj, const jf_geom_t* g)
{
    const jf_mouth_geom_t* m = &g->mouth;
    const lv_color_t light = lv_color_hex(JF_LIGHT);
    if (m->kind == JF_MG_ARC)
        jf_arc_glow(layer, obj, m->center, m->radius, m->width, m->start, m->end, true, g, light, LV_OPA_COVER);
    else if (m->kind == JF_MG_RECT)
        jf_fill_glow(layer, obj, &m->rect, m->rect_radius, g, light);
}

static void jf_draw_cb(lv_event_t* e)
{
    jf_t* f = (jf_t*)lv_event_get_user_data(e);
    lv_layer_t* layer = lv_event_get_layer(e);
    if (f == NULL || layer == NULL || f->geom_state != JF_GEOM_READY) return;
    const jf_geom_t* g = &f->geom;
    jf_draw_ring(layer, f->obj, g);
    jf_draw_eye(layer, f->obj, g, &g->eye[0]);
    jf_draw_eye(layer, f->obj, g, &g->eye[1]);
    jf_draw_mouth(layer, f->obj, g);
}

/* ---------------------------------------------------------------- events */

static void jf_size_cb(lv_event_t* e)
{
    jf_t* f = (jf_t*)lv_event_get_user_data(e);
    if (f == NULL) return;
    jf_compute(f, &f->geom);
    f->geom_state = JF_GEOM_READY;
    lv_obj_invalidate(f->obj);
}

static void jf_delete_cb(lv_event_t* e)
{
    jf_t* f = (jf_t*)lv_event_get_user_data(e);
    if (f == NULL) return;
    if (f->timer != NULL) lv_timer_delete(f->timer);
    lv_obj_set_user_data(f->obj, NULL);
    f->magic = 0;
    lv_free(f);
}

static void jf_timer_cb(lv_timer_t* t)
{
    jf_t* f = (jf_t*)lv_timer_get_user_data(t);
    const uint32_t elapsed = lv_tick_elaps(f->last_tick);
    f->last_tick = lv_tick_get();
    float dt = (float)elapsed / 1000.0f;
    if (dt > JF_DT_MAX) dt = JF_DT_MAX;
    if (f->pre_cb != NULL) f->pre_cb(f->obj, f->pre_user);
    jako_face_advance(f->obj, dt);
}

/* ------------------------------------------------------------- public API */

lv_obj_t* jako_face_create(lv_obj_t* parent)
{
    jf_t* f = (jf_t*)lv_malloc_zeroed(sizeof(jf_t));
    if (f == NULL) return NULL;
    lv_obj_t* obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_size(obj, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(obj, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_CLICK_FOCUSABLE);

    f->magic = JF_MAGIC;
    f->obj = obj;
    f->rng = 0x2545F491u;
    /* the engine's constructor, jako-face.js:68-77 */
    f->mood_set = M_NEUTRAL;
    f->state_set = JAKO_FACE_STATE_IDLE;
    f->cur[0] = (jako_face_box_t){-JF_EYE_X, -0.5f, 5.0f, 7.0f, 1.8f, 0.0f};
    f->cur[1] = (jako_face_box_t){JF_EYE_X, -0.5f, 5.0f, 7.0f, 1.8f, 0.0f};
    f->open = 0.0f;
    f->open_target = 0.15f;
    f->blink = 0.0f;
    f->next_blink = 2.5f;
    f->gnext = 1.5f;
    f->think_side = 1.0f;
    f->ring_color = -1;
    f->ring_alpha = 0.0f;
    f->ring_level = 1.0f;
    f->c_mode = JAKO_FACE_RING_NONE;
    f->c_color = -1;
    f->c_opa = 0;
    f->ring_age = 1.0f;
    jf_resolve(f);

    lv_obj_set_user_data(obj, f);
    lv_obj_add_event_cb(obj, jf_draw_cb, LV_EVENT_DRAW_MAIN, f);
    lv_obj_add_event_cb(obj, jf_size_cb, LV_EVENT_SIZE_CHANGED, f);
    lv_obj_add_event_cb(obj, jf_delete_cb, LV_EVENT_DELETE, f);
    f->last_tick = lv_tick_get();
    f->timer = lv_timer_create(jf_timer_cb, JF_TICK_MS, f);
    return obj;
}

void jako_face_set_expression(lv_obj_t* obj, const char* name)
{
    jf_t* f = jf_get(obj);
    if (f == NULL) return;
    int m = name != NULL ? jf_find_mood(name) : -1;
    if (m < 0) {
        if (name != NULL && name[0] != '\0') jf_warn_once(name);
        m = M_NEUTRAL;
    }
    f->mood_set = (uint8_t)m;
    jf_resolve(f);
}

void jako_face_set_state(lv_obj_t* obj, jako_face_state_t state)
{
    jf_t* f = jf_get(obj);
    if (f == NULL) return;
    if ((int)state < (int)JAKO_FACE_STATE_IDLE || (int)state > (int)JAKO_FACE_STATE_ERROR) state = JAKO_FACE_STATE_IDLE;
    f->state_set = state;
    jf_resolve(f);
}

void jako_face_set_mouth_level(lv_obj_t* obj, float level01)
{
    jf_t* f = jf_get(obj);
    if (f == NULL) return;
    if (!(level01 >= 0.0f)) level01 = 0.0f; /* also catches NaN */
    f->open_target = 0.15f + 0.85f * jf_clampf(level01, 0.0f, 1.0f);
}

void jako_face_set_reduced_motion(lv_obj_t* obj, bool on)
{
    jf_t* f = jf_get(obj);
    if (f != NULL) f->reduced = on;
}

void jako_face_set_rest(lv_obj_t* obj, bool on)
{
    jf_t* f = jf_get(obj);
    if (f == NULL) return;
    f->rest = on;
    jf_resolve(f);
}

void jako_face_body_event(lv_obj_t* obj, jako_face_body_event_t event)
{
    jf_t* f = jf_get(obj);
    if (f == NULL) return;
    if (event == JAKO_FACE_BODY_DIZZY) {
        f->body = JF_BODY_DIZZY;
        f->body_left = JF_BODY_DIZZY_S;
        jf_resolve(f);
    }
}

/* snap() in jako-face.js:99-106 */
void jako_face_snap(lv_obj_t* obj)
{
    jf_t* f = jf_get(obj);
    if (f == NULL) return;
    f->cur[0] = f->tgt[0];
    f->cur[1] = f->tgt[1];
    f->blink = 0.0f;
    const bool thinking = f->state == JAKO_FACE_STATE_THINKING;
    f->gx = thinking ? 1.7f * f->think_side : 0.0f;
    f->gy = thinking ? -2.1f : 0.0f;
    const int32_t accent = STATE_COLOR[f->state];
    if (accent >= 0) f->ring_color = accent;
    f->ring_alpha = accent >= 0 ? 1.0f : 0.0f;
    f->ring_level = 1.0f;
    if (jf_talking(f)) f->open = 0.6f;
    lv_obj_update_layout(obj);
    jf_ring_commit(f, 0.0f, true);
    jf_refresh_geom(f);
}

void jako_face_set_seed(lv_obj_t* obj, uint32_t seed)
{
    jf_t* f = jf_get(obj);
    if (f != NULL) f->rng = seed != 0 ? seed : 1u;
}

void jako_face_set_pre_tick(lv_obj_t* obj, jako_face_pre_tick_cb_t cb, void* user)
{
    jf_t* f = jf_get(obj);
    if (f == NULL) return;
    f->pre_cb = cb;
    f->pre_user = user;
}

void jako_face_advance(lv_obj_t* obj, float dt)
{
    jf_t* f = jf_get(obj);
    if (f == NULL) return;
    if (!(dt >= 0.0f)) dt = 0.0f;
    lv_obj_update_layout(obj);
    jf_update(f, dt);
    jf_ring_commit(f, dt, false);
    jf_refresh_geom(f);
}

void jako_face_get_debug(lv_obj_t* obj, jako_face_debug_t* out)
{
    jf_t* f = jf_get(obj);
    if (f == NULL || out == NULL) return;
    memset(out, 0, sizeof(*out));
    out->expression = MOODS[f->mood].name;
    out->state = f->state;
    out->left = f->cur[0];
    out->right = f->cur[1];
    out->glance_x = f->gx;
    out->glance_y = f->gy;
    out->blink = jf_blink_amount(f);
    out->next_blink = f->next_blink;
    out->ring_color = f->ring_color;
    out->ring_alpha = f->ring_alpha;
    out->ring_level = f->ring_level;
    out->ring_mode = f->c_mode;
    out->ring_opa = (uint8_t)f->c_opa;
    out->ring_commits = f->ring_commits;
    out->ring_commit_time = f->ring_commit_time;
    out->mouth = jf_talking(f) ? JAKO_FACE_MOUTH_TALK : f->mouth_kind;
    out->mouth_open = f->open;
    out->hearts = jf_hearts(f);
    out->time = f->t;
}

void jako_face_debug_set_blink(lv_obj_t* obj, float progress)
{
    jf_t* f = jf_get(obj);
    if (f == NULL) return;
    f->blink = progress > 0.0f ? JF_BLINK * (1.0f - jf_clampf(progress, 0.0f, 1.0f)) : 0.0f;
    jf_refresh_geom(f);
}
