// Copyright 2026 Alpas Made. All rights reserved.
/* A scene's wire data, version 1: the parser for the data string of the
   self.scene.update tool and the seq gate the board keeps per socket.
   Plain C with cJSON and no device header, so the Mac renderer runs the
   same code the board does.

   The data, keys in this order (the order is the server's promise; the
   device reads the keys wherever they are):

     {"v":1,"seq":S,"id":"money-owed","state":"loading"|"ready"|"empty"|"error",
      "src":"sample"|"desk","age":A,"stale":T,"hold":H,"st":"Sample data",
      "tx":[[x,y,size,opa,align,"text"],...],"sh":"..."}

   The envelope and tx are read with cJSON. sh is ONE string holding every
   other shape, read from the data's own bytes by a small integer scanner
   that allocates nothing. Shapes are separated by ";". A shape is one
   lower-case kind letter, then its integers, each after ONE space. Only a
   to z, 0 to 9, space and ";" are allowed; no sign, no other character, at
   most 5 digits a number; an empty string is allowed, a trailing ";" is
   refused. The kinds (x, y in screen px 0 to 465, opa 0 to 255, angles in
   whole degrees clockwise from twelve, cap 0 square or 1 round):

     c x y r w opa                  circle outline, outer radius r 1..197, width w 1..64, w <= r
     a x y r w a0 a1 opa cap        arc as c, a0 0..359, a1 a0+1..a0+360
     d x y r opa                    filled disc, r 1..64
     l x1 y1 x2 y2 w opa cap        line, w 1..32
     p w opa cap x1 y1 x2 y2 ...    polyline, w 1..32, 2 to 64 points
     g opa x1 y1 x2 y2 x3 y3 ...    filled triangles, 1 to 64 a shape (6 integers each)
     r x y w h rad opa              filled rounded rect, w, h 1..466, x+w and y+h at most 466, rad 0..233
     b x y w h                      black rect (a plate), as r

   At most 400 shapes and 2,048 points (p points and g corners) in all, at
   most 7,000 characters of sh, at most 16 texts. A text is exactly
   [x, y, size, opa, align, "text"], all integers: x, y 0..465, y the centre
   of the line box; size 20 or 22; opa 0..255, raised to 158 when below;
   align 0 left of x, 1 centred on x, 2 right of x; text 1 to 32 printable
   ASCII characters. Draw order: sh in order, then tx in order.

   The edge, checked here for every shape (the widget checks the texts,
   which need the fonts): every shape has anchors and a reach in px, and no
   lit pixel lies farther than its reach from its anchors or the hull
   between them (measured on the Mac against LVGL's own pixels: at the last
   place this allows, the farthest lit pixel is 196.9 px out).
     c a d   the centre, reach r + 1 (an arc counts as its whole circle)
     l p     every point, reach (w + 1) / 2 + 1 (integer division)
     g       every corner, reach 1
     r b     the four corner pixels (x, y) to (x + w - 1, y + h - 1), reach 0
   The edge: every anchor A has (A.x - 233)^2 + (A.y - 233)^2 <= (197 - reach)^2.
   A scene breaking it is refused whole. The status slot, y 371 and below,
   is not a refusal: the widget clips every shape and text above it, and
   only its own status line is drawn there (its box is centred at y 388.5).

   Numbers in the envelope are clamped: age 0..86400, stale 0 (never) or
   60..86400, hold 10..600 (default 60). st is cut to 24 characters and
   keeps only printable ASCII. Unknown keys are ignored. A loading scene's
   tx and sh are not read (the device draws its loading look).
   Refused: data that is not one JSON object (text after it included),
   nested deeper than 3, more than JAKO_SCENE_MAX_VALUES values in all,
   v, seq, id, state or src missing or of the wrong type, v not 1, seq
   below 1, an id that is not 1 to 24 of a-z, 0-9 and "-", an unknown state
   or src, a bad text, a bad shape, a shape past the edge.
   A refusal leaves *out exactly as it was, so the picture stays. */
