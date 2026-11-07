// Copyright 2026 Alpas Made. All rights reserved.
/* The screen slot: what is in front of the face, as a pure reducer.

   One slot decides which of the device's screens is in front: nothing (the
   whole face), the dial (the home screen), the radar or a scene (one of
   Jako's visuals). The board file feeds every event that can move the slot
   into jako_screens_reduce and only applies the output: it shows, hides and
   deletes the objects, sets the face's show mode, routes the long press,
   wakes the dimmed screen and resets the seq gates. The rules live here,
   where the Mac tests them (screens_check.c), not in the board file.

   Plain C with no LVGL and no device header. Nothing is allocated and
   nothing is kept between calls: the caller owns the state and passes it in
   by value, so the same input always gives the same output.

   The rules (the plan's "The slot on the device", section 4, lead decisions
   L2, L6 and L8, review amendments 6, 8, 13, 14, 18 and 24):

   - The radar and a scene ride on the conversation. An applied update opens
     either one in front; each closes the other ("a scene opened"), and
     neither ever deletes the dial: the dial only hides under them.
   - Leaving a conversation (any state but listening and speaking) closes the
     radar or the scene, resets every seq gate and the dial's last ask
     (amendment 8), and ends a "show my day".
   - The full face while Jako listens, thinks and speaks in an ordinary
     conversation. The dial is in front during a conversation only after it
     was asked for ("show my day"), until "show your face" (the dial close),
     a scene, the radar or the end of the conversation (L6, amendment 18).
   - At rest (the device idle) the dial is in front when the last applied
     dial update had home on, the dial says its cache is showable (it covers
     now and is under 12 h old, the dial's own judgment) and no dial close
     came after that update. In every other state the full face shows.
   - The dial close hides the dial now and at rest until the next applied
     dial update; the dial's data is kept (5.2). The dial object, once
     made, is never deleted.
   - Face show mode: FULL with nothing in front, SMALL with the dial, and
     RING_ONLY with the radar or a scene.
   - Long press: the radar in front, the next plane; the dial in front,
     nothing in slice 1 (the look cycle is slice 2, L2); a scene, nothing.
     With anything but the bare face in front, the dimmed screen is woken
     first (amendment 14), since a person pressing is looking at a screen.
   - The error face for 2.4 s (DESIGN-TRUTH.md section 1, the remote data
     screens): when the radar or a scene turns to error, and when the dial
     comes to the front, or is in front, showing error with no cache. It
     stops early when the screen in front changes, as CloseRadar does today.

   Updates that arrive outside a conversation are ignored here as well as by
   the board, which refuses them before applying ("not in a conversation").
   Closes and expiries act whenever the screen they name is open; the board
   still raises a seq gate only inside a conversation.

   How the board maps its DeviceState (device_state.h):
     kDeviceStateIdle      -> JAKO_SCREENS_DEVICE_IDLE
     kDeviceStateListening -> JAKO_SCREENS_DEVICE_LISTENING
     kDeviceStateSpeaking  -> JAKO_SCREENS_DEVICE_SPEAKING
     every other state     -> JAKO_SCREENS_DEVICE_OTHER (starting, Wi-Fi
       setup, connecting, notifying, upgrading, activating, audio testing,
       fatal error, unknown): not at rest and not in a conversation, so the
       full face shows and the radar or scene closes. */
#ifndef JAKO_SCREENS_H
#define JAKO_SCREENS_H

