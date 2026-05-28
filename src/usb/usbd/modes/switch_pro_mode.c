// switch_pro_mode.c - Nintendo Switch Pro Controller USB device mode (full protocol)
// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 (joypad-os contribution)
//
// "Switch Pro" output mode: full Nintendo Switch Pro Controller emulation with
// handshake, SPI calibration, subcommands, IMU (motion) and HD rumble.
//
// This is ADDITIVE alongside the existing basic "Switch" mode (switch_mode.c):
//   - "Switch"      = HORI Pokken HID gamepad, no handshake, max compatibility,
//                     NO motion / NO rumble.
//   - "Switch Pro"  = genuine Pro Controller (0x057E/0x2009), handshake REQUIRED,
//                     provides motion + HD rumble.
//
// Clean-source attribution:
//   - Handshake / SPI / subcommands / 0x30 structure:  GP2040-CE SwitchProDriver (MIT)
//   - Report format / IMU layout / rumble tables:       dekuNukem public RE docs
//   - Switch 2 LRA rumble ENCODE:                        SDL EncodeHDRumble (zlib),
//                                                        BlueRetro sw2.c (Apache-2.0)
//   - Mode scaffolding / IMU consumption pattern:        joypad-os (Apache-2.0, sinput_mode.c)

#include "tusb.h"
#include "../usbd_mode.h"
#include "../usbd.h"
#include "descriptors/switch_pro_descriptors.h"
#include "core/buttons.h"
#include "platform/platform.h"   // platform_time_us(), platform_get_unique_id()
#include <string.h>


// ============================================================================
// STATE
// ============================================================================

static switch_pro_input_report_t pro_report;
static uint8_t report_timer = 0;

// Feedback state (filled by rumble handling; consumed via get_feedback)
static output_feedback_t pro_feedback;

// ============================================================================
// CONVERSION HELPERS
// ============================================================================

// Pack a 12-bit X/Y stick pair into the Pro Controller's 3-byte format.
// Source: GP2040-CE SwitchAnalog packing (MIT).
static void pack_stick(uint8_t out[3], uint16_t x, uint16_t y)
{
    out[0] = x & 0xFF;
    out[1] = ((x >> 8) & 0x0F) | ((y & 0x0F) << 4);
    out[2] = (y >> 4) & 0xFF;
}

// Map joypad-os 8-bit stick (0..255, 0x80 center) to Pro 12-bit (0..0xFFF).
static uint16_t stick8_to_12(uint8_t v)
{
    return (uint16_t)(((uint32_t)v * SWITCH_PRO_STICK_MAX) / 255U);
}

// ============================================================================
// PROTOCOL STATE MACHINE
// Pro Controller handshake / subcommand handling.
// Adapted from GP2040-CE SwitchProDriver.cpp (MIT) for the protocol structure,
// with SPI calibration values from dekuNukem public docs (spi_flash_notes.md).
// ============================================================================

// --- Handshake/protocol state (reset in init) ---
static bool     pro_imu_enabled       = false;
static bool     pro_vibration_enabled = false;
static bool     pro_handshake_done    = false;  // host completed USB init
static uint8_t  pro_player_leds       = 0;
static uint8_t  pro_input_mode        = 0x30;   // default full input report mode
static uint8_t  pro_reply_counter     = 0;

// Emulated MAC address (reversed on the wire). Randomized at init.
static uint8_t  pro_mac[6] = {0x7c, 0xbb, 0x8a, 0x00, 0x00, 0x00};

// Device-info reply body (REQUEST_DEVICE_INFO 0x02).
// Layout per dekuNukem docs: fw major/minor, controller type (0x03 = Pro),
// unknown(0x02), MAC[6] (big-endian on wire), unknown(0x01), colors-stored(0x02).
static const uint8_t PRO_FW_MAJOR = 0x04;
static const uint8_t PRO_FW_MINOR = 0x91;
static const uint8_t PRO_CONTROLLER_TYPE = 0x03;  // Pro Controller

