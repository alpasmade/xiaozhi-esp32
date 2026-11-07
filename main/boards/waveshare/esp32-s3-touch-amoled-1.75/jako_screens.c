// Copyright 2026 Alpas Made. All rights reserved.
/* The screen slot as a pure reducer. The rules are in jako_screens.h. */
#include "jako_screens.h"

#include <stdbool.h>

static bool in_conversation(jako_screens_device_t device) {
    return device == JAKO_SCREENS_DEVICE_LISTENING || device == JAKO_SCREENS_DEVICE_SPEAKING;
}

static bool overlay_open(jako_screens_front_t front) {
    return front == JAKO_SCREENS_FRONT_RADAR || front == JAKO_SCREENS_FRONT_SCENE;
}

static bool event_in_range(jako_screens_event_t e) {
    if ((unsigned)e.kind >= (unsigned)JAKO_SCREENS_EV_COUNT) {
        return false;
    }
    switch (e.kind) {
        case JAKO_SCREENS_EV_DEVICE_STATE:
            return (unsigned)e.device < (unsigned)JAKO_SCREENS_DEVICE_COUNT;
        case JAKO_SCREENS_EV_RADAR_APPLIED:
        case JAKO_SCREENS_EV_SCENE_APPLIED:
            return (unsigned)e.data < (unsigned)JAKO_SCREENS_DATA_COUNT;
        case JAKO_SCREENS_EV_DIAL_APPLIED:
            return (unsigned)e.data < (unsigned)JAKO_SCREENS_DATA_COUNT &&
                   (unsigned)e.home < (unsigned)JAKO_SCREENS_HOME_COUNT &&
                   (unsigned)e.cache < (unsigned)JAKO_SCREENS_CACHE_COUNT &&
                   (unsigned)e.call < (unsigned)JAKO_SCREENS_DIAL_CALL_COUNT;
        default:
            return true;
    }
}

/* With no radar and no scene open: the dial, or the whole face. */
static jako_screens_front_t dial_or_face(const jako_screens_t* s) {
    if (s->dial != JAKO_SCREENS_DIAL_PRESENT || s->hide == JAKO_SCREENS_DIAL_HIDDEN_BY_CLOSE) {
        return JAKO_SCREENS_FRONT_NONE;
    }
    if (in_conversation(s->device)) {
        /* An ordinary conversation is the full face (L6). */
        return s->asked == JAKO_SCREENS_DIAL_ASKED ? JAKO_SCREENS_FRONT_DIAL : JAKO_SCREENS_FRONT_NONE;
    }
    if (s->device == JAKO_SCREENS_DEVICE_IDLE && s->home == JAKO_SCREENS_HOME_ON &&
        s->cache == JAKO_SCREENS_CACHE_SHOWABLE) {
        return JAKO_SCREENS_FRONT_DIAL;
    }
    return JAKO_SCREENS_FRONT_NONE;
}

jako_screens_t jako_screens_init(void) {
    jako_screens_t s;
    s.front = JAKO_SCREENS_FRONT_NONE;
    s.device = JAKO_SCREENS_DEVICE_OTHER;
    s.dial = JAKO_SCREENS_DIAL_ABSENT;
    s.home = JAKO_SCREENS_HOME_OFF;
    s.cache = JAKO_SCREENS_CACHE_NONE;
    s.asked = JAKO_SCREENS_DIAL_NOT_ASKED;
    s.hide = JAKO_SCREENS_DIAL_ALLOWED;
    s.error_face = JAKO_SCREENS_ERROR_FACE_OFF;
    s.radar_drawn = JAKO_SCREENS_DATA_LOADING;
    s.scene_drawn = JAKO_SCREENS_DATA_LOADING;
    s.dial_drawn = JAKO_SCREENS_DATA_LOADING;
    return s;
}

