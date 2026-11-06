// Copyright 2026 Alpas Made. All rights reserved.
/* The flight radar for LVGL 9.5: the Scope, a port of
   mockup/radar-concepts/scope.js and the parts of shared.js it draws with.
   You at the centre, north fixed at twelve, rings at 5, 10 and 15 miles,
   and a sweep that turns clockwise once every 5 s, in one of two styles
   picked by each update (jako_radar_style_t):
   - the ring (the default): three short ticks at east, south and west and
     a light of six stepped segments running round the 15 mile ring. Marks
     sit where the newest update puts them, at .62, and glow as the light
     passes their bearing.
   - the line: a crosshair, a faint square grid, ticks round the rim, and a
     line out to the edge of the radar with a 45 degree wedge of afterglow.
     As the line passes a plane it paints the plane's mark where the plane
     is now; the mark steps down to .62 and stays there until the next pass.
   A change of style redraws the whole radar once.

   Built like jako_face: one transparent full-size LVGL object that draws
   itself in LV_EVENT_DRAW_MAIN and runs its own 33 ms timer. Each tick it
   works out integer geometry, compares it with what was drawn, and
   invalidates only what changed, in at most JAKO_RADAR_INV_MAX areas. It
   lies above the face, which the board puts in JAKO_FACE_SHOW_RING_ONLY,
   so the state ring shows round it. Nothing it draws goes past 197 px
   from the centre. Colour: Light only.

   Call every function from the LVGL task or with the display lock held.
   Open: create, then apply the first update. Close: lv_obj_delete. */
#ifndef JAKO_RADAR_H
#define JAKO_RADAR_H

#include <stdbool.h>
#include <stdint.h>

#include "jako_face.h"
#include "jako_radar_data.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

#define JAKO_RADAR_LIGHT 0xF1F0EC     /* DESIGN-TRUTH section 1, Light: the only colour the radar draws */
#define JAKO_RADAR_TICK_MS 33         /* its own timer, 30 a second */
#define JAKO_RADAR_EXPIRE_MS 60000    /* no applied update for 60 s: the expire callback fires */
#define JAKO_RADAR_PICK_MS 20000      /* a long-press pick lasts 20 s */
#define JAKO_RADAR_FLIGHTS_STALE_S 15 /* age plus time since receipt */
#define JAKO_RADAR_POSITION_STALE_S 180
#define JAKO_RADAR_INV_MAX 12         /* areas invalidated per tick, at most (LVGL keeps 32 for the whole screen) */
#define JAKO_RADAR_CONTENT_R 197      /* px from the centre: nothing is lit beyond */

/* What the sweep is doing: the line, or in the ring style the ring's light. */
typedef enum {
    JAKO_RADAR_LINE_STOPPED = 0, /* not shown: loading, error, flights old, reduced motion */
    JAKO_RADAR_LINE_MOVING,
    JAKO_RADAR_LINE_HELD,        /* shown, waiting where it stopped while the face thinks */
} jako_radar_line_t;

/* What the screen shows, from the update's state and the two local ages
   (the mock's seven states). */
typedef enum {
    JAKO_RADAR_VIEW_LOADING = 0,
    JAKO_RADAR_VIEW_LIVE,
    JAKO_RADAR_VIEW_EMPTY,
    JAKO_RADAR_VIEW_ERROR,
    JAKO_RADAR_VIEW_STALE_FLIGHTS,
    JAKO_RADAR_VIEW_STALE_POSITION,
    JAKO_RADAR_VIEW_STALE_BOTH,
} jako_radar_view_t;

typedef struct {
    uint16_t id;
    int16_t x, y;      /* px on the object */
    uint8_t opa;       /* the mark's opacity now, after the flights-old dim */
    bool in_range;     /* false: a dot on the rim */
    bool focus;
    uint32_t painted_ms; /* radar clock when the sweep last passed it (the line painted it, or the ring's
                            light lit its glow), or UINT32_MAX for none */
    uint16_t glow_k;     /* the pass glow now, thousandths: 1000, 600, 300 in the second after a pass, else 0 */
} jako_radar_mark_debug_t;

