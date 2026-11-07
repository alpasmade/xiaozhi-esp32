// Copyright 2026 Alpas Made. All rights reserved.
/* The dial's wire data, version 1: the parser for the data string of the
   self.dial.update tool and the gate the board keeps per socket. Plain C
   with cJSON and no device header, so the Mac renderer runs the same code
   the board does. The contract is docs/screens.md, section 5.2 of the plan:

   {"v":1,"seq":S,"state":"loading"|"ready"|"empty"|"error","src":"desk"|"sample",
    "age":A,"stale":T,"home":H,"t":X,"days":[[W0,D0],[W1,D1]],"ask":K,
    "view":"today"|"tomorrow","look":L,"sun":[S0,S1],"ev":[[d,s,e,"short"],...]}

   The rules are the contract's device rules: numbers are clamped into their
   ranges, strings are cut to their lengths and keep only printable ASCII,
   events past 32 and unknown keys are ignored, a malformed event is
   skipped. Data that is not one JSON object (text after it included), is
   nested past 3, lacks v, seq, state or src, has v other than 1, a seq
   below 1, or names an unknown state, src or view is refused, and a
   refusal leaves the caller's last good update untouched.

   Slice 1: ask, view and look are read and clamped, and the gate keeps the
   last ask, but the widget draws only the today view at look 0, so the
   tool's result is always 0. */
#ifndef JAKO_DIAL_DATA_H
#define JAKO_DIAL_DATA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define JAKO_DIAL_DATA_MAX 3072   /* bytes; the tool's SetMaxLength */
#define JAKO_DIAL_MAX_EVENTS 32
#define JAKO_DIAL_NAME_MAX 10     /* an event's short name */
#define JAKO_DIAL_MAX_DEPTH 3     /* { ev:[ [ is the deepest legal nesting */
#define JAKO_DIAL_MAX_VALUES 240  /* values counted before cJSON: the largest legal update has 183 */
#define JAKO_DIAL_STALE_DEFAULT_S 7200
#define JAKO_DIAL_DAY_S 86400
#define JAKO_DIAL_DAY_MIN 1440

typedef enum {
    JAKO_DIAL_STATE_LOADING = 0, /* no store answer yet (the probe) */
    JAKO_DIAL_STATE_READY,       /* one or more events today */
    JAKO_DIAL_STATE_EMPTY,       /* none today (tomorrow's may still be listed) */
    JAKO_DIAL_STATE_ERROR,       /* the store could not be read; no events */
} jako_dial_state_t;

typedef enum {
    JAKO_DIAL_SRC_DESK = 0,
    JAKO_DIAL_SRC_SAMPLE, /* the invented sample day: the dial says "Sample day" */
} jako_dial_src_t;

typedef enum {
    JAKO_DIAL_VIEW_TODAY = 0,
    JAKO_DIAL_VIEW_TOMORROW, /* read and kept; not drawn in slice 1 */
} jako_dial_view_t;

typedef struct {
    int8_t weekday; /* 0 Sunday .. 6, or -1 when the update did not say */
    int8_t mday;    /* 1 .. 31, or 0 when the update did not say */
} jako_dial_day_t;

typedef struct {
    uint8_t d;  /* 0 today, 1 tomorrow (of the update's day) */
    int16_t s;  /* -1440 .. 2879 minutes from day d's midnight */
    int16_t e;  /* s + 1 .. s + 1440, at most 2880 */
    char name[JAKO_DIAL_NAME_MAX + 1]; /* printable ASCII, "" when none or only spaces */
} jako_dial_event_t;