// --- SPI flash calibration data (dekuNukem spi_flash_notes.md, public) ---
// Factory stick calibration block, returned for reads at 0x603D.
// Values are dekuNukem's documented factory defaults.
static const uint8_t PRO_SPI_FACTORY_STICK[] = {
    0xBA, 0x15, 0x62, 0x11, 0xB8, 0x7F, 0x29, 0x06, 0x5B,
    0xFF, 0xE7, 0x7E, 0x0E, 0x36, 0x56, 0x9E, 0x85, 0x60, 0xFF
};
// Factory IMU (motion) calibration block, returned for reads at 0x6020.
static const uint8_t PRO_SPI_FACTORY_IMU[] = {
    0x50, 0xFD, 0x00, 0x00, 0xC6, 0x0F, 0x0F, 0x30, 0x61,
    0xAE, 0x90, 0xD9, 0xD4, 0x14, 0x54, 0x41, 0x15, 0x54, 0xC7, 0x79, 0x9C, 0x33, 0x36, 0x63
};
// Stick device parameters, returned for reads at 0x6086 / 0x6098.
static const uint8_t PRO_SPI_STICK_PARAMS[] = {
    0x0F, 0x30, 0x61, 0x96, 0x30, 0xF3, 0xD4, 0x14, 0x54, 0x41,
    0x15, 0x54, 0xC7, 0x79, 0x9C, 0x33, 0x36, 0x63
};
// User calibration region at 0x8010/0x8026 — return 0xFF (no user cal = use factory).

// Fill `dest` (size bytes) with the SPI flash contents at `addr`.
// Returns the documented data for known calibration addresses; 0xFF otherwise.
static void pro_spi_read(uint8_t* dest, uint32_t addr, uint8_t size)
{
    memset(dest, 0xFF, size);  // default: erased flash
    switch (addr) {
        case 0x6020:  // factory IMU calibration
            memcpy(dest, PRO_SPI_FACTORY_IMU,
                   size < sizeof(PRO_SPI_FACTORY_IMU) ? size : sizeof(PRO_SPI_FACTORY_IMU));
            break;
        case 0x603D:  // factory stick calibration
            memcpy(dest, PRO_SPI_FACTORY_STICK,
                   size < sizeof(PRO_SPI_FACTORY_STICK) ? size : sizeof(PRO_SPI_FACTORY_STICK));
            break;
        case 0x6086:  // left stick device parameters
        case 0x6098:  // right stick device parameters
            memcpy(dest, PRO_SPI_STICK_PARAMS,
                   size < sizeof(PRO_SPI_STICK_PARAMS) ? size : sizeof(PRO_SPI_STICK_PARAMS));
            break;
        case 0x6050:  // controller color (body/buttons/grips) — return neutral grey
            if (size >= 12) {
                static const uint8_t colors[12] = {
                    0x32, 0x32, 0x32,  0xFF, 0xFF, 0xFF,
                    0x46, 0x46, 0x46,  0x46, 0x46, 0x46
                };
                memcpy(dest, colors, 12);
            }
            break;
        default:
            break;  // unknown address: leave as 0xFF
    }
}

// Build the common 0x21 subcommand-reply preamble into `r`.
// r[0]=0x21, r[1]=counter, r[2]=battery/conn, r[3..11]=neutral input snapshot,
// r[12]=0x00. The subcommand-specific bytes start at r[13].
static void pro_build_2110_preamble(uint8_t* r)
{
    memset(r, 0, 64);
    r[0]  = 0x21;
    r[1]  = pro_reply_counter++;
    r[2]  = 0x90;       // battery full + USB powered
    // r[3..11] left as neutral input (sticks centered handled host-side via cal)
    r[3]  = 0x00; r[4] = 0x00; r[5] = 0x00;  // buttons
    // centered sticks (0x800 packed)
    r[6]  = 0x00; r[7] = 0x08; r[8] = 0x80;
    r[9]  = 0x00; r[10] = 0x08; r[11] = 0x80;
    r[12] = 0x00;       // vibrator input report
}

static void switch_pro_protocol_init(void)
{
    pro_imu_enabled       = false;
    pro_vibration_enabled = false;
    pro_handshake_done    = false;
    pro_player_leds       = 0;
    pro_input_mode        = 0x30;
    pro_reply_counter     = 0;
    // Derive the low 3 MAC bytes from the board's unique ID so multiple
    // dongles don't collide, and the address is stable across reboots.
    uint8_t uid[8] = {0};
    platform_get_unique_id(uid, sizeof(uid));
    pro_mac[3] = uid[5];
    pro_mac[4] = uid[6];
    pro_mac[5] = uid[7];
}

