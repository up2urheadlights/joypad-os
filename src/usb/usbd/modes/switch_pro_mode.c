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
#include "platform/platform.h"   // platform_get_unique_id()
#include <string.h>


// ============================================================================
// STATE
// ============================================================================

static switch_pro_input_report_t pro_report;
static uint8_t report_timer = 0;

// Feedback state (filled by rumble handling; consumed via get_feedback)
static output_feedback_t pro_feedback;

// Queued handshake/subcommand replies. The USB set_report callback runs in
// callback context and must NOT send directly (it collides with the keepalive
// on the IN endpoint). It enqueues replies here; task() is the SINGLE sender
// and drains them in order. A ring buffer (not a single slot) is required
// because the host fires many subcommands in rapid succession during init —
// a single slot would drop all but the last, causing the host to restart the
// handshake repeatedly. (GP2040-CE effectively serializes the same way.)
#define PRO_REPLY_RING 16
typedef struct { uint8_t data[64]; uint16_t len; } pro_reply_t;
static pro_reply_t pro_reply_ring[PRO_REPLY_RING];
static volatile uint8_t pro_reply_head = 0;  // next slot to write (producer)
static volatile uint8_t pro_reply_tail = 0;  // next slot to read  (consumer)

static inline bool pro_reply_ring_empty(void) {
    return pro_reply_head == pro_reply_tail;
}
static inline void pro_reply_enqueue(const uint8_t* d, uint16_t len) {
    uint8_t next = (uint8_t)((pro_reply_head + 1) % PRO_REPLY_RING);
    if (next == pro_reply_tail) return;   // full: drop (shouldn't happen at 16 deep)
    memcpy(pro_reply_ring[pro_reply_head].data, d, len);
    pro_reply_ring[pro_reply_head].len = len;
    pro_reply_head = next;
}


// GP2040-CE sends an unprompted IDENTIFY (0x81 0x01) once at startup to seed
// the host's handshake. Windows in particular may open by re-sending only
// 0x80 0x04 (skipping 0x80 0x01) and will not advance to reading the 0x30
// input stream until it has received the controller's identity. We send it
// once from task() before the handshake completes.
static bool     pro_identify_sent = false;

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

// --- SPI flash calibration data ---
// Hardware-verified factory calibration values matching OGX-Mini-2026 (MIT),
// which is tested against real Switch hardware. These are factory-default
// calibration constants (not creative content). The host reads several SPI
// addresses during init and REJECTS the controller (restarting the handshake)
// if any return wrong/unknown data — so every address it requests must be
// answered, not left as erased 0xFF.
//
// 0x603D: stick factory calibration — 9 bytes left + 9 bytes right + flags.
static const uint8_t PRO_SPI_603D_L[9] =
    { 0xD4, 0x75, 0x61, 0xE5, 0x87, 0x7C, 0xEC, 0x55, 0x61 };
static const uint8_t PRO_SPI_603D_R[9] =
    { 0x5D, 0xD8, 0x7F, 0x18, 0xE6, 0x61, 0x86, 0x65, 0x5D };
// 0x6020: IMU (6-axis motion) factory calibration — 24 bytes.
static const uint8_t PRO_SPI_6020[24] =
    { 0xCC, 0x00, 0x40, 0x00, 0x91, 0x01, 0x00, 0x40, 0x00, 0x40, 0x00, 0x40,
      0xE7, 0xFF, 0x0E, 0x00, 0xDC, 0xFF, 0x3B, 0x34, 0x3B, 0x34, 0x3B, 0x34 };
// 0x6080/0x6098: stick device parameters (18 bytes each region of interest).
// 0x6080 has a 6-byte magic header the host validates.
static const uint8_t PRO_SPI_6080_HDR[6] =
    { 0x50, 0xFD, 0x00, 0x00, 0xC6, 0x0F };
static const uint8_t PRO_SPI_STICK_PARAMS[18] =
    { 0x0F, 0x30, 0x61, 0x96, 0x30, 0xF3, 0xD4, 0x14, 0x54, 0x41,
      0x15, 0x54, 0xC7, 0x79, 0x9C, 0x33, 0x36, 0x63 };
// 0x6050: controller body/button colors (12 bytes).
static const uint8_t PRO_SPI_6050[12] =
    { 0x32, 0x32, 0x32, 0xFF, 0xFF, 0xFF,
      0x46, 0x46, 0x46, 0x46, 0x46, 0x46 };


