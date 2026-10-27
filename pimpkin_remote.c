/*
 * Pimpkin remote for Flipper Zero
 *
 * On-screen remote in the style of the stock "Keypad/Remote" app:
 *
 *   up    -> keyboard Up          (box cue)
 *   down  -> keyboard Down        (ghost cue)
 *   left  -> keyboard Left        (bolt cue)
 *   OK    -> keyboard Space       (bolt-plus cue)
 *   right -> 315 MHz smoke toggle (cloud cue): start, press again to stop
 *   back  -> exit
 *
 * The smoke captures are built in (Princeton, 315 MHz, AM650) and written
 * to temp files in the app-data folder before each send.
 *
 * BLE HID needs a firmware that exposes the ble_profile fap library
 * (Unleashed / Momentum and similar custom firmware).
 */

#include <furi.h>
#include <furi_hal_bt.h>
#include <furi_hal_power.h>
#include <furi_hal_usb_hid.h>

#include <extra_profiles/hid_profile.h>
#include <bt/bt_service/bt.h>

#include <gui/canvas.h>
#include <gui/view.h>
#include <gui/view_dispatcher.h>

#include <input/input.h>

#include <notification/notification.h>
#include <notification/notification_messages.h>

#include <storage/storage.h>
#include <flipper_format/flipper_format.h>

#include <lib/subghz/blocks/custom_btn.h>
#include <lib/subghz/blocks/generic.h>
#include <lib/subghz/devices/devices.h>
#include <lib/subghz/devices/cc1101_int/cc1101_int_interconnect.h>
#include <lib/subghz/environment.h>
#include <lib/subghz/subghz_protocol_registry.h>
#include <lib/subghz/transmitter.h>
#include <lib/subghz/types.h>
#include <lib/subghz/protocols/raw.h>

#include "pimpkin_remote_icons.h"

#define TAG "PimpkinRemote"

/*
 * Change these if you prefer a different layout of keyboard cues.
 * The physical arrow buttons follow the stock Keynote remote: press and hold
 * sends the key, release releases it.
 */
#define REMOTE_KEY_UP     HID_KEYBOARD_UP_ARROW
#define REMOTE_KEY_DOWN   HID_KEYBOARD_DOWN_ARROW
#define REMOTE_KEY_LEFT   HID_KEYBOARD_LEFT_ARROW
#define REMOTE_KEY_CENTER HID_KEYBOARD_SPACEBAR

#define SMOKE_START_TX_PATH APP_DATA_PATH("tx_start.sub")
#define SMOKE_STOP_TX_PATH  APP_DATA_PATH("tx_stop.sub")

static const char SMOKE_START_SUB[] = "Filetype: Flipper SubGhz Key File\n"
                                      "Version: 1\n"
                                      "Frequency: 315000000\n"
                                      "Preset: FuriHalSubGhzPresetOok650Async\n"
                                      "Protocol: Princeton\n"
                                      "Bit: 24\n"
                                      "Key: 00 00 00 00 00 45 55 0C\n"
                                      "TE: 326\n";

static const char SMOKE_STOP_SUB[] = "Filetype: Flipper SubGhz Key File\n"
                                     "Version: 1\n"
                                     "Frequency: 315000000\n"
                                     "Preset: FuriHalSubGhzPresetOok650Async\n"
                                     "Protocol: Princeton\n"
                                     "Bit: 24\n"
                                     "Key: 00 00 00 00 00 45 55 30\n"
                                     "TE: 321\n";

#define SMOKE_TX_TIMEOUT_MS 8000
#define SMOKE_STOP_SENDS    3
#define SMOKE_STOP_GAP_MS   200
#define STATUS_TIMEOUT_MS   2200
#define KEY_LED_TIME_MS     200

/* Screen layout on the rotated 64x128 canvas. */
#define PILL_Y      13
#define PILL_H      11
#define ROW_Y0      25
#define ROW_STEP    17
#define ROW_H       16
#define SEPARATOR_Y 111
#define EXIT_Y      114

/* Animation: one frame per tick, a lightning strike every STRIKE_PERIOD frames. */
#define ANIM_TICK_MS  100
#define STRIKE_PERIOD 30

static const NotificationSequence led_off = {
    &message_red_0,
    &message_green_0,
    &message_blue_0,
    NULL,
};