#ifndef JAKO_SCENE_DATA_H
#define JAKO_SCENE_DATA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define JAKO_SCENE_DATA_MAX 8192   /* bytes; the tool's SetMaxLength */
#define JAKO_SCENE_SH_MAX 7000     /* characters of sh */
#define JAKO_SCENE_MAX_SHAPES 400
#define JAKO_SCENE_MAX_POINTS 2048 /* p points and g corners, in all */
#define JAKO_SCENE_MAX_POLY 64     /* points in one p */
#define JAKO_SCENE_MAX_TRIS 64     /* triangles in one g */
#define JAKO_SCENE_MAX_TEXTS 16
#define JAKO_SCENE_TEXT_MAX 32     /* characters in one text */
#define JAKO_SCENE_ID_MAX 24
#define JAKO_SCENE_ST_MAX 24
#define JAKO_SCENE_MAX_DEPTH 3     /* { tx:[ [ is the deepest legal nesting */
#define JAKO_SCENE_MAX_VALUES 200  /* JSON values in all; the largest legal envelope has 124 */
#define JAKO_SCENE_CENTRE 233      /* the screen's centre, x and y */
#define JAKO_SCENE_EDGE_R 197      /* px from the centre: nothing is lit beyond */
#define JAKO_SCENE_SLOT_Y 371      /* the first row of the status slot: shapes and texts are clipped above it */
#define JAKO_SCENE_TEXT_OPA_MIN 158 /* .62: fainter text is raised to it */
#define JAKO_SCENE_HOLD_DEFAULT 60

typedef enum {
    JAKO_SCENE_STATE_LOADING = 0, /* the probe: the device draws its loading look, tx and sh are not read */
    JAKO_SCENE_STATE_READY,
    JAKO_SCENE_STATE_EMPTY,
    JAKO_SCENE_STATE_ERROR,       /* the composer's empty frame, the status in full Light */
} jako_scene_state_t;

typedef enum {
    JAKO_SCENE_SRC_SAMPLE = 0, /* invented figures: the status line always says "Sample data" */
    JAKO_SCENE_SRC_DESK,
} jako_scene_src_t;

typedef enum {
    JAKO_SCENE_KIND_CIRCLE = 'c',
    JAKO_SCENE_KIND_ARC = 'a',
    JAKO_SCENE_KIND_DISC = 'd',
    JAKO_SCENE_KIND_LINE = 'l',
    JAKO_SCENE_KIND_POLYLINE = 'p',
    JAKO_SCENE_KIND_TRIANGLES = 'g',
    JAKO_SCENE_KIND_RECT = 'r',
    JAKO_SCENE_KIND_PLATE = 'b',
} jako_scene_kind_t;

typedef enum {
    JAKO_SCENE_ALIGN_LEFT = 0, /* the text starts at x */
    JAKO_SCENE_ALIGN_CENTRE,   /* its box is centred on x: it starts at x - width / 2 */
    JAKO_SCENE_ALIGN_RIGHT,    /* it ends at x: it starts at x - width */
} jako_scene_align_t;

typedef struct {
    int16_t x, y;
} jako_scene_point_t;

/* One shape of sh, in 22 bytes with no padding. Fields a kind does not use are 0. */
typedef struct {
    uint8_t kind;  /* jako_scene_kind_t */
    uint8_t opa;   /* 0..255; b: 255 */
    uint8_t cap;   /* a l p: 0 square, 1 round */
    uint8_t w;     /* stroke width: c a 1..64, l p 1..32 */
    int16_t x, y;  /* c a d: the centre; l: the start; r b: the top left */
    int16_t x2, y2; /* l: the end; r b: the width and the height */
    int16_t r;     /* c a d: the outer radius; r: the corner radius */
    int16_t a0, a1; /* a: degrees clockwise from twelve */
    uint16_t pt;   /* p g: the first point in points[] */
    uint16_t pn;   /* p: points; g: corners, 3 a triangle */
} jako_scene_shape_t;

typedef struct {
    int16_t x, y;  /* y: the centre of the line box */
    uint8_t size;  /* 20 or 22 */
    uint8_t opa;   /* 158..255 */
    uint8_t align; /* jako_scene_align_t */
    char text[JAKO_SCENE_TEXT_MAX + 1];
} jako_scene_text_t;

