// Copyright 2026 Alpas Made. All rights reserved.
/* The radar's wire data, version 1. See jako_radar_data.h for the rules.

   Nothing here trusts a length or a type from the wire: the nesting is
   measured before cJSON sees the text (so its recursion stays five deep on
   the main task's stack), every number is clamped from cJSON's saturated
   int, and every string is copied byte by byte through a printable ASCII
   filter into a fixed buffer. */
#include "jako_radar_data.h"

#include <string.h>

#include "cJSON.h"

#define JRD_SEQ_MAX 2147483647
#define JRD_ID_MAX 65535
#define JRD_AGE_MAX 3600
#define JRD_POS_AGE_MAX 86400
#define JRD_B_MAX 359
#define JRD_D_MAX 999
#define JRD_A_MIN (-1000)
#define JRD_A_MAX 60000
static const char JRD_FALLBACK_NAME[] = "Aircraft"; /* the server's own word when it knows nothing better */

static int32_t jrd_clamp(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }

/* How deep the brackets go, outside strings; -1 when a string never closes. */
static int jrd_depth(const char* s, size_t n)
{
    int depth = 0, most = 0;
    bool in_str = false, esc = false;
    for (size_t i = 0; i < n; i++) {
        const char ch = s[i];
        if (in_str) {
            if (esc)
                esc = false;
            else if (ch == '\\')
                esc = true;
            else if (ch == '"')
                in_str = false;
            continue;
        }
        if (ch == '"') {
            in_str = true;
        } else if (ch == '{' || ch == '[') {
            if (++depth > most) most = depth;
            if (most > JAKO_RADAR_MAX_DEPTH) return most;
        } else if (ch == '}' || ch == ']') {
            if (depth > 0) depth--;
        }
    }
    return in_str ? -1 : most;
}

static bool jrd_int(const cJSON* obj, const char* key, int32_t* out)
{
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!cJSON_IsNumber(item)) return false;
    *out = (int32_t)item->valueint; /* cJSON saturates to INT_MIN..INT_MAX and truncates fractions */
    return true;
}

static int32_t jrd_int_or(const cJSON* obj, const char* key, int32_t fallback, int32_t lo, int32_t hi)
{
    int32_t v;
    return jrd_int(obj, key, &v) ? jrd_clamp(v, lo, hi) : fallback;
}

/* Copy a JSON string keeping only bytes 0x20..0x7E, at most cap of them.
   A string of nothing but spaces comes out empty, so it never shows as a blank line. */
static void jrd_text(const cJSON* obj, const char* key, char* dst, size_t cap)
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