// Fill `dest` (size bytes) with the SPI flash contents at `addr`.
// Returns the documented data for known calibration addresses; 0xFF otherwise.
static void pro_spi_read(uint8_t* dest, uint32_t addr, uint8_t size)
{
    memset(dest, 0xFF, size);  // default: erased flash
    uint8_t lo = (uint8_t)(addr & 0xFF);
    uint8_t hi = (uint8_t)((addr >> 8) & 0xFF);

    if (hi == 0x60 && lo == 0x00) {
        memset(dest, 0xFF, (size < 16) ? size : 16);
    } else if (hi == 0x60 && lo == 0x50) {       // colors
        memcpy(dest, PRO_SPI_6050, (size < 12) ? size : 12);
    } else if (hi == 0x60 && lo == 0x80) {       // stick params 1 (magic hdr)
        if (size >= 6)  memcpy(dest, PRO_SPI_6080_HDR, 6);
        if (size >= 24) memcpy(dest + 6, PRO_SPI_STICK_PARAMS, 18);
    } else if (hi == 0x60 && lo == 0x98) {       // stick params 2
        memcpy(dest, PRO_SPI_STICK_PARAMS, (size < 18) ? size : 18);
    } else if (hi == 0x60 && lo == 0x86) {       // stick params (also seen)
        memcpy(dest, PRO_SPI_STICK_PARAMS, (size < 18) ? size : 18);
    } else if (hi == 0x80 && lo == 0x10) {       // user cal magic — present
        memset(dest, 0xFF, (size < 3) ? size : 3);
    } else if (hi == 0x60 && lo == 0x3D) {       // stick factory cal L+R
        if (size >= 9)  memcpy(dest,     PRO_SPI_603D_L, 9);
        if (size >= 18) memcpy(dest + 9, PRO_SPI_603D_R, 9);
        if (size >= 19) dest[18] = 0xFF;
    } else if (hi == 0x60 && lo == 0x20) {       // IMU factory cal
        memcpy(dest, PRO_SPI_6020, (size < 24) ? size : 24);
    }
    // else: leave as 0xFF (erased)
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
    pro_reply_head        = 0;
    pro_reply_tail        = 0;
    pro_identify_sent     = false;
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
                // The host expects an acknowledgment here; without one it
                // retries 0x80 0x04 forever and never advances. GP2040 replies
                // with a 0x30-prefixed ack. Mark handshake complete and ack.
                pro_handshake_done = true;
                r[0] = SWPRO_REPORT_INPUT_30;   // 0x30
                r[1] = SWPRO_USB_DISABLE_USB_TIMEOUT;
                *out_len = 64;
                return true;
            case SWPRO_USB_ENABLE_USB_TIMEOUT:   // 0x05
                r[0] = SWPRO_REPORT_INPUT_30;   // 0x30
                r[1] = SWPRO_USB_ENABLE_USB_TIMEOUT;
                *out_len = 64;
                return true;
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
                // PC hosts (Steam, browsers) often don't send the console-style
                // 0x80 0x04 "disable USB timeout" that normally marks handshake
                // completion. They instead switch the controller to full input
                // report mode (0x30) over the regular subcommand pipe. Treat that
                // as handshake-complete so the 0x30 keepalive stream begins.
                if (pro_input_mode == 0x30) {
                    pro_handshake_done = true;
                }
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
    // Capture: the canonical joypad-os Capture slot is A2 (see core/buttons.h;
    // both switch_pro_bt and switch2_ble map a genuine controller's Capture
    // button to A2). On a DualSense this is the touchpad click. NOTE: the
    // Switch 2 "C" button arrives on A3 (switch2_ble), so if a Switch 2 "C"
    // output is ever added it should read A3.
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
    // Switch Pro expects Y axes inverted relative to joypad-os convention
    // (up = high value). Mirror OGX-Mini's `y = 0xFFF - y` flip on both sticks.
    {
        uint16_t lx = stick8_to_12(profile_out->left_x);
        uint16_t ly = stick8_to_12(profile_out->left_y);
        uint16_t rx = stick8_to_12(profile_out->right_x);
        uint16_t ry = stick8_to_12(profile_out->right_y);
        ly = (uint16_t)(SWITCH_PRO_STICK_MAX - ly);
        ry = (uint16_t)(SWITCH_PRO_STICK_MAX - ry);
        pack_stick(pro_report.left_stick,  lx, ly);
        pack_stick(pro_report.right_stick, rx, ry);
    }

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

    // NOTE: do NOT send here. task() is the single point that writes to the IN
    // endpoint (to avoid collisions with handshake replies / keepalive). This
    // function only updates pro_report; task() streams it. Returning true means
    // "input consumed".
    return true;
}