static const NotificationSequence led_blue = {
    &message_red_0,
    &message_green_0,
    &message_blue_255,
    NULL,
};

static const NotificationSequence led_purple = {
    &message_red_255,
    &message_green_0,
    &message_blue_255,
    NULL,
};

typedef struct PimpkinRemote PimpkinRemote;

typedef struct {
    bool connected;
    bool smoke_on;
    bool up_pressed;
    bool down_pressed;
    bool left_pressed;
    bool right_pressed;
    bool center_pressed;
    bool back_pressed;
    uint32_t frame;
    char status[18];
} RemoteModel;

struct PimpkinRemote {
    Gui* gui;
    ViewDispatcher* view_dispatcher;
    View* view;
    Bt* bt;
    FuriHalBleProfileBase* ble_profile;
    NotificationApp* notifications;
    FuriTimer* status_timer;
    FuriTimer* key_led_timer;
    FuriTimer* anim_timer;

    bool bt_opened;
    bool bt_started;
    bool connected;
    bool smoke_on; /* last command sent was start */
    bool smoke_busy; /* a start/stop transmission is in progress */
    uint8_t hid_keys_held;
    char error[18];

    FuriThread* smoke_thread;
};

typedef enum {
    RemoteEventExit = 1,
    RemoteEventSmokeOk,
    RemoteEventSmokeNoFile,
    RemoteEventSmokeRegion,
    RemoteEventSmokeError,
    RemoteEventClearStatus,
} RemoteEvent;

typedef enum {
    SmokeResultOk,
    SmokeResultNoFile,
    SmokeResultRegion,
    SmokeResultError,
} SmokeResult;

static void remote_update_status(PimpkinRemote* app) {
    const char* status = app->connected ? "READY" : "PAIR BT";
    if(app->smoke_busy) {
        status = "SENDING";
    } else if(app->error[0]) {
        status = app->error;
    } else if(app->smoke_on) {
        status = "SMOKING";
    }

    with_view_model(
        app->view,
        RemoteModel * model,
        {
            model->connected = app->connected;
            model->smoke_on = app->smoke_on;
            snprintf(model->status, sizeof(model->status), "%s", status);
        },
        true);
}

static void remote_set_error(PimpkinRemote* app, const char* error) {
    snprintf(app->error, sizeof(app->error), "%s", error);
    remote_update_status(app);
    furi_timer_start(app->status_timer, furi_ms_to_ticks(STATUS_TIMEOUT_MS));
}

static void remote_update_led(PimpkinRemote* app) {
    if(app->smoke_on) {
        notification_internal_message(app->notifications, &led_purple);
    } else if(app->hid_keys_held > 0) {
        notification_internal_message(app->notifications, &led_blue);
    } else {
        notification_internal_message(app->notifications, &led_off);
    }
}

static void remote_key_led_timer_callback(void* context) {
    PimpkinRemote* app = context;
    if(app->hid_keys_held == 0) {
        remote_update_led(app);
    }
}

/* -------------------------------------------------------------------------- */
/* Bluetooth HID                                                              */
/* -------------------------------------------------------------------------- */

static void remote_bt_status_callback(BtStatus status, void* context) {
    PimpkinRemote* app = context;
    bool connected = (status == BtStatusConnected);

    app->connected = connected;
    app->hid_keys_held = 0;
    furi_timer_stop(app->key_led_timer);
    if(app->ble_profile) {
        ble_profile_hid_kb_release_all(app->ble_profile);
    }
    remote_update_status(app);
    remote_update_led(app);
}

static bool remote_bt_start(PimpkinRemote* app) {
    app->bt = furi_record_open(RECORD_BT);
    app->bt_opened = true;

    bt_disconnect(app->bt);
    furi_delay_ms(200);

    bt_keys_storage_set_storage_path(app->bt, APP_DATA_PATH(".bt_hid.keys"));

    static const BleProfileHidParams params = {
        .device_name_prefix = "Pimpkin",
        .mac_xor = 0x504B,
    };

    app->ble_profile =
        bt_profile_start(app->bt, ble_profile_hid, (FuriHalBleProfileParams)&params);
    if(!app->ble_profile) {
        FURI_LOG_E(TAG, "Could not start BLE HID profile");
        return false;
    }

    app->bt_started = true;
    furi_hal_bt_start_advertising();
    bt_set_status_changed_callback(app->bt, remote_bt_status_callback, app);
    return true;
}

