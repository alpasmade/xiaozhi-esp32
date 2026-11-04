#include "wifi_board.h"
#include "display/lcd_display.h"
#include "esp_lcd_co5300.h"

#include "codecs/box_audio_codec.h"
#include "application.h"
#include "button.h"
#include "led/single_led.h"
#include "mcp_server.h"
#include "config.h"
#include "power_save_timer.h"
#include "axp2101.h"
#include "i2c_device.h"

#include <esp_log.h>
#include <esp_lcd_panel_vendor.h>
#include <driver/i2c_master.h>
#include <driver/spi_master.h>
#include "esp_io_expander_tca9554.h"
#include "settings.h"

#include <esp_lcd_touch_cst9217.h>
#include <esp_lvgl_port.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <lvgl.h>

#include <atomic>
#include <cmath>
#include <cstring>

#include "jako_face.h"

#define TAG "WaveshareEsp32s3TouchAMOLED1inch75"

class Pmic : public Axp2101 {
public:
    Pmic(i2c_master_bus_handle_t i2c_bus, uint8_t addr) : Axp2101(i2c_bus, addr) {
        WriteReg(0x22, 0b110); // PWRON > OFFLEVEL as POWEROFF Source enable
        WriteReg(0x27, 0x10);  // hold 4s to power off

        // Disable All DCs but DC1
        WriteReg(0x80, 0x01);
        // Disable All LDOs
        WriteReg(0x90, 0x00);
        WriteReg(0x91, 0x00);

        // Set DC1 to 3.3V
        WriteReg(0x82, (3300 - 1500) / 100);

        // Set ALDO1 to 3.3V
        WriteReg(0x92, (3300 - 500) / 100);

        // Enable ALDO1(MIC)
        WriteReg(0x90, 0x01);

        WriteReg(0x64, 0x02); // CV charger voltage setting to 4.1V

        WriteReg(0x61, 0x02); // set Main battery precharge current to 50mA
        WriteReg(0x62, 0x08); // set Main battery charger current to 400mA ( 0x08-200mA, 0x09-300mA, 0x0A-400mA )
        WriteReg(0x63, 0x01); // set Main battery term charge current to 25mA
    }
};

#define LCD_OPCODE_WRITE_CMD (0x02ULL)
#define LCD_OPCODE_READ_CMD (0x03ULL)
#define LCD_OPCODE_WRITE_COLOR (0x32ULL)

static const co5300_lcd_init_cmd_t vendor_specific_init[] = {
    // set display to qspi mode
    {0xFE, (uint8_t[]){0x20}, 1, 0},
    {0x19, (uint8_t[]){0x10}, 1, 0},
    {0x1C, (uint8_t[]){0xA0}, 1, 0},

    {0xFE, (uint8_t[]){0x00}, 1, 0},
    {0xC4, (uint8_t[]){0x80}, 1, 0},
    {0x3A, (uint8_t[]){0x55}, 1, 0},
    {0x35, (uint8_t[]){0x00}, 1, 0},
    {0x53, (uint8_t[]){0x20}, 1, 0},
    {0x51, (uint8_t[]){0xFF}, 1, 0},
    {0x63, (uint8_t[]){0xFF}, 1, 0},
    {0x2A, (uint8_t[]){0x00, 0x06, 0x01, 0xD7}, 4, 0},
    {0x2B, (uint8_t[]){0x00, 0x00, 0x01, 0xD1}, 4, 600},
    {0x11, NULL, 0, 600},
    {0x29, NULL, 0, 0},
};

// 在waveshare_amoled_1_75类之前添加新的显示类
// The device shows Jako's face full screen instead of the emoji, the status
// bar and the subtitle (the stage shows the words). The status objects stay
// alive and keep updating, hidden. If the face cannot be created the stock
// UI is left as it was and every override falls back to the base class.
class CustomLcdDisplay : public SpiLcdDisplay {
public:
    static void rounder_event_cb(lv_event_t* e) {
        lv_area_t* area = (lv_area_t* )lv_event_get_param(e);
        uint16_t x1 = area->x1;
        uint16_t x2 = area->x2;

        uint16_t y1 = area->y1;
        uint16_t y2 = area->y2;

        // round the start of coordinate down to the nearest 2M number
        area->x1 = (x1 >> 1) << 1;
        area->y1 = (y1 >> 1) << 1;
        // round the end of coordinate up to the nearest 2N+1 number
        area->x2 = ((x2 >> 1) << 1) + 1;
        area->y2 = ((y2 >> 1) << 1) + 1;
    }