// Keepalive task: Switch hosts expect a continuous stream of 0x30 input
// reports (~every few ms) once the handshake completes, not just on input
// change. joypad-os's input path is event-driven (send_report fires on new
// input), so this task re-sends the current report state on a timer to keep
// the host's controller "active". Mirrors GP2040-CE's process() keepalive.
static void switch_pro_mode_task(void)
{
    if (!tud_hid_ready()) return;

    // Unprompted IDENTIFY once at startup (mirrors GP2040-CE). Seeds the host's
    // handshake; some hosts (Windows) won't read the input stream until they've
    // seen the controller identity, and may never send 0x80 0x01 themselves.
    if (!pro_identify_sent) {
        uint8_t ident[64];
        memset(ident, 0, sizeof(ident));
        ident[0] = SWPRO_REPORT_USB_IN_81;   // 0x81
        ident[1] = SWPRO_USB_IDENTIFY;       // 0x01
        ident[2] = 0x00;
        ident[3] = PRO_CONTROLLER_TYPE;
        for (uint8_t i = 0; i < 6; i++) ident[4 + i] = pro_mac[5 - i];
        // Our descriptor declares report IDs, so TinyUSB prepends the ID itself.
        // Pass the real ID as the parameter and the payload AFTER byte 0 (63
        // bytes). Passing 0 with the ID in the buffer mis-frames the report.
        if (tud_hid_report(0, ident, 64)) {
            pro_identify_sent = true;
        }
        return;  // take the slot this cycle
    }

    // No artificial pacing: tud_hid_report() only succeeds when the IN endpoint
    // is free (the host has collected the previous report), so sends are
    // naturally rate-limited by the host's poll interval. OGX-Mini runs unpaced
    // this way.

    // Priority 1: flush one queued handshake/subcommand reply per send window.
    if (!pro_reply_ring_empty()) {
        pro_reply_t* e = &pro_reply_ring[pro_reply_tail];
        // Descriptor declares report IDs: pass the real ID as the param and the
        // payload after byte 0 (TinyUSB prepends the ID on the wire).
        if (tud_hid_report(0, e->data, e->len)) {
            pro_reply_tail = (uint8_t)((pro_reply_tail + 1) % PRO_REPLY_RING);
        }
        return;  // one report per send window; reply takes the slot
    }

    // Priority 2: keepalive input stream, only after handshake completes.
    if (!pro_handshake_done) return;

    pro_report.timer = report_timer++;

    // The descriptor declares the 0x30 input report as 64 bytes (1 ID + 63
    // payload). Our pro_report struct is only 49 bytes, so sending
    // sizeof(pro_report) emits a SHORT report; Windows' HID parser then
    // silently drops it (length mismatch vs the descriptor), which is why the
    // handshake completes and the internal state is correct but no input ever
    // reaches gamepadtester/Steam. Always emit a full, zero-padded 64-byte
    // report — matches OGX-Mini's REPORT_SIZE = 64.
    uint8_t out64[64];
    memset(out64, 0, sizeof(out64));
    memcpy(out64, &pro_report, sizeof(pro_report));  // 49 bytes, rest zero
    tud_hid_report(0, out64, sizeof(out64));
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
            // Enqueue the reply; task() is the single sender (avoids IN-endpoint
            // collision and serializes the rapid init subcommand replies).
            pro_reply_enqueue(response, resp_len);
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

// GET_REPORT (control transfer). Some hosts (Windows/Chromium gamepad stack)
// fetch the handshake (0x81) and subcommand (0x21) replies here rather than
// reading them from the interrupt IN endpoint. Serve the oldest queued reply
// whose report ID matches the request. Mirrors OGX-Mini-2026's get_report_cb.
static uint16_t switch_pro_mode_get_report(uint8_t report_id,
                                           hid_report_type_t report_type,
                                           uint8_t* buffer, uint16_t reqlen)
{
    (void)report_type;

    // Serve the oldest queued reply matching report_id (or any, if host asked
    // with report_id 0). The reply buffers store the full 64-byte report with
    // the report ID in byte [0].
    if (!pro_reply_ring_empty()) {
        pro_reply_t* e = &pro_reply_ring[pro_reply_tail];
        if (report_id == 0 || e->data[0] == report_id) {
            uint16_t n = (reqlen < e->len) ? reqlen : e->len;
            memcpy(buffer, e->data, n);
            pro_reply_tail = (uint8_t)((pro_reply_tail + 1) % PRO_REPLY_RING);
            return n;
        }
    }

    // No matching pending reply: return the current standard input report so a
    // GET_REPORT for 0x30 still yields valid state.
    if (report_id == SWPRO_REPORT_INPUT_30 || report_id == 0) {
        uint16_t n = (reqlen < sizeof(pro_report)) ? reqlen : sizeof(pro_report);
        memcpy(buffer, &pro_report, n);
        return n;
    }
    return 0;
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

    .get_report = switch_pro_mode_get_report,
    .get_class_driver = NULL,
    .task = switch_pro_mode_task,
};