static void remote_bt_stop(PimpkinRemote* app) {
    if(!app->bt_opened) return;

    if(app->ble_profile) {
        ble_profile_hid_kb_release_all(app->ble_profile);
    }
    app->hid_keys_held = 0;
    bt_set_status_changed_callback(app->bt, NULL, NULL);

    if(app->bt_started) {
        bt_disconnect(app->bt);
        furi_delay_ms(200);
        furi_hal_bt_stop_advertising();
        bt_keys_storage_set_default_path(app->bt);
        bt_profile_restore_default(app->bt);
        app->bt_started = false;
    }

    app->ble_profile = NULL;
    furi_record_close(RECORD_BT);
    app->bt = NULL;
    app->bt_opened = false;
}

static void remote_hid_press(PimpkinRemote* app, uint16_t key) {
    if(!app->connected) {
        remote_set_error(app, "PAIR BT");
        return;
    }
    if(app->ble_profile) {
        ble_profile_hid_kb_press(app->ble_profile, key);
        if(app->hid_keys_held < UINT8_MAX) {
            app->hid_keys_held++;
        }
        remote_update_led(app);
    }
}

static void remote_hid_release(PimpkinRemote* app, uint16_t key) {
    bool was_held = app->hid_keys_held > 0;

    if(app->ble_profile) {
        ble_profile_hid_kb_release(app->ble_profile, key);
    }
    if(app->hid_keys_held > 0) {
        app->hid_keys_held--;
    }

    if(was_held && app->hid_keys_held == 0) {
        /* Keep the blue LED visible even for a quick tap. */
        furi_timer_start(app->key_led_timer, furi_ms_to_ticks(KEY_LED_TIME_MS));
    } else if(!was_held) {
        remote_update_led(app);
    }
}

/* -------------------------------------------------------------------------- */
/* Sub-GHz                                                                    */
/* -------------------------------------------------------------------------- */

static bool preset_from_name(const char* name, FuriHalSubGhzPreset* preset) {
    if(!strcmp(name, "FuriHalSubGhzPresetOok270Async")) {
        *preset = FuriHalSubGhzPresetOok270Async;
    } else if(!strcmp(name, "FuriHalSubGhzPresetOok650Async")) {
        *preset = FuriHalSubGhzPresetOok650Async;
    } else if(!strcmp(name, "FuriHalSubGhzPreset2FSKDev238Async")) {
        *preset = FuriHalSubGhzPreset2FSKDev238Async;
    } else if(!strcmp(name, "FuriHalSubGhzPreset2FSKDev12KAsync")) {
        *preset = FuriHalSubGhzPreset2FSKDev12KAsync;
    } else if(!strcmp(name, "FuriHalSubGhzPreset2FSKDev476Async")) {
        *preset = FuriHalSubGhzPreset2FSKDev476Async;
    } else {
        return false;
    }
    return true;
}

static bool smoke_write_temp(const char* path, const char* data) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* file = storage_file_alloc(storage);
    bool ok = false;
    size_t size = strlen(data);

    if(storage_file_open(file, path, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        ok = storage_file_write(file, data, size) == size;
    }

    storage_file_close(file);
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
    return ok;
}

