// Copyright 2026 Alpas Made. All rights reserved.
/* Events from the body, the detectors. See jako_body.h. */
#include "jako_body.h"

#include <string.h>

static int32_t jb_abs(int32_t v) { return v < 0 ? -v : v; }

static int32_t jb_word(const uint8_t* p, jako_body_byte_order_t order)
{
    const uint16_t u = order == JAKO_BODY_ORDER_LITTLE ? (uint16_t)(p[0] | (p[1] << 8)) : (uint16_t)((p[0] << 8) | p[1]);
    return (int32_t)(int16_t)u;
}

static void jb_decode(const uint8_t raw[6], jako_body_byte_order_t order, int32_t mg[3])
{
    for (int i = 0; i < 3; i++) mg[i] = jb_word(raw + 2 * i, order) * 1000 / JAKO_BODY_LSB_PER_G;
}

static bool jb_plausible(const int32_t mg[3])
{
    const int64_t sq = (int64_t)mg[0] * mg[0] + (int64_t)mg[1] * mg[1] + (int64_t)mg[2] * mg[2];
    const int64_t lo = (int64_t)JAKO_BODY_SANE_MIN_MG * JAKO_BODY_SANE_MIN_MG;
    const int64_t hi = (int64_t)JAKO_BODY_SANE_MAX_MG * JAKO_BODY_SANE_MAX_MG;
    return sq >= lo && sq <= hi;
}

/* ------------------------------------------------------------- motion */

void jako_body_motion_init(jako_body_motion_t* m)
{
    if (m == NULL) return;
    memset(m, 0, sizeof(*m));
    m->state = JAKO_BODY_MOTION_CHECKING;
    m->order = JAKO_BODY_ORDER_LITTLE;
}

void jako_body_motion_gap(jako_body_motion_t* m)
{
    if (m != NULL) m->has_prev = false;
}

bool jako_body_motion_decode(const jako_body_motion_t* m, const uint8_t raw[6], int32_t out_mg[3])
{
    if (m == NULL || raw == NULL || out_mg == NULL || m->state != JAKO_BODY_MOTION_ARMED) return false;
    jb_decode(raw, m->order, out_mg);
    return true;
}

/* Both byte orders are tried on the same samples. A wrong order turns small
   values into large ones and the low byte's rollovers into jumps of about
   a g, so only an order whose every sample sits near 1 g is trusted. */
static void jb_check(jako_body_motion_t* m, const uint8_t raw[6])
{
    int32_t le[3], be[3];
    jb_decode(raw, JAKO_BODY_ORDER_LITTLE, le);
    jb_decode(raw, JAKO_BODY_ORDER_BIG, be);
    m->ok_little += jb_plausible(le) ? 1 : 0;
    m->ok_big += jb_plausible(be) ? 1 : 0;
    if (++m->window_n < JAKO_BODY_SANE_SAMPLES) return;

    const bool little = m->ok_little >= JAKO_BODY_SANE_PASS, big = m->ok_big >= JAKO_BODY_SANE_PASS;
    const uint8_t best = m->ok_little > m->ok_big ? m->ok_little : m->ok_big;
    if (little != big) {
        m->state = JAKO_BODY_MOTION_ARMED;
        m->order = little ? JAKO_BODY_ORDER_LITTLE : JAKO_BODY_ORDER_BIG;
        m->has_prev = false;
    } else if (best < JAKO_BODY_SANE_HOPE) {
        if (++m->bad_windows >= JAKO_BODY_SANE_TRIES) m->state = JAKO_BODY_MOTION_REFUSED;
    } else {
        /* moving, or both orders plausible at once: neither counts, look again */
        m->bad_windows = 0;
    }
    m->window_n = 0;
    m->ok_little = 0;
    m->ok_big = 0;
}