/* For the preview's checks and for logs. */
typedef struct {
    uint32_t clock_ms;            /* the radar's own clock since create */
    uint32_t since_update_ms;
    uint32_t updates;             /* applied */
    jako_radar_state_t state;     /* the last update's */
    jako_radar_src_t src;
    jako_radar_style_t style;     /* the style drawn: the last applied update's */
    jako_radar_view_t view;
    int32_t flights_age_s, position_age_s;
    jako_radar_line_t line;
    int32_t head_tenths;          /* the sweep's head (the line, or the light on the ring), tenths of a degree,
                                     committed in 2.4 degree steps */
    uint16_t focus_id;            /* shown */
    uint16_t server_focus;        /* the last update's f */
    uint16_t pick_id;             /* the long-press pick, 0 for none */
    uint8_t plane_n;
    uint8_t mark_n;
    jako_radar_mark_debug_t marks[JAKO_RADAR_MAX_PLANES];
    const char* status;           /* the status line, or "" */
    uint8_t status_opa;
    uint8_t block_lines;          /* the focus block's lines shown: 0, 1 or 2 */
    lv_area_t block;              /* its knockout, or empty */
    uint8_t text_px_min;          /* the smallest text size drawn (20 or 22), 0 for none */
    uint8_t text_opa_min;         /* the faintest text drawn, 0 for none */
    uint32_t inval_areas;         /* areas invalidated by the last tick */
    uint32_t inval_px;            /* their pixels, before the display's rounder */
    bool expired;
} jako_radar_debug_t;

/* Called once from the radar's timer when no update has been applied for
   JAKO_RADAR_EXPIRE_MS. It may delete the radar: the widget touches
   nothing after calling it. */
typedef void (*jako_radar_expire_cb_t)(lv_obj_t* radar, void* user);

/* A full-size, transparent radar on parent, with no planes and the loading
   look until an update is applied. It takes no input (not clickable, not
   scrollable) and starts its timer. */
lv_obj_t* jako_radar_create(lv_obj_t* parent);

/* Show a parsed update. The caller has already gated it with
   jako_radar_seq_admit. Ages start counting from now. An update of the
   other style redraws the whole radar once in its style. Returns the plane
   id in focus on screen afterwards, 0 for none. */
uint16_t jako_radar_apply(lv_obj_t* radar, const jako_radar_update_t* update);

/* The long press: focus the entry after the shown focus, in list order,
   wrapping, for JAKO_RADAR_PICK_MS. Returns the new focus id, 0 when no
   plane is listed. Nothing is sent anywhere; the next update's result
   reports it. */
uint16_t jako_radar_next(lv_obj_t* radar);

/* The plane id in focus on screen, 0 for none: the result of an update
   that the seq gate dropped. */
uint16_t jako_radar_focus(lv_obj_t* radar);

/* The face's state as drawn: the sweep holds while it is thinking. */
void jako_radar_set_face_state(lv_obj_t* radar, jako_face_state_t state);

/* No sweep (no line and wedge, no light on the ring); the loading pulse holds at its middle. */
void jako_radar_set_reduced_motion(lv_obj_t* radar, bool on);

void jako_radar_set_expire_cb(lv_obj_t* radar, jako_radar_expire_cb_t cb, void* user);

/* Move the radar on by ms and invalidate what changed. Its own timer calls
   this; the preview calls it directly. advance(radar, 0) refreshes the
   geometry without moving time. */
void jako_radar_advance(lv_obj_t* radar, uint32_t ms);

void jako_radar_get_debug(lv_obj_t* radar, jako_radar_debug_t* out);

#ifdef __cplusplus
}
#endif

#endif /* JAKO_RADAR_H */
