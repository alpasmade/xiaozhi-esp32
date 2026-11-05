// Copyright 2026 Alpas Made. All rights reserved.
/* Events from the body: turns the motion chip's readings and the power
   chip's charging flags into named events (DESIGN-TRUTH.md section 5,
   docs/body-events.md). Plain C with no ESP or LVGL headers, so the Mac
   preview compiles and tests the same file (face-preview/body_check.c).

   The two detectors are separate structs so their callers never share
   state: the motion one is fed from the LVGL task, the power one from the
   main task. Times are uint32 milliseconds compared by unsigned
   subtraction, so the 49 day wrap is harmless.

   Every number below is a first guess to tune on the board; each names the
   line in docs/hardware-tests.md that tunes it. */
#ifndef JAKO_BODY_H
#define JAKO_BODY_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- the plausibility gate (hardware test: "Motion data is real") ---- */
/* First guess, tune on the board: samples per check window, 2 s at the 80 ms poll. */
#define JAKO_BODY_SANE_SAMPLES 25
/* First guess, tune on the board: a sample is plausible when its magnitude is 0.7 to 1.3 g. */
#define JAKO_BODY_SANE_MIN_MG 700
#define JAKO_BODY_SANE_MAX_MG 1300
/* First guess, tune on the board: a byte order arms when 23 of 25 samples (90 percent) are plausible. */
#define JAKO_BODY_SANE_PASS 23
/* First guess, tune on the board: a window where neither byte order reaches 13 of 25
   (about half) counts against the chip; one in between (the device moving) does not. */
#define JAKO_BODY_SANE_HOPE 13
/* First guess, tune on the board: three bad windows in a row and the chip is refused for good. */
#define JAKO_BODY_SANE_TRIES 3
/* The chip's scale at plus or minus 8 g: 4096 counts per g (QMI8658 datasheet). */
#define JAKO_BODY_LSB_PER_G 4096

/* ---- shake (hardware tests: "Shake" and "False shakes") ---- */
/* First guess, tune on the board: a hit is |dx| + |dy| + |dz| over 2000 mg between two
   samples 80 ms apart. The number comes from a single-hit detector on another product. */
#define JAKO_BODY_SHAKE_SCORE_MG 2000
/* First guess, tune on the board: a shake is three hits that share no sample... */
#define JAKO_BODY_SHAKE_HITS 3
/* ...within 500 ms. A knock on one sample makes two hits that share it, and an impact that
   rings across two samples, or a set-down with one bounce, makes two hits that do not; three
   is the fewest that none of those reach, and a real shake passes it in 320 ms. */
#define JAKO_BODY_SHAKE_WINDOW_MS 500u
/* First guess, tune on the board: no second shake for 2 s after one. */
#define JAKO_BODY_SHAKE_COOLDOWN_MS 2000u

/* ---- power (hardware tests: "Cable in" and "Battery full") ---- */
/* First guess, tune on the board: a cable in within 3 s of the last cable out is the same
   plug-in (a wiggle) and is dropped. */
#define JAKO_BODY_CABLE_SETTLE_MS 3000u
/* First guess, tune on the board: battery full needs standby (neither charging nor
   discharging) on two reads in a row spanning at least 1.5 s after charging, so an
   unplug that passes through standby for one read is not a full battery. */
#define JAKO_BODY_FULL_READS 2
#define JAKO_BODY_FULL_HOLD_MS 1500u

typedef enum {
    JAKO_BODY_NONE = 0,
    JAKO_BODY_SHAKE,
    JAKO_BODY_CABLE_IN,
    JAKO_BODY_BATTERY_FULL,
} jako_body_event_t;

/* Named states, not flags: the gate can fail. */
typedef enum {
    JAKO_BODY_MOTION_CHECKING = 0, /* collecting a window to find the byte order */
    JAKO_BODY_MOTION_ARMED,        /* plausible; the shake detector runs */
    JAKO_BODY_MOTION_REFUSED,      /* never plausible; no event is ever sent */
} jako_body_motion_state_t;

typedef enum {
    JAKO_BODY_ORDER_LITTLE = 0, /* low byte first, as the chip is set up */
    JAKO_BODY_ORDER_BIG,
} jako_body_byte_order_t;

typedef struct {
    jako_body_motion_state_t state;
    jako_body_byte_order_t order;
    /* the gate */
    uint8_t window_n, ok_little, ok_big, bad_windows;
    /* the shake detector */
    bool has_prev;
    int32_t prev[3];
    uint32_t index;         /* samples seen while armed */
    uint32_t last_hit_index;
    uint8_t hits;
    uint32_t first_hit_ms;
    bool cooling;
    uint32_t cooldown_from_ms;
} jako_body_motion_t;

typedef enum {
    JAKO_BODY_POWER_UNSEEN = 0, /* no read yet: the first only seeds */
    JAKO_BODY_POWER_SEEN,
} jako_body_power_seen_t;

typedef enum {
    JAKO_BODY_FULL_IDLE = 0, /* not charging in this plug-in */
    JAKO_BODY_FULL_CHARGING, /* charging seen; waiting for standby */
    JAKO_BODY_FULL_STANDBY,  /* standby seen after charging; confirming */
    JAKO_BODY_FULL_SENT,     /* sent once; quiet until the next plug-in */
} jako_body_full_state_t;

typedef struct {
    jako_body_power_seen_t seen;
    bool discharging;
    bool cable_out_seen;
    uint32_t cable_out_ms;
    jako_body_full_state_t full;
    uint8_t standby_reads;
    uint32_t standby_from_ms;
} jako_body_power_t;

void jako_body_motion_init(jako_body_motion_t* m);
/* One accelerometer read: the six data bytes exactly as they come off the
   chip (x low, x high, y, z). Returns SHAKE or NONE. */
jako_body_event_t jako_body_motion_sample(jako_body_motion_t* m, const uint8_t raw[6], uint32_t now_ms);
/* A read failed: the next sample is not compared with the last good one. */
void jako_body_motion_gap(jako_body_motion_t* m);
/* One sample in milli-g, in the armed byte order (for logs). False before arming. */
bool jako_body_motion_decode(const jako_body_motion_t* m, const uint8_t raw[6], int32_t out_mg[3]);

void jako_body_power_init(jako_body_power_t* p);
/* One power reading: the flags the board already reads. Returns CABLE_IN,
   BATTERY_FULL or NONE. */
jako_body_event_t jako_body_power_sample(jako_body_power_t* p, bool charging, bool discharging, uint32_t now_ms);

#ifdef __cplusplus
}
#endif

#endif /* JAKO_BODY_H */
