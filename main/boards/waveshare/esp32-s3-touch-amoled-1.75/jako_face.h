// Copyright 2026 Alpas Made. All rights reserved.
/* Jako's face for LVGL 9.5: two light blocks and a small mouth on black,
   with the state ring round the edge. A port of brand/jako-face.js; every
   number comes from DESIGN-TRUTH.md section 2 and its Firmware budget.

   The face is a plain LVGL object that draws itself in LV_EVENT_DRAW_MAIN
   and runs its own 30 Hz timer. Call every function from the LVGL task or
   with the display lock held. Sizes are in units of a 26-unit grid mapped
   to the object's width. */
#ifndef JAKO_FACE_H
#define JAKO_FACE_H

#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    JAKO_FACE_STATE_IDLE = 0,
    JAKO_FACE_STATE_LISTENING,
    JAKO_FACE_STATE_THINKING,
    JAKO_FACE_STATE_SPEAKING,
    JAKO_FACE_STATE_ERROR,
} jako_face_state_t;

typedef enum {
    JAKO_FACE_BODY_DIZZY = 0, /* dizzy for 2 s of face time, above error (shake) */
    JAKO_FACE_BODY_HAPPY,     /* happy for 2 s of face time, above error (cable in, battery full) */
    JAKO_FACE_BODY_BLINK,     /* one blink now, under the natural blink's rules (picked up) */
} jako_face_body_event_t;

typedef enum {
    JAKO_FACE_MOUTH_SMILE = 0,
    JAKO_FACE_MOUTH_GRIN,
    JAKO_FACE_MOUTH_FROWN,
    JAKO_FACE_MOUTH_DOT,
    JAKO_FACE_MOUTH_FLAT,
    JAKO_FACE_MOUTH_O,
    JAKO_FACE_MOUTH_TALK,
} jako_face_mouth_t;

/* What the face draws. RING_ONLY keeps the state ring and draws no eyes or
   mouth, for a screen (the radar) that covers the middle: the ring stays
   true and the eyes cost no redraws underneath. SMALL draws the eyes,
   mouth, glow and hearts at half the unit (the face as the engine draws it
   at half the size) about the screen centre, for the dial round it, and the
   ring at the full unit exactly as FULL does. */
typedef enum {
    JAKO_FACE_SHOW_FULL = 0,
    JAKO_FACE_SHOW_RING_ONLY,
    JAKO_FACE_SHOW_SMALL,
} jako_face_show_t;

typedef enum {
    JAKO_FACE_RING_NONE = 0,
    JAKO_FACE_RING_FULL,
    JAKO_FACE_RING_COMET,
} jako_face_ring_mode_t;

/* One eye box in units: centre offset from the screen centre, size, corner radius, tilt in radians. */
typedef struct {
    float x, y, w, h, r, tilt;
} jako_face_box_t;

/* What the face is doing right now, for the preview's checks. */
typedef struct {
    const char* expression;   /* drawn expression, after precedence */
    jako_face_state_t state;  /* drawn state, after precedence */
    jako_face_box_t left, right; /* eased eye boxes */
    float glance_x, glance_y;
    float blink;              /* 0 open, 1 shut */
    float next_blink;         /* seconds */
    int32_t ring_color;       /* 0xRRGGBB, or -1 before any state has coloured it */
    float ring_alpha, ring_level;
    jako_face_ring_mode_t ring_mode; /* committed to the screen */
    uint8_t ring_opa;                /* committed to the screen */
    uint32_t ring_commits;           /* visible full-ring changes so far */
    float ring_commit_time;          /* face time of the last one */
    jako_face_mouth_t mouth;  /* drawn mouth: talk while speaking */
    float mouth_open;
    bool hearts;
    float time;
    const char* body;         /* timed body reaction: "none", "dizzy" or "happy" */
    float body_left;          /* seconds of it left */
    float lean_x, lean_y;     /* the held lean in units, after the clamp */
    jako_face_show_t show;    /* full, the ring alone, or small */
} jako_face_debug_t;

typedef void (*jako_face_pre_tick_cb_t)(lv_obj_t* face, void* user);

/* A full-size face on parent with its own black background. */
lv_obj_t* jako_face_create(lv_obj_t* parent);

/* An expression or any firmware emotion name (the ALIASES table). Unknown
   names draw neutral and are logged once each. */
void jako_face_set_expression(lv_obj_t* face, const char* name);
void jako_face_set_state(lv_obj_t* face, jako_face_state_t state);
/* Speaking mouth from the audio level, 0 to 1. Attack 30 ms, release 80 ms. */
void jako_face_set_mouth_level(lv_obj_t* face, float level01);
void jako_face_set_reduced_motion(lv_obj_t* face, bool on);
/* Power save: sleepy, but only while idle with the neutral expression. */
void jako_face_set_rest(lv_obj_t* face, bool on);
/* A reaction to the body. DIZZY and HAPPY show for 2 s of face time above
   error, then the session's expression and state return; a new one replaces
   a running one and starts its 2 s again. BLINK blinks once now, unless the
   drawn face may not blink (error, sleepy, wink, love), the natural blink's
   rule. Unknown events are ignored. */
void jako_face_body_event(lv_obj_t* face, jako_face_body_event_t event);
/* Tilt in the hand: while idle and not under reduced motion, the eyes look
   toward (x, y) units instead of glancing, eased at the glance rate. The
   vector is clamped to a length of 0.6; anything not finite reads as 0.
   (0, 0) lets the glance return. Whoever drives it sets (0, 0) when its
   source stops, so a lean never sticks. */
void jako_face_set_lean(lv_obj_t* face, float x, float y);
/* FULL (the default), RING_ONLY or SMALL. Everything else keeps running in
   every mode (state, blinks, glances, the ring's commits); RING_ONLY draws
   no eyes or mouth, SMALL draws them at half the unit. Entering RING_ONLY
   invalidates the eyes and mouth once; leaving it invalidates the whole
   face. A change between FULL and SMALL invalidates the eyes and mouth (old
   and new bounds), never the ring. */
void jako_face_set_show(lv_obj_t* face, jako_face_show_t show);
/* Jump every easing to its target, as snap() in the engine. */
void jako_face_snap(lv_obj_t* face);
/* Seed for blinks and glances (xorshift32). Zero is replaced by one. */
void jako_face_set_seed(lv_obj_t* face, uint32_t seed);
/* Called at the start of every tick, before the face moves. */
void jako_face_set_pre_tick(lv_obj_t* face, jako_face_pre_tick_cb_t cb, void* user);
/* Move the face on by dt seconds and invalidate what changed. The face's
   own timer calls this; the preview calls it directly. */
void jako_face_advance(lv_obj_t* face, float dt);
void jako_face_get_debug(lv_obj_t* face, jako_face_debug_t* out);
/* Preview only: hold a blink at progress 0 to 1 of its 0.26 s. */
void jako_face_debug_set_blink(lv_obj_t* face, float progress);

#ifdef __cplusplus
}
#endif

#endif /* JAKO_FACE_H */