#ifdef __cplusplus
extern "C" {
#endif

/* ---- the state ---- */

/* What is in front of the face. */
typedef enum {
    JAKO_SCREENS_FRONT_NONE = 0, /* the whole face */
    JAKO_SCREENS_FRONT_DIAL,
    JAKO_SCREENS_FRONT_RADAR,
    JAKO_SCREENS_FRONT_SCENE,
    JAKO_SCREENS_FRONT_COUNT
} jako_screens_front_t;

/* The device's state, as the slot sees it (the mapping is above). */
typedef enum {
    JAKO_SCREENS_DEVICE_OTHER = 0, /* starting, connecting, notifying, any error: the full face */
    JAKO_SCREENS_DEVICE_IDLE,      /* at rest: the dial may stand in front */
    JAKO_SCREENS_DEVICE_LISTENING, /* in a conversation */
    JAKO_SCREENS_DEVICE_SPEAKING,  /* in a conversation */
    JAKO_SCREENS_DEVICE_COUNT
} jako_screens_device_t;

/* Whether the dial object exists. It is made by the first applied dial
   update and never deleted after that (the cache lives in it). */
typedef enum {
    JAKO_SCREENS_DIAL_ABSENT = 0,
    JAKO_SCREENS_DIAL_PRESENT,
    JAKO_SCREENS_DIAL_PRESENCE_COUNT
} jako_screens_dial_presence_t;

/* The home key of the last applied dial update: keep the dial at rest. */
typedef enum {
    JAKO_SCREENS_HOME_OFF = 0,
    JAKO_SCREENS_HOME_ON,
    JAKO_SCREENS_HOME_COUNT
} jako_screens_home_t;

/* The dial's own judgment of its cache: SHOWABLE when it may stand alone at
   rest (it covers now, before the end of its second day, and is under 12 h
   old). The dial reports it after each applied update; it reports the end
   of it with JAKO_SCREENS_EV_DIAL_CACHE_LAPSED. */
typedef enum {
    JAKO_SCREENS_CACHE_NONE = 0,
    JAKO_SCREENS_CACHE_SHOWABLE,
    JAKO_SCREENS_CACHE_COUNT
} jako_screens_cache_t;

/* Whether the dial was asked to the front in this conversation. */
typedef enum {
    JAKO_SCREENS_DIAL_NOT_ASKED = 0,
    JAKO_SCREENS_DIAL_ASKED,
    JAKO_SCREENS_ASKED_COUNT
} jako_screens_asked_t;

/* The dial close's mark: hidden now and at rest until the next applied
   dial update. */
typedef enum {
    JAKO_SCREENS_DIAL_ALLOWED = 0,
    JAKO_SCREENS_DIAL_HIDDEN_BY_CLOSE,
    JAKO_SCREENS_HIDE_COUNT
} jako_screens_hide_t;

/* The error face (the face shows its error state for 2.4 s). */
typedef enum {
    JAKO_SCREENS_ERROR_FACE_OFF = 0,
    JAKO_SCREENS_ERROR_FACE_SHOWING,
    JAKO_SCREENS_ERROR_FACE_COUNT
} jako_screens_error_face_t;

/* The data state of an applied update (the radar's LIVE maps to READY). */
typedef enum {
    JAKO_SCREENS_DATA_LOADING = 0,
    JAKO_SCREENS_DATA_READY,
    JAKO_SCREENS_DATA_EMPTY,
    JAKO_SCREENS_DATA_ERROR,
    JAKO_SCREENS_DATA_COUNT
} jako_screens_data_t;

typedef struct {
    jako_screens_front_t front;
    jako_screens_device_t device;
    jako_screens_dial_presence_t dial;
    jako_screens_home_t home;
    jako_screens_cache_t cache;
    jako_screens_asked_t asked;
    jako_screens_hide_t hide;
    jako_screens_error_face_t error_face;
    /* What each screen last drew, for the error face's "turns to error".
       The radar's and the scene's are LOADING whenever they are closed; the
       dial's is LOADING outside a conversation. */
    jako_screens_data_t radar_drawn;
    jako_screens_data_t scene_drawn;
    jako_screens_data_t dial_drawn;
} jako_screens_t;

/* ---- the events ---- */

typedef enum {
    JAKO_SCREENS_EV_DEVICE_STATE = 0,  /* SetDeviceState: .device */
    JAKO_SCREENS_EV_RADAR_APPLIED,     /* an admitted radar update was drawn: .data */
    JAKO_SCREENS_EV_RADAR_CLOSED,      /* the close tool */
    JAKO_SCREENS_EV_RADAR_EXPIRED,     /* the radar's own 60 s timer */
    JAKO_SCREENS_EV_SCENE_APPLIED,     /* an admitted scene update was drawn: .data */
    JAKO_SCREENS_EV_SCENE_CLOSED,      /* the close tool */
    JAKO_SCREENS_EV_SCENE_EXPIRED,     /* the scene's own hold timer */
    JAKO_SCREENS_EV_DIAL_APPLIED,      /* an admitted dial update was drawn: .data, .home, .cache, .call */
    JAKO_SCREENS_EV_DIAL_CLOSED,       /* the close tool ("show your face", "close the dial") */
    JAKO_SCREENS_EV_DIAL_CACHE_LAPSED, /* the dial's cache stopped being showable */
    JAKO_SCREENS_EV_LONG_PRESS,        /* the boot button held 1.5 s */
    JAKO_SCREENS_EV_ERROR_FACE_ELAPSED,/* 2.4 s since the output said START */
    JAKO_SCREENS_EV_LOW_BATTERY_SHOWN, /* the popup draws above every screen: the slot never moves */
    JAKO_SCREENS_EV_LOW_BATTERY_HIDDEN,
    JAKO_SCREENS_EV_COUNT
} jako_screens_event_kind_t;

/* Why a dial update came. SHOW_MY_DAY is the answer to a voice ask for the
   dial: it brings the dial to the front, closing the radar or a scene.
   REFRESH is every other update (the probe, the 60 s loop, a desk change):
   it refreshes the dial where it is and never fronts it in a conversation.
   How the board tells them apart is the dial contract's business (stage A);
   in slice 1 it is jako_dial_ask_admit: an update whose ask is not 0 and
   differs from the last ask of this conversation. ask 0 means "never
   asked": the server's probe and every refresh before "show my day" carry
   it, so the first dial update of an ordinary conversation is a REFRESH and
   the full face stays (L6). The gates reset puts the last ask back to 0. */
typedef enum {
    JAKO_SCREENS_DIAL_REFRESH = 0,
    JAKO_SCREENS_DIAL_SHOW_MY_DAY,
    JAKO_SCREENS_DIAL_CALL_COUNT
} jako_screens_dial_call_t;

typedef struct {
    jako_screens_event_kind_t kind;
    jako_screens_device_t device;  /* EV_DEVICE_STATE */
    jako_screens_data_t data;      /* EV_*_APPLIED */
    jako_screens_home_t home;      /* EV_DIAL_APPLIED */
    jako_screens_cache_t cache;    /* EV_DIAL_APPLIED: the dial's judgment after drawing it */
    jako_screens_dial_call_t call; /* EV_DIAL_APPLIED */
} jako_screens_event_t;

/* ---- the output ---- */

/* What one widget should be after the step. The radar and a scene are
   ABSENT or SHOWN (they never hide); the dial is ABSENT until its first
   update, then HIDDEN or SHOWN. */
typedef enum {
    JAKO_SCREENS_WIDGET_ABSENT = 0,
    JAKO_SCREENS_WIDGET_HIDDEN,
    JAKO_SCREENS_WIDGET_SHOWN,
    JAKO_SCREENS_WIDGET_COUNT
} jako_screens_widget_t;

/* The one object to delete now. The radar and a scene close each other, so
   at most one goes per step; the dial is never deleted. */
typedef enum {
    JAKO_SCREENS_DELETE_NONE = 0,
    JAKO_SCREENS_DELETE_RADAR,
    JAKO_SCREENS_DELETE_SCENE,
    JAKO_SCREENS_DELETE_COUNT
} jako_screens_delete_t;

/* The face's show mode; the board maps them to JAKO_FACE_SHOW_FULL,
   JAKO_FACE_SHOW_RING_ONLY and JAKO_FACE_SHOW_SMALL. */
typedef enum {
    JAKO_SCREENS_FACE_FULL = 0,
    JAKO_SCREENS_FACE_RING_ONLY,
    JAKO_SCREENS_FACE_SMALL,
    JAKO_SCREENS_FACE_COUNT
} jako_screens_face_t;

/* Where a long press goes. Slice 2 adds the dial's look cycle here. */
typedef enum {
    JAKO_SCREENS_PRESS_NONE = 0,
    JAKO_SCREENS_PRESS_NEXT_PLANE, /* jako_radar_next */
    JAKO_SCREENS_PRESS_COUNT
} jako_screens_press_t;

/* Whether to wake the dimmed screen (power_save_timer_->WakeUp()) before
   anything else in the output. */
typedef enum {
    JAKO_SCREENS_WAKE_NONE = 0,
    JAKO_SCREENS_WAKE_SCREEN,
    JAKO_SCREENS_WAKE_COUNT
} jako_screens_wake_t;

/* The seq gates. RESET_ALL: the radar's, the dial's and the scene's gates,
   and the dial's last ask (amendment 8), all back to the start. */
typedef enum {
    JAKO_SCREENS_GATES_KEEP = 0,
    JAKO_SCREENS_GATES_RESET_ALL,
    JAKO_SCREENS_GATES_COUNT
} jako_screens_gates_t;

/* The error face: START (note the time, show the error state), STOP (back
   to the state underneath), or KEEP as it is. */
typedef enum {
    JAKO_SCREENS_ERROR_FACE_KEEP = 0,
    JAKO_SCREENS_ERROR_FACE_START,
    JAKO_SCREENS_ERROR_FACE_STOP,
    JAKO_SCREENS_ERROR_FACE_CHANGE_COUNT
} jako_screens_error_face_change_t;

/* What the reducer made of the event, for the log. An ignored event leaves
   the state exactly as it was. */
typedef enum {
    JAKO_SCREENS_TAKEN = 0,
    JAKO_SCREENS_IGNORED_NOT_IN_CONVERSATION, /* an update outside listening and speaking */
    JAKO_SCREENS_IGNORED_NOT_OPEN,            /* a close or expiry for a screen that is not open */
    JAKO_SCREENS_IGNORED_BAD_EVENT,           /* a kind or a field out of its range */
    JAKO_SCREENS_VERDICT_COUNT
} jako_screens_verdict_t;

typedef struct {
    jako_screens_front_t front;
    jako_screens_widget_t radar;
    jako_screens_widget_t scene;
    jako_screens_widget_t dial;
    jako_screens_delete_t deleted;
    jako_screens_face_t face;
    jako_screens_press_t press;
    jako_screens_wake_t wake;
    jako_screens_gates_t gates;
    jako_screens_error_face_change_t error_face;
    jako_screens_verdict_t verdict;
} jako_screens_out_t;

typedef struct {
    jako_screens_t state;
    jako_screens_out_t out;
} jako_screens_step_t;

/* ---- the API ---- */

/* The state at boot: nothing in front, the device OTHER (starting), no dial. */
jako_screens_t jako_screens_init(void);

/* The one step: the new state and what the board must apply, in this order:
   wake, delete, widgets, face, press, gates, error face. */
jako_screens_step_t jako_screens_reduce(jako_screens_t state, jako_screens_event_t event);

/* The picture a state stands for, with no action (deleted NONE, press NONE,
   wake NONE, gates KEEP, error face KEEP, verdict TAKEN). For SetupUI. */
jako_screens_out_t jako_screens_view(jako_screens_t state);

/* Names for the log; "?" for a value out of range. */
const char* jako_screens_front_name(jako_screens_front_t front);
const char* jako_screens_event_name(jako_screens_event_kind_t kind);
const char* jako_screens_face_name(jako_screens_face_t face);
const char* jako_screens_verdict_name(jako_screens_verdict_t verdict);

/* ---- event constructors (C and C++) ---- */

static inline jako_screens_event_t jako_screens_ev(jako_screens_event_kind_t kind) {
    jako_screens_event_t e;
    e.kind = kind;
    e.device = JAKO_SCREENS_DEVICE_OTHER;
    e.data = JAKO_SCREENS_DATA_LOADING;
    e.home = JAKO_SCREENS_HOME_OFF;
    e.cache = JAKO_SCREENS_CACHE_NONE;
    e.call = JAKO_SCREENS_DIAL_REFRESH;
    return e;
}

static inline jako_screens_event_t jako_screens_ev_device(jako_screens_device_t device) {
    jako_screens_event_t e = jako_screens_ev(JAKO_SCREENS_EV_DEVICE_STATE);
    e.device = device;
    return e;
}

static inline jako_screens_event_t jako_screens_ev_radar_applied(jako_screens_data_t data) {
    jako_screens_event_t e = jako_screens_ev(JAKO_SCREENS_EV_RADAR_APPLIED);
    e.data = data;
    return e;
}

static inline jako_screens_event_t jako_screens_ev_scene_applied(jako_screens_data_t data) {
    jako_screens_event_t e = jako_screens_ev(JAKO_SCREENS_EV_SCENE_APPLIED);
    e.data = data;
    return e;
}

static inline jako_screens_event_t jako_screens_ev_dial_applied(jako_screens_data_t data,
                                                                jako_screens_home_t home,
                                                                jako_screens_cache_t cache,
                                                                jako_screens_dial_call_t call) {
    jako_screens_event_t e = jako_screens_ev(JAKO_SCREENS_EV_DIAL_APPLIED);
    e.data = data;
    e.home = home;
    e.cache = cache;
    e.call = call;
    return e;
}

#ifdef __cplusplus
}
#endif

#endif /* JAKO_SCREENS_H */