jako_screens_out_t jako_screens_view(jako_screens_t s) {
    jako_screens_out_t o;
    o.front = s.front;
    o.radar = s.front == JAKO_SCREENS_FRONT_RADAR ? JAKO_SCREENS_WIDGET_SHOWN : JAKO_SCREENS_WIDGET_ABSENT;
    o.scene = s.front == JAKO_SCREENS_FRONT_SCENE ? JAKO_SCREENS_WIDGET_SHOWN : JAKO_SCREENS_WIDGET_ABSENT;
    if (s.dial != JAKO_SCREENS_DIAL_PRESENT) {
        o.dial = JAKO_SCREENS_WIDGET_ABSENT;
    } else {
        o.dial = s.front == JAKO_SCREENS_FRONT_DIAL ? JAKO_SCREENS_WIDGET_SHOWN : JAKO_SCREENS_WIDGET_HIDDEN;
    }
    o.deleted = JAKO_SCREENS_DELETE_NONE;
    switch (s.front) {
        case JAKO_SCREENS_FRONT_DIAL:
            o.face = JAKO_SCREENS_FACE_SMALL;
            break;
        case JAKO_SCREENS_FRONT_RADAR:
        case JAKO_SCREENS_FRONT_SCENE:
            o.face = JAKO_SCREENS_FACE_RING_ONLY;
            break;
        default:
            o.face = JAKO_SCREENS_FACE_FULL;
            break;
    }
    o.press = JAKO_SCREENS_PRESS_NONE;
    o.wake = JAKO_SCREENS_WAKE_NONE;
    o.gates = JAKO_SCREENS_GATES_KEEP;
    o.error_face = JAKO_SCREENS_ERROR_FACE_KEEP;
    o.verdict = JAKO_SCREENS_TAKEN;
    return o;
}

static jako_screens_step_t ignored(jako_screens_t s, jako_screens_verdict_t why) {
    jako_screens_step_t step;
    step.state = s;
    step.out = jako_screens_view(s);
    step.out.verdict = why;
    return step;
}

