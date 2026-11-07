// Copyright 2026 Alpas Made. All rights reserved.
/* A scene's wire data, version 1. See jako_scene_data.h for the rules.

   Nothing here trusts a length or a type from the wire. One pass over the
   bytes before cJSON measures the nesting and counts the values (so
   cJSON's recursion stays three deep and its tree small on the main
   task), and finds the raw bytes of the top level "sh" string. sh is read
   from those bytes, not from cJSON's copy, and the two must be equal, so
   an escape anywhere in sh (a "\u0000" included) is a bad shape. Every
   check runs before *out is touched: the texts and the shapes are first
   checked with nothing written, then written in a second pass. */
#include "jako_scene_data.h"

#include <string.h>

#include "cJSON.h"

#define JSD_SEQ_MAX 2147483647
#define JSD_AGE_MAX 86400
#define JSD_STALE_MIN 60
#define JSD_HOLD_MIN 10
#define JSD_HOLD_MAX 600
#define JSD_XY_MAX 465
#define JSD_SCREEN 466
#define JSD_DIGITS 5 /* 99999 is past every range */

static int32_t jsd_clamp(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }

/* ------------------------------------------------------------ the pre-pass */

typedef struct {
    int depth;            /* the deepest nesting seen */
    int values;           /* 1 + commas + opening brackets: at least the number of JSON values */
    bool closed;          /* every string closed */
    bool sh_found;        /* the first top level key spelled exactly "sh" with a string value */
    size_t sh_at, sh_len; /* its raw bytes, between the quotes */
} jsd_pre_t;

static void jsd_prescan(const char* s, size_t n, jsd_pre_t* p)
{
    memset(p, 0, sizeof(*p));
    p->values = 1;
    int depth = 0;
    bool in_str = false, esc = false;
    /* at depth 1: is the next string a key, and was the last key "sh"? */
    bool expect_key = false, last_key_sh = false, sh_seen = false;
    bool str_is_key = false, str_is_sh_value = false;
    size_t str_at = 0;
    for (size_t i = 0; i < n; i++) {
        const char ch = s[i];
        if (in_str) {
            if (esc) {
                esc = false;
            } else if (ch == '\\') {
                esc = true;
            } else if (ch == '"') {
                in_str = false;
                if (str_is_key) {
                    last_key_sh = i - str_at == 2 && s[str_at] == 's' && s[str_at + 1] == 'h';
                } else if (str_is_sh_value) {
                    p->sh_found = true;
                    p->sh_at = str_at;
                    p->sh_len = i - str_at;
                }
            }
            continue;
        }
        if (ch == '"') {
            in_str = true;
            str_at = i + 1;
            str_is_key = depth == 1 && expect_key;
            str_is_sh_value = depth == 1 && !expect_key && last_key_sh && !sh_seen;
            if (str_is_key) expect_key = false;
            if (str_is_sh_value) sh_seen = true;
        } else if (ch == '{' || ch == '[') {
            p->values++;
            if (++depth > p->depth) p->depth = depth;
            if (p->depth > JAKO_SCENE_MAX_DEPTH) return;
            if (depth == 1) expect_key = ch == '{';
            if (depth == 2 && last_key_sh) sh_seen = true; /* the first "sh" holds no string: none is read */
        } else if (ch == '}' || ch == ']') {
            if (depth > 0) depth--;
        } else if (ch == ',') {
            p->values++;
            if (depth == 1) {
                expect_key = true;
                if (last_key_sh) sh_seen = true;
                last_key_sh = false;
            }
        }
    }
    p->closed = !in_str;
}

/* ------------------------------------------------------------ the envelope */

/* A JSON number that is a whole number from lo to hi. */
static bool jsd_whole(const cJSON* item, int32_t lo, int32_t hi, int32_t* out)
{
    if (!cJSON_IsNumber(item)) return false;
    const double d = item->valuedouble;
    if (!(d >= (double)lo && d <= (double)hi)) return false; /* NaN fails too */
    const int32_t v = (int32_t)d;
    if ((double)v != d) return false;
    *out = v;
    return true;
}