    CustomLcdDisplay(esp_lcd_panel_io_handle_t io_handle,
                     esp_lcd_panel_handle_t panel_handle,
                     int width,
                     int height,
                     int offset_x,
                     int offset_y,
                     bool mirror_x,
                     bool mirror_y,
                     bool swap_xy)
        : SpiLcdDisplay(io_handle, panel_handle,
                        width, height, offset_x, offset_y, mirror_x, mirror_y, swap_xy) {
        // Note: UI customization should be done in SetupUI(), not in constructor
        // to ensure lvgl objects are created before accessing them
    }

    virtual void SetupUI() override {
        if (IsSetupUICalled()) {
            return;
        }
        // Call parent SetupUI() first to create all lvgl objects
        SpiLcdDisplay::SetupUI();

        {
            DisplayLockGuard lock(this);
            lv_obj_set_style_pad_left(status_bar_, LV_HOR_RES*  0.1, 0);
            lv_obj_set_style_pad_right(status_bar_, LV_HOR_RES*  0.1, 0);
            lv_display_add_event_cb(display_, rounder_event_cb, LV_EVENT_INVALIDATE_AREA, NULL);

            face_ = jako_face_create(lv_screen_active());
            if (face_ == nullptr) {
                ESP_LOGE(TAG, "No memory for the face, keeping the stock UI");
                return;
            }
            // Just above container_, under everything else: the low battery
            // popup and a preview image still draw on top of the face.
            lv_obj_move_to_index(face_, 1);
            jako_face_set_seed(face_, esp_random());
            jako_face_set_pre_tick(face_, PreTick, this);
            for (lv_obj_t* obj : {emoji_box_, top_bar_, status_bar_}) {
                if (obj != nullptr) {
                    lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
                }
            }
            Apply();
        }
        LcdDisplay::SetHideSubtitle(true);
    }

    virtual bool SetEmotion(const char* emotion) override {
        DisplayLockGuard lock(this);
        if (face_ == nullptr) {
            return LcdDisplay::SetEmotion(emotion);
        }
        // An alert's icon names raise the error state until the next state.
        // Only neutral clears it (DismissAlert and the idle handler send it);
        // a reply's emotion queued behind an alert must not wipe the error.
        if (IsErrorEmotion(emotion)) {
            layer_ = FaceLayer::kError;
        } else if (layer_ == FaceLayer::kError && emotion != nullptr && strcmp(emotion, "neutral") == 0) {
            layer_ = FaceLayer::kNone;
        }
        jako_face_set_expression(face_, emotion);
        Apply();
        return true;
    }

    virtual void SetDeviceState(DeviceState state) override {
        DisplayLockGuard lock(this);
        device_state_ = state;
        if (face_ == nullptr) {
            return;
        }
        if (layer_ == FaceLayer::kThinking) {
            layer_ = FaceLayer::kNone;
        }
        // Idle must not clear an error: the network error path sets idle
        // before Alert(), and that idle event is handled after the alert.
        if (layer_ == FaceLayer::kError && state != kDeviceStateIdle &&
            state != kDeviceStateUnknown) {
            layer_ = FaceLayer::kNone;
        }
        if (state == kDeviceStateFatalError) {
            layer_ = FaceLayer::kError;
        }
        Apply();
    }

    virtual void SetChatMessage(const char* role, const char* content) override {
        LcdDisplay::SetChatMessage(role, content);
        DisplayLockGuard lock(this);
        if (face_ == nullptr) {
            return;
        }
        // Thinking runs from the user's words (stt) until tts start. Only
        // from listening: stt while speaking (realtime mode) is ignored.
        if (role != nullptr && strcmp(role, "user") == 0 && content != nullptr &&
            content[0] != '\0' && device_state_ == kDeviceStateListening &&
            layer_ == FaceLayer::kNone) {
            layer_ = FaceLayer::kThinking;
            thinking_since_ms_ = (uint32_t)(esp_timer_get_time() / 1000);
            Apply();
        }
    }

    virtual void SetPowerSaveMode(bool on) override {
        DisplayLockGuard lock(this);
        if (face_ == nullptr) {
            LcdDisplay::SetPowerSaveMode(on);
            return;
        }
        // Rest only. The base would call SetEmotion("sleepy"), which would
        // clear a latched error.
        jako_face_set_rest(face_, on);
    }