bool switch_pro_handshake_complete(void)
{
    return pro_handshake_done;
}

bool switch_pro_imu_is_enabled(void)
{
    return pro_imu_enabled;
}

// Handle a host OUT report. Returns true if it was a handshake/subcommand the
// protocol layer consumed (response written to out_response/out_len). Returns
// false for pure-rumble reports so the caller can route them to rumble decode.
//
// Report-ID handling: the Switch report IDs (0x80/0x01/0x10) are declared as
// OUTPUT report IDs in our HID descriptor, so TinyUSB delivers them in the
// `report_id` argument with `data` pointing at the payload that follows. To be
// robust against stacks that instead leave the ID in the buffer, we normalize:
// if report_id is one of ours, the payload is `data`; otherwise the ID is the
// first payload byte. We then index payload bytes from a common base.
static bool switch_pro_protocol_handle_output(uint8_t report_id,
                                              const uint8_t* data, uint16_t len,
                                              uint8_t* out_response, uint16_t* out_len)
{
    *out_len = 0;
    if (len < 1 && report_id == 0) return false;

    uint8_t sw_report_id;
    const uint8_t* p;   // payload pointer: p[0] is the first byte AFTER the report ID
    uint16_t plen;      // length of payload at p

    if (report_id == SWPRO_REPORT_CONFIG_80 ||
        report_id == SWPRO_REPORT_FEATURE  ||
        report_id == SWPRO_REPORT_OUTPUT_10) {
        // ID was stripped into report_id; data is the payload.
        sw_report_id = report_id;
        p = data;
        plen = len;
    } else {
        // ID is in the buffer; payload starts at data[1].
        sw_report_id = data[0];
        p = data + 1;
        plen = (len > 0) ? (len - 1) : 0;
    }

    // --- 0x80 family: USB handshake/config ---
    if (sw_report_id == SWPRO_REPORT_CONFIG_80) {
        if (plen < 1) return false;
        uint8_t subtype = p[0];
        uint8_t* r = out_response;
        memset(r, 0, 64);
        switch (subtype) {
            case SWPRO_USB_IDENTIFY:           // 0x01: report identity + MAC
                r[0] = SWPRO_REPORT_USB_IN_81;
                r[1] = SWPRO_USB_IDENTIFY;
                r[2] = 0x00;
                r[3] = PRO_CONTROLLER_TYPE;
                for (uint8_t i = 0; i < 6; i++) r[4 + i] = pro_mac[5 - i];
                *out_len = 64;
                return true;
            case SWPRO_USB_HANDSHAKE:          // 0x02
                r[0] = SWPRO_REPORT_USB_IN_81;
                r[1] = SWPRO_USB_HANDSHAKE;
                *out_len = 64;
                return true;
            case SWPRO_USB_BAUD_RATE:          // 0x03
                r[0] = SWPRO_REPORT_USB_IN_81;
                r[1] = SWPRO_USB_BAUD_RATE;
                *out_len = 64;
                return true;
            case SWPRO_USB_DISABLE_USB_TIMEOUT:  // 0x04: host hands over — ready
                pro_handshake_done = true;
                return false;  // no reply; switch to streaming 0x30 reports
            case SWPRO_USB_ENABLE_USB_TIMEOUT:   // 0x05
                return false;
            default:
                return false;
        }
    }

    // --- 0x01 family: rumble + subcommand ---
    // Payload layout (p, past report ID): [counter][rumble:8][subcmd][args...]
    //   p[0]      = counter
    //   p[1..8]   = rumble data (4 bytes per motor)
    //   p[9]      = subcommand id
    //   p[10..]   = subcommand arguments
    if (sw_report_id == SWPRO_REPORT_FEATURE) {
        if (plen < 10) return false;
        uint8_t subcmd = p[9];
        uint8_t* r = out_response;
        pro_build_2110_preamble(r);

        switch (subcmd) {
            case SWPRO_CMD_REQ_DEVICE_INFO: {   // 0x02
                r[13] = 0x82;
                r[14] = SWPRO_CMD_REQ_DEVICE_INFO;
                r[15] = PRO_FW_MAJOR;
                r[16] = PRO_FW_MINOR;
                r[17] = PRO_CONTROLLER_TYPE;
                r[18] = 0x02;
                for (uint8_t i = 0; i < 6; i++) r[19 + i] = pro_mac[i];
                r[25] = 0x01;
                r[26] = 0x02;
                *out_len = 64;
                return true;
            }
            case SWPRO_CMD_SET_MODE:            // 0x03
                pro_input_mode = p[10];
                r[13] = 0x80;
                r[14] = SWPRO_CMD_SET_MODE;
                *out_len = 64;
                return true;
            case SWPRO_CMD_TRIGGER_BUTTONS:     // 0x04
                r[13] = 0x83;
                r[14] = SWPRO_CMD_TRIGGER_BUTTONS;
                *out_len = 64;
                return true;
            case SWPRO_CMD_SET_SHIPMENT:        // 0x08
                r[13] = 0x80;
                r[14] = SWPRO_CMD_SET_SHIPMENT;
                *out_len = 64;
                return true;
            case SWPRO_CMD_SPI_READ: {          // 0x10
                uint32_t spi_addr = (uint32_t)p[10] |
                                    ((uint32_t)p[11] << 8) |
                                    ((uint32_t)p[12] << 16) |
                                    ((uint32_t)p[13] << 24);
                uint8_t  spi_size = p[14];
                if (spi_size > 0x1D) spi_size = 0x1D;
                r[13] = 0x90;
                r[14] = SWPRO_CMD_SPI_READ;
                r[15] = p[10]; r[16] = p[11];   // echo address
                r[17] = p[12]; r[18] = p[13];
                r[19] = spi_size;
                pro_spi_read(&r[20], spi_addr, spi_size);
                *out_len = 64;
                return true;
            }
            case SWPRO_CMD_SET_NFC_IR_CONFIG:   // 0x21 — stub-ACK (no NFC yet)
            case SWPRO_CMD_SET_NFC_IR_STATE:    // 0x22 — stub-ACK
                r[13] = 0x80;
                r[14] = subcmd;
                *out_len = 64;
                return true;
            case SWPRO_CMD_SET_PLAYER_LIGHTS:   // 0x30
                pro_player_leds = p[10];
                r[13] = 0x80;
                r[14] = SWPRO_CMD_SET_PLAYER_LIGHTS;
                *out_len = 64;
                return true;
            case SWPRO_CMD_GET_PLAYER_LIGHTS:   // 0x31
                r[13] = 0xB0;
                r[14] = SWPRO_CMD_GET_PLAYER_LIGHTS;
                r[15] = pro_player_leds;
                *out_len = 64;
                return true;
            case SWPRO_CMD_SET_HOME_LIGHT:      // 0x38
                r[13] = 0x80;
                r[14] = SWPRO_CMD_SET_HOME_LIGHT;
                *out_len = 64;
                return true;
            case SWPRO_CMD_TOGGLE_IMU:          // 0x40
                pro_imu_enabled = (p[10] != 0);
                r[13] = 0x80;
                r[14] = SWPRO_CMD_TOGGLE_IMU;
                *out_len = 64;
                return true;
            case SWPRO_CMD_IMU_SENSITIVITY:     // 0x41
                r[13] = 0x80;
                r[14] = SWPRO_CMD_IMU_SENSITIVITY;
                *out_len = 64;
                return true;
            case SWPRO_CMD_ENABLE_VIBRATION:    // 0x48
                pro_vibration_enabled = (p[10] != 0);
                r[13] = 0x80;
                r[14] = SWPRO_CMD_ENABLE_VIBRATION;
                *out_len = 64;
                return true;
            case SWPRO_CMD_GET_VOLTAGE:         // 0x50
                r[13] = 0xD0;
                r[14] = SWPRO_CMD_GET_VOLTAGE;
                r[15] = 0x83;  // ~4.1V (full)
                r[16] = 0x06;
                *out_len = 64;
                return true;
            default:
                // Unknown subcommand: generic ACK so the host doesn't stall.
                r[13] = 0x80;
                r[14] = subcmd;
                r[15] = 0x03;
                *out_len = 64;
                return true;
        }
    }

    // 0x10 (rumble-only) and anything else: not a protocol report.
    return false;
}

