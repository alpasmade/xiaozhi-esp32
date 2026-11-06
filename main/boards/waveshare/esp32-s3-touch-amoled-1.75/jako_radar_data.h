// Copyright 2026 Alpas Made. All rights reserved.
/* The radar's wire data, version 1: the parser for the data string of the
   self.radar.update tool and the seq gate the board keeps per socket.
   Plain C with cJSON and no device header, so the Mac renderer runs the
   same code the board does.

   The rules are the contract's DEVICE RULES: numbers are clamped into
   their ranges, strings are cut to their lengths and keep only printable
   ASCII, planes past 16, trail points past 3 and unknown keys are ignored.
   Data that is not JSON (text after the object included), is nested
   deeper than the contract's shape, or lacks v, seq or state is refused,
   and so is any v other than 1, a seq below 1 and a src that is not "feed"
   or "sample"; a refusal leaves the caller's last good update untouched.
   The optional "ln" picks the style: a number equal to 1 is the line, and
   anything else (absent, 0, true, "1", null) is the ring; it is never a
   refusal. */
#ifndef JAKO_RADAR_DATA_H
#define JAKO_RADAR_DATA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define JAKO_RADAR_DATA_MAX 3072  /* bytes; the tool's SetMaxLength */
#define JAKO_RADAR_MAX_PLANES 16
#define JAKO_RADAR_MAX_TRAIL 3
#define JAKO_RADAR_NAME_MAX 18    /* s: line 1 */
#define JAKO_RADAR_TYPE_MAX 4     /* c: the type code */
#define JAKO_RADAR_MAX_DEPTH 5    /* { planes:[ { tr:[ [ is the deepest legal nesting */
#define JAKO_RADAR_IN_RANGE_D 154 /* d in tenths of a mile: inside 15.4 mi a plane is on the scope */

typedef enum {
    JAKO_RADAR_STATE_LOADING = 0, /* the show is open, no data yet */
    JAKO_RADAR_STATE_LIVE,        /* one or more planes */
    JAKO_RADAR_STATE_EMPTY,       /* fetched, nothing airborne in range */
    JAKO_RADAR_STATE_ERROR,       /* no data in this show, or data older than 120 s */
} jako_radar_state_t;

typedef enum {
    JAKO_RADAR_SRC_FEED = 0,
    JAKO_RADAR_SRC_SAMPLE, /* invented flights: the screen says "Sample sky" */
} jako_radar_src_t;

/* How the scope sweeps, from the update's "ln". */
typedef enum {
    JAKO_RADAR_STYLE_RING = 0, /* the default: a light runs round the 15 mile ring */
    JAKO_RADAR_STYLE_LINE,     /* "ln":1, the sweeping line with its wedge, the data clock */
} jako_radar_style_t;

typedef struct {
    int16_t b; /* 0..359, degrees from You, clockwise from north */
    int16_t d; /* 0..999, tenths of a statute mile */
} jako_radar_point_t;

typedef struct {
    uint16_t id; /* 1..65535, stable per aircraft for the show */
    int16_t b;   /* 0..359 */
    int16_t d;   /* 0..999 */
    int16_t t;   /* -1..359, track; -1 draws a round dot */
    int32_t a;   /* -1000..60000 feet */
    char s[JAKO_RADAR_NAME_MAX + 1]; /* line 1, printable ASCII, never empty or all spaces after parsing */
    char c[JAKO_RADAR_TYPE_MAX + 1]; /* type code, may be empty */
    uint8_t trail_n;
    jako_radar_point_t trail[JAKO_RADAR_MAX_TRAIL]; /* newest first */
} jako_radar_plane_t;

typedef struct {
    int32_t seq;     /* 1..2147483647 */
    jako_radar_state_t state;
    jako_radar_src_t src;
    int32_t age;     /* 0..3600 s since the flights were fetched, at send time */
    int32_t pos_age; /* 0..86400 s since the phone's fix, at send time */
    uint16_t focus;  /* f: the server's focus id, 0 for none */
    jako_radar_style_t style; /* ln: the line only when it is a number equal to 1 */
    uint8_t plane_n; /* 0 unless state is live */
    jako_radar_plane_t planes[JAKO_RADAR_MAX_PLANES];
} jako_radar_update_t;

typedef enum {
    JAKO_RADAR_PARSE_OK = 0,
    JAKO_RADAR_PARSE_EMPTY,     /* no data, or longer than 3072 bytes */
    JAKO_RADAR_PARSE_TOO_DEEP,  /* nested deeper than the contract allows */
    JAKO_RADAR_PARSE_NOT_JSON,  /* cJSON refused it (bad text or no memory), or it is not an object */
    JAKO_RADAR_PARSE_MISSING,   /* v, seq or state is missing or of the wrong type, or seq is below 1 */
    JAKO_RADAR_PARSE_VERSION,   /* v is not 1 */
    JAKO_RADAR_PARSE_STATE,     /* state is not one of the four names */
    JAKO_RADAR_PARSE_SRC,       /* src is missing or not "feed" or "sample" */
} jako_radar_parse_result_t;

/* Parse len bytes of data into *out. On anything but JAKO_RADAR_PARSE_OK,
   *out is left exactly as it was, so the previous picture stays. The data
   need not be NUL terminated. cJSON allocates while parsing and everything
   is freed before this returns. */
jako_radar_parse_result_t jako_radar_parse(const char* data, size_t len, jako_radar_update_t* out);

/* A short English reason for a log line or the tool's error text. */
const char* jako_radar_parse_reason(jako_radar_parse_result_t r);

/* The seq rules, one per socket, kept by the board (the radar object lives
   only while the radar is open, the gate outlives it). */
typedef struct {
    int32_t last_seq; /* 0 at the start of a conversation */
} jako_radar_seq_t;

/* true when seq is newer than every update and close seen, and records it.
   false: do not draw this update, answer with the current focus. */
bool jako_radar_seq_admit(jako_radar_seq_t* g, int32_t seq);
/* The close tool: last_seq = max(last_seq, seq), so a late update cannot reopen the radar. */
void jako_radar_seq_close(jako_radar_seq_t* g, int32_t seq);
/* The device state left listening and speaking: a new socket starts its ids at 1. */
void jako_radar_seq_reset(jako_radar_seq_t* g);

#ifdef __cplusplus
}
#endif

#endif /* JAKO_RADAR_DATA_H */