static bool jsd_int(const cJSON* obj, const char* key, int32_t* out)
{
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!cJSON_IsNumber(item)) return false;
    *out = (int32_t)item->valueint; /* cJSON saturates to INT_MIN..INT_MAX and truncates fractions */
    return true;
}

static int32_t jsd_int_or(const cJSON* obj, const char* key, int32_t fallback, int32_t lo, int32_t hi)
{
    int32_t v;
    return jsd_int(obj, key, &v) ? jsd_clamp(v, lo, hi) : fallback;
}

/* Copy a JSON string keeping only bytes 0x20..0x7E, at most cap of them; all spaces comes out empty. */
static void jsd_text(const cJSON* obj, const char* key, char* dst, size_t cap)
{
    dst[0] = '\0';
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!cJSON_IsString(item) || item->valuestring == NULL) return;
    size_t n = 0;
    bool blank = true;
    for (const unsigned char* p = (const unsigned char*)item->valuestring; *p != '\0' && n < cap; p++) {
        if (*p < 0x20 || *p > 0x7E) continue;
        blank = blank && *p == ' ';
        dst[n++] = (char)*p;
    }
    dst[blank ? 0 : n] = '\0';
}

static bool jsd_rest_is_space(const char* p, const char* end)
{
    for (; p < end; p++)
        if (*p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') return false;
    return true;
}

static bool jsd_id_ok(const char* s)
{
    size_t n = 0;
    for (; s[n] != '\0'; n++) {
        const char c = s[n];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return false;
        if (n >= JAKO_SCENE_ID_MAX) return false;
    }
    return n >= 1;
}

/* One text of tx, checked; written to *out when out is not NULL. */
static bool jsd_one_text(const cJSON* item, jako_scene_text_t* out)
{
    if (!cJSON_IsArray(item) || cJSON_GetArraySize(item) != 6) return false;
    int32_t v[5];
    static const int32_t lo[5] = {0, 0, 20, 0, 0}, hi[5] = {JSD_XY_MAX, JSD_XY_MAX, 22, 255, 2};
    for (int i = 0; i < 5; i++)
        if (!jsd_whole(cJSON_GetArrayItem(item, i), lo[i], hi[i], &v[i])) return false;
    if (v[2] != 20 && v[2] != 22) return false;
    const cJSON* t = cJSON_GetArrayItem(item, 5);
    if (!cJSON_IsString(t) || t->valuestring == NULL) return false;
    size_t n = 0;
    for (const unsigned char* p = (const unsigned char*)t->valuestring; *p != '\0'; p++, n++)
        if (*p < 0x20 || *p > 0x7E || n >= JAKO_SCENE_TEXT_MAX) return false;
    if (n == 0) return false;
    if (out != NULL) {
        out->x = (int16_t)v[0];
        out->y = (int16_t)v[1];
        out->size = (uint8_t)v[2];
        out->opa = (uint8_t)(v[3] < JAKO_SCENE_TEXT_OPA_MIN ? JAKO_SCENE_TEXT_OPA_MIN : v[3]);
        out->align = (uint8_t)v[4];
        memcpy(out->text, t->valuestring, n + 1);
    }
    return true;
}

/* tx: absent is none; otherwise a list of at most 16 good texts. */
static bool jsd_texts(const cJSON* root, jako_scene_update_t* out)
{
    const cJSON* tx = cJSON_GetObjectItemCaseSensitive(root, "tx");
    if (tx == NULL) return true;
    if (!cJSON_IsArray(tx) || cJSON_GetArraySize(tx) > JAKO_SCENE_MAX_TEXTS) return false;
    uint8_t n = 0;
    const cJSON* item = NULL;
    cJSON_ArrayForEach(item, tx)
    {
        if (!jsd_one_text(item, out != NULL ? &out->texts[n] : NULL)) return false;
        n++;
    }
    if (out != NULL) out->text_n = n;
    return true;
}

/* -------------------------------------------------------------- the shapes */

/* Is anchor (x, y) with reach inside the edge? */
static jako_scene_parse_result_t jsd_anchor(int32_t x, int32_t y, int32_t reach)
{
    const int32_t dx = x - JAKO_SCENE_CENTRE, dy = y - JAKO_SCENE_CENTRE, room = JAKO_SCENE_EDGE_R - reach;
    if (room < 0 || dx * dx + dy * dy > room * room) return JAKO_SCENE_PARSE_PAST_EDGE;
    return JAKO_SCENE_PARSE_OK;
}

static int32_t jsd_stroke_reach(int32_t w) { return (w + 1) / 2 + 1; }

/* The integers a fixed kind takes, or 0 for p and g. */
static int jsd_fixed_count(char k)
{
    switch (k) {
    case JAKO_SCENE_KIND_CIRCLE: return 5;
    case JAKO_SCENE_KIND_ARC: return 8;
    case JAKO_SCENE_KIND_DISC: return 4;
    case JAKO_SCENE_KIND_LINE: return 7;
    case JAKO_SCENE_KIND_RECT: return 6;
    case JAKO_SCENE_KIND_PLATE: return 4;
    default: return 0;
    }
}

static bool jsd_in(int32_t v, int32_t lo, int32_t hi) { return v >= lo && v <= hi; }

/* A fixed kind's integers: ranges, then the edge. */
static jako_scene_parse_result_t jsd_fixed(char k, const int32_t* v, jako_scene_shape_t* sh)
{
    const jako_scene_parse_result_t bad = JAKO_SCENE_PARSE_BAD_SHAPE;
    jako_scene_parse_result_t r = JAKO_SCENE_PARSE_OK;
    memset(sh, 0, sizeof(*sh));
    sh->kind = (uint8_t)k;
    switch (k) {
    case JAKO_SCENE_KIND_CIRCLE:
    case JAKO_SCENE_KIND_ARC: {
        const bool arc = k == JAKO_SCENE_KIND_ARC;
        if (!jsd_in(v[0], 0, JSD_XY_MAX) || !jsd_in(v[1], 0, JSD_XY_MAX) || !jsd_in(v[2], 1, JAKO_SCENE_EDGE_R) ||
            !jsd_in(v[3], 1, 64) || v[3] > v[2])
            return bad;
        const int32_t opa = arc ? v[6] : v[4];
        if (!jsd_in(opa, 0, 255)) return bad;
        if (arc && (!jsd_in(v[4], 0, 359) || !jsd_in(v[5], v[4] + 1, v[4] + 360) || !jsd_in(v[7], 0, 1))) return bad;
        sh->x = (int16_t)v[0];
        sh->y = (int16_t)v[1];
        sh->r = (int16_t)v[2];
        sh->w = (uint8_t)v[3];
        sh->opa = (uint8_t)opa;
        if (arc) {
            sh->a0 = (int16_t)v[4];
            sh->a1 = (int16_t)v[5];
            sh->cap = (uint8_t)v[7];
        }
        r = jsd_anchor(v[0], v[1], v[2] + 1);
        break;
    }
    case JAKO_SCENE_KIND_DISC:
        if (!jsd_in(v[0], 0, JSD_XY_MAX) || !jsd_in(v[1], 0, JSD_XY_MAX) || !jsd_in(v[2], 1, 64) ||
            !jsd_in(v[3], 0, 255))
            return bad;
        sh->x = (int16_t)v[0];
        sh->y = (int16_t)v[1];
        sh->r = (int16_t)v[2];
        sh->opa = (uint8_t)v[3];
        r = jsd_anchor(v[0], v[1], v[2] + 1);
        break;
    case JAKO_SCENE_KIND_LINE:
        for (int i = 0; i < 4; i++)
            if (!jsd_in(v[i], 0, JSD_XY_MAX)) return bad;
        if (!jsd_in(v[4], 1, 32) || !jsd_in(v[5], 0, 255) || !jsd_in(v[6], 0, 1)) return bad;
        sh->x = (int16_t)v[0];
        sh->y = (int16_t)v[1];
        sh->x2 = (int16_t)v[2];
        sh->y2 = (int16_t)v[3];
        sh->w = (uint8_t)v[4];
        sh->opa = (uint8_t)v[5];
        sh->cap = (uint8_t)v[6];
        r = jsd_anchor(v[0], v[1], jsd_stroke_reach(v[4]));
        if (r == JAKO_SCENE_PARSE_OK) r = jsd_anchor(v[2], v[3], jsd_stroke_reach(v[4]));
        break;
    case JAKO_SCENE_KIND_RECT:
    case JAKO_SCENE_KIND_PLATE: {
        const bool rect = k == JAKO_SCENE_KIND_RECT;
        if (!jsd_in(v[0], 0, JSD_XY_MAX) || !jsd_in(v[1], 0, JSD_XY_MAX) || !jsd_in(v[2], 1, JSD_SCREEN - v[0]) ||
            !jsd_in(v[3], 1, JSD_SCREEN - v[1]))
            return bad;
        if (rect && (!jsd_in(v[4], 0, JAKO_SCENE_CENTRE) || !jsd_in(v[5], 0, 255))) return bad;
        sh->x = (int16_t)v[0];
        sh->y = (int16_t)v[1];
        sh->x2 = (int16_t)v[2];
        sh->y2 = (int16_t)v[3];
        sh->r = rect ? (int16_t)v[4] : 0;
        sh->opa = rect ? (uint8_t)v[5] : 255;
        const int32_t xs[2] = {v[0], v[0] + v[2] - 1}, ys[2] = {v[1], v[1] + v[3] - 1};
        for (int i = 0; i < 2 && r == JAKO_SCENE_PARSE_OK; i++)
            for (int j = 0; j < 2 && r == JAKO_SCENE_PARSE_OK; j++) r = jsd_anchor(xs[i], ys[j], 0);
        break;
    }
    default: return bad;
    }
    return r;
}

/* Read sh: the shapes, checked; written to *out when out is not NULL. */
static jako_scene_parse_result_t jsd_shapes(const char* s, size_t n, jako_scene_update_t* out)
{
    const jako_scene_parse_result_t bad = JAKO_SCENE_PARSE_BAD_SHAPE;
    if (n > JAKO_SCENE_SH_MAX) return bad;
    uint32_t shapes = 0, points = 0;
    size_t i = 0;
    while (i < n) {
        const char k = s[i++];
        const int fixed = jsd_fixed_count(k);
        if (fixed == 0 && k != JAKO_SCENE_KIND_POLYLINE && k != JAKO_SCENE_KIND_TRIANGLES) return bad;
        if (shapes >= JAKO_SCENE_MAX_SHAPES) return bad;
        int32_t head[8] = {0};
        int count = 0;          /* integers read */
        int32_t reach = 0;      /* p g: for each point */
        int32_t px = 0;         /* p g: the x waiting for its y */
        uint32_t first = points; /* p g: the shape's first point */
        const int head_n = k == JAKO_SCENE_KIND_POLYLINE ? 3 : k == JAKO_SCENE_KIND_TRIANGLES ? 1 : fixed;
        while (i < n && s[i] == ' ') {
            i++;
            int32_t v = 0;
            int digits = 0;
            while (i < n && s[i] >= '0' && s[i] <= '9') {
                if (++digits > JSD_DIGITS) return bad;
                v = v * 10 + (s[i] - '0');
                i++;
            }
            if (digits == 0) return bad; /* two spaces, a space at the end, or a sign */
            if (count < head_n) {
                head[count++] = v;
                if (count == head_n && fixed == 0) {
                    /* the header of p or g */
                    if (k == JAKO_SCENE_KIND_POLYLINE) {
                        if (!jsd_in(head[0], 1, 32) || !jsd_in(head[1], 0, 255) || !jsd_in(head[2], 0, 1)) return bad;
                        reach = jsd_stroke_reach(head[0]);
                    } else {
                        if (!jsd_in(head[0], 0, 255)) return bad;
                        reach = 1;
                    }
                }
                continue;
            }
            if (fixed != 0) return bad; /* too many integers for the kind */
            /* a coordinate of p or g */
            if (!jsd_in(v, 0, JSD_XY_MAX)) return bad;
            if (((count - head_n) & 1) == 0) {
                px = v;
            } else {
                if (points >= JAKO_SCENE_MAX_POINTS) return bad;
                const uint32_t in_shape = points - first + 1;
                if (k == JAKO_SCENE_KIND_POLYLINE ? in_shape > JAKO_SCENE_MAX_POLY : in_shape > JAKO_SCENE_MAX_TRIS * 3)
                    return bad;
                const jako_scene_parse_result_t r = jsd_anchor(px, v, reach);
                if (r != JAKO_SCENE_PARSE_OK) return r;
                if (out != NULL) {
                    out->points[points].x = (int16_t)px;
                    out->points[points].y = (int16_t)v;
                }
                points++;
            }
            count++;
        }
        /* the shape ends at the end of sh or at ";", which must be followed by another shape */
        if (i < n) {
            if (s[i] != ';') return bad;
            i++;
            if (i >= n) return bad;
        }
        jako_scene_shape_t shape;
        if (fixed != 0) {
            if (count != fixed) return bad;
            const jako_scene_parse_result_t r = jsd_fixed(k, head, &shape);
            if (r != JAKO_SCENE_PARSE_OK) return r;
        } else {
            if (count < head_n || ((count - head_n) & 1) != 0) return bad;
            const uint32_t pn = points - first;
            if (k == JAKO_SCENE_KIND_POLYLINE ? pn < 2 : (pn < 3 || pn % 3 != 0)) return bad;
            memset(&shape, 0, sizeof(shape));
            shape.kind = (uint8_t)k;
            shape.pt = (uint16_t)first;
            shape.pn = (uint16_t)pn;
            if (k == JAKO_SCENE_KIND_POLYLINE) {
                shape.w = (uint8_t)head[0];
                shape.opa = (uint8_t)head[1];
                shape.cap = (uint8_t)head[2];
            } else {
                shape.opa = (uint8_t)head[0];
            }
        }
        if (out != NULL) out->shapes[shapes] = shape;
        shapes++;
    }
    if (out != NULL) {
        out->shape_n = (uint16_t)shapes;
        out->point_n = (uint16_t)points;
    }
    return JAKO_SCENE_PARSE_OK;
}

/* ------------------------------------------------------------------ parse */

jako_scene_parse_result_t jako_scene_parse(const char* data, size_t len, jako_scene_update_t* out)
{
    if (data == NULL || out == NULL || len == 0 || len > JAKO_SCENE_DATA_MAX) return JAKO_SCENE_PARSE_EMPTY;
    jsd_pre_t pre;
    jsd_prescan(data, len, &pre);
    if (pre.depth > JAKO_SCENE_MAX_DEPTH) return JAKO_SCENE_PARSE_TOO_DEEP;
    if (!pre.closed) return JAKO_SCENE_PARSE_NOT_JSON;
    if (pre.values > JAKO_SCENE_MAX_VALUES) return JAKO_SCENE_PARSE_TOO_MANY;

    const char* end = NULL;
    cJSON* root = cJSON_ParseWithLengthOpts(data, len, &end, false);
    if (root == NULL) return JAKO_SCENE_PARSE_NOT_JSON; /* bad text, or cJSON could not allocate */
    jako_scene_parse_result_t r = JAKO_SCENE_PARSE_OK;
    int32_t seq = 0;
    const cJSON* vi = cJSON_GetObjectItemCaseSensitive(root, "v");
    const cJSON* id = cJSON_GetObjectItemCaseSensitive(root, "id");
    const cJSON* st = cJSON_GetObjectItemCaseSensitive(root, "state");
    const cJSON* sr = cJSON_GetObjectItemCaseSensitive(root, "src");
    const cJSON* sh = cJSON_GetObjectItemCaseSensitive(root, "sh");
    jako_scene_state_t state = JAKO_SCENE_STATE_LOADING;
    jako_scene_src_t src = JAKO_SCENE_SRC_SAMPLE;
    if (!cJSON_IsObject(root) || end == NULL || !jsd_rest_is_space(end, data + len)) {
        r = JAKO_SCENE_PARSE_NOT_JSON;
    } else if (!cJSON_IsNumber(vi) || !jsd_int(root, "seq", &seq) || seq < 1 || !cJSON_IsString(id) ||
               id->valuestring == NULL || !cJSON_IsString(st) || st->valuestring == NULL || !cJSON_IsString(sr) ||
               sr->valuestring == NULL) {
        r = JAKO_SCENE_PARSE_MISSING;
    } else if (vi->valuedouble != 1.0) {
        r = JAKO_SCENE_PARSE_VERSION;
    } else if (!jsd_id_ok(id->valuestring)) {
        r = JAKO_SCENE_PARSE_ID;
    } else if (strcmp(st->valuestring, "loading") == 0) {
        state = JAKO_SCENE_STATE_LOADING;
    } else if (strcmp(st->valuestring, "ready") == 0) {
        state = JAKO_SCENE_STATE_READY;
    } else if (strcmp(st->valuestring, "empty") == 0) {
        state = JAKO_SCENE_STATE_EMPTY;
    } else if (strcmp(st->valuestring, "error") == 0) {
        state = JAKO_SCENE_STATE_ERROR;
    } else {
        r = JAKO_SCENE_PARSE_STATE;
    }
    if (r == JAKO_SCENE_PARSE_OK) {
        if (strcmp(sr->valuestring, "sample") == 0)
            src = JAKO_SCENE_SRC_SAMPLE;
        else if (strcmp(sr->valuestring, "desk") == 0)
            src = JAKO_SCENE_SRC_DESK;
        else
            r = JAKO_SCENE_PARSE_SRC;
    }
    /* the picture: checked whole before anything is written */
    const bool drawn = state != JAKO_SCENE_STATE_LOADING;
    const char* sh_raw = NULL;
    size_t sh_len = 0;
    if (r == JAKO_SCENE_PARSE_OK && drawn) {
        if (!jsd_texts(root, NULL)) r = JAKO_SCENE_PARSE_BAD_TEXT;
    }
    if (r == JAKO_SCENE_PARSE_OK && drawn && sh != NULL) {
        /* the raw bytes the pre-pass found must be the very string cJSON read: no escape, no NUL */
        if (!cJSON_IsString(sh) || sh->valuestring == NULL || !pre.sh_found ||
            strlen(sh->valuestring) != pre.sh_len || memcmp(sh->valuestring, data + pre.sh_at, pre.sh_len) != 0) {
            r = JAKO_SCENE_PARSE_BAD_SHAPE;
        } else {
            sh_raw = data + pre.sh_at;
            sh_len = pre.sh_len;
            r = jsd_shapes(sh_raw, sh_len, NULL);
        }
    }
    if (r != JAKO_SCENE_PARSE_OK) {
        cJSON_Delete(root);
        return r;
    }

    /* From here nothing can fail: *out is filled. */
    memset(out, 0, sizeof(*out));
    out->seq = jsd_clamp(seq, 1, JSD_SEQ_MAX);
    out->state = state;
    out->src = src;
    out->age = jsd_int_or(root, "age", 0, 0, JSD_AGE_MAX);
    const int32_t stale = jsd_int_or(root, "stale", 0, 0, JSD_AGE_MAX);
    out->stale = stale <= 0 ? 0 : jsd_clamp(stale, JSD_STALE_MIN, JSD_AGE_MAX);
    out->hold = jsd_int_or(root, "hold", JAKO_SCENE_HOLD_DEFAULT, JSD_HOLD_MIN, JSD_HOLD_MAX);
    memcpy(out->id, id->valuestring, strlen(id->valuestring) + 1);
    jsd_text(root, "st", out->st, JAKO_SCENE_ST_MAX);
    if (drawn) {
        jsd_texts(root, out);
        if (sh_raw != NULL) jsd_shapes(sh_raw, sh_len, out);
    }
    cJSON_Delete(root);
    return JAKO_SCENE_PARSE_OK;
}

const char* jako_scene_parse_reason(jako_scene_parse_result_t r)
{
    switch (r) {
    case JAKO_SCENE_PARSE_OK: return "ok";
    case JAKO_SCENE_PARSE_EMPTY: return "no data or over 8192 bytes";
    case JAKO_SCENE_PARSE_TOO_DEEP: return "nested too deep";
    case JAKO_SCENE_PARSE_TOO_MANY: return "too many values";
    case JAKO_SCENE_PARSE_NOT_JSON: return "not a JSON object, or no memory to parse it";
    case JAKO_SCENE_PARSE_MISSING: return "v, seq, id, state or src missing, or seq below 1";
    case JAKO_SCENE_PARSE_VERSION: return "unknown version";
    case JAKO_SCENE_PARSE_ID: return "bad id";
    case JAKO_SCENE_PARSE_STATE: return "unknown state";
    case JAKO_SCENE_PARSE_SRC: return "src missing or unknown";
    case JAKO_SCENE_PARSE_BAD_TEXT: return "bad text";
    case JAKO_SCENE_PARSE_BAD_SHAPE: return "bad shape";
    case JAKO_SCENE_PARSE_PAST_EDGE: return "past the edge";
    default: return "unknown";
    }
}

bool jako_scene_same(const jako_scene_update_t* a, const jako_scene_update_t* b)
{
    if (a == NULL || b == NULL) return false;
    if (a->state != b->state || a->src != b->src || a->age != b->age || a->stale != b->stale || a->hold != b->hold ||
        strcmp(a->id, b->id) != 0 || strcmp(a->st, b->st) != 0 || a->shape_n != b->shape_n ||
        a->point_n != b->point_n || a->text_n != b->text_n)
        return false;
    if (a->shape_n > JAKO_SCENE_MAX_SHAPES || a->point_n > JAKO_SCENE_MAX_POINTS || a->text_n > JAKO_SCENE_MAX_TEXTS)
        return false;
    for (uint8_t i = 0; i < a->text_n; i++) {
        const jako_scene_text_t *p = &a->texts[i], *q = &b->texts[i];
        if (p->x != q->x || p->y != q->y || p->size != q->size || p->opa != q->opa || p->align != q->align ||
            strncmp(p->text, q->text, sizeof(p->text)) != 0)
            return false;
    }
    return memcmp(a->shapes, b->shapes, sizeof(a->shapes[0]) * a->shape_n) == 0 &&
           memcmp(a->points, b->points, sizeof(a->points[0]) * a->point_n) == 0;
}

bool jako_scene_seq_admit(jako_scene_seq_t* g, int32_t seq)
{
    if (g == NULL || seq <= g->last_seq) return false;
    g->last_seq = seq;
    return true;
}

void jako_scene_seq_close(jako_scene_seq_t* g, int32_t seq)
{
    if (g != NULL && seq > g->last_seq) g->last_seq = seq;
}

void jako_scene_seq_reset(jako_scene_seq_t* g)
{
    if (g != NULL) g->last_seq = 0;
}
