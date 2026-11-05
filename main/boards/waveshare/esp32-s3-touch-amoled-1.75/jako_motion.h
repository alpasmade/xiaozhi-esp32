// Copyright 2026 Alpas Made. All rights reserved.
/* The six-axis motion chip (QST QMI8658) on the board's shared I2C bus,
   accelerometer only, polled. Ported from the cyber-vocat-v2.0 board in
   this firmware, with every call returning an error instead of stopping
   the device: a chip that is missing, answers wrong or fails mid-way only
   ever turns body events off (docs/body-events.md). */
#ifndef JAKO_MOTION_H
#define JAKO_MOTION_H

#include <driver/i2c_master.h>
#include <esp_err.h>

#include <cstdint>

class JakoMotion {
public:
    // Probes 0x6A and 0x6B, checks WHO_AM_I at each, and sets the chip to
    // accelerometer only, plus or minus 8 g, about 250 Hz. ESP_ERR_NOT_FOUND
    // when no address holds the chip; on any error nothing is left attached.
    esp_err_t Start(i2c_master_bus_handle_t bus);
    // The six accelerometer data bytes from 0x35, exactly as read.
    esp_err_t Read(uint8_t raw[6]);
    // Turns the accelerometer off (best effort) and leaves the bus.
    void Stop();

    bool started() const { return dev_ != nullptr; }
    uint8_t address() const { return address_; }
    uint8_t revision() const { return revision_; }

private:
    esp_err_t ReadReg(uint8_t reg, uint8_t* data, size_t len);
    esp_err_t WriteReg(uint8_t reg, uint8_t value);

    i2c_master_dev_handle_t dev_ = nullptr;
    uint8_t address_ = 0;
    uint8_t revision_ = 0;
};

#endif  // JAKO_MOTION_H