jako_screens_step_t jako_screens_reduce(jako_screens_t s, jako_screens_event_t e) {
    if (!event_in_range(e)) {
        return ignored(s, JAKO_SCREENS_IGNORED_BAD_EVENT);
    }
    const jako_screens_t before = s;
    jako_screens_press_t press = JAKO_SCREENS_PRESS_NONE;
    jako_screens_wake_t wake = JAKO_SCREENS_WAKE_NONE;
    jako_screens_gates_t gates = JAKO_SCREENS_GATES_KEEP;
    jako_screens_error_face_change_t error_face = JAKO_SCREENS_ERROR_FACE_KEEP;
    /* The dial's error face waits for the front to be settled. */
    bool dial_error_candidate = false;

    switch (e.kind) {
        case JAKO_SCREENS_EV_DEVICE_STATE:
            s.device = e.device;
            if (!in_conversation(s.device)) {
                /* The radar and scenes ride on the conversation; the next
                   socket's ids start again at 1. Idempotent, as today. */
                if (overlay_open(s.front)) {
                    s.front = JAKO_SCREENS_FRONT_NONE;
                }
                s.asked = JAKO_SCREENS_DIAL_NOT_ASKED;
                s.dial_drawn = JAKO_SCREENS_DATA_LOADING;
                gates = JAKO_SCREENS_GATES_RESET_ALL;
            }
            break;

        case JAKO_SCREENS_EV_RADAR_APPLIED:
        case JAKO_SCREENS_EV_SCENE_APPLIED: {
            if (!in_conversation(s.device)) {
                return ignored(before, JAKO_SCREENS_IGNORED_NOT_IN_CONVERSATION);
            }
            const bool radar = e.kind == JAKO_SCREENS_EV_RADAR_APPLIED;
            jako_screens_data_t* drawn = radar ? &s.radar_drawn : &s.scene_drawn;
            /* drawn is LOADING whenever this screen is closed, so a screen
               that opens on an error starts the error face too. */
            if (e.data == JAKO_SCREENS_DATA_ERROR && *drawn != JAKO_SCREENS_DATA_ERROR) {
                error_face = JAKO_SCREENS_ERROR_FACE_START;
            }
            *drawn = e.data;
            s.front = radar ? JAKO_SCREENS_FRONT_RADAR : JAKO_SCREENS_FRONT_SCENE;
            s.asked = JAKO_SCREENS_DIAL_NOT_ASKED;
            break;
        }

        case JAKO_SCREENS_EV_RADAR_CLOSED:
        case JAKO_SCREENS_EV_RADAR_EXPIRED:
            if (s.front != JAKO_SCREENS_FRONT_RADAR) {
                return ignored(before, JAKO_SCREENS_IGNORED_NOT_OPEN);
            }
            s.front = JAKO_SCREENS_FRONT_NONE;
            break;

        case JAKO_SCREENS_EV_SCENE_CLOSED:
        case JAKO_SCREENS_EV_SCENE_EXPIRED:
            if (s.front != JAKO_SCREENS_FRONT_SCENE) {
                return ignored(before, JAKO_SCREENS_IGNORED_NOT_OPEN);
            }
            s.front = JAKO_SCREENS_FRONT_NONE;
            break;

        case JAKO_SCREENS_EV_DIAL_APPLIED:
            if (!in_conversation(s.device)) {
                return ignored(before, JAKO_SCREENS_IGNORED_NOT_IN_CONVERSATION);
            }
            s.dial = JAKO_SCREENS_DIAL_PRESENT;
            s.home = e.home;
            s.cache = e.cache;
            s.hide = JAKO_SCREENS_DIAL_ALLOWED;
            if (e.call == JAKO_SCREENS_DIAL_SHOW_MY_DAY) {
                s.asked = JAKO_SCREENS_DIAL_ASKED;
                s.front = JAKO_SCREENS_FRONT_NONE; /* the radar or a scene closes */
            }
            /* "No calendar" with nothing cached: the error face, once, when
               the dial comes to the front with it or turns to it in front. */
            if (e.data == JAKO_SCREENS_DATA_ERROR && e.cache == JAKO_SCREENS_CACHE_NONE &&
                (before.front != JAKO_SCREENS_FRONT_DIAL || before.dial_drawn != JAKO_SCREENS_DATA_ERROR)) {
                dial_error_candidate = true;
            }
            s.dial_drawn = e.data;
            break;

        case JAKO_SCREENS_EV_DIAL_CLOSED:
            if (s.dial != JAKO_SCREENS_DIAL_PRESENT) {
                return ignored(before, JAKO_SCREENS_IGNORED_NOT_OPEN);
            }
            s.hide = JAKO_SCREENS_DIAL_HIDDEN_BY_CLOSE;
            s.asked = JAKO_SCREENS_DIAL_NOT_ASKED;
            break;

        case JAKO_SCREENS_EV_DIAL_CACHE_LAPSED:
            if (s.dial != JAKO_SCREENS_DIAL_PRESENT) {
                return ignored(before, JAKO_SCREENS_IGNORED_NOT_OPEN);
            }
            s.cache = JAKO_SCREENS_CACHE_NONE;
            break;

        case JAKO_SCREENS_EV_LONG_PRESS:
            if (s.front != JAKO_SCREENS_FRONT_NONE) {
                wake = JAKO_SCREENS_WAKE_SCREEN;
            }
            if (s.front == JAKO_SCREENS_FRONT_RADAR) {
                press = JAKO_SCREENS_PRESS_NEXT_PLANE;
            }
            break;

        case JAKO_SCREENS_EV_ERROR_FACE_ELAPSED:
            if (s.error_face != JAKO_SCREENS_ERROR_FACE_SHOWING) {
                return ignored(before, JAKO_SCREENS_IGNORED_NOT_OPEN);
            }
            s.error_face = JAKO_SCREENS_ERROR_FACE_OFF;
            error_face = JAKO_SCREENS_ERROR_FACE_STOP;
            break;

        case JAKO_SCREENS_EV_LOW_BATTERY_SHOWN:
        case JAKO_SCREENS_EV_LOW_BATTERY_HIDDEN:
            /* The popup draws above every screen; the slot stays. */
            break;

        default:
            return ignored(before, JAKO_SCREENS_IGNORED_BAD_EVENT);
    }

    /* Settle the front: with no radar or scene, the dial or the face. */
    if (!overlay_open(s.front)) {
        s.front = dial_or_face(&s);
    }
    if (s.front != JAKO_SCREENS_FRONT_RADAR) {
        s.radar_drawn = JAKO_SCREENS_DATA_LOADING;
    }
    if (s.front != JAKO_SCREENS_FRONT_SCENE) {
        s.scene_drawn = JAKO_SCREENS_DATA_LOADING;
    }
    if (dial_error_candidate && s.front == JAKO_SCREENS_FRONT_DIAL) {
        error_face = JAKO_SCREENS_ERROR_FACE_START;
    }

    /* The error face belongs to the screen that raised it: a change of
       front ends it early, unless this very step starts it again. */
    if (error_face == JAKO_SCREENS_ERROR_FACE_START) {
        s.error_face = JAKO_SCREENS_ERROR_FACE_SHOWING;
    } else if (s.error_face == JAKO_SCREENS_ERROR_FACE_SHOWING && s.front != before.front) {
        s.error_face = JAKO_SCREENS_ERROR_FACE_OFF;
        error_face = JAKO_SCREENS_ERROR_FACE_STOP;
    }

    jako_screens_step_t step;
    step.state = s;
    step.out = jako_screens_view(s);
    if (before.front == JAKO_SCREENS_FRONT_RADAR && s.front != JAKO_SCREENS_FRONT_RADAR) {
        step.out.deleted = JAKO_SCREENS_DELETE_RADAR;
    } else if (before.front == JAKO_SCREENS_FRONT_SCENE && s.front != JAKO_SCREENS_FRONT_SCENE) {
        step.out.deleted = JAKO_SCREENS_DELETE_SCENE;
    }
    step.out.press = press;
    step.out.wake = wake;
    step.out.gates = gates;
    step.out.error_face = error_face;
    step.out.verdict = JAKO_SCREENS_TAKEN;
    return step;
}