    virtual void SetHideSubtitle(bool hide) override {
        DisplayLockGuard lock(this);
        // The device never shows the subtitle while the face is up.
        LcdDisplay::SetHideSubtitle(face_ != nullptr ? true : hide);
    }

    virtual void SetPreviewImage(std::unique_ptr<LvglImage> image) override {
        // Under one lock, so the LVGL task never draws the stock icon that
        // the base shows again when the preview ends. The mutex is recursive.
        DisplayLockGuard lock(this);
        LcdDisplay::SetPreviewImage(std::move(image));
        if (face_ != nullptr && emoji_box_ != nullptr) {
            lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
        }
    }

    // Called from the audio output task once per frame: no lock, no log.
    virtual void SetAudioLevel(float level) override {
        level_.store(level, std::memory_order_relaxed);
        level_ms_.store((uint32_t)(esp_timer_get_time() / 1000), std::memory_order_release);
    }

private:
    // What sits on top of the device state. Named states, not flags.
    enum class FaceLayer { kNone, kThinking, kError };

    // A level older than this means playback has paused: the mouth closes.
    static constexpr uint32_t kLevelTimeoutMs = 100;
    // Thinking with no reply after this long (a merged or dropped turn) goes
    // back to the device state, so the comet never stays up while listening.
    static constexpr uint32_t kThinkingTimeoutMs = 15000;
    // dBFS that maps to a closed and a fully open mouth. Tune on the board.
    static constexpr float kLevelFloorDb = -45.0f;
    static constexpr float kLevelRangeDb = 35.0f;

    lv_obj_t* face_ = nullptr;
    DeviceState device_state_ = kDeviceStateStarting;
    FaceLayer layer_ = FaceLayer::kNone;
    uint32_t thinking_since_ms_ = 0;
    std::atomic<float> level_{0.0f};
    // Milliseconds, 32 bits so the store is a single word on this chip.
    std::atomic<uint32_t> level_ms_{0};

    static bool IsErrorEmotion(const char* emotion) {
        return emotion != nullptr &&
               (strcmp(emotion, "warning") == 0 || strcmp(emotion, "cancel") == 0 ||
                strcmp(emotion, "cloud_off") == 0);
    }

    // Under the display lock.
    void Apply() {
        jako_face_state_t state = JAKO_FACE_STATE_IDLE;
        if (layer_ == FaceLayer::kError) {
            state = JAKO_FACE_STATE_ERROR;
        } else if (layer_ == FaceLayer::kThinking) {
            state = JAKO_FACE_STATE_THINKING;
        } else {
            switch (device_state_) {
                case kDeviceStateListening:
                    state = JAKO_FACE_STATE_LISTENING;
                    break;
                case kDeviceStateSpeaking:
                case kDeviceStateNotifying:  // Notifying plays Jako's voice
                    state = JAKO_FACE_STATE_SPEAKING;
                    break;
                case kDeviceStateFatalError:
                    state = JAKO_FACE_STATE_ERROR;
                    break;
                default:
                    state = JAKO_FACE_STATE_IDLE;
                    break;
            }
        }
        jako_face_set_state(face_, state);
    }

    // Runs inside the face's LVGL timer, which already holds the lock.
    static void PreTick(lv_obj_t* face, void* user) {
        auto* self = static_cast<CustomLcdDisplay*>(user);
        const uint32_t at = self->level_ms_.load(std::memory_order_acquire);
        const float rms = self->level_.load(std::memory_order_relaxed);
        const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        if (self->layer_ == FaceLayer::kThinking && now - self->thinking_since_ms_ > kThinkingTimeoutMs) {
            self->layer_ = FaceLayer::kNone;
            self->Apply();
        }
        float level = 0.0f;
        if (now - at <= kLevelTimeoutMs && rms > 0.0f) {
            level = (20.0f * log10f(rms) - kLevelFloorDb) / kLevelRangeDb;
            level = level < 0.0f ? 0.0f : (level > 1.0f ? 1.0f : level);
        }
        jako_face_set_mouth_level(face, level);
    }
};

class CustomBacklight : public Backlight {
public:
    CustomBacklight(esp_lcd_panel_io_handle_t panel_io) : Backlight(), panel_io_(panel_io) {}

protected:
    esp_lcd_panel_io_handle_t panel_io_;

