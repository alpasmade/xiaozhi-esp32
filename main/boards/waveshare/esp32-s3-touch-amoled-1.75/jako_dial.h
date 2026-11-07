// Copyright 2026 Alpas Made. All rights reserved.
/* The dial of today for LVGL 9.5: the desk's day dial on the round screen,
   round the face in JAKO_FACE_SHOW_SMALL. The plan's section 2 as cut for
   slice 1: the today view only, the four data screens plus sample and
   stale, the minute turn as a cut (1 degree every 2 minutes), the seconds
   bead, digits-only numerals, the now mark at twelve, the seam at ten
   o'clock, sunset, the night dim, and the rules for showing it at rest.
   Not in slice 1: the long press look cycle, the voice look, the tomorrow
   view, a stepped turn. The update's ask, view and look are parsed and
   kept, but the dial always draws the today view at look 0.

   The time map: 30 degrees an hour, half a degree a minute. The reference
   under the now mark is now rounded down to an even minute, so every arc
   end is a whole degree. The window is the reference minus 120 min to plus
   600 min, the seam at 300 degrees where they meet. The dial keeps its own
   clock: on each applied update it stores t (local seconds since its day's
   midnight) and counts from there; at local midnight day 1 becomes today.

   Built like jako_radar: one transparent full-size LVGL object that draws
   itself in LV_EVENT_DRAW_MAIN and runs its own timer. Each tick it works
   out what changed and invalidates only that, in at most
   JAKO_DIAL_INV_MAX areas: the clock text and the seconds bead each
   second, the annulus (r 99 to 198) in eight tiles when the dial turns or
   its data changes, the track's band in eight tiles for a loading pulse.
   The bead is not invalidated on a turn tick (the tiles cover it). An
   update that changes only t, age or ask redraws only the clock text and
   the bead. Its draw callback returns at once for areas inside the plate
   that miss its text, so the face's blinks cost the dial nothing. Nothing
   it lights is past 197 px from the centre. Colour: Light only; the marks
   that are neon on the desk each have a named constant below.

   Call every function from the LVGL task or with the display lock held. */
#ifndef JAKO_DIAL_H
#define JAKO_DIAL_H

#include <stdbool.h>
#include <stdint.h>

#include "jako_dial_data.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

#define JAKO_DIAL_LIGHT 0xF1F0EC          /* DESIGN-TRUTH section 1, Light */
#define JAKO_DIAL_NOW_RGB JAKO_DIAL_LIGHT /* the now mark: neon on the desk, Light on the device (decision 10.2) */
#define JAKO_DIAL_NOW_EVENT_RGB JAKO_DIAL_LIGHT /* the event on now: neon on the desk */
#define JAKO_DIAL_NOW_TIME_RGB JAKO_DIAL_LIGHT  /* the now time: neon on the desk */
#define JAKO_DIAL_TICK_MS 100             /* its own timer: the bead steps on the second within 100 ms */
#define JAKO_DIAL_INV_MAX 10              /* areas invalidated per tick, at most (ring 16 + face 3 + 10 < 32) */
#define JAKO_DIAL_CONTENT_R 197           /* px from the centre: nothing is lit beyond */
#define JAKO_DIAL_REST_AGE_S 43200        /* at rest only while the cache is younger than 12 h */
#define JAKO_DIAL_ERROR_FACE_MS 2400      /* the board shows the face's error this long on JAKO_DIAL_SCREEN_ERROR */

/* What the dial shows, from the updates applied and its own clock. */
typedef enum {
    JAKO_DIAL_SCREEN_LOADING = 0, /* a loading update and no cache: the frame, the track pulsing, "Checking" */
    JAKO_DIAL_SCREEN_READY,       /* the cache's events, the bottom line the event on now, the next, or the date */
    JAKO_DIAL_SCREEN_EMPTY,       /* no events today: the frame, the date */
    JAKO_DIAL_SCREEN_ERROR,       /* the store could not be read and no cache: the frame, "No agenda" */
    JAKO_DIAL_SCREEN_ERROR_CACHED,/* the store could not be read: the cache at half alpha, "No agenda" */
    JAKO_DIAL_SCREEN_STALE,       /* the cache is older than its stale: arcs at half alpha, "45 min old" */
    JAKO_DIAL_SCREEN_SAMPLE,      /* the invented sample day: "Sample day", always */
    JAKO_DIAL_SCREEN_PAST,        /* the clock is past the days the cache covers: the frame, no bottom line */
} jako_dial_screen_t;