jako_body_event_t jako_body_motion_sample(jako_body_motion_t* m, const uint8_t raw[6], uint32_t now_ms)
{
    if (m == NULL || raw == NULL) return JAKO_BODY_NONE;
    if (m->state == JAKO_BODY_MOTION_CHECKING) {
        jb_check(m, raw);
        return JAKO_BODY_NONE;
    }
    if (m->state != JAKO_BODY_MOTION_ARMED) return JAKO_BODY_NONE;

    int32_t mg[3];
    jb_decode(raw, m->order, mg);
    m->index++;
    if (!m->has_prev) {
        memcpy(m->prev, mg, sizeof(mg));
        m->has_prev = true;
        return JAKO_BODY_NONE;
    }
    const int32_t score = jb_abs(mg[0] - m->prev[0]) + jb_abs(mg[1] - m->prev[1]) + jb_abs(mg[2] - m->prev[2]);
    memcpy(m->prev, mg, sizeof(mg));

    if (m->cooling) {
        if ((uint32_t)(now_ms - m->cooldown_from_ms) < JAKO_BODY_SHAKE_COOLDOWN_MS) return JAKO_BODY_NONE;
        m->cooling = false;
        m->hits = 0;
    }
    if (score <= JAKO_BODY_SHAKE_SCORE_MG) return JAKO_BODY_NONE;
    /* this hit's pair shares a sample with the last counted hit: one impact, not two */
    if (m->hits > 0 && (uint32_t)(m->index - m->last_hit_index) < 2u) return JAKO_BODY_NONE;
    if (m->hits == 0 || (uint32_t)(now_ms - m->first_hit_ms) > JAKO_BODY_SHAKE_WINDOW_MS) {
        m->hits = 0;
        m->first_hit_ms = now_ms;
    }
    m->hits++;
    m->last_hit_index = m->index;
    if (m->hits < JAKO_BODY_SHAKE_HITS) return JAKO_BODY_NONE;
    m->hits = 0;
    m->cooling = true;
    m->cooldown_from_ms = now_ms;
    return JAKO_BODY_SHAKE;
}

/* -------------------------------------------------------------- power */

void jako_body_power_init(jako_body_power_t* p)
{
    if (p == NULL) return;
    memset(p, 0, sizeof(*p));
    p->seen = JAKO_BODY_POWER_UNSEEN;
    p->full = JAKO_BODY_FULL_IDLE;
}

jako_body_event_t jako_body_power_sample(jako_body_power_t* p, bool charging, bool discharging, uint32_t now_ms)
{
    if (p == NULL) return JAKO_BODY_NONE;
    if (p->seen == JAKO_BODY_POWER_UNSEEN) {
        /* the first read only seeds: a device booted on the charger is quiet */
        p->seen = JAKO_BODY_POWER_SEEN;
        p->discharging = discharging;
        p->full = !discharging && charging ? JAKO_BODY_FULL_CHARGING : JAKO_BODY_FULL_IDLE;
        return JAKO_BODY_NONE;
    }

    jako_body_event_t ev = JAKO_BODY_NONE;
    if (discharging && !p->discharging) {
        /* cable out */
        p->cable_out_seen = true;
        p->cable_out_ms = now_ms;
        p->full = JAKO_BODY_FULL_IDLE;
        p->standby_reads = 0;
    } else if (!discharging && p->discharging) {
        /* cable in, unless it is the same plug-in wiggled */
        const bool wiggle = p->cable_out_seen && (uint32_t)(now_ms - p->cable_out_ms) < JAKO_BODY_CABLE_SETTLE_MS;
        if (!wiggle) ev = JAKO_BODY_CABLE_IN;
        p->full = JAKO_BODY_FULL_IDLE;
        p->standby_reads = 0;
    }
    p->discharging = discharging;
    if (discharging) return ev;

    switch (p->full) {
    case JAKO_BODY_FULL_IDLE:
        if (charging) p->full = JAKO_BODY_FULL_CHARGING;
        break;
    case JAKO_BODY_FULL_CHARGING:
        if (!charging) {
            p->full = JAKO_BODY_FULL_STANDBY;
            p->standby_reads = 1;
            p->standby_from_ms = now_ms;
        }
        break;
    case JAKO_BODY_FULL_STANDBY:
        if (charging) {
            p->full = JAKO_BODY_FULL_CHARGING;
            p->standby_reads = 0;
        } else {
            if (p->standby_reads < 255) p->standby_reads++;
            if (p->standby_reads >= JAKO_BODY_FULL_READS &&
                (uint32_t)(now_ms - p->standby_from_ms) >= JAKO_BODY_FULL_HOLD_MS) {
                p->full = JAKO_BODY_FULL_SENT;
                if (ev == JAKO_BODY_NONE) ev = JAKO_BODY_BATTERY_FULL;
            }
        }
        break;
    case JAKO_BODY_FULL_SENT:
    default:
        break;
    }
    return ev;
}