/* About 18 KB: the board keeps one in the LVGL pool to parse into, the widget keeps its own copy. */
typedef struct {
    int32_t seq;  /* 1..2147483647 */
    jako_scene_state_t state;
    jako_scene_src_t src;
    int32_t age;   /* 0..86400 s since the data was read, at send time */
    int32_t stale; /* 0 never, or 60..86400 s: age plus time since receipt past this is stale */
    int32_t hold;  /* 10..600 s with no applied update before the widget's expire callback */
    char id[JAKO_SCENE_ID_MAX + 1];
    char st[JAKO_SCENE_ST_MAX + 1]; /* printable ASCII, may be empty */
    uint16_t shape_n;
    uint16_t point_n;
    uint8_t text_n;
    jako_scene_text_t texts[JAKO_SCENE_MAX_TEXTS];
    jako_scene_shape_t shapes[JAKO_SCENE_MAX_SHAPES];
    jako_scene_point_t points[JAKO_SCENE_MAX_POINTS];
} jako_scene_update_t;

typedef enum {
    JAKO_SCENE_PARSE_OK = 0,
    JAKO_SCENE_PARSE_EMPTY,     /* no data, or longer than 8192 bytes */
    JAKO_SCENE_PARSE_TOO_DEEP,  /* nested deeper than the contract allows */
    JAKO_SCENE_PARSE_TOO_MANY,  /* more JSON values than the contract can hold */
    JAKO_SCENE_PARSE_NOT_JSON,  /* cJSON refused it (bad text or no memory), it is not an object, or text follows */
    JAKO_SCENE_PARSE_MISSING,   /* v, seq, id, state or src missing or of the wrong type, or seq below 1 */
    JAKO_SCENE_PARSE_VERSION,   /* v is not 1 */
    JAKO_SCENE_PARSE_ID,        /* id is not 1 to 24 of a-z, 0-9 and "-" */
    JAKO_SCENE_PARSE_STATE,     /* state is not one of the four names */
    JAKO_SCENE_PARSE_SRC,       /* src is not "sample" or "desk" */
    JAKO_SCENE_PARSE_BAD_TEXT,  /* tx is not a list of at most 16 well-formed texts */
    JAKO_SCENE_PARSE_BAD_SHAPE, /* sh breaks the grammar, a count, a range or a limit */
    JAKO_SCENE_PARSE_PAST_EDGE, /* a shape reaches past 197 px from the centre */
} jako_scene_parse_result_t;

/* Parse len bytes of data into *out. On anything but JAKO_SCENE_PARSE_OK,
   *out is left exactly as it was. The data need not be NUL terminated.
   cJSON allocates while parsing and everything is freed before this
   returns; the shape scanner allocates nothing. */
jako_scene_parse_result_t jako_scene_parse(const char* data, size_t len, jako_scene_update_t* out);

/* A short English reason for a log line or the tool's error text, e.g. "bad shape";
   the board prefixes "scene: ". */
const char* jako_scene_parse_reason(jako_scene_parse_result_t r);

/* true when a and b draw the same scene: everything but seq is equal. A
   scene whose bytes after seq equal the shown one's is always the same. */
bool jako_scene_same(const jako_scene_update_t* a, const jako_scene_update_t* b);

/* The seq rules, one per socket, kept by the board (the widget lives only
   while a scene is open, the gate outlives it). */
typedef struct {
    int32_t last_seq; /* 0 at the start of a conversation */
} jako_scene_seq_t;

/* true when seq is newer than every update and close seen, and records it.
   false: do not apply this update, answer with the id shown. */
bool jako_scene_seq_admit(jako_scene_seq_t* g, int32_t seq);
/* The close tool: last_seq = max(last_seq, seq), so a late update cannot reopen the scene. */
void jako_scene_seq_close(jako_scene_seq_t* g, int32_t seq);
/* The device state left listening and speaking: a new socket starts its ids at 1. */
void jako_scene_seq_reset(jako_scene_seq_t* g);

#ifdef __cplusplus
}
#endif

#endif /* JAKO_SCENE_DATA_H */