static SmokeResult smoke_tx_file(const char* path) {
    SmokeResult result = SmokeResultError;
    FURI_LOG_I(TAG, "TX file %s", path);

    Storage* storage = NULL;
    FlipperFormat* format = NULL;
    FlipperFormat* raw_format = NULL;
    FuriString* header = NULL;
    FuriString* temp = NULL;
    SubGhzEnvironment* environment = NULL;
    SubGhzTransmitter* transmitter = NULL;
    const SubGhzDevice* device = NULL;

    bool devices_inited = false;
    bool device_begun = false;
    bool charger_suppressed = false;
    bool tx_complete = false;
    bool is_raw = false;

    uint32_t version = 0;
    uint32_t frequency = 0;
    FuriHalSubGhzPreset preset = FuriHalSubGhzPresetIDLE;

    storage = furi_record_open(RECORD_STORAGE);
    if(!storage_file_exists(storage, path)) {
        result = SmokeResultNoFile;
        goto cleanup;
    }

    format = flipper_format_file_alloc(storage);
    header = furi_string_alloc();
    temp = furi_string_alloc();

    do {
        if(!flipper_format_file_open_existing(format, path)) break;
        if(!flipper_format_read_header(format, header, &version)) break;
        if(version != SUBGHZ_KEY_FILE_VERSION) break;

        const char* type = furi_string_get_cstr(header);
        bool is_key = !strcmp(type, SUBGHZ_KEY_FILE_TYPE);
        is_raw = !strcmp(type, SUBGHZ_RAW_FILE_TYPE);
        if(!is_key && !is_raw) break;

        if(!flipper_format_read_uint32(format, "Frequency", &frequency, 1)) break;
        if(!flipper_format_read_string(format, "Preset", temp)) break;
        if(!preset_from_name(furi_string_get_cstr(temp), &preset)) break;
        FURI_LOG_I(TAG, "TX %lu Hz, preset %s", frequency, furi_string_get_cstr(temp));

        subghz_devices_init();
        devices_inited = true;

        device = subghz_devices_get_by_name(SUBGHZ_DEVICE_CC1101_INT_NAME);
        if(!device) break;
        if(!subghz_devices_is_frequency_valid(device, frequency)) {
            result = SmokeResultRegion;
            break;
        }

        environment = subghz_environment_alloc();
        subghz_environment_set_protocol_registry(environment, &subghz_protocol_registry);

        /*
         * The internal CC1101 interconnect has no begin() callback, so the
         * generic helper returns false even though the radio is usable.
         * The stock Sub-GHz CLI ignores the result for the same reason.
         */
        subghz_devices_begin(device);
        device_begun = true;

        subghz_devices_reset(device);
        subghz_devices_load_preset(device, preset, NULL);
        subghz_devices_set_frequency(device, frequency);

        if(!flipper_format_read_string(format, "Protocol", temp)) break;

        /*
         * Unleashed keeps button/counter overrides from the Sub-GHz app in
         * global state. Princeton would otherwise swap the button, send a
         * different key and write it back to our file.
         */
        subghz_custom_btns_reset();
        subghz_block_generic_global_reset(NULL);

        if(is_raw) {
            raw_format = flipper_format_string_alloc();
            subghz_protocol_raw_gen_fff_data(raw_format, path, subghz_devices_get_name(device));
            transmitter = subghz_transmitter_alloc_init(environment, "RAW");
            if(!transmitter) break;
            if(subghz_transmitter_deserialize(transmitter, raw_format) != SubGhzProtocolStatusOk) {
                break;
            }
        } else {
            transmitter = subghz_transmitter_alloc_init(environment, furi_string_get_cstr(temp));
            if(!transmitter) break;
            if(subghz_transmitter_deserialize(transmitter, format) != SubGhzProtocolStatusOk) {
                break;
            }
        }

        furi_hal_power_suppress_charge_enter();
        charger_suppressed = true;

        furi_delay_ms(100);
        if(!subghz_devices_start_async_tx(device, subghz_transmitter_yield, transmitter)) {
            FURI_LOG_E(TAG, "TX start refused (region?)");
            result = SmokeResultRegion;
            break;
        }

        uint32_t started_at = furi_get_tick();
        while(!subghz_devices_is_async_complete_tx(device)) {
            if(furi_get_tick() - started_at > furi_ms_to_ticks(SMOKE_TX_TIMEOUT_MS)) {
                FURI_LOG_E(TAG, "TX timeout");
                break;
            }
            furi_delay_ms(10);
        }

        tx_complete = subghz_devices_is_async_complete_tx(device);
        subghz_devices_stop_async_tx(device);
        if(tx_complete) {
            result = SmokeResultOk;
        }
    } while(false);

cleanup:
    if(charger_suppressed) furi_hal_power_suppress_charge_exit();
    if(transmitter) subghz_transmitter_free(transmitter);
    if(device_begun) {
        subghz_devices_sleep(device);
        subghz_devices_end(device);
    }
    if(environment) subghz_environment_free(environment);
    if(devices_inited) subghz_devices_deinit();
    if(raw_format) flipper_format_free(raw_format);
    if(format) {
        flipper_format_file_close(format);
        flipper_format_free(format);
    }
    if(header) furi_string_free(header);
    if(temp) furi_string_free(temp);
    if(storage) furi_record_close(RECORD_STORAGE);

    FURI_LOG_I(TAG, "TX result %d", result);
    return result;
}