// ============================================================================
// MODE INTERFACE IMPLEMENTATION
// ============================================================================

static void switch_pro_mode_init(void)
{
    memset(&pro_report, 0, sizeof(pro_report));
    pro_report.report_id = SWPRO_REPORT_INPUT_30;
    pro_report.conn_info = 0x90;  // USB connected, full battery (dekuNukem docs)

    uint16_t mid = SWITCH_PRO_STICK_MID;
    pack_stick(pro_report.left_stick,  mid, mid);
    pack_stick(pro_report.right_stick, mid, mid);

    memset(&pro_feedback, 0, sizeof(pro_feedback));
    report_timer = 0;

    switch_pro_protocol_init();
}

static bool switch_pro_mode_is_ready(void)
{
    // Only stream 0x30 input reports once the USB handshake is complete.
    // Before that, the host drives the conversation via OUT reports and we
    // reply through handle_output(); streaming early can desync some hosts.
    return tud_hid_ready() && switch_pro_handshake_complete();
}

static bool switch_pro_mode_send_report(uint8_t player_index,
                                        const input_event_t* event,
                                        const profile_output_t* profile_out,
                                        uint32_t buttons)
{
    (void)player_index;

    // --- Buttons: map joypad-os neutral buttons -> Pro 3-byte layout ---
    uint8_t b0 = 0, b1 = 0, b2 = 0;

    // Byte 0: right-side + right shoulders (position-based, matches GP2040-CE)
    if (buttons & JP_BUTTON_B1) b0 |= SWITCH_PRO_MASK_B;   // bottom -> B
    if (buttons & JP_BUTTON_B2) b0 |= SWITCH_PRO_MASK_A;   // right  -> A
    if (buttons & JP_BUTTON_B3) b0 |= SWITCH_PRO_MASK_Y;   // left   -> Y
    if (buttons & JP_BUTTON_B4) b0 |= SWITCH_PRO_MASK_X;   // top    -> X
    if (buttons & JP_BUTTON_R1) b0 |= SWITCH_PRO_MASK_R;
    if (buttons & JP_BUTTON_R2) b0 |= SWITCH_PRO_MASK_ZR;

    // Byte 1: system + stick clicks
    if (buttons & JP_BUTTON_S1) b1 |= SWITCH_PRO_MASK_MINUS;
    if (buttons & JP_BUTTON_S2) b1 |= SWITCH_PRO_MASK_PLUS;
    if (buttons & JP_BUTTON_L3) b1 |= SWITCH_PRO_MASK_L3;
    if (buttons & JP_BUTTON_R3) b1 |= SWITCH_PRO_MASK_R3;
    if (buttons & JP_BUTTON_A1) b1 |= SWITCH_PRO_MASK_HOME;
    if (buttons & JP_BUTTON_A2) b1 |= SWITCH_PRO_MASK_CAPTURE;

    // Byte 2: dpad + left shoulders
    if (buttons & JP_BUTTON_DU) b2 |= SWITCH_PRO_MASK_UP;
    if (buttons & JP_BUTTON_DD) b2 |= SWITCH_PRO_MASK_DOWN;
    if (buttons & JP_BUTTON_DL) b2 |= SWITCH_PRO_MASK_LEFT;
    if (buttons & JP_BUTTON_DR) b2 |= SWITCH_PRO_MASK_RIGHT;
    if (buttons & JP_BUTTON_L1) b2 |= SWITCH_PRO_MASK_L;
    if (buttons & JP_BUTTON_L2) b2 |= SWITCH_PRO_MASK_ZL;

    pro_report.buttons[0] = b0;
    pro_report.buttons[1] = b1;
    pro_report.buttons[2] = b2;

    // --- Sticks ---
    pack_stick(pro_report.left_stick,
               stick8_to_12(profile_out->left_x),
               stick8_to_12(profile_out->left_y));
    pack_stick(pro_report.right_stick,
               stick8_to_12(profile_out->right_x),
               stick8_to_12(profile_out->right_y));

    // --- Timer (rolls over) ---
    pro_report.timer = report_timer++;

    // --- IMU --------------------------------------------------------
    // TODO: populate pro_report.imu[36] from joypad-os neutral motion.
    // Pattern mirrors sinput_mode.c:
    //     if (event->has_motion) { use event->gyro[0..2], event->accel[0..2];
    //         convert from event->gyro_range/accel_range to Pro IMU scale;
    //         write 3 identical frames (or interpolate) into pro_report.imu }
    //     else { memset(pro_report.imu, 0, sizeof(pro_report.imu)); }
    // Test plan: pair a DS5 (already parsed into event->gyro/accel) and verify
    // gyro appears in Citron's direct Pro driver / on hardwaretester.com.
    (void)event;
    memset(pro_report.imu, 0, sizeof(pro_report.imu));  // placeholder

    return tud_hid_report(0, &pro_report, sizeof(pro_report));
}

