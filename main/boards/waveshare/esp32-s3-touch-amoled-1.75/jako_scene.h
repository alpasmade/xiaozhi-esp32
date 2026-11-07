// Copyright 2026 Alpas Made. All rights reserved.
/* A scene for LVGL 9.5: one of Jako's visuals, drawn from a parsed
   self.scene.update (jako_scene_data.h). The server composes the picture;
   this widget only draws it, in Light on black, in the order sent: the
   shapes of sh, then the texts of tx, then the device's own status line.

   Built like jako_radar: one transparent full-size LVGL object that draws
   itself in LV_EVENT_DRAW_MAIN and runs its own timer. All geometry is
   integer. A scene is still: it is drawn once when applied, and after that
   only the loading pulse, the change to stale and the status line's text
   are redrawn, each in its own boxes, at most JAKO_SCENE_INV_MAX areas a
   tick. A scene equal to the one shown (every field but seq) only resets
   the hold timer. Every shape and text is clipped above y 371: the status
   slot below holds the status line alone. It lies above the face, which
   the board puts in JAKO_FACE_SHOW_RING_ONLY, so the state ring shows
   round it.

   Every shape is drawn opaque, in Light already mixed with black at its
   alpha (as jako_radar.c draws its strokes), so caps and joints never
   double up and a later shape covers an earlier one. Texts use the board's
   two baked fonts, Inter SemiBold 20 and 22, at their own alpha. A black
   plate never reaches past 193 px, so the state ring's glow stays whole.

   The views: loading (the probe, or nothing applied yet) draws a ring of
   outer radius 150 round (233, 218), 2 px, pulsing .06 to .16 in eight
   steps every 2.4 s, and the status; ready and empty draw the scene;
   error draws the scene with the status in full Light (the board shows
   the face's error for 2.4 s); stale (ready or empty, stale set, age plus
   time since the update reached it) draws every shape but the plates at
   half alpha. The status line, first that applies: loading, st or
   "Checking"; error, st or "No data", full Light; src sample, "Sample
   data", always; stale, "N min old" or "N h old"; else st, or nothing.
   It is 20 px, centred, baseline 396 (its box is centred at y 388.5, the
   radar's caption slot), at most 200 px wide (words drop from the end).

   Colour: Light only. A scene carries no colour, so a mark that would be
   neon on the desk is full Light here; the one colour is JAKO_SCENE_LIGHT.

   Call every function from the LVGL task or with the display lock held. */
#ifndef JAKO_SCENE_H
#define JAKO_SCENE_H

#include <stdbool.h>
#include <stdint.h>

#include "jako_scene_data.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

#define JAKO_SCENE_LIGHT 0xF1F0EC  /* DESIGN-TRUTH section 1, Light: the only colour a scene draws */
#define JAKO_SCENE_TICK_MS 100     /* its own timer: the pulse steps every 300 ms */
#define JAKO_SCENE_INV_MAX 10      /* areas invalidated per tick, at most (LVGL keeps 32; the ring takes 16) */
#define JAKO_SCENE_PLATE_R 193     /* a plate stops here: the state ring's glow begins at 194 */
#define JAKO_SCENE_STATUS_BASE 396 /* the status line's baseline, the radar's caption slot */
#define JAKO_SCENE_STATUS_W 200    /* px, at most */
#define JAKO_SCENE_PULSE_X 233     /* the loading ring's centre and outer radius */
#define JAKO_SCENE_PULSE_Y 218
#define JAKO_SCENE_PULSE_R 150
#define JAKO_SCENE_PULSE_W 2

typedef enum {
    JAKO_SCENE_VIEW_LOADING = 0,
    JAKO_SCENE_VIEW_READY,
    JAKO_SCENE_VIEW_EMPTY,
    JAKO_SCENE_VIEW_ERROR,
    JAKO_SCENE_VIEW_STALE, /* ready or empty, past its stale age: shapes at half alpha */
} jako_scene_view_t;

typedef enum {
    JAKO_SCENE_HOLD_RUNNING = 0,
    JAKO_SCENE_HOLD_EXPIRED, /* hold seconds passed with no applied update: the expire callback ran */
} jako_scene_hold_t;

