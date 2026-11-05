// Copyright 2026 Alpas Made. All rights reserved.
/* The QMI8658 reader. No ESP_ERROR_CHECK anywhere: every failure is
   returned to the board, which logs one line and carries on without body
   events. Register numbers from the QMI8658 datasheet and the
   cyber-vocat-v2.0 board (cyber_vocat.cc). */
#include "jako_motion.h"

#include <esp_log.h>

#define TAG "JakoMotion"

namespace {

constexpr uint8_t kAddresses[] = {0x6A, 0x6B};
constexpr uint8_t kRegWhoAmI = 0x00;
constexpr uint8_t kRegRevision = 0x01;
constexpr uint8_t kRegCtrl1 = 0x02;
constexpr uint8_t kRegCtrl2 = 0x03;
constexpr uint8_t kRegCtrl7 = 0x08;
constexpr uint8_t kRegAccelX = 0x35;
constexpr uint8_t kWhoAmI = 0x05;
// CTRL1: register address auto increment, read data little endian. The
// template writes 0x60; bit 5 is "SPI read big endian" in the datasheet,
// so it is left clear here and the byte order is also checked from the
// data (jako_body.c) whatever the bit does over I2C.
constexpr uint8_t kCtrl1 = 0x40;
// CTRL2: plus or minus 8 g (4096 counts per g), about 250 Hz.
constexpr uint8_t kCtrl2 = 0x25;
// CTRL7: the accelerometer on, the gyroscope off.
constexpr uint8_t kCtrl7On = 0x01;
constexpr uint8_t kCtrl7Off = 0x00;
// First guess, tune on the board: how long a missing address may take to
// answer at boot. Two addresses, so at most about 100 ms of boot.
constexpr int kMotionProbeTimeoutMs = 50;
// First guess, tune on the board: each transfer's timeout, well under the
// 100 ms the power chip and codec reads allow, so a poll can never make
// one of them time out on the shared bus.
constexpr int kMotionI2cTimeoutMs = 20;
constexpr uint32_t kMotionI2cHz = 400000; /* the speed the touch panel on this bus already runs at */

}  // namespace

esp_err_t JakoMotion::ReadReg(uint8_t reg, uint8_t* data, size_t len) {
    if (dev_ == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    return i2c_master_transmit_receive(dev_, &reg, 1, data, len, kMotionI2cTimeoutMs);
}

esp_err_t JakoMotion::WriteReg(uint8_t reg, uint8_t value) {
    if (dev_ == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    const uint8_t data[] = {reg, value};
    return i2c_master_transmit(dev_, data, sizeof(data), kMotionI2cTimeoutMs);
}

esp_err_t JakoMotion::Start(i2c_master_bus_handle_t bus) {
    if (dev_ != nullptr) {
        return ESP_OK;
    }
    if (bus == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t found = ESP_ERR_NOT_FOUND;
    // Every address is tried: another chip answering at 0x6A must not hide
    // the motion chip at 0x6B.
    for (uint8_t address : kAddresses) {
        if (i2c_master_probe(bus, address, kMotionProbeTimeoutMs) != ESP_OK) {
            continue;
        }
        i2c_device_config_t config = {};
        config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
        config.device_address = address;
        config.scl_speed_hz = kMotionI2cHz;
        if (i2c_master_bus_add_device(bus, &config, &dev_) != ESP_OK) {
            dev_ = nullptr;
            continue;
        }
        uint8_t id = 0;
        if (ReadReg(kRegWhoAmI, &id, 1) == ESP_OK && id == kWhoAmI) {
            address_ = address;
            found = ESP_OK;
            break;
        }
        ESP_LOGI(TAG, "0x%02X answers but is not the motion chip (id 0x%02X)", address, id);
        i2c_master_bus_rm_device(dev_);
        dev_ = nullptr;
    }
    if (found != ESP_OK) {
        return found;
    }
    revision_ = 0;
    (void)ReadReg(kRegRevision, &revision_, 1);  // logged only; a failure leaves 0
    esp_err_t ret = WriteReg(kRegCtrl1, kCtrl1);
    if (ret == ESP_OK) {
        ret = WriteReg(kRegCtrl2, kCtrl2);
    }
    if (ret == ESP_OK) {
        ret = WriteReg(kRegCtrl7, kCtrl7On);
    }
    if (ret != ESP_OK) {
        Stop();
    }
    return ret;
}

esp_err_t JakoMotion::Read(uint8_t raw[6]) {
    return ReadReg(kRegAccelX, raw, 6);
}

void JakoMotion::Stop() {
    if (dev_ == nullptr) {
        return;
    }
    // Back to the chip's quiet default so a rejected chip does not keep
    // drawing current at 250 Hz. Best effort: an error here changes nothing.
    WriteReg(kRegCtrl7, kCtrl7Off);
    i2c_master_bus_rm_device(dev_);
    dev_ = nullptr;
}