/* Only JSON whitespace may follow the object. */
static bool jrd_rest_is_space(const char* p, const char* end)
{
    for (; p < end; p++)
        if (*p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') return false;
    return true;
}

static uint8_t jrd_trail(const cJSON* plane, jako_radar_point_t* tr)
{
    const cJSON* arr = cJSON_GetObjectItemCaseSensitive(plane, "tr");
    if (!cJSON_IsArray(arr)) return 0;
    uint8_t n = 0;
    const cJSON* pt = NULL;
    cJSON_ArrayForEach(pt, arr)
    {
        if (n >= JAKO_RADAR_MAX_TRAIL) break;
        if (!cJSON_IsArray(pt)) continue;
        const cJSON* b = cJSON_GetArrayItem(pt, 0);
        const cJSON* d = cJSON_GetArrayItem(pt, 1);
        if (!cJSON_IsNumber(b) || !cJSON_IsNumber(d)) continue;
        tr[n].b = (int16_t)jrd_clamp((int32_t)b->valueint, 0, JRD_B_MAX);
        tr[n].d = (int16_t)jrd_clamp((int32_t)d->valueint, 0, JRD_D_MAX);
        n++;
    }
    return n;
}

static uint8_t jrd_planes(const cJSON* root, jako_radar_plane_t* planes)
{
    const cJSON* arr = cJSON_GetObjectItemCaseSensitive(root, "planes");
    if (!cJSON_IsArray(arr)) return 0;
    uint8_t n = 0;
    const cJSON* item = NULL;
    cJSON_ArrayForEach(item, arr)
    {
        if (n >= JAKO_RADAR_MAX_PLANES) break;
        if (!cJSON_IsObject(item)) continue;
        int32_t id, b, d;
        if (!jrd_int(item, "id", &id) || !jrd_int(item, "b", &b) || !jrd_int(item, "d", &d)) continue;
        id = jrd_clamp(id, 1, JRD_ID_MAX);
        bool seen = false;
        for (uint8_t k = 0; k < n; k++) seen = seen || planes[k].id == (uint16_t)id;
        if (seen) continue; /* one record per id: a repeated id is dropped */
        jako_radar_plane_t* p = &planes[n];
        memset(p, 0, sizeof(*p));
        p->id = (uint16_t)id;
        p->b = (int16_t)jrd_clamp(b, 0, JRD_B_MAX);
        p->d = (int16_t)jrd_clamp(d, 0, JRD_D_MAX);
        p->t = (int16_t)jrd_int_or(item, "t", -1, -1, JRD_B_MAX);
        p->a = jrd_int_or(item, "a", 0, JRD_A_MIN, JRD_A_MAX);
        jrd_text(item, "s", p->s, JAKO_RADAR_NAME_MAX);
        if (p->s[0] == '\0') memcpy(p->s, JRD_FALLBACK_NAME, sizeof(JRD_FALLBACK_NAME));
        jrd_text(item, "c", p->c, JAKO_RADAR_TYPE_MAX);
        p->trail_n = jrd_trail(item, p->trail);
        n++;
    }
    return n;
}

jako_radar_parse_result_t jako_radar_parse(const char* data, size_t len, jako_radar_update_t* out)
{
    if (data == NULL || out == NULL || len == 0 || len > JAKO_RADAR_DATA_MAX) return JAKO_RADAR_PARSE_EMPTY;
    const int depth = jrd_depth(data, len);
    if (depth < 0) return JAKO_RADAR_PARSE_NOT_JSON;
    if (depth > JAKO_RADAR_MAX_DEPTH) return JAKO_RADAR_PARSE_TOO_DEEP;

    const char* end = NULL;
    cJSON* root = cJSON_ParseWithLengthOpts(data, len, &end, false);
    if (root == NULL) return JAKO_RADAR_PARSE_NOT_JSON; /* bad text, or cJSON could not allocate */
    jako_radar_parse_result_t r = JAKO_RADAR_PARSE_OK;
    int32_t seq = 0;
    const cJSON* vi = cJSON_GetObjectItemCaseSensitive(root, "v");
    const cJSON* st = cJSON_GetObjectItemCaseSensitive(root, "state");
    const cJSON* sr = cJSON_GetObjectItemCaseSensitive(root, "src");
    jako_radar_state_t state = JAKO_RADAR_STATE_LOADING;
    jako_radar_src_t src = JAKO_RADAR_SRC_FEED;
    if (!cJSON_IsObject(root) || end == NULL || !jrd_rest_is_space(end, data + len)) {
        r = JAKO_RADAR_PARSE_NOT_JSON; /* not an object, or text after it */
    } else if (!cJSON_IsNumber(vi) || !jrd_int(root, "seq", &seq) || seq < 1 || !cJSON_IsString(st) ||
               st->valuestring == NULL) {
        r = JAKO_RADAR_PARSE_MISSING;
    } else if (vi->valuedouble != 1.0) {
        r = JAKO_RADAR_PARSE_VERSION;
    } else if (strcmp(st->valuestring, "loading") == 0) {
        state = JAKO_RADAR_STATE_LOADING;
    } else if (strcmp(st->valuestring, "live") == 0) {
        state = JAKO_RADAR_STATE_LIVE;
    } else if (strcmp(st->valuestring, "empty") == 0) {
        state = JAKO_RADAR_STATE_EMPTY;
    } else if (strcmp(st->valuestring, "error") == 0) {
        state = JAKO_RADAR_STATE_ERROR;
    } else {
        r = JAKO_RADAR_PARSE_STATE;
    }
    /* src names who made the planes, so invented ones always say "Sample sky": no guessing */
    if (r == JAKO_RADAR_PARSE_OK) {
        if (cJSON_IsString(sr) && sr->valuestring != NULL && strcmp(sr->valuestring, "sample") == 0)
            src = JAKO_RADAR_SRC_SAMPLE;
        else if (!cJSON_IsString(sr) || sr->valuestring == NULL || strcmp(sr->valuestring, "feed") != 0)
            r = JAKO_RADAR_PARSE_SRC;
    }
    if (r != JAKO_RADAR_PARSE_OK) {
        cJSON_Delete(root);
        return r;
    }

    /* From here nothing can fail: the header is good, so *out is filled. */
    memset(out, 0, sizeof(*out));
    out->seq = jrd_clamp(seq, 1, JRD_SEQ_MAX);
    out->state = state;
    out->src = src;
    out->age = jrd_int_or(root, "age", 0, 0, JRD_AGE_MAX);
    out->pos_age = jrd_int_or(root, "pos_age", 0, 0, JRD_POS_AGE_MAX);
    out->focus = (uint16_t)jrd_int_or(root, "f", 0, 0, JRD_ID_MAX);
    /* ln: exactly the number 1 (1, 1.0, 1e0) is the line; anything else, true and "1" included, is the ring */
    const cJSON* ln = cJSON_GetObjectItemCaseSensitive(root, "ln");
    out->style = (cJSON_IsNumber(ln) && ln->valuedouble == 1.0) ? JAKO_RADAR_STYLE_LINE : JAKO_RADAR_STYLE_RING;
    if (state == JAKO_RADAR_STATE_LIVE) {
        out->plane_n = jrd_planes(root, out->planes);
        /* live always carries a plane; live with none listed reads as a clear sky */
        if (out->plane_n == 0) out->state = JAKO_RADAR_STATE_EMPTY;
    }
    cJSON_Delete(root);
    return JAKO_RADAR_PARSE_OK;
}

const char* jako_radar_parse_reason(jako_radar_parse_result_t r)
{
    switch (r) {
    case JAKO_RADAR_PARSE_OK: return "ok";
    case JAKO_RADAR_PARSE_EMPTY: return "no data or over 3072 bytes";
    case JAKO_RADAR_PARSE_TOO_DEEP: return "nested too deep";
    case JAKO_RADAR_PARSE_NOT_JSON: return "not a JSON object, or no memory to parse it";
    case JAKO_RADAR_PARSE_MISSING: return "v, seq or state missing, or seq below 1";
    case JAKO_RADAR_PARSE_VERSION: return "unknown version";
    case JAKO_RADAR_PARSE_STATE: return "unknown state";
    case JAKO_RADAR_PARSE_SRC: return "src missing or unknown";
    default: return "unknown";
    }
}

bool jako_radar_seq_admit(jako_radar_seq_t* g, int32_t seq)
{
    if (g == NULL || seq <= g->last_seq) return false;
    g->last_seq = seq;
    return true;
}

void jako_radar_seq_close(jako_radar_seq_t* g, int32_t seq)
{
    if (g != NULL && seq > g->last_seq) g->last_seq = seq;
}

void jako_radar_seq_reset(jako_radar_seq_t* g)
{
    if (g != NULL) g->last_seq = 0;
}