static SmokeResult smoke_send(bool stop) {
    const char* path = stop ? SMOKE_STOP_TX_PATH : SMOKE_START_TX_PATH;
    const char* data = stop ? SMOKE_STOP_SUB : SMOKE_START_SUB;
    uint8_t sends = stop ? SMOKE_STOP_SENDS : 1;

    SmokeResult worst = SmokeResultOk;
    for(uint8_t i = 0; i < sends; i++) {
        SmokeResult result = SmokeResultError;
        if(smoke_write_temp(path, data)) {
            result = smoke_tx_file(path);
        }
        if(result != SmokeResultOk) {
            worst = result;
        }
        if(i + 1 < sends) {
            furi_delay_ms(SMOKE_STOP_GAP_MS);
        }
    }
    return worst;
}

static int32_t smoke_toggle_thread(void* context) {
    PimpkinRemote* app = context;

    /* smoke_on only changes on the GUI thread once this thread reports back. */
    SmokeResult result = smoke_send(app->smoke_on);

    uint32_t event = RemoteEventSmokeOk;
    if(result == SmokeResultNoFile) {
        event = RemoteEventSmokeNoFile;
    } else if(result == SmokeResultRegion) {
        event = RemoteEventSmokeRegion;
    } else if(result != SmokeResultOk) {
        event = RemoteEventSmokeError;
    }

    view_dispatcher_send_custom_event(app->view_dispatcher, event);
    return 0;
}

static void remote_smoke_toggle(PimpkinRemote* app) {
    if(app->smoke_busy) return;

    if(app->smoke_thread) {
        furi_thread_join(app->smoke_thread);
        furi_thread_free(app->smoke_thread);
        app->smoke_thread = NULL;
    }

    app->smoke_busy = true;
    app->error[0] = '\0';

    app->smoke_thread = furi_thread_alloc_ex("PimpkinSmoke", 4096, smoke_toggle_thread, app);
    remote_update_status(app);
    furi_thread_start(app->smoke_thread);
}

/* -------------------------------------------------------------------------- */
/* Drawing                                                                    */
/* -------------------------------------------------------------------------- */

typedef enum {
    ActionRowCenter,
    ActionRowUp,
    ActionRowDown,
    ActionRowLeft,
    ActionRowRight,
    ActionRowCount,
} ActionRow;

typedef struct {
    const Icon* icon;
    const char* label;
    bool has_direction;
    CanvasDirection direction;
} ActionRowSpec;

static const ActionRowSpec action_rows[ActionRowCount] = {
    [ActionRowCenter] = {&I_bolt_plus_10x10, "SPACE", false, CanvasDirectionLeftToRight},
    [ActionRowUp] = {&I_box_10x10, "BOX", true, CanvasDirectionBottomToTop},
    [ActionRowDown] = {&I_ghost_10x10, "GHOST", true, CanvasDirectionTopToBottom},
    [ActionRowLeft] = {&I_bolt_10x10, "BOLT", true, CanvasDirectionRightToLeft},
    [ActionRowRight] = {&I_cloud_10x10, "SMOKE", true, CanvasDirectionLeftToRight},
};

/* Candle flicker: mostly steady, with an irregular dip now and then. */
static const Icon* const pumpkin_frames[] = {
    &I_pumpkin_10px,
    &I_pumpkin_10px_1,
    &I_pumpkin_10px_2,
    &I_pumpkin_10px_3,
};
static const uint8_t pumpkin_flicker[] = {0, 0, 0, 1, 0, 0, 3, 0, 0, 0, 1, 1, 0, 3, 0, 0};
static const uint8_t pumpkin_glow[] = {2, 2, 2, 1, 2, 2, 2, 0, 2, 2, 1, 2, 2, 2, 3, 2};

/* A strike blinks the bolt off twice in quick succession. */
static bool bolt_is_dark(uint32_t frame, uint32_t offset) {
    uint32_t phase = (frame + offset) % STRIKE_PERIOD;
    return phase == 0 || phase == 2;
}

static const Icon* smoke_icon(const RemoteModel* model) {
    if(!model->smoke_on) return &I_cloud_10x10;
    /* Puff while smoking. */
    return (model->frame / 5) % 2 ? &I_cloud_10x10 : &I_cloud_filled_10x10;
}