// --- Host output (rumble / subcommands) -----------------------------------
// The host sends OUT reports: handshake/subcommands (0x80/0x01) and rumble
// (0x10/0x01). Handshake/subcommands are handled by the protocol state machine
// Rumble payloads are decoded below.
static void switch_pro_mode_handle_output(uint8_t report_id,
                                          const uint8_t* data, uint16_t len)
{
    uint8_t response[64];
    uint16_t resp_len = 0;

    // Let the protocol state machine handle handshake/subcommands.
    if (switch_pro_protocol_handle_output(report_id, data, len, response, &resp_len)) {
        if (resp_len) {
            tud_hid_report(0, response, resp_len);
        }
        return;
    }

    // Rumble-bearing reports (0x10 = rumble only, 0x01 = rumble + subcmd).
    if (report_id == SWPRO_REPORT_OUTPUT_10 || report_id == SWPRO_REPORT_FEATURE) {
        // Pro Controller rumble payload: 8 bytes total, 4 bytes per motor
        // (left motor = data[2..5], right motor = data[6..9]) per dekuNukem.
        // ----------------------------------------------------------------
        // TODO: implement rumble decode from dekuNukem rumble_data_table.md
        //   switch_pro_rumble_decode(const uint8_t *rumble4, hd_rumble_t *out)
        //   decodes one 4-byte motor word into {hi_freq, hi_amp, lo_freq, lo_amp}.
        // Once decoded, the encode step re-encodes to the Switch 2 LRA
        // and writes into pro_feedback for get_feedback().
        // ----------------------------------------------------------------
        // Placeholder until decode + encode are implemented:
        (void)data; (void)len;
        // pro_feedback.rumble_left  = ...
        // pro_feedback.rumble_right = ...
    }
}