/* Whether the dial may show at rest (the device idle, no socket), and if not, why. */
typedef enum {
    JAKO_DIAL_REST_SHOWABLE = 0,
    JAKO_DIAL_REST_HOME_OFF,  /* the last update had home 0 */
    JAKO_DIAL_REST_NO_CACHE,  /* no ready or empty update applied yet */
    JAKO_DIAL_REST_PAST_DAYS, /* the clock is past the end of the days the cache covers */
    JAKO_DIAL_REST_TOO_OLD,   /* the cache's age plus time since receipt is 12 h or more */
} jako_dial_rest_t;

/* For the preview's checks and for logs. */
typedef struct {
    uint32_t clock_ms;        /* the dial's own clock since create */
    uint32_t updates;         /* applied */
    jako_dial_screen_t screen;
    jako_dial_rest_t rest;
    int32_t now_s;            /* seconds since the midnight of the update's day (past 86400 after midnight) */
    int32_t day;              /* 0 or 1: which of the update's two days is today; 2 past them */
    int32_t ref_min;          /* the reference under the now mark, minutes since today's midnight */
    int32_t age_s;            /* the cache's age now, -1 without a cache */
    uint8_t night;            /* 1 from 23:00 to 05:00 */
    uint8_t dim_half;         /* 1 when the arcs are at half alpha (stale, or the cache under an error) */
    char time_text[8];
    char bottom_text[24];
    uint8_t bottom_opa;
    int32_t bead_deg;         /* the seconds bead's bearing, -1 when not drawn */
    uint32_t arc_n, tick_n, numeral_n;
    uint8_t sunset_shown;
    uint8_t text_px_min;      /* the smallest text size drawn (20 or 22), 0 for none */
    uint8_t text_opa_min;     /* the faintest text drawn, 0 for none */
    uint8_t text_n;           /* the line boxes of every text drawn: the time, the bottom line, the numerals */
    lv_area_t text_box[8];
    lv_area_t now_mark;       /* the now mark's box */
    uint32_t inval_areas;     /* areas invalidated by the last tick */
    uint32_t inval_px;        /* their pixels, before the display's rounder */
    uint32_t turns;           /* annulus redraws so far (turns, data changes, screen changes) */
    uint8_t turned;           /* 1 when the last tick redrew the annulus */
} jako_dial_debug_t;

/* Called once from the dial's timer when the dial stops being showable at
   rest (jako_dial_rest leaves JAKO_DIAL_REST_SHOWABLE). It may hide or
   delete the dial: the widget touches nothing after calling it. */
typedef void (*jako_dial_rest_cb_t)(lv_obj_t* dial, void* user);

/* A full-size, transparent dial on parent, created above the face, in the
   loading look until an update is applied. It takes no input (not
   clickable, not scrollable) and starts its timer. */
lv_obj_t* jako_dial_create(lv_obj_t* parent);

/* Apply a parsed update the caller has already gated with
   jako_dial_seq_admit. The clock restarts from the update's t. A ready or
   empty update replaces the cache (its events, sunsets, source, age, stale
   and home); an error update keeps the cache and shows it at half alpha
   under "No agenda"; a loading update never replaces the cache and only
   resyncs t and days. The cache keeps its own day pair: a loading or error
   update on the cache's day 1 moves every cached event and sunset back a
   day (the cache then covers that one day), and one on any other day drops
   the cache, so yesterday's events never draw as today's. Returns the look
   shown in minutes: always 0 in slice 1 (1440 will be the tomorrow view). */
int32_t jako_dial_apply(lv_obj_t* dial, const jako_dial_update_t* update);

/* What it shows now. After an apply, JAKO_DIAL_SCREEN_ERROR asks the
   board to show the face's error for JAKO_DIAL_ERROR_FACE_MS. */
jako_dial_screen_t jako_dial_screen(lv_obj_t* dial);

/* Whether it may show at rest: home 1, a cache that covers now (before the
   end of the days it covers, two or one after a move) and an age under 12 h. */
jako_dial_rest_t jako_dial_rest(lv_obj_t* dial);

void jako_dial_set_rest_cb(lv_obj_t* dial, jako_dial_rest_cb_t cb, void* user);

/* The loading pulse holds at its middle. The bead, a clock, still steps. */
void jako_dial_set_reduced_motion(lv_obj_t* dial, bool on);

/* Move the dial on by ms and invalidate what changed. Its own timer calls
   this; the preview calls it directly. advance(dial, 0) refreshes the
   geometry without moving time. */
void jako_dial_advance(lv_obj_t* dial, uint32_t ms);

void jako_dial_get_debug(lv_obj_t* dial, jako_dial_debug_t* out);

/* Preview only: draw no text (to check that no text box lies over a drawn shape). */
void jako_dial_debug_set_hide_text(lv_obj_t* dial, bool hide);

#ifdef __cplusplus
}
#endif

#endif /* JAKO_DIAL_H */
