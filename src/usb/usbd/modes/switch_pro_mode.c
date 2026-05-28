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
#include "platform/platform.h"   // platform_time_us(), platform_random()
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
// PROTOCOL STATE MACHINE  (stub — to be implemented from
// GP2040-CE SwitchProDriver.cpp (MIT) + dekuNukem public docs).
// Kept file-local (static) for now; refactor to its
// own file.
// ============================================================================

static void switch_pro_protocol_init(void)
{
    // TODO: reset handshake state machine, IMU-enable flag, vibration flag,
    // player count, SPI calibration cache, MAC, etc.
}

// Returns true if the report was a handshake/subcommand the protocol layer
// handled (and optionally produced a response in out_response/out_len).
// Returns false if it's not a protocol report (e.g. pure rumble), so the
// caller can route it to rumble decode.
static bool switch_pro_protocol_handle_output(uint8_t report_id,
                                              const uint8_t* data, uint16_t len,
                                              uint8_t* out_response, uint16_t* out_len)
{
    (void)report_id; (void)data; (void)len;
    (void)out_response;
    *out_len = 0;

    // TODO: implement
    //   - 0x80 IDENTIFY/HANDSHAKE/BAUD_RATE/timeout (USB handshake)
    //   - 0x01 subcommands: REQ_DEVICE_INFO, SPI_READ (calibration),
    //     SET_MODE, TOGGLE_IMU (0x40), ENABLE_VIBRATION (0x48),
    //     SET_PLAYER_LIGHTS, SET_NFC_IR_* (stub-ACK), etc.
    //   - all replying with the 0x21 subcommand-reply / 0x81 USB-reply formats
    //     and the 0x80-ACK byte conventions (dekuNukem docs).
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
    return tud_hid_ready();
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