typedef struct {
    int32_t seq;               /* 1 .. 2147483647 */
    jako_dial_state_t state;
    jako_dial_src_t src;
    int32_t age;               /* 0 .. 86400 s since the store was read, at send time */
    int32_t stale;             /* 60 .. 86400 s; 7200 when absent */
    uint8_t home;              /* 0 or 1: keep the dial at rest; 0 when absent */
    int32_t t;                 /* 0 .. 86399 local seconds since today's midnight; 0 when absent */
    jako_dial_day_t days[2];   /* today and tomorrow */
    uint16_t ask;              /* 0 .. 65535 */
    jako_dial_view_t view;     /* today when absent */
    int16_t look;              /* 0 .. 600 minutes */
    int16_t sun[2];            /* sunset minutes 0 .. 1439 today and tomorrow, -1 for none */
    uint8_t ev_n;              /* 0 unless state is ready or empty */
    jako_dial_event_t ev[JAKO_DIAL_MAX_EVENTS];
} jako_dial_update_t;

typedef enum {
    JAKO_DIAL_PARSE_OK = 0,
    JAKO_DIAL_PARSE_EMPTY,    /* no data, or longer than 3072 bytes */
    JAKO_DIAL_PARSE_TOO_DEEP, /* nested past 3 */
    JAKO_DIAL_PARSE_TOO_MANY, /* more than 240 values (1, plus one per bracket and comma outside strings) */
    JAKO_DIAL_PARSE_NOT_JSON, /* cJSON refused it (bad text or no memory), or it is not one object */
    JAKO_DIAL_PARSE_MISSING,  /* v, seq, state or src missing or of the wrong type, or seq below 1 */
    JAKO_DIAL_PARSE_VERSION,  /* v is not 1 */
    JAKO_DIAL_PARSE_STATE,    /* state is not one of the four names */
    JAKO_DIAL_PARSE_SRC,      /* src is not "desk" or "sample" */
    JAKO_DIAL_PARSE_VIEW,     /* view is present and not "today" or "tomorrow" */
} jako_dial_parse_result_t;

/* Parse len bytes of data into *out. On anything but JAKO_DIAL_PARSE_OK,
   *out is left exactly as it was, so the previous picture stays. The data
   need not be NUL terminated. The nesting and the values are counted
   before cJSON sees the text, so cJSON builds at most about 280 nodes (240
   values and their containers; the largest legal update builds about 220)
   on the main task, and everything is freed before this returns. */
jako_dial_parse_result_t jako_dial_parse(const char* data, size_t len, jako_dial_update_t* out);

/* A short English reason for a log line or the tool's error text ("dial: <reason>"). */
const char* jako_dial_parse_reason(jako_dial_parse_result_t r);

/* The seq and ask rules, one per socket, kept by the board. */
typedef struct {
    int32_t last_seq; /* 0 at the start of a conversation */
    int32_t last_ask; /* 0 at the start of a conversation: never asked */
} jako_dial_seq_t;

/* true when seq is newer than every update and close seen, and records it.
   false: do not draw this update, answer with the current look. */
bool jako_dial_seq_admit(jako_dial_seq_t* g, int32_t seq);
/* The close tool: last_seq = max(last_seq, seq), so a late update cannot reopen the dial. */
void jako_dial_seq_close(jako_dial_seq_t* g, int32_t seq);
/* true once per new ask value (view and look are applied once per ask), and records it.
   ask 0 means "never asked" and is never new: the server's probe and every refresh before
   "show my day" carry ask 0, and a voice ask raises it to 1 .. 65535 (wrapping to 1, never 0).
   The board maps true to JAKO_SCREENS_DIAL_SHOW_MY_DAY and false to JAKO_SCREENS_DIAL_REFRESH,
   so an ordinary conversation's first dial update never fronts the dial (L6). */
bool jako_dial_ask_admit(jako_dial_seq_t* g, uint16_t ask);
/* The device state left listening and speaking: a new socket starts its ids at 1 and its asks
   again, so last_seq and last_ask go to 0 together (amendment 8). */
void jako_dial_seq_reset(jako_dial_seq_t* g);

#ifdef __cplusplus
}
#endif

#endif /* JAKO_DIAL_DATA_H */