static bool switch_pro_mode_get_feedback(output_feedback_t* fb)
{
    if (!fb) return false;
    *fb = pro_feedback;
    return true;
}

static const uint8_t* switch_pro_mode_get_device_descriptor(void)
{
    return (const uint8_t*)&switch_pro_device_descriptor;
}

static const uint8_t* switch_pro_mode_get_config_descriptor(void)
{
    return switch_pro_config_descriptor;
}

static const uint8_t* switch_pro_mode_get_report_descriptor(void)
{
    return switch_pro_report_descriptor;
}

// ============================================================================
// MODE EXPORT
// ============================================================================

const usbd_mode_t switch_pro_mode = {
    .name = "Switch Pro",
    .mode = USB_OUTPUT_MODE_SWITCH_PRO,

    .get_device_descriptor = switch_pro_mode_get_device_descriptor,
    .get_config_descriptor = switch_pro_mode_get_config_descriptor,
    .get_report_descriptor = switch_pro_mode_get_report_descriptor,

    .init = switch_pro_mode_init,
    .send_report = switch_pro_mode_send_report,
    .is_ready = switch_pro_mode_is_ready,

    // Full Pro mode SUPPORTS feedback (unlike basic Switch mode):
    .handle_output = switch_pro_mode_handle_output,
    .get_rumble = NULL,                       // legacy simple-rumble iface unused
    .get_feedback = switch_pro_mode_get_feedback,

    .get_report = NULL,
    .get_class_driver = NULL,
    .task = NULL,
};