static void draw_arrow_glyph(Canvas* canvas, int32_t y, CanvasDirection direction) {
    const int32_t cx = 9;
    const int32_t cy = y + 8;

    switch(direction) {
    case CanvasDirectionBottomToTop:
        canvas_draw_line(canvas, cx, cy + 3, cx, cy - 2);
        canvas_draw_triangle(canvas, cx, cy - 2, 5, 3, direction);
        break;
    case CanvasDirectionTopToBottom:
        canvas_draw_line(canvas, cx, cy - 3, cx, cy + 2);
        canvas_draw_triangle(canvas, cx, cy + 2, 5, 3, direction);
        break;
    case CanvasDirectionRightToLeft:
        canvas_draw_line(canvas, cx + 2, cy, cx - 3, cy);
        canvas_draw_triangle(canvas, cx - 3, cy, 5, 3, direction);
        break;
    case CanvasDirectionLeftToRight:
        canvas_draw_line(canvas, cx - 2, cy, cx + 3, cy);
        canvas_draw_triangle(canvas, cx + 3, cy, 5, 3, direction);
        break;
    }
}

/* One framed command row. A NULL icon leaves the cue slot dark. */
static void draw_action_row(Canvas* canvas, ActionRow row, const Icon* icon, bool pressed) {
    const ActionRowSpec* spec = &action_rows[row];
    const int32_t y = ROW_Y0 + row * ROW_STEP;

    if(pressed) {
        canvas_draw_rbox(canvas, 1, y, 62, ROW_H, 3);
        canvas_set_color(canvas, ColorWhite);
    } else {
        canvas_draw_rframe(canvas, 1, y, 62, ROW_H, 3);
    }

    if(spec->has_direction) {
        draw_arrow_glyph(canvas, y, spec->direction);
    } else {
        /* The dot is the physical OK button. */
        canvas_draw_disc(canvas, 9, y + 8, 2);
    }

    if(icon) canvas_draw_icon(canvas, 18, y + 3, icon);
    canvas_draw_str_aligned(canvas, 30, y + 4, AlignLeft, AlignTop, spec->label);

    canvas_set_color(canvas, ColorBlack);
}

static void draw_header(Canvas* canvas, const RemoteModel* model) {
    const uint8_t* flicker = model->smoke_on ? pumpkin_glow : pumpkin_flicker;
    uint8_t pumpkin = flicker[(model->frame / 2) % sizeof(pumpkin_flicker)];
    canvas_draw_icon(canvas, 1, 1, pumpkin_frames[pumpkin]);

    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 14, 2, AlignLeft, AlignTop, "Pimpkin");

    /* The title bolt jumps up whenever a row bolt goes dark. */
    if(bolt_is_dark(model->frame, 0) || bolt_is_dark(model->frame, STRIKE_PERIOD / 2)) {
        canvas_draw_icon(canvas, 54, 1, &I_bolt_10x10);
    }

    /* Status pill: BT link, status text, smoke state. */
    canvas_draw_rbox(canvas, 0, PILL_Y, 64, PILL_H, 2);
    canvas_set_color(canvas, ColorWhite);
    if(model->connected) {
        canvas_draw_disc(canvas, 5, PILL_Y + 5, 2);
    } else {
        canvas_draw_circle(canvas, 5, PILL_Y + 5, 2);
    }
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(canvas, 30, PILL_Y + 2, AlignCenter, AlignTop, model->status);
    canvas_draw_icon(canvas, 53, PILL_Y, smoke_icon(model));
    canvas_set_color(canvas, ColorBlack);
}

/* Exit is set apart from the commands: no frame, below a dotted rule. */
static void draw_exit(Canvas* canvas, bool pressed) {
    for(int32_t x = 0; x < 64; x += 2) {
        canvas_draw_dot(canvas, x, SEPARATOR_Y);
    }

    if(pressed) {
        canvas_draw_rbox(canvas, 1, EXIT_Y, 36, 13, 3);
        canvas_set_color(canvas, ColorWhite);
    }
    canvas_draw_icon(canvas, 4, EXIT_Y + 3, &I_back_arrow_10x8);
    canvas_draw_str_aligned(canvas, 17, EXIT_Y + 3, AlignLeft, AlignTop, "Exit");
    canvas_set_color(canvas, ColorBlack);
}