const char* jako_screens_front_name(jako_screens_front_t front) {
    switch (front) {
        case JAKO_SCREENS_FRONT_NONE: return "face";
        case JAKO_SCREENS_FRONT_DIAL: return "dial";
        case JAKO_SCREENS_FRONT_RADAR: return "radar";
        case JAKO_SCREENS_FRONT_SCENE: return "scene";
        default: return "?";
    }
}

const char* jako_screens_event_name(jako_screens_event_kind_t kind) {
    switch (kind) {
        case JAKO_SCREENS_EV_DEVICE_STATE: return "device state";
        case JAKO_SCREENS_EV_RADAR_APPLIED: return "radar update";
        case JAKO_SCREENS_EV_RADAR_CLOSED: return "radar close";
        case JAKO_SCREENS_EV_RADAR_EXPIRED: return "radar expired";
        case JAKO_SCREENS_EV_SCENE_APPLIED: return "scene update";
        case JAKO_SCREENS_EV_SCENE_CLOSED: return "scene close";
        case JAKO_SCREENS_EV_SCENE_EXPIRED: return "scene expired";
        case JAKO_SCREENS_EV_DIAL_APPLIED: return "dial update";
        case JAKO_SCREENS_EV_DIAL_CLOSED: return "dial close";
        case JAKO_SCREENS_EV_DIAL_CACHE_LAPSED: return "dial cache lapsed";
        case JAKO_SCREENS_EV_LONG_PRESS: return "long press";
        case JAKO_SCREENS_EV_ERROR_FACE_ELAPSED: return "error face elapsed";
        case JAKO_SCREENS_EV_LOW_BATTERY_SHOWN: return "low battery shown";
        case JAKO_SCREENS_EV_LOW_BATTERY_HIDDEN: return "low battery hidden";
        default: return "?";
    }
}

const char* jako_screens_face_name(jako_screens_face_t face) {
    switch (face) {
        case JAKO_SCREENS_FACE_FULL: return "full";
        case JAKO_SCREENS_FACE_RING_ONLY: return "ring only";
        case JAKO_SCREENS_FACE_SMALL: return "small";
        default: return "?";
    }
}

const char* jako_screens_verdict_name(jako_screens_verdict_t verdict) {
    switch (verdict) {
        case JAKO_SCREENS_TAKEN: return "taken";
        case JAKO_SCREENS_IGNORED_NOT_IN_CONVERSATION: return "ignored: not in a conversation";
        case JAKO_SCREENS_IGNORED_NOT_OPEN: return "ignored: not open";
        case JAKO_SCREENS_IGNORED_BAD_EVENT: return "ignored: bad event";
        default: return "?";
    }
}
