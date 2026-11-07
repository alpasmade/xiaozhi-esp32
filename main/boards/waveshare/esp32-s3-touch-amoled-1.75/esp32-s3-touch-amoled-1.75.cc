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

#include <esp_heap_caps.h>
#include <esp_lcd_touch_cst9217.h>
#include <esp_lvgl_port.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <lvgl.h>

#include <atomic>
#include <cinttypes>
#include <cmath>
#include <cstring>
#include <expected>
#include <functional>
#include <string>

#include "jako_body.h"
#include "jako_dial.h"
#include "jako_face.h"
#include "jako_motion.h"
#include "jako_radar.h"
#include "jako_scene.h"
#include "jako_screens.h"

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
            jako_face_set_reduced_motion(face_, kReducedMotion);
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
        const DeviceState was = device_state_;
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
        // The radar and a scene ride on the conversation: any other state
        // (the socket closed, an error, idle) closes them, every seq gate
        // resets so the next socket's ids start again at 1, and at rest the
        // dial may come to the front (the slot's rules, jako_screens.h).
        Screens(jako_screens_ev_device(SlotDevice(state)),
                InConversation(state)  ? "a conversation"
                : InConversation(was) ? "the conversation ended"
                                      : "no conversation");
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

    // A reaction to the body (docs/body-events.md), from any task.
    void BodyEvent(jako_face_body_event_t event) {
        DisplayLockGuard lock(this);
        if (face_ != nullptr) {
            jako_face_body_event(face_, event);
        }
    }

    // Tilt in the hand, for the board days: no trigger calls it yet. Set
    // (0, 0) whenever the source stops, so a lean never sticks.
    void SetLean(float x, float y) {
        DisplayLockGuard lock(this);
        if (face_ != nullptr) {
            jako_face_set_lean(face_, x, y);
        }
    }

    // Called from the audio output task once per frame: no lock, no log.
    virtual void SetAudioLevel(float level) override {
        level_.store(level, std::memory_order_relaxed);
        level_ms_.store((uint32_t)(esp_timer_get_time() / 1000), std::memory_order_release);
    }

    // The screens in front of the face: the flight radar (docs/radar.md,
    // wire contract v1), the dial of today and Jako's pictures, the scenes
    // (docs/screens.md). The tools parse on the main task and hand a checked
    // update here; the rest runs under the display lock, which also guards
    // the seq gates, so a state change and the widgets' own timers agree
    // with the tools. Which screen is in front, the face's show mode, the
    // long press, the error face and the gates are the slot's to decide
    // (jako_screens.c, tested on the Mac); this class only applies its
    // output (Screens below).

    // Draws a radar update: opens the radar on the first one the gate
    // admits. Returns the plane id in focus on screen, 0 for none, or the
    // refusal.
    std::expected<int, std::string> RadarUpdate(const jako_radar_update_t& update) {
        DisplayLockGuard lock(this);
        if (face_ == nullptr) {
            return std::unexpected(std::string("radar: no face on this screen"));
        }
        if (!InConversation(device_state_)) {
            return std::unexpected(std::string("radar: not in a conversation"));
        }
        if (!jako_radar_seq_admit(&radar_gate_, update.seq)) {
            // A retry or a late update: not drawn, the focus on screen is the answer.
            return radar_ != nullptr ? (int)jako_radar_focus(radar_) : 0;
        }
        if (radar_ == nullptr && !OpenRadar()) {
            return std::unexpected(std::string("radar: no memory"));
        }
        const int focus = (int)jako_radar_apply(radar_, &update);
        // "No flight data" shows the face's error for 2.4 s (the slot starts it).
        Screens(jako_screens_ev_radar_applied(RadarData(update.state)), "a radar opened");
        LogRadarHeap();
        return focus;
    }

    // The radar's close tool. Outside a conversation the radar is already
    // closed and the gate already reset; a close that arrives late must not
    // raise it for the next socket, so it changes nothing (plan 5.1).
    void RadarClose(int seq) {
        DisplayLockGuard lock(this);
        if (face_ == nullptr || !InConversation(device_state_)) {
            return;
        }
        jako_radar_seq_close(&radar_gate_, seq);
        Screens(jako_screens_ev(JAKO_SCREENS_EV_RADAR_CLOSED), "the close tool");
    }

    // Draws a dial update. The dial object is made by the first one the gate
    // admits and never deleted after that: its cache is what it shows at
    // rest. Whether it comes to the front is the slot's call: only after
    // "show my day" in a conversation, and at rest with home on. Returns the
    // look shown (always 0 in slice 1), or the refusal.
    std::expected<int, std::string> DialUpdate(const jako_dial_update_t& update) {
        DisplayLockGuard lock(this);
        if (face_ == nullptr) {
            return std::unexpected(std::string("dial: no face on this screen"));
        }
        if (!InConversation(device_state_)) {
            return std::unexpected(std::string("dial: not in a conversation"));
        }
        if (!jako_dial_seq_admit(&dial_gate_, update.seq)) {
            return 0;  // a retry or a late update: not drawn, the look on screen is now
        }
        if (dial_ == nullptr && !MakeDial()) {
            return std::unexpected(std::string("dial: no memory"));
        }
        // "Show my day": an admitted update whose ask is not 0 and new in this
        // conversation. ask 0 is never admitted, so the server's probe and its
        // refreshes keep the full face (L6).
        const jako_screens_dial_call_t call = jako_dial_ask_admit(&dial_gate_, update.ask)
                                                  ? JAKO_SCREENS_DIAL_SHOW_MY_DAY
                                                  : JAKO_SCREENS_DIAL_REFRESH;
        const int32_t look = jako_dial_apply(dial_, &update);
        const jako_screens_cache_t cache = jako_dial_rest(dial_) == JAKO_DIAL_REST_SHOWABLE
                                               ? JAKO_SCREENS_CACHE_SHOWABLE
                                               : JAKO_SCREENS_CACHE_NONE;
        Screens(jako_screens_ev_dial_applied(DialData(jako_dial_screen(dial_)),
                                             update.home != 0 ? JAKO_SCREENS_HOME_ON : JAKO_SCREENS_HOME_OFF,
                                             cache, call),
                call == JAKO_SCREENS_DIAL_SHOW_MY_DAY ? "show my day" : "a dial refresh");
        return (int)look;
    }

    // The dial's close tool ("show your face", "close the dial"): hides the
    // dial now and at rest until its next applied update; the cache stays.
    // Outside a conversation it changes nothing (plan 5.1): a late close
    // must not hide the home screen at rest.
    void DialClose(int seq) {
        DisplayLockGuard lock(this);
        if (face_ == nullptr) {
            return;
        }
        if (!InConversation(device_state_)) {
            ESP_LOGI(TAG, "Dial close outside a conversation: nothing changes");
            return;
        }
        jako_dial_seq_close(&dial_gate_, seq);
        Screens(jako_screens_ev(JAKO_SCREENS_EV_DIAL_CLOSED), "the close tool");
    }

    // Draws a scene: opens one on the first update the gate admits, in
    // front, closing the radar. The order is parse (the caller), the gate,
    // the device's own edge check of the texts, then apply, so a refusal
    // never opens anything and keeps the picture. Returns the id of the
    // scene shown ("" for none), or the refusal.
    std::expected<std::string, std::string> SceneUpdate(const jako_scene_update_t& update) {
        DisplayLockGuard lock(this);
        if (face_ == nullptr) {
            return std::unexpected(std::string("scene: no face on this screen"));
        }
        if (!InConversation(device_state_)) {
            return std::unexpected(std::string("scene: not in a conversation"));
        }
        if (!jako_scene_seq_admit(&scene_gate_, update.seq)) {
            // A retry or a late update: not drawn, the scene on screen is the answer.
            return std::string(scene_ != nullptr ? jako_scene_id(scene_) : "");
        }
        const jako_scene_apply_t checked = jako_scene_check(&update);
        if (checked != JAKO_SCENE_APPLY_SHOWN) {
            return std::unexpected(std::string("scene: ") + jako_scene_apply_reason(checked));
        }
        if (scene_ == nullptr && !OpenScene()) {
            return std::unexpected(std::string("scene: no memory"));
        }
        const jako_scene_apply_t applied = jako_scene_apply(scene_, &update);
        if (applied != JAKO_SCENE_APPLY_SHOWN && applied != JAKO_SCENE_APPLY_SAME) {
            // Not expected after the check. A scene that opened for this
            // update and drew nothing closes again; an open one keeps its picture.
            if (screens_.front != JAKO_SCREENS_FRONT_SCENE) {
                DeleteScene("its first update was refused");
            }
            return std::unexpected(std::string("scene: ") + jako_scene_apply_reason(applied));
        }
        // An error scene shows the face's error for 2.4 s (the slot starts it).
        Screens(jako_screens_ev_scene_applied(SceneData(update.state)), "a scene opened");
        LogSceneHeap();
        return std::string(scene_ != nullptr ? jako_scene_id(scene_) : "");
    }

    // The scene's close tool. Outside a conversation the scene is already
    // closed and the gate already reset, so it changes nothing.
    void SceneClose(int seq) {
        DisplayLockGuard lock(this);
        if (face_ == nullptr || !InConversation(device_state_)) {
            return;
        }
        jako_scene_seq_close(&scene_gate_, seq);
        Screens(jako_screens_ev(JAKO_SCREENS_EV_SCENE_CLOSED), "the close tool");
    }

    // The long press: the radar's next plane while it is in front, nothing
    // for the dial and a scene in slice 1. With any screen in front the
    // dimmed screen wakes first, since a person pressing is looking at it.
    void LongPress() {
        DisplayLockGuard lock(this);
        if (face_ == nullptr) {
            return;
        }
        Screens(jako_screens_ev(JAKO_SCREENS_EV_LONG_PRESS), "long press");
    }

    // How the slot wakes the dimmed screen (the board's power save timer).
    // Set once by the board after both exist; called on the main task.
    void SetWakeScreen(std::function<void()> wake) {
        DisplayLockGuard lock(this);
        wake_screen_ = std::move(wake);
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
    // The face's reduced motion (jako_face's default, off). The radar, the
    // dial and scenes take the same flag: no line and no wedge on the
    // radar, no pulse on the dial's or a scene's loading track.
    static constexpr bool kReducedMotion = false;
    // Calms the face alone while the radar is open (the plan review's
    // fallback if the breathing ring's commits under the radar cost too
    // much on the board); the line and the wedge keep turning. Off until the
    // board's frame time says otherwise.
    static constexpr bool kRadarCalmFace = false;
    // DESIGN-TRUTH.md section 1, the remote data screens: the face shows
    // the error state for 2.4 s, for the radar, the dial and a scene alike.
    static constexpr uint32_t kErrorFaceMs = 2400;
    static_assert(kErrorFaceMs == JAKO_DIAL_ERROR_FACE_MS, "one error face for every screen");
    // How often the internal heap is logged while the radar or a scene is open.
    static constexpr uint32_t kRadarHeapLogMs = 60000;
    static constexpr uint32_t kSceneHeapLogMs = 60000;

    lv_obj_t* face_ = nullptr;
    DeviceState device_state_ = kDeviceStateStarting;
    FaceLayer layer_ = FaceLayer::kNone;
    uint32_t thinking_since_ms_ = 0;
    std::atomic<float> level_{0.0f};
    // Milliseconds, 32 bits so the store is a single word on this chip.
    std::atomic<uint32_t> level_ms_{0};

    // The slot: what is in front of the face, and the error face. Changed
    // only by Screens, under the display lock.
    jako_screens_t screens_ = jako_screens_init();
    uint32_t error_face_since_ms_ = 0;
    std::function<void()> wake_screen_;

    // The radar: the object while it is open, else nullptr. The gate lives
    // for the whole conversation, the object only while it is shown.
    lv_obj_t* radar_ = nullptr;
    jako_radar_seq_t radar_gate_ = {};
    uint32_t radar_heap_log_ms_ = 0;
    // What Apply last gave the face, handed to a radar that opens later.
    jako_face_state_t face_state_ = JAKO_FACE_STATE_IDLE;

    // The dial: made by its first applied update and kept for good (hidden,
    // never deleted), so its cache and clock live on at rest. Its gate holds
    // the last seq and the last ask of this conversation.
    lv_obj_t* dial_ = nullptr;
    jako_dial_seq_t dial_gate_ = MakeDialGate();

    // A scene: the object while it is open, else nullptr.
    lv_obj_t* scene_ = nullptr;
    jako_scene_seq_t scene_gate_ = {};
    uint32_t scene_heap_log_ms_ = 0;

    static uint32_t NowMs() { return (uint32_t)(esp_timer_get_time() / 1000); }

    static bool InConversation(DeviceState state) {
        return state == kDeviceStateListening || state == kDeviceStateSpeaking;
    }

    // jako_dial_seq_reset, not {0}: the gate's documented start.
    static jako_dial_seq_t MakeDialGate() {
        jako_dial_seq_t gate;
        jako_dial_seq_reset(&gate);
        return gate;
    }

    // The device state as the slot sees it (jako_screens.h).
    static jako_screens_device_t SlotDevice(DeviceState state) {
        switch (state) {
            case kDeviceStateIdle:
                return JAKO_SCREENS_DEVICE_IDLE;
            case kDeviceStateListening:
                return JAKO_SCREENS_DEVICE_LISTENING;
            case kDeviceStateSpeaking:
                return JAKO_SCREENS_DEVICE_SPEAKING;
            default:
                return JAKO_SCREENS_DEVICE_OTHER;
        }
    }

    static jako_screens_data_t RadarData(jako_radar_state_t state) {
        switch (state) {
            case JAKO_RADAR_STATE_LIVE:
                return JAKO_SCREENS_DATA_READY;
            case JAKO_RADAR_STATE_EMPTY:
                return JAKO_SCREENS_DATA_EMPTY;
            case JAKO_RADAR_STATE_ERROR:
                return JAKO_SCREENS_DATA_ERROR;
            default:
                return JAKO_SCREENS_DATA_LOADING;
        }
    }

    static jako_screens_data_t SceneData(jako_scene_state_t state) {
        switch (state) {
            case JAKO_SCENE_STATE_READY:
                return JAKO_SCREENS_DATA_READY;
            case JAKO_SCENE_STATE_EMPTY:
                return JAKO_SCREENS_DATA_EMPTY;
            case JAKO_SCENE_STATE_ERROR:
                return JAKO_SCREENS_DATA_ERROR;
            default:
                return JAKO_SCREENS_DATA_LOADING;
        }
    }

    // What the dial shows after an apply, for the slot's error face: only an
    // error with nothing cached ("No agenda" on the bare frame) is an error;
    // an error over a cache still shows the day, dimmed.
    static jako_screens_data_t DialData(jako_dial_screen_t screen) {
        switch (screen) {
            case JAKO_DIAL_SCREEN_LOADING:
                return JAKO_SCREENS_DATA_LOADING;
            case JAKO_DIAL_SCREEN_EMPTY:
                return JAKO_SCREENS_DATA_EMPTY;
            case JAKO_DIAL_SCREEN_ERROR:
                return JAKO_SCREENS_DATA_ERROR;
            default:
                return JAKO_SCREENS_DATA_READY;
        }
    }

    // Under the display lock. One step of the slot, applied in its order:
    // wake, delete, widgets, face, press, gates, error face.
    void Screens(jako_screens_event_t event, const char* why) {
        if (face_ == nullptr) {
            return;
        }
        const jako_screens_front_t was = screens_.front;
        const jako_screens_step_t step = jako_screens_reduce(screens_, event);
        screens_ = step.state;
        const jako_screens_out_t& out = step.out;

        if (out.wake == JAKO_SCREENS_WAKE_SCREEN && wake_screen_) {
            wake_screen_();
        }
        if (out.deleted == JAKO_SCREENS_DELETE_RADAR || (out.radar == JAKO_SCREENS_WIDGET_ABSENT && radar_ != nullptr)) {
            DeleteRadar(why);
        }
        if (out.deleted == JAKO_SCREENS_DELETE_SCENE || (out.scene == JAKO_SCREENS_WIDGET_ABSENT && scene_ != nullptr)) {
            DeleteScene(why);
        }
        if (dial_ != nullptr) {
            if (out.dial == JAKO_SCREENS_WIDGET_SHOWN) {
                lv_obj_remove_flag(dial_, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(dial_, LV_OBJ_FLAG_HIDDEN);
            }
        }
        jako_face_set_show(face_, out.face == JAKO_SCREENS_FACE_SMALL       ? JAKO_FACE_SHOW_SMALL
                                  : out.face == JAKO_SCREENS_FACE_RING_ONLY ? JAKO_FACE_SHOW_RING_ONLY
                                                                            : JAKO_FACE_SHOW_FULL);
        if (out.press == JAKO_SCREENS_PRESS_NEXT_PLANE && radar_ != nullptr) {
            [[maybe_unused]] const uint16_t focus = jako_radar_next(radar_);  // outside the log: it may compile out
            ESP_LOGI(TAG, "Radar: long press, focus %u", (unsigned)focus);
        }
        if (out.gates == JAKO_SCREENS_GATES_RESET_ALL) {
            // The next socket's ids start again at 1, and its "show my day"
            // counts from ask 1 again (amendment 8).
            jako_radar_seq_reset(&radar_gate_);
            jako_dial_seq_reset(&dial_gate_);
            jako_scene_seq_reset(&scene_gate_);
        }
        if (out.error_face == JAKO_SCREENS_ERROR_FACE_START) {
            error_face_since_ms_ = NowMs();
            Apply();
        } else if (out.error_face == JAKO_SCREENS_ERROR_FACE_STOP) {
            Apply();
        }
        // One line per step a person could see, or that was refused: an
        // update or a state change that moves nothing stays quiet, so the
        // radar's and the dial's loops do not fill the log.
        const bool routine = event.kind == JAKO_SCREENS_EV_DEVICE_STATE ||
                             event.kind == JAKO_SCREENS_EV_RADAR_APPLIED ||
                             event.kind == JAKO_SCREENS_EV_SCENE_APPLIED ||
                             event.kind == JAKO_SCREENS_EV_DIAL_APPLIED;
        if (!routine || out.front != was || out.deleted != JAKO_SCREENS_DELETE_NONE ||
            out.verdict != JAKO_SCREENS_TAKEN || out.error_face != JAKO_SCREENS_ERROR_FACE_KEEP) {
            ESP_LOGI(TAG, "Screens: %s (%s) -> %s, face %s, %s", jako_screens_event_name(event.kind), why,
                     jako_screens_front_name(out.front), jako_screens_face_name(out.face),
                     jako_screens_verdict_name(out.verdict));
        }
    }

    // Where a radar or a scene goes: above the dial when there is one, else
    // just above the face, under everything else, so the low battery popup
    // and a preview image still draw on top.
    void PlaceAboveDial(lv_obj_t* obj) {
        lv_obj_move_to_index(obj, lv_obj_get_index(dial_ != nullptr ? dial_ : face_) + 1);
    }

    // Under the display lock. The slot sets the face's show mode once the
    // update is applied.
    bool OpenRadar() {
        radar_ = jako_radar_create(lv_screen_active());
        if (radar_ == nullptr) {
            ESP_LOGE(TAG, "No memory for the radar");
            return false;
        }
        PlaceAboveDial(radar_);
        jako_radar_set_expire_cb(radar_, RadarExpired, this);
        jako_radar_set_face_state(radar_, face_state_);
        jako_radar_set_reduced_motion(radar_, kReducedMotion);
        jako_face_set_reduced_motion(face_, kReducedMotion || kRadarCalmFace);
        jako_face_set_show(face_, JAKO_FACE_SHOW_RING_ONLY);
        radar_heap_log_ms_ = NowMs() - kRadarHeapLogMs;  // the first update logs
        ESP_LOGI(TAG, "Radar open");
        return true;
    }

    // Under the display lock, from Screens only. The face's show mode is the slot's.
    void DeleteRadar(const char* why) {
        if (radar_ == nullptr) {
            return;
        }
        lv_obj_t* radar = radar_;
        radar_ = nullptr;
        lv_obj_delete(radar);
        jako_face_set_reduced_motion(face_, kReducedMotion);
        ESP_LOGI(TAG, "Radar closed: %s", why);
    }

    // From the radar's own timer, in the LVGL task with the lock held, after
    // 60 s with no applied update. The gate is left as it is: a late update
    // of this conversation must not reopen the radar.
    static void RadarExpired(lv_obj_t* radar, void* user) {
        auto* self = static_cast<CustomLcdDisplay*>(user);
        if (self->radar_ == radar) {
            self->Screens(jako_screens_ev(JAKO_SCREENS_EV_RADAR_EXPIRED), "no update for 60 s");
        }
    }

    // Under the display lock. Just above the face and under any radar or
    // scene; hidden until the slot says otherwise.
    bool MakeDial() {
        dial_ = jako_dial_create(lv_screen_active());
        if (dial_ == nullptr) {
            ESP_LOGE(TAG, "No memory for the dial");
            return false;
        }
        lv_obj_add_flag(dial_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_to_index(dial_, lv_obj_get_index(face_) + 1);
        jako_dial_set_rest_cb(dial_, DialRestLapsed, this);
        jako_dial_set_reduced_motion(dial_, kReducedMotion);
        ESP_LOGI(TAG, "Dial made");
        return true;
    }

    // From the dial's own timer, in the LVGL task with the lock held: its
    // cache stopped covering now or turned 12 h old, so it may no longer
    // stand alone at rest. The dial is hidden, never deleted.
    static void DialRestLapsed(lv_obj_t* dial, void* user) {
        auto* self = static_cast<CustomLcdDisplay*>(user);
        if (self->dial_ == dial) {
            self->Screens(jako_screens_ev(JAKO_SCREENS_EV_DIAL_CACHE_LAPSED), "the dial's day ran out");
        }
    }

    // Under the display lock. The slot sets the face's show mode once the
    // update is applied.
    bool OpenScene() {
        scene_ = jako_scene_create(lv_screen_active());
        if (scene_ == nullptr) {
            ESP_LOGE(TAG, "No memory for the scene");
            return false;
        }
        PlaceAboveDial(scene_);
        jako_scene_set_expire_cb(scene_, SceneExpired, this);
        jako_scene_set_reduced_motion(scene_, kReducedMotion);
        scene_heap_log_ms_ = NowMs() - kSceneHeapLogMs;  // the first update logs
        ESP_LOGI(TAG, "Scene open");
        return true;
    }

    // Under the display lock, from Screens and SceneUpdate only.
    void DeleteScene(const char* why) {
        if (scene_ == nullptr) {
            return;
        }
        lv_obj_t* scene = scene_;
        scene_ = nullptr;
        lv_obj_delete(scene);
        ESP_LOGI(TAG, "Scene closed: %s", why);
    }

    // From the scene's own timer, in the LVGL task with the lock held, after
    // its hold (60 s) with no applied update. The gate is left as it is.
    static void SceneExpired(lv_obj_t* scene, void* user) {
        auto* self = static_cast<CustomLcdDisplay*>(user);
        if (self->scene_ == scene) {
            self->Screens(jako_screens_ev(JAKO_SCREENS_EV_SCENE_EXPIRED), "no update for its hold");
        }
    }

    // Under the display lock, after an applied update.
    void LogRadarHeap() {
        const uint32_t now = NowMs();
        if (now - radar_heap_log_ms_ < kRadarHeapLogMs) {
            return;
        }
        radar_heap_log_ms_ = now;
        ESP_LOGI(TAG, "Radar heap: internal free %u, largest block %u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    }

    // Under the display lock, after an applied update: the internal heap
    // (cJSON's nodes) and PSRAM (the data strings, the LVGL pool's neighbours).
    void LogSceneHeap() {
        const uint32_t now = NowMs();
        if (now - scene_heap_log_ms_ < kSceneHeapLogMs) {
            return;
        }
        scene_heap_log_ms_ = now;
        ESP_LOGI(TAG, "Scene heap: internal free %u, largest block %u, PSRAM free %u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    }

    static bool IsErrorEmotion(const char* emotion) {
        return emotion != nullptr &&
               (strcmp(emotion, "warning") == 0 || strcmp(emotion, "cancel") == 0 ||
                strcmp(emotion, "cloud_off") == 0);
    }

    // Under the display lock.
    void Apply() {
        jako_face_state_t state = JAKO_FACE_STATE_IDLE;
        if (layer_ == FaceLayer::kError || screens_.error_face == JAKO_SCREENS_ERROR_FACE_SHOWING) {
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
        face_state_ = state;
        jako_face_set_state(face_, state);
        if (radar_ != nullptr) {
            jako_radar_set_face_state(radar_, state);  // the line holds while thinking
        }
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
        if (self->screens_.error_face == JAKO_SCREENS_ERROR_FACE_SHOWING &&
            now - self->error_face_since_ms_ >= kErrorFaceMs) {
            self->Screens(jako_screens_ev(JAKO_SCREENS_EV_ERROR_FACE_ELAPSED), "2.4 s");
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

    // Body events (DESIGN-TRUTH.md section 5, docs/body-events.md). The
    // reader's state is named, not a flag: it can be missing, refused or failed.
    enum class MotionState { kOff, kNotFound, kChecking, kArmed, kRefused, kFailed };
    // The whole motion reader. false turns it off in one line, and the device
    // works exactly as it did before body events.
    static constexpr bool kBodyMotionEnabled = true;
    // A shake also stops Jako mid-reply. Off until the board shows false
    // shakes are rare (docs/hardware-tests.md, "Shake stops speaking").
    static constexpr bool kShakeStopsSpeaking = false;
    // First guess, tune on the board: the poll period. A shake shows within
    // this plus one 33 ms face tick (hardware test "The poll does not cost the face").
    static constexpr uint32_t kMotionPollMs = 80;
    // First guess, tune on the board: read errors in a row before the poll
    // stops for good (hardware test "Failure containment").
    static constexpr int kMotionMaxFailures = 5;

    JakoMotion motion_;
    jako_body_motion_t motion_body_ = {};
    jako_body_power_t power_body_ = {};
    MotionState motion_state_ = MotionState::kOff;
    lv_timer_t* motion_timer_ = nullptr;
    int motion_failures_ = 0;

    // The radar update being parsed: 856 bytes, kept here and not on the
    // main task's stack. Only the main task touches it.
    jako_radar_update_t radar_update_ = {};
    // The dial and scene updates being parsed (624 bytes and 17,712), in
    // PSRAM for the board's life, never on the main task's stack. Only the
    // main task touches them; the widgets keep their own copies. nullptr
    // when PSRAM had no room at boot: the tool then refuses with "no memory".
    jako_dial_update_t* dial_update_ = nullptr;
    jako_scene_update_t* scene_update_ = nullptr;

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

        // Long press (1500 ms, fired while held, never also a click), routed
        // by what is in front (jako_screens.c): the next plane on the radar;
        // nothing for the dial and a scene in slice 1; with any screen in
        // front the dimmed screen wakes first. Button callbacks run on the
        // timer task, so it hops to the main task first.
        boot_button_.OnLongPress([this]() {
            Application::GetInstance().Schedule([this]() { display_->LongPress(); });
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

    // Body events. Nothing here can stop the device booting or talking: a
    // missing or misread chip logs one line and leaves body motion off.
    void InitializeBodyEvents() {
        jako_body_power_init(&power_body_);
        jako_body_motion_init(&motion_body_);
        if (!kBodyMotionEnabled) {
            ESP_LOGI(TAG, "Body motion off (kBodyMotionEnabled)");
            return;
        }
        const esp_err_t ret = motion_.Start(i2c_bus_);
        if (ret != ESP_OK) {
            motion_state_ = MotionState::kNotFound;
            ESP_LOGW(TAG, "Motion chip not found at 0x6A or 0x6B (%s); body motion off, everything else as before",
                     esp_err_to_name(ret));
            return;
        }
        ESP_LOGI(TAG, "Motion chip at 0x%02X, WHO_AM_I 0x05, revision 0x%02X", motion_.address(),
                 motion_.revision());
        {
            // The LVGL port is running (InitializeDisplay); its timers need its lock.
            DisplayLockGuard lock(display_);
            motion_timer_ = lv_timer_create(MotionTick, kMotionPollMs, this);
        }
        if (motion_timer_ == nullptr) {
            motion_.Stop();
            motion_state_ = MotionState::kFailed;
            ESP_LOGW(TAG, "No memory for the motion poll; body motion off, everything else as before");
            return;
        }
        motion_state_ = MotionState::kChecking;
    }

    // Runs in the LVGL task, which already holds the display lock (the port's
    // mutex is recursive): no new task, no new stack.
    static void MotionTick(lv_timer_t* timer) {
        auto* self = static_cast<WaveshareEsp32s3TouchAMOLED1inch75*>(lv_timer_get_user_data(timer));
        if (self != nullptr) {
            self->OnMotionTick();
        }
    }

    void OnMotionTick() {
        if (motion_state_ != MotionState::kChecking && motion_state_ != MotionState::kArmed) {
            return;
        }
        uint8_t raw[6] = {};
        if (motion_.Read(raw) != ESP_OK) {
            jako_body_motion_gap(&motion_body_);
            if (++motion_failures_ >= kMotionMaxFailures) {
                StopMotion(MotionState::kFailed, "reads failed five times in a row");
            }
            return;
        }
        motion_failures_ = 0;
        const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        const jako_body_event_t event = jako_body_motion_sample(&motion_body_, raw, now);
        if (motion_body_.state == JAKO_BODY_MOTION_REFUSED) {
            StopMotion(MotionState::kRefused, "readings never near 1 g in either byte order");
            return;
        }
        if (motion_body_.state == JAKO_BODY_MOTION_ARMED && motion_state_ == MotionState::kChecking) {
            motion_state_ = MotionState::kArmed;
            int32_t mg[3] = {};
            jako_body_motion_decode(&motion_body_, raw, mg);
            ESP_LOGI(TAG, "Body motion armed, %s endian, x %" PRId32 " y %" PRId32 " z %" PRId32 " mg",
                     motion_body_.order == JAKO_BODY_ORDER_LITTLE ? "little" : "big", mg[0], mg[1], mg[2]);
        }
        if (event == JAKO_BODY_SHAKE) {
            ESP_LOGI(TAG, "Body: shake");
            display_->BodyEvent(JAKO_FACE_BODY_DIZZY);
            auto& app = Application::GetInstance();
            if (kShakeStopsSpeaking && app.GetDeviceState() == kDeviceStateSpeaking) {
                app.Schedule([]() {
                    // Checked again: the state may have moved on by the time the main task runs this.
                    auto& app = Application::GetInstance();
                    if (app.GetDeviceState() == kDeviceStateSpeaking) {
                        app.AbortSpeaking(kAbortReasonNone);
                    }
                });
            }
        }
    }

    // For good, until the next boot. In the LVGL task.
    void StopMotion(MotionState state, const char* why) {
        if (motion_timer_ != nullptr) {
            lv_timer_pause(motion_timer_);
        }
        motion_.Stop();
        motion_state_ = state;
        display_->SetLean(0.0f, 0.0f);  // a lean never outlives its source
        ESP_LOGW(TAG, "Body motion off: %s; everything else as before", why);
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
        InitializeRadarTools(mcp_server);
        InitializeScreenTools(mcp_server);
    }

    // The flight radar's two tools (docs/radar.md, wire contract v1), in
    // both build entries. User-only: never listed, the server calls them by
    // name during a conversation. The callbacks run on the main task.
    void InitializeRadarTools(McpServer& mcp_server) {
        mcp_server.AddUserOnlyTool("self.radar.update",
            "Draw one flight radar update from the server. Returns the plane id in focus, 0 for none.",
            PropertyList({Property("data", kPropertyTypeString).SetMaxLength(JAKO_RADAR_DATA_MAX)}),
            [this](const PropertyList& properties) -> ToolResult {
                return RadarUpdate(properties["data"].value<std::string>());
            });
        mcp_server.AddUserOnlyTool("self.radar.close",
            "Close the flight radar and show the face again. seq is this call's id.",
            PropertyList({Property("seq", kPropertyTypeInteger, 1, 2147483647)}),
            [this](const PropertyList& properties) -> ToolResult {
                display_->RadarClose(properties["seq"].value<int>());
                return true;
            });
    }

    // On the main task. The state check and the parse come before the display
    // lock, so LVGL never waits on cJSON; the display checks the state again
    // under the lock.
    ToolResult RadarUpdate(const std::string& data) {
        const DeviceState state = Application::GetInstance().GetDeviceState();
        if (state != kDeviceStateListening && state != kDeviceStateSpeaking) {
            return std::unexpected(std::string("radar: not in a conversation"));
        }
        const jako_radar_parse_result_t parsed =
            jako_radar_parse(data.data(), data.size(), &radar_update_);
        if (parsed != JAKO_RADAR_PARSE_OK) {
            return std::unexpected(std::string("radar: ") + jako_radar_parse_reason(parsed));
        }
        auto shown = display_->RadarUpdate(radar_update_);
        if (!shown) {
            return std::unexpected(shown.error());
        }
        return *shown;
    }

    // The dial's and the scenes' four tools (docs/screens.md, wire contract
    // v1), in both build entries. User-only, like the radar's: never listed,
    // the server calls them by name during a conversation. The callbacks run
    // on the main task.
    void InitializeScreenTools(McpServer& mcp_server) {
        dial_update_ = static_cast<jako_dial_update_t*>(
            heap_caps_calloc(1, sizeof(jako_dial_update_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        scene_update_ = static_cast<jako_scene_update_t*>(
            heap_caps_calloc(1, sizeof(jako_scene_update_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (dial_update_ == nullptr || scene_update_ == nullptr) {
            ESP_LOGE(TAG, "No PSRAM for the screen buffers (dial %s, scene %s); those tools refuse",
                     dial_update_ != nullptr ? "ok" : "missing", scene_update_ != nullptr ? "ok" : "missing");
        }
        display_->SetWakeScreen([this]() { power_save_timer_->WakeUp(); });

        mcp_server.AddUserOnlyTool("self.dial.update",
            "Draw one update of the dial of today from the server. Returns the look shown, 0 for now.",
            PropertyList({Property("data", kPropertyTypeString).SetMaxLength(JAKO_DIAL_DATA_MAX)}),
            [this](const PropertyList& properties) -> ToolResult {
                return DialUpdate(properties["data"].value<std::string>());
            });
        mcp_server.AddUserOnlyTool("self.dial.close",
            "Take the dial off the screen until its next update. seq is this call's id.",
            PropertyList({Property("seq", kPropertyTypeInteger, 1, 2147483647)}),
            [this](const PropertyList& properties) -> ToolResult {
                display_->DialClose(properties["seq"].value<int>());
                return true;
            });
        mcp_server.AddUserOnlyTool("self.scene.update",
            "Draw one of Jako's pictures from the server. Returns the id of the picture shown.",
            PropertyList({Property("data", kPropertyTypeString).SetMaxLength(JAKO_SCENE_DATA_MAX)}),
            [this](const PropertyList& properties) -> ToolResult {
                return SceneUpdate(properties["data"].value<std::string>());
            });
        mcp_server.AddUserOnlyTool("self.scene.close",
            "Close the picture and show the face again. seq is this call's id.",
            PropertyList({Property("seq", kPropertyTypeInteger, 1, 2147483647)}),
            [this](const PropertyList& properties) -> ToolResult {
                display_->SceneClose(properties["seq"].value<int>());
                return true;
            });
    }

    // On the main task, as the radar's: the state check and the parse come
    // before the display lock, so LVGL never waits on cJSON.
    ToolResult DialUpdate(const std::string& data) {
        const DeviceState state = Application::GetInstance().GetDeviceState();
        if (state != kDeviceStateListening && state != kDeviceStateSpeaking) {
            return std::unexpected(std::string("dial: not in a conversation"));
        }
        if (dial_update_ == nullptr) {
            return std::unexpected(std::string("dial: no memory"));
        }
        const jako_dial_parse_result_t parsed = jako_dial_parse(data.data(), data.size(), dial_update_);
        if (parsed != JAKO_DIAL_PARSE_OK) {
            return std::unexpected(std::string("dial: ") + jako_dial_parse_reason(parsed));
        }
        auto shown = display_->DialUpdate(*dial_update_);
        if (!shown) {
            return std::unexpected(shown.error());
        }
        return *shown;
    }

    ToolResult SceneUpdate(const std::string& data) {
        const DeviceState state = Application::GetInstance().GetDeviceState();
        if (state != kDeviceStateListening && state != kDeviceStateSpeaking) {
            return std::unexpected(std::string("scene: not in a conversation"));
        }
        if (scene_update_ == nullptr) {
            return std::unexpected(std::string("scene: no memory"));
        }
        const jako_scene_parse_result_t parsed = jako_scene_parse(data.data(), data.size(), scene_update_);
        if (parsed != JAKO_SCENE_PARSE_OK) {
            return std::unexpected(std::string("scene: ") + jako_scene_parse_reason(parsed));
        }
        auto shown = display_->SceneUpdate(*scene_update_);
        if (!shown) {
            return std::unexpected(shown.error());
        }
        return *shown;
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
        InitializeBodyEvents();
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
        // Cable in and battery full, from the two reads above: no new register
        // read. Only the main task gets here (the clock tick, and MCP tools,
        // which Application::Schedule runs there), so no lock. The screen is
        // already awake: SetEnabled(false) wakes it on cable in.
        const jako_body_event_t body = jako_body_power_sample(&power_body_, charging, discharging,
                                                              (uint32_t)(esp_timer_get_time() / 1000));
        if (body == JAKO_BODY_CABLE_IN || body == JAKO_BODY_BATTERY_FULL) {
            ESP_LOGI(TAG, "Body: %s", body == JAKO_BODY_CABLE_IN ? "cable in" : "battery full");
            display_->BodyEvent(JAKO_FACE_BODY_HAPPY);
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
