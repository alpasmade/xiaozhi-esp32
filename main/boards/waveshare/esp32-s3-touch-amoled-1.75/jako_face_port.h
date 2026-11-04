// Copyright 2026 Alpas Made. All rights reserved.
/* The thin layer between Jako's face and the platform it runs on, and the
   declarations shared by jako_face.c and the generated jako_face_hearts.c.
   On the device (ESP_PLATFORM) logging goes to ESP_LOGW; on the Mac preview
   it goes to stderr. Nothing else in the face depends on the platform. */
#ifndef JAKO_FACE_PORT_H
#define JAKO_FACE_PORT_H

#include <stdint.h>
#include "lvgl.h"

#ifdef ESP_PLATFORM
#include "esp_log.h"
#define JAKO_FACE_LOGW(fmt, ...) ESP_LOGW("jako_face", fmt, ##__VA_ARGS__)
#else
#include <stdio.h>
#define JAKO_FACE_LOGW(fmt, ...) fprintf(stderr, "W jako_face: " fmt "\n", ##__VA_ARGS__)
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* A pre-rendered heart eye: an A8 mask drawn in the face colour, and where
   the heart path's centre sits inside the mask. */
typedef struct {
    lv_image_dsc_t img;
    int16_t origin_x;
    int16_t origin_y;
    int16_t box_w_tenths; /* the eye box width it was rendered for, in tenths of a unit */
} jako_face_heart_t;

extern const jako_face_heart_t jako_face_heart_64;
extern const jako_face_heart_t jako_face_heart_68;

#ifdef __cplusplus
}
#endif

#endif /* JAKO_FACE_PORT_H */