/* What an apply did. Anything but SHOWN and SAME leaves the picture as it was. */
typedef enum {
    JAKO_SCENE_APPLY_SHOWN = 0, /* a new picture */
    JAKO_SCENE_APPLY_SAME,      /* equal to the one shown: only the hold timer was reset */
    JAKO_SCENE_APPLY_PAST_EDGE, /* a text reaches past 197 px from the centre */
    JAKO_SCENE_APPLY_BAD,       /* not a parsed update (counts or point ranges out of bounds), or no widget */
} jako_scene_apply_t;

typedef struct {
    int16_t x1, y1, x2, y2; /* screen px, inclusive; x2 < x1 for none */
} jako_scene_box_t;

/* For the preview's checks and for logs. */
typedef struct {
    uint32_t clock_ms;         /* the widget's own clock since create */
    uint32_t since_update_ms;  /* since the last SHOWN apply */
    uint32_t since_hold_ms;    /* since the last SHOWN or SAME apply */
    uint32_t updates;          /* SHOWN applies */
    uint32_t sames;            /* SAME applies */
    jako_scene_state_t state;  /* the shown update's */
    jako_scene_src_t src;
    jako_scene_view_t view;
    jako_scene_hold_t hold;
    int32_t age_s;             /* age plus time since the update */
    const char* id;            /* the shown scene's id, "" before any */
    uint16_t shape_n, point_n;
    uint8_t text_n;
    uint8_t pulse_opa;         /* the loading ring now, 0 when not loading */
    uint8_t dim_half;          /* 1 while shapes draw at half alpha */
    const char* status;        /* the status line, or "" */
    uint8_t status_opa;
    jako_scene_box_t status_box; /* its line box */
    jako_scene_box_t status_lit; /* its line box joined with its glyphs' ink */
    int16_t text_w[JAKO_SCENE_MAX_TEXTS];           /* each text's width: the sum of its advances */
    jako_scene_box_t text_box[JAKO_SCENE_MAX_TEXTS]; /* each text's line box */
    jako_scene_box_t text_lit[JAKO_SCENE_MAX_TEXTS]; /* its line box joined with its glyphs' ink */
    jako_scene_box_t content;  /* every shape, text and the loading ring, joined (not the status line) */
    uint8_t text_px_min;       /* the smallest text size drawn (20 or 22), 0 for none, status included */
    uint8_t text_opa_min;      /* the faintest text drawn, 0 for none */
    uint32_t inval_areas;      /* areas invalidated by the last tick */
    uint32_t inval_px;         /* their pixels, before the display's rounder */
} jako_scene_debug_t;

/* Called once from the widget's timer when no update has been applied
   (SHOWN or SAME) for the shown update's hold seconds (60 before any). It
   may delete the widget: the widget touches nothing after calling it. */
typedef void (*jako_scene_expire_cb_t)(lv_obj_t* scene, void* user);

/* A full-size, transparent scene on parent, in the loading look until an
   update is applied. It takes no input and starts its timer. NULL when
   the LVGL pool has no room (about 26 KB). */
lv_obj_t* jako_scene_create(lv_obj_t* parent);

/* Check an update's texts against the edge with the device's fonts, and its counts and point ranges, without a widget: the
   board can refuse before it opens anything. SHOWN means it would draw. */
jako_scene_apply_t jako_scene_check(const jako_scene_update_t* update);

/* Show a parsed update. The caller has already gated it with
   jako_scene_seq_admit. A refusal leaves the picture as it was. */
jako_scene_apply_t jako_scene_apply(lv_obj_t* scene, const jako_scene_update_t* update);

/* A short English reason for a refusal, e.g. "past the edge"; the board prefixes "scene: ". */
const char* jako_scene_apply_reason(jako_scene_apply_t r);

/* The id of the scene shown, "" before any: the tool's result. */
const char* jako_scene_id(lv_obj_t* scene);

/* The loading pulse holds at .11. */
void jako_scene_set_reduced_motion(lv_obj_t* scene, bool on);

void jako_scene_set_expire_cb(lv_obj_t* scene, jako_scene_expire_cb_t cb, void* user);

/* Move the widget on by ms and invalidate what changed. Its own timer
   calls this; the preview calls it directly. advance(scene, 0) refreshes
   without moving time. */
void jako_scene_advance(lv_obj_t* scene, uint32_t ms);

void jako_scene_get_debug(lv_obj_t* scene, jako_scene_debug_t* out);

#ifdef __cplusplus
}
#endif

#endif /* JAKO_SCENE_H */