static void remote_draw_callback(Canvas* canvas, void* context) {
    furi_assert(context);
    RemoteModel* model = context;

    /*
     * Vertical layout: the Flipper is held upright. The view is rotated, so
     * this callback draws on a 64x128 canvas.
     */
    canvas_clear(canvas);
    canvas_set_color(canvas, ColorBlack);

    draw_header(canvas, model);
    canvas_set_font(canvas, FontSecondary);

    /* Button legend: physical control, cue icon, action. */
    const Icon* space_icon = bolt_is_dark(model->frame, 0) ? NULL : &I_bolt_plus_10x10;
    const Icon* bolt_icon = bolt_is_dark(model->frame, STRIKE_PERIOD / 2) ? NULL : &I_bolt_10x10;

    draw_action_row(canvas, ActionRowCenter, space_icon, model->center_pressed);
    draw_action_row(canvas, ActionRowUp, &I_box_10x10, model->up_pressed);
    draw_action_row(canvas, ActionRowDown, &I_ghost_10x10, model->down_pressed);
    draw_action_row(canvas, ActionRowLeft, bolt_icon, model->left_pressed);
    draw_action_row(canvas, ActionRowRight, smoke_icon(model), model->right_pressed);

    draw_exit(canvas, model->back_pressed);
}

static bool remote_input_handler(InputEvent* event, void* context) {
    furi_assert(context);
    PimpkinRemote* app = context;

    if(event->type == InputTypePress) {
        switch(event->key) {
        case InputKeyUp:
            with_view_model(app->view, RemoteModel * model, { model->up_pressed = true; }, true);
            remote_hid_press(app, REMOTE_KEY_UP);
            break;
        case InputKeyDown:
            with_view_model(app->view, RemoteModel * model, { model->down_pressed = true; }, true);
            remote_hid_press(app, REMOTE_KEY_DOWN);
            break;
        case InputKeyLeft:
            with_view_model(app->view, RemoteModel * model, { model->left_pressed = true; }, true);
            remote_hid_press(app, REMOTE_KEY_LEFT);
            break;
        case InputKeyRight:
            with_view_model(
                app->view, RemoteModel * model, { model->right_pressed = true; }, true);
            break;
        case InputKeyOk:
            with_view_model(
                app->view, RemoteModel * model, { model->center_pressed = true; }, true);
            remote_hid_press(app, REMOTE_KEY_CENTER);
            break;
        case InputKeyBack:
            with_view_model(app->view, RemoteModel * model, { model->back_pressed = true; }, true);
            break;
        default:
            break;
        }
    } else if(event->type == InputTypeRelease) {
        switch(event->key) {
        case InputKeyUp:
            with_view_model(app->view, RemoteModel * model, { model->up_pressed = false; }, true);
            remote_hid_release(app, REMOTE_KEY_UP);
            break;
        case InputKeyDown:
            with_view_model(
                app->view, RemoteModel * model, { model->down_pressed = false; }, true);
            remote_hid_release(app, REMOTE_KEY_DOWN);
            break;
        case InputKeyLeft:
            with_view_model(
                app->view, RemoteModel * model, { model->left_pressed = false; }, true);
            remote_hid_release(app, REMOTE_KEY_LEFT);
            break;
        case InputKeyRight:
            with_view_model(
                app->view, RemoteModel * model, { model->right_pressed = false; }, true);
            break;
        case InputKeyOk:
            with_view_model(
                app->view, RemoteModel * model, { model->center_pressed = false; }, true);
            remote_hid_release(app, REMOTE_KEY_CENTER);
            break;
        case InputKeyBack:
            with_view_model(
                app->view, RemoteModel * model, { model->back_pressed = false; }, true);
            break;
        default:
            break;
        }
    } else if(event->type == InputTypeShort) {
        if(event->key == InputKeyRight) {
            remote_smoke_toggle(app);
        } else if(event->key == InputKeyBack) {
            if(app->ble_profile) ble_profile_hid_kb_release_all(app->ble_profile);
            view_dispatcher_send_custom_event(app->view_dispatcher, RemoteEventExit);
        }
    } else if(event->type == InputTypeLong) {
        if(event->key == InputKeyBack) {
            if(app->ble_profile) ble_profile_hid_kb_release_all(app->ble_profile);
            view_dispatcher_send_custom_event(app->view_dispatcher, RemoteEventExit);
        }
    }

    return true;
}

/* -------------------------------------------------------------------------- */
/* App                                                                        */
/* -------------------------------------------------------------------------- */

