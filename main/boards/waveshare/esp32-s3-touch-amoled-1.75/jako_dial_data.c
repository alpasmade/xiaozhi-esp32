// Copyright 2026 Alpas Made. All rights reserved.
/* The dial's wire data, version 1. See jako_dial_data.h for the rules.

   Nothing here trusts a length or a type from the wire: the nesting is
   measured before cJSON sees the text (so its recursion stays three deep on
   the main task's stack), every number is clamped from cJSON's saturated
   int, and every string is copied byte by byte through a printable ASCII
   filter into a fixed buffer. */
#include "jako_dial_data.h"

#include <string.h>

#include "cJSON.h"

#define JDD_SEQ_MAX 2147483647
#define JDD_AGE_MAX 86400
#define JDD_STALE_MIN 60
#define JDD_STALE_MAX 86400
#define JDD_T_MAX 86399
#define JDD_ASK_MAX 65535
#define JDD_LOOK_MAX 600
#define JDD_SUN_MAX 1439
#define JDD_S_MIN (-1440)
#define JDD_S_MAX 2879
#define JDD_E_MAX 2880

static int32_t jdd_clamp(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }

/* How deep the brackets go, outside strings, and how many values there are (1, plus one for every
   bracket and comma outside strings, as the scene's prescan counts them); depth -1 when a string
   never closes. */
static int jdd_depth(const char* s, size_t n, uint32_t* values)
{
    int depth = 0, most = 0;
    bool in_str = false, esc = false;
    *values = 1;
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
            ++*values;
            if (++depth > most) most = depth;
            if (most > JAKO_DIAL_MAX_DEPTH) return most;
        } else if (ch == '}' || ch == ']') {
            if (depth > 0) depth--;
        } else if (ch == ',') {
            ++*values;
        }
    }
    return in_str ? -1 : most;
}

static bool jdd_num(const cJSON* item, int32_t* out)
{
    if (!cJSON_IsNumber(item)) return false;
    *out = (int32_t)item->valueint; /* cJSON saturates to INT_MIN..INT_MAX and truncates fractions */
    return true;
}

static int32_t jdd_int_or(const cJSON* obj, const char* key, int32_t fallback, int32_t lo, int32_t hi)
{
    int32_t v;
    return jdd_num(cJSON_GetObjectItemCaseSensitive(obj, key), &v) ? jdd_clamp(v, lo, hi) : fallback;
}

/* Copy a JSON string keeping only bytes 0x20..0x7E, at most cap of them.
   A string of nothing but spaces comes out empty, so it never shows as a blank name. */