    virtual void SetBrightnessImpl(uint8_t brightness) override {
        auto display = Board::GetInstance().GetDisplay();
        DisplayLockGuard lock(display);
        uint8_t data[1] = {((uint8_t)((255*  brightness) / 100))};
        int lcd_cmd = 0x51;
        lcd_cmd &= 0xff;
        lcd_cmd <<= 8;
        lcd_cmd |= LCD_OPCODE_WRITE_CMD << 24;
        esp_lcd_panel_io_tx_param(panel_io_, lcd_cmd, &data, sizeof(data));
    }
};

class WaveshareEsp32s3TouchAMOLED1inch75 : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_;
    Pmic* pmic_ = nullptr;
    Button boot_button_;
    CustomLcdDisplay* display_;
    CustomBacklight* backlight_;
    esp_io_expander_handle_t io_expander = NULL;
    PowerSaveTimer* power_save_timer_;
    bool battery_seen_ = false;
    bool last_discharging_ = false;

    void InitializePowerSaveTimer() {
        // Dim after 60 s of quiet and never power off, so the wake word keeps listening.
        power_save_timer_ = new PowerSaveTimer(-1, 60, -1);
        power_save_timer_->OnEnterSleepMode([this]() {
            GetDisplay()->SetPowerSaveMode(true);
            GetBacklight()->SetBrightness(20); });
        power_save_timer_->OnExitSleepMode([this]() {
            GetDisplay()->SetPowerSaveMode(false);
            GetBacklight()->RestoreBrightness(); });
        power_save_timer_->SetEnabled(true);
    }

    void InitializeCodecI2c() {
        // Initialize I2C peripheral
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = I2C_NUM_0,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));
    }

    void InitializeTca9554(void) {
        esp_err_t ret = esp_io_expander_new_i2c_tca9554(i2c_bus_, I2C_ADDRESS, &io_expander);
        if (ret != ESP_OK)
            ESP_LOGE(TAG, "TCA9554 create returned error");
        ret = esp_io_expander_set_dir(io_expander, IO_EXPANDER_PIN_NUM_4, IO_EXPANDER_INPUT);
        ESP_ERROR_CHECK(ret);
    }

    void InitializeAxp2101() {
        ESP_LOGI(TAG, "Init AXP2101");
        pmic_ = new Pmic(i2c_bus_, 0x34);
    }

    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.sclk_io_num = EXAMPLE_PIN_NUM_LCD_PCLK;
        buscfg.data0_io_num = EXAMPLE_PIN_NUM_LCD_DATA0;
        buscfg.data1_io_num = EXAMPLE_PIN_NUM_LCD_DATA1;
        buscfg.data2_io_num = EXAMPLE_PIN_NUM_LCD_DATA2;
        buscfg.data3_io_num = EXAMPLE_PIN_NUM_LCD_DATA3;
        buscfg.max_transfer_sz = DISPLAY_WIDTH*  DISPLAY_HEIGHT*  sizeof(uint16_t);
        buscfg.flags = SPICOMMON_BUSFLAG_QUAD;
        ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });

        // Double click turns voice wake on or off; the choice is stored.
        boot_button_.OnDoubleClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateIdle) {
                power_save_timer_->WakeUp();
                app.SetVoiceWakeEnabled(!app.IsVoiceWakeEnabled());
            }
        });
    }

    void InitializeDisplay() {
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;

        // 液晶屏控制IO初始化
        ESP_LOGD(TAG, "Install panel IO");
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = EXAMPLE_PIN_NUM_LCD_CS;
        io_config.dc_gpio_num = GPIO_NUM_NC;
        io_config.spi_mode = 0;
        io_config.pclk_hz = 40 * 1000 * 1000;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 32;
        io_config.lcd_param_bits = 8;
        io_config.flags.quad_mode = true;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI2_HOST, &io_config, &panel_io));

        // 初始化液晶屏驱动芯片
        ESP_LOGD(TAG, "Install LCD driver");
        const co5300_vendor_config_t vendor_config = {
            .init_cmds = &vendor_specific_init[0],
            .init_cmds_size = sizeof(vendor_specific_init) / sizeof(co5300_lcd_init_cmd_t),
            .flags = {
                .use_qspi_interface = 1,
            }};

        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = EXAMPLE_PIN_NUM_LCD_RST;
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
        panel_config.bits_per_pixel = 16;
        panel_config.vendor_config = (void* )&vendor_config;
        ESP_ERROR_CHECK(esp_lcd_new_panel_co5300(panel_io, &panel_config, &panel));
        esp_lcd_panel_set_gap(panel, 0x06, 0);
        esp_lcd_panel_reset(panel);
        esp_lcd_panel_init(panel);
        esp_lcd_panel_invert_color(panel, false);
        esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
        esp_lcd_panel_disp_on_off(panel, true);
        display_ = new CustomLcdDisplay(panel_io, panel,
                                        DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
        backlight_ = new CustomBacklight(panel_io);
        backlight_->RestoreBrightness();
    }

    void InitializeTouch() {
        esp_lcd_touch_handle_t tp;
        esp_lcd_touch_config_t tp_cfg = {
            .x_max = DISPLAY_WIDTH - 1,
            .y_max = DISPLAY_HEIGHT - 1,
            .rst_gpio_num = PIN_NUM_TOUCH_RST,
            .int_gpio_num = PIN_NUM_TOUCH_INT,
            .levels = {
                .reset = 0,
                .interrupt = 0,
            },
            .flags = {
                .swap_xy = 0,
                .mirror_x = 1,
                .mirror_y = 1,
            },
        };
        esp_lcd_panel_io_handle_t tp_io_handle = NULL;
        esp_lcd_panel_io_i2c_config_t tp_io_config = ESP_LCD_TOUCH_IO_I2C_CST9217_CONFIG();
        tp_io_config.scl_speed_hz = 400*  1000;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(i2c_bus_, &tp_io_config, &tp_io_handle));
        ESP_LOGI(TAG, "Initialize touch controller");
        ESP_ERROR_CHECK(esp_lcd_touch_new_i2c_cst9217(tp_io_handle, &tp_cfg, &tp));
        const lvgl_port_touch_cfg_t touch_cfg = {
            .disp = lv_display_get_default(),
            .handle = tp,
        };
        lvgl_port_add_touch(&touch_cfg);
        ESP_LOGI(TAG, "Touch panel initialized successfully");
    }

    // 初始化工具
    void InitializeTools() {
        auto &mcp_server = McpServer::GetInstance();
        mcp_server.AddTool("self.system.reconfigure_wifi",
            "End this conversation and enter WiFi configuration mode.\n"
            "**CAUTION** You must ask the user to confirm this action.",
            PropertyList(), [this](const PropertyList& properties) {
                EnterWifiConfigMode();
                return true;
            });
    }