static bool remote_custom_event_callback(void* context, uint32_t event) {
    PimpkinRemote* app = context;

    switch(event) {
    case RemoteEventExit:
        view_dispatcher_stop(app->view_dispatcher);
        return true;
    case RemoteEventClearStatus:
        app->error[0] = '\0';
        remote_update_status(app);
        return true;
    case RemoteEventSmokeOk:
        app->smoke_busy = false;
        app->smoke_on = !app->smoke_on;
        app->error[0] = '\0';
        remote_update_status(app);
        remote_update_led(app);
        notification_message(app->notifications, &sequence_single_vibro);
        return true;
    case RemoteEventSmokeNoFile:
        app->smoke_busy = false;
        remote_set_error(app, "NO FILE");
        remote_update_led(app);
        notification_message(app->notifications, &sequence_error);
        return true;
    case RemoteEventSmokeRegion:
        app->smoke_busy = false;
        remote_set_error(app, "BLOCKED");
        remote_update_led(app);
        notification_message(app->notifications, &sequence_error);
        return true;
    case RemoteEventSmokeError:
        app->smoke_busy = false;
        remote_set_error(app, "TX FAIL");
        remote_update_led(app);
        notification_message(app->notifications, &sequence_error);
        return true;
    default:
        return false;
    }
}

static void remote_anim_timer_callback(void* context) {
    PimpkinRemote* app = context;
    with_view_model(app->view, RemoteModel * model, { model->frame++; }, true);
}

static void remote_status_timer_callback(void* context) {
    PimpkinRemote* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, RemoteEventClearStatus);
}

int32_t pimpkin_remote_app(void* p) {
    UNUSED(p);

    PimpkinRemote* app = malloc(sizeof(PimpkinRemote));
    memset(app, 0, sizeof(PimpkinRemote));

    app->gui = furi_record_open(RECORD_GUI);
    app->notifications = furi_record_open(RECORD_NOTIFICATION);
    notification_internal_message(app->notifications, &led_off);

    app->view = view_alloc();
    view_set_context(app->view, app);
    view_set_orientation(app->view, ViewOrientationVertical);
    view_allocate_model(app->view, ViewModelTypeLocking, sizeof(RemoteModel));
    view_set_draw_callback(app->view, remote_draw_callback);
    view_set_input_callback(app->view, remote_input_handler);

    app->view_dispatcher = view_dispatcher_alloc();
    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_custom_event_callback(app->view_dispatcher, remote_custom_event_callback);
    view_dispatcher_add_view(app->view_dispatcher, 0, app->view);
    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);

    app->status_timer = furi_timer_alloc(remote_status_timer_callback, FuriTimerTypeOnce, app);
    app->key_led_timer = furi_timer_alloc(remote_key_led_timer_callback, FuriTimerTypeOnce, app);
    app->anim_timer = furi_timer_alloc(remote_anim_timer_callback, FuriTimerTypePeriodic, app);

    remote_update_status(app);

    if(!remote_bt_start(app)) {
        remote_set_error(app, "BT ERR");
    } else {
        remote_update_status(app);
    }

    view_dispatcher_switch_to_view(app->view_dispatcher, 0);
    furi_timer_start(app->anim_timer, furi_ms_to_ticks(ANIM_TICK_MS));
    view_dispatcher_run(app->view_dispatcher);
    furi_timer_stop(app->anim_timer);

    if(app->smoke_thread) {
        furi_thread_join(app->smoke_thread);
        furi_thread_free(app->smoke_thread);
        app->smoke_thread = NULL;
    }
    /* Pending toggle results were not processed; never leave the machine smoking. */
    if(app->smoke_on || app->smoke_busy) {
        smoke_send(true);
    }
    subghz_custom_btns_reset();
    subghz_block_generic_global_reset(NULL);

    furi_timer_stop(app->status_timer);
    furi_timer_free(app->status_timer);
    furi_timer_stop(app->key_led_timer);
    furi_timer_free(app->key_led_timer);
    furi_timer_free(app->anim_timer);

    remote_bt_stop(app);

    notification_internal_message(app->notifications, &led_off);

    view_dispatcher_remove_view(app->view_dispatcher, 0);
    view_dispatcher_free(app->view_dispatcher);
    view_free(app->view);

    furi_record_close(RECORD_NOTIFICATION);
    furi_record_close(RECORD_GUI);

    free(app);
    return 0;
}