static void jdd_text(const cJSON* item, char* dst, size_t cap)
{
    dst[0] = '\0';
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
static bool jdd_rest_is_space(const char* p, const char* end)
{
    for (; p < end; p++)
        if (*p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') return false;
    return true;
}

static void jdd_days(const cJSON* root, jako_dial_day_t* days)
{
    for (int i = 0; i < 2; i++) {
        days[i].weekday = -1;
        days[i].mday = 0;
    }
    const cJSON* arr = cJSON_GetObjectItemCaseSensitive(root, "days");
    if (!cJSON_IsArray(arr)) return;
    for (int i = 0; i < 2; i++) {
        const cJSON* pair = cJSON_GetArrayItem(arr, i);
        int32_t w, d;
        if (!cJSON_IsArray(pair) || !jdd_num(cJSON_GetArrayItem(pair, 0), &w) ||
            !jdd_num(cJSON_GetArrayItem(pair, 1), &d))
            continue;
        days[i].weekday = (int8_t)jdd_clamp(w, 0, 6);
        days[i].mday = (int8_t)jdd_clamp(d, 1, 31);
    }
}

static void jdd_sun(const cJSON* root, int16_t* sun)
{
    sun[0] = -1;
    sun[1] = -1;
    const cJSON* arr = cJSON_GetObjectItemCaseSensitive(root, "sun");
    if (!cJSON_IsArray(arr)) return;
    for (int i = 0; i < 2; i++) {
        int32_t v;
        if (jdd_num(cJSON_GetArrayItem(arr, i), &v)) sun[i] = (int16_t)jdd_clamp(v, -1, JDD_SUN_MAX);
    }
}

static uint8_t jdd_events(const cJSON* root, jako_dial_event_t* ev)
{
    const cJSON* arr = cJSON_GetObjectItemCaseSensitive(root, "ev");
    if (!cJSON_IsArray(arr)) return 0;
    uint8_t n = 0;
    const cJSON* item = NULL;
    cJSON_ArrayForEach(item, arr)
    {
        if (n >= JAKO_DIAL_MAX_EVENTS) break;
        if (!cJSON_IsArray(item)) continue;
        int32_t d, s, e;
        if (!jdd_num(cJSON_GetArrayItem(item, 0), &d) || !jdd_num(cJSON_GetArrayItem(item, 1), &s) ||
            !jdd_num(cJSON_GetArrayItem(item, 2), &e))
            continue;
        jako_dial_event_t* p = &ev[n];
        memset(p, 0, sizeof(*p));
        p->d = (uint8_t)jdd_clamp(d, 0, 1);
        s = jdd_clamp(s, JDD_S_MIN, JDD_S_MAX);
        const int32_t e_max = s + JAKO_DIAL_DAY_MIN < JDD_E_MAX ? s + JAKO_DIAL_DAY_MIN : JDD_E_MAX;
        p->s = (int16_t)s;
        p->e = (int16_t)jdd_clamp(e, s + 1, e_max);
        jdd_text(cJSON_GetArrayItem(item, 3), p->name, JAKO_DIAL_NAME_MAX);
        n++;
    }
    return n;
}

jako_dial_parse_result_t jako_dial_parse(const char* data, size_t len, jako_dial_update_t* out)
{
    if (data == NULL || out == NULL || len == 0 || len > JAKO_DIAL_DATA_MAX) return JAKO_DIAL_PARSE_EMPTY;
    uint32_t values = 0;
    const int depth = jdd_depth(data, len, &values);
    if (depth < 0) return JAKO_DIAL_PARSE_NOT_JSON;
    if (depth > JAKO_DIAL_MAX_DEPTH) return JAKO_DIAL_PARSE_TOO_DEEP;
    if (values > JAKO_DIAL_MAX_VALUES) return JAKO_DIAL_PARSE_TOO_MANY; /* cJSON never builds the tree */

    const char* end = NULL;
    cJSON* root = cJSON_ParseWithLengthOpts(data, len, &end, false);
    if (root == NULL) return JAKO_DIAL_PARSE_NOT_JSON; /* bad text, or cJSON could not allocate */
    jako_dial_parse_result_t r = JAKO_DIAL_PARSE_OK;
    int32_t seq = 0;
    const cJSON* vi = cJSON_GetObjectItemCaseSensitive(root, "v");
    const cJSON* st = cJSON_GetObjectItemCaseSensitive(root, "state");
    const cJSON* sr = cJSON_GetObjectItemCaseSensitive(root, "src");
    const cJSON* vw = cJSON_GetObjectItemCaseSensitive(root, "view");
    jako_dial_state_t state = JAKO_DIAL_STATE_LOADING;
    jako_dial_src_t src = JAKO_DIAL_SRC_DESK;
    jako_dial_view_t view = JAKO_DIAL_VIEW_TODAY;
    if (!cJSON_IsObject(root) || end == NULL || !jdd_rest_is_space(end, data + len)) {
        r = JAKO_DIAL_PARSE_NOT_JSON; /* not an object, or text after it */
    } else if (!cJSON_IsNumber(vi) || !jdd_num(cJSON_GetObjectItemCaseSensitive(root, "seq"), &seq) || seq < 1 ||
               !cJSON_IsString(st) || st->valuestring == NULL || !cJSON_IsString(sr) || sr->valuestring == NULL) {
        r = JAKO_DIAL_PARSE_MISSING;
    } else if (vi->valuedouble != 1.0) {
        r = JAKO_DIAL_PARSE_VERSION;
    } else if (strcmp(st->valuestring, "loading") == 0) {
        state = JAKO_DIAL_STATE_LOADING;
    } else if (strcmp(st->valuestring, "ready") == 0) {
        state = JAKO_DIAL_STATE_READY;
    } else if (strcmp(st->valuestring, "empty") == 0) {
        state = JAKO_DIAL_STATE_EMPTY;
    } else if (strcmp(st->valuestring, "error") == 0) {
        state = JAKO_DIAL_STATE_ERROR;
    } else {
        r = JAKO_DIAL_PARSE_STATE;
    }
    /* src names who made the events, so an invented day always says "Sample day": no guessing */
    if (r == JAKO_DIAL_PARSE_OK) {
        if (strcmp(sr->valuestring, "sample") == 0)
            src = JAKO_DIAL_SRC_SAMPLE;
        else if (strcmp(sr->valuestring, "desk") != 0)
            r = JAKO_DIAL_PARSE_SRC;
    }
    if (r == JAKO_DIAL_PARSE_OK && vw != NULL) {
        if (cJSON_IsString(vw) && vw->valuestring != NULL && strcmp(vw->valuestring, "tomorrow") == 0)
            view = JAKO_DIAL_VIEW_TOMORROW;
        else if (!cJSON_IsString(vw) || vw->valuestring == NULL || strcmp(vw->valuestring, "today") != 0)
            r = JAKO_DIAL_PARSE_VIEW;
    }
    if (r != JAKO_DIAL_PARSE_OK) {
        cJSON_Delete(root);
        return r;
    }

    /* From here nothing can fail: the header is good, so *out is filled. */
    memset(out, 0, sizeof(*out));
    out->seq = jdd_clamp(seq, 1, JDD_SEQ_MAX);
    out->state = state;
    out->src = src;
    out->view = view;
    out->age = jdd_int_or(root, "age", 0, 0, JDD_AGE_MAX);
    out->stale = jdd_int_or(root, "stale", JAKO_DIAL_STALE_DEFAULT_S, JDD_STALE_MIN, JDD_STALE_MAX);
    out->home = (uint8_t)jdd_int_or(root, "home", 0, 0, 1);
    out->t = jdd_int_or(root, "t", 0, 0, JDD_T_MAX);
    out->ask = (uint16_t)jdd_int_or(root, "ask", 0, 0, JDD_ASK_MAX);
    out->look = (int16_t)jdd_int_or(root, "look", 0, 0, JDD_LOOK_MAX);
    jdd_days(root, out->days);
    jdd_sun(root, out->sun);
    /* loading has no store answer and error could not read it: their events are never used */
    if (state == JAKO_DIAL_STATE_READY || state == JAKO_DIAL_STATE_EMPTY) out->ev_n = jdd_events(root, out->ev);
    cJSON_Delete(root);
    return JAKO_DIAL_PARSE_OK;
}

const char* jako_dial_parse_reason(jako_dial_parse_result_t r)
{
    switch (r) {
    case JAKO_DIAL_PARSE_OK: return "ok";
    case JAKO_DIAL_PARSE_EMPTY: return "no data or over 3072 bytes";
    case JAKO_DIAL_PARSE_TOO_DEEP: return "nested too deep";
    case JAKO_DIAL_PARSE_TOO_MANY: return "more than 240 values";
    case JAKO_DIAL_PARSE_NOT_JSON: return "not a JSON object, or no memory to parse it";
    case JAKO_DIAL_PARSE_MISSING: return "v, seq, state or src missing, or seq below 1";
    case JAKO_DIAL_PARSE_VERSION: return "unknown version";
    case JAKO_DIAL_PARSE_STATE: return "unknown state";
    case JAKO_DIAL_PARSE_SRC: return "unknown src";
    case JAKO_DIAL_PARSE_VIEW: return "unknown view";
    default: return "unknown";
    }
}

bool jako_dial_seq_admit(jako_dial_seq_t* g, int32_t seq)
{
    if (g == NULL || seq <= g->last_seq) return false;
    g->last_seq = seq;
    return true;
}

void jako_dial_seq_close(jako_dial_seq_t* g, int32_t seq)
{
    if (g != NULL && seq > g->last_seq) g->last_seq = seq;
}

bool jako_dial_ask_admit(jako_dial_seq_t* g, uint16_t ask)
{
    /* ask 0 is "never asked": the probe and every refresh before "show my day" carry it */
    if (g == NULL || ask == 0 || (int32_t)ask == g->last_ask) return false;
    g->last_ask = ask;
    return true;
}

void jako_dial_seq_reset(jako_dial_seq_t* g)
{
    if (g == NULL) return;
    g->last_seq = 0;
    g->last_ask = 0;
}