public:
    WaveshareEsp32s3TouchAMOLED1inch75() : boot_button_(BOOT_BUTTON_GPIO) {
        InitializePowerSaveTimer();
        InitializeCodecI2c();
#if CONFIG_BOARD_TYPE_WAVESHARE_ESP32_S3_TOUCH_AMOLED_1_75
        InitializeTca9554();
#endif
        InitializeAxp2101();
        InitializeSpi();
        InitializeDisplay();
        InitializeTouch();
        InitializeButtons();
        InitializeTools();
    }

    virtual AudioCodec* GetAudioCodec() override {
        static BoxAudioCodec audio_codec(
            i2c_bus_, 
            AUDIO_INPUT_SAMPLE_RATE, 
            AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, 
            AUDIO_I2S_GPIO_BCLK, 
            AUDIO_I2S_GPIO_WS, 
            AUDIO_I2S_GPIO_DOUT, 
            AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN, 
            AUDIO_CODEC_ES8311_ADDR, 
            AUDIO_CODEC_ES7210_ADDR, 
            AUDIO_INPUT_REFERENCE);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }

    virtual Backlight* GetBacklight() override {
        return backlight_;
    }

    virtual bool GetBatteryLevel(int &level, bool &charging, bool &discharging) override {
        charging = pmic_->IsCharging();
        discharging = pmic_->IsDischarging();
        // The first read always applies, so a device booted on the charger is handled too.
        if (!battery_seen_ || discharging != last_discharging_) {
            power_save_timer_->SetEnabled(discharging);
            last_discharging_ = discharging;
            battery_seen_ = true;
        }

        level = pmic_->GetBatteryLevel();
        return true;
    }

    virtual void SetPowerSaveLevel(PowerSaveLevel level) override {
        if (level != PowerSaveLevel::LOW_POWER) {
            power_save_timer_->WakeUp();
        }
        WifiBoard::SetPowerSaveLevel(level);
    }
};

DECLARE_BOARD(WaveshareEsp32s3TouchAMOLED1inch75);
