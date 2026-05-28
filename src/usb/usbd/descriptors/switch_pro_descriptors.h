// switch_pro_descriptors.h - Nintendo Switch Pro Controller USB descriptors
// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 (joypad-os contribution)
//
// Full Nintendo Switch Pro Controller emulation (VID 0x057E / PID 0x2009).
// Unlike switch_descriptors.h (which uses the HORI Pokken VID/PID to avoid the
// Pro Controller handshake), this presents the genuine Pro Controller identity
// and therefore REQUIRES the handshake/subcommand state machine implemented in
// switch_pro_mode.c. The full protocol unlocks IMU (motion) and HD rumble,
// which the basic HORI-based "Switch" mode cannot provide.
//
// Clean-source attribution:
//   - HID report descriptor + USB device/config layout:
//       GP2040-CE SwitchProDescriptors.h
//       SPDX-FileCopyrightText: Copyright (c) 2021 Jason Skuby (mytechtoybox.com)
//       License: MIT  (MIT -> Apache-2.0 compatible)
//   - Report-format / IMU layout / rumble:
//       dekuNukem/Nintendo_Switch_Reverse_Engineering (public docs)
//   - File style / tusb macro conventions: joypad-os switch_descriptors.h (Apache-2.0)
//
// NOTE: Following joypad-os convention, descriptor blobs are defined inline here
// as `static const` (matching switch_descriptors.h), so no separate .c is needed.

#ifndef SWITCH_PRO_DESCRIPTORS_H
#define SWITCH_PRO_DESCRIPTORS_H

#include <stdint.h>
#include "tusb.h"

// ============================================================================
// SWITCH PRO USB IDENTIFIERS
// ============================================================================

#define SWITCH_PRO_VID          0x057E  // Nintendo
#define SWITCH_PRO_PID          0x2009  // Pro Controller
#define SWITCH_PRO_BCD_DEVICE   0x0200  // v2.0
#define SWITCH_PRO_ENDPOINT_SIZE 64

// ============================================================================
// BUTTON DEFINITIONS  (Pro Controller 3-byte button layout, dekuNukem 0x30)
// Source: GP2040-CE SwitchProDescriptors.h (MIT) + dekuNukem public docs
// ============================================================================

// Byte 0 (right-side buttons + right shoulders)
#define SWITCH_PRO_MASK_Y       (1U << 0)
#define SWITCH_PRO_MASK_X       (1U << 1)
#define SWITCH_PRO_MASK_B       (1U << 2)
#define SWITCH_PRO_MASK_A       (1U << 3)
#define SWITCH_PRO_MASK_R       (1U << 6)
#define SWITCH_PRO_MASK_ZR      (1U << 7)

// Byte 1 (system + stick clicks)
#define SWITCH_PRO_MASK_MINUS   (1U << 0)
#define SWITCH_PRO_MASK_PLUS    (1U << 1)
#define SWITCH_PRO_MASK_R3      (1U << 2)
#define SWITCH_PRO_MASK_L3      (1U << 3)
#define SWITCH_PRO_MASK_HOME    (1U << 4)
#define SWITCH_PRO_MASK_CAPTURE (1U << 5)

// Byte 2 (dpad + left shoulders)
#define SWITCH_PRO_MASK_DOWN    (1U << 0)
#define SWITCH_PRO_MASK_UP      (1U << 1)
#define SWITCH_PRO_MASK_RIGHT   (1U << 2)
#define SWITCH_PRO_MASK_LEFT    (1U << 3)
#define SWITCH_PRO_MASK_L       (1U << 6)
#define SWITCH_PRO_MASK_ZL      (1U << 7)

// Analog stick range (Pro Controller 12-bit packed values, 0x000-0xFFF)
#define SWITCH_PRO_STICK_MIN    0x0000
#define SWITCH_PRO_STICK_MID    0x0800
#define SWITCH_PRO_STICK_MAX    0x0FFF

// ============================================================================
// REPORT IDs / SUBCOMMANDS
// Source: GP2040-CE SwitchProDescriptors.h (MIT) + dekuNukem public docs
// ============================================================================

typedef enum {
    SWPRO_REPORT_OUTPUT_00   = 0x00,
    SWPRO_REPORT_FEATURE     = 0x01,  // rumble + subcommand (OUT)
    SWPRO_REPORT_OUTPUT_10   = 0x10,  // rumble-only (OUT)
    SWPRO_REPORT_OUTPUT_21   = 0x21,
    SWPRO_REPORT_INPUT_30    = 0x30,  // standard full input report (IN)
    SWPRO_REPORT_CONFIG_80   = 0x80,  // USB config / handshake (OUT)
    SWPRO_REPORT_USB_IN_81   = 0x81,
} switch_pro_report_id_t;

typedef enum {
    SWPRO_USB_IDENTIFY            = 0x01,
    SWPRO_USB_HANDSHAKE           = 0x02,
    SWPRO_USB_BAUD_RATE           = 0x03,
    SWPRO_USB_DISABLE_USB_TIMEOUT = 0x04,
    SWPRO_USB_ENABLE_USB_TIMEOUT  = 0x05,
} switch_pro_usb_subtype_t;

typedef enum {
    SWPRO_CMD_GET_STATE         = 0x00,
    SWPRO_CMD_BT_PAIR           = 0x01,
    SWPRO_CMD_REQ_DEVICE_INFO   = 0x02,
    SWPRO_CMD_SET_MODE          = 0x03,
    SWPRO_CMD_TRIGGER_BUTTONS   = 0x04,
    SWPRO_CMD_SET_SHIPMENT      = 0x08,
    SWPRO_CMD_SPI_READ          = 0x10,
    SWPRO_CMD_SET_NFC_IR_CONFIG = 0x21,  // stub-ACK (NFC/amiibo = future work)
    SWPRO_CMD_SET_NFC_IR_STATE  = 0x22,  // stub-ACK
    SWPRO_CMD_SET_PLAYER_LIGHTS = 0x30,
    SWPRO_CMD_GET_PLAYER_LIGHTS = 0x31,
    SWPRO_CMD_SET_HOME_LIGHT    = 0x38,
    SWPRO_CMD_TOGGLE_IMU        = 0x40,
    SWPRO_CMD_IMU_SENSITIVITY   = 0x41,
    SWPRO_CMD_READ_IMU          = 0x43,
    SWPRO_CMD_ENABLE_VIBRATION  = 0x48,
    SWPRO_CMD_GET_VOLTAGE       = 0x50,
} switch_pro_command_t;

// ============================================================================
// 0x30 INPUT REPORT  (dekuNukem report-format + GP2040-CE MIT field layout)
// [id][timer][conn][btn0..2][L x3][R x3][vib_ack][IMU 36 = 3 frames]
// ============================================================================

typedef struct __attribute__((packed)) {
    uint8_t  report_id;        // 0x30
    uint8_t  timer;
    uint8_t  conn_info;        // battery + connection nibble
    uint8_t  buttons[3];
    uint8_t  left_stick[3];    // 12-bit packed X/Y
    uint8_t  right_stick[3];
    uint8_t  vibrator_ack;
    uint8_t  imu[36];          // 3 frames * (accel xyz + gyro xyz), int16 LE
} switch_pro_input_report_t;

// ============================================================================
// HID REPORT DESCRIPTOR
// Verbatim from GP2040-CE SwitchProDescriptors.h (MIT). 203 bytes.
// ============================================================================

static const uint8_t switch_pro_report_descriptor[] = {
    0x05, 0x01,        // Usage Page (Generic Desktop Ctrls)
    0x15, 0x00,        // Logical Minimum (0)
    0x09, 0x04,        // Usage (Joystick)
    0xA1, 0x01,        // Collection (Application)
    0x85, 0x30,        //   Report ID (48)
    0x05, 0x01,        //   Usage Page (Generic Desktop Ctrls)
    0x05, 0x09,        //   Usage Page (Button)
    0x19, 0x01,        //   Usage Minimum (0x01)
    0x29, 0x0A,        //   Usage Maximum (0x0A)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x01,        //   Logical Maximum (1)
    0x75, 0x01,        //   Report Size (1)
    0x95, 0x0A,        //   Report Count (10)
    0x55, 0x00,        //   Unit Exponent (0)
    0x65, 0x00,        //   Unit (None)
    0x81, 0x02,        //   Input (Data,Var,Abs)
    0x05, 0x09,        //   Usage Page (Button)
    0x19, 0x0B,        //   Usage Minimum (0x0B)
    0x29, 0x0E,        //   Usage Maximum (0x0E)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x01,        //   Logical Maximum (1)
    0x75, 0x01,        //   Report Size (1)
    0x95, 0x04,        //   Report Count (4)
    0x81, 0x02,        //   Input (Data,Var,Abs)
    0x75, 0x01,        //   Report Size (1)
    0x95, 0x02,        //   Report Count (2)
    0x81, 0x03,        //   Input (Const,Var,Abs)
    0x0B, 0x01, 0x00, 0x01, 0x00,  //   Usage (0x010001)
    0xA1, 0x00,        //   Collection (Physical)
    0x0B, 0x30, 0x00, 0x01, 0x00,  //     Usage (0x010030)
    0x0B, 0x31, 0x00, 0x01, 0x00,  //     Usage (0x010031)
    0x0B, 0x32, 0x00, 0x01, 0x00,  //     Usage (0x010032)
    0x0B, 0x35, 0x00, 0x01, 0x00,  //     Usage (0x010035)
    0x15, 0x00,        //     Logical Minimum (0)
    0x27, 0xFF, 0xFF, 0x00, 0x00,  //     Logical Maximum (65534)
    0x75, 0x10,        //     Report Size (16)
    0x95, 0x04,        //     Report Count (4)
    0x81, 0x02,        //     Input (Data,Var,Abs)
    0xC0,              //   End Collection
    0x0B, 0x39, 0x00, 0x01, 0x00,  //   Usage (0x010039)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x07,        //   Logical Maximum (7)
    0x35, 0x00,        //   Physical Minimum (0)
    0x46, 0x3B, 0x01,  //   Physical Maximum (315)
    0x65, 0x14,        //   Unit (Eng Rot:Angular Pos)
    0x75, 0x04,        //   Report Size (4)
    0x95, 0x01,        //   Report Count (1)
    0x81, 0x02,        //   Input (Data,Var,Abs)
    0x05, 0x09,        //   Usage Page (Button)
    0x19, 0x0F,        //   Usage Minimum (0x0F)
    0x29, 0x12,        //   Usage Maximum (0x12)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x01,        //   Logical Maximum (1)
    0x75, 0x01,        //   Report Size (1)
    0x95, 0x04,        //   Report Count (4)
    0x81, 0x02,        //   Input (Data,Var,Abs)
    0x75, 0x08,        //   Report Size (8)
    0x95, 0x34,        //   Report Count (52)
    0x81, 0x03,        //   Input (Const,Var,Abs)
    0x06, 0x00, 0xFF,  //   Usage Page (Vendor Defined 0xFF00)
    0x85, 0x21,        //   Report ID (33)
    0x09, 0x01,        //   Usage (0x01)
    0x75, 0x08,        //   Report Size (8)
    0x95, 0x3F,        //   Report Count (63)
    0x81, 0x03,        //   Input (Const,Var,Abs)
    0x85, 0x81,        //   Report ID (-127)
    0x09, 0x02,        //   Usage (0x02)
    0x75, 0x08,        //   Report Size (8)
    0x95, 0x3F,        //   Report Count (63)
    0x81, 0x03,        //   Input (Const,Var,Abs)
    0x85, 0x01,        //   Report ID (1)
    0x09, 0x03,        //   Usage (0x03)
    0x75, 0x08,        //   Report Size (8)
    0x95, 0x3F,        //   Report Count (63)
    0x91, 0x83,        //   Output (Const,Var,Abs,Volatile)
    0x85, 0x10,        //   Report ID (16)
    0x09, 0x04,        //   Usage (0x04)
    0x75, 0x08,        //   Report Size (8)
    0x95, 0x3F,        //   Report Count (63)
    0x91, 0x83,        //   Output (Const,Var,Abs,Volatile)
    0x85, 0x80,        //   Report ID (-128)
    0x09, 0x05,        //   Usage (0x05)
    0x75, 0x08,        //   Report Size (8)
    0x95, 0x3F,        //   Report Count (63)
    0x91, 0x83,        //   Output (Const,Var,Abs,Volatile)
    0x85, 0x82,        //   Report ID (-126)
    0x09, 0x06,        //   Usage (0x06)
    0x75, 0x08,        //   Report Size (8)
    0x95, 0x3F,        //   Report Count (63)
    0x91, 0x83,        //   Output (Const,Var,Abs,Volatile)
    0xC0,              // End Collection
};

// ============================================================================
// DEVICE DESCRIPTOR  (joypad-os tusb_desc_device_t convention)
// ============================================================================

static const tusb_desc_device_t switch_pro_device_descriptor = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = 0x00,
    .bDeviceSubClass    = 0x00,
    .bDeviceProtocol    = 0x00,
    .bMaxPacketSize0    = 64,
    .idVendor           = SWITCH_PRO_VID,
    .idProduct          = SWITCH_PRO_PID,
    .bcdDevice          = SWITCH_PRO_BCD_DEVICE,
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x00,
    .bNumConfigurations = 0x01
};

// ============================================================================
// CONFIGURATION DESCRIPTOR  (joypad-os tusb macro convention)
// 9 (config) + 9 (interface) + 9 (HID) + 7 (EP IN) + 7 (EP OUT) = 41
// ============================================================================

#define SWITCH_PRO_CONFIG_TOTAL_LEN  (TUD_CONFIG_DESC_LEN + TUD_HID_INOUT_DESC_LEN)

static const uint8_t switch_pro_config_descriptor[] = {
    // Config descriptor (1 interface, config value 1, 500mA, remote wakeup)
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, SWITCH_PRO_CONFIG_TOTAL_LEN, 0xA0, 250),

    // Interface (2 endpoints, HID class)
    9, TUSB_DESC_INTERFACE, 0, 0, 2, TUSB_CLASS_HID, 0, 0, 0,

    // HID descriptor (bcdHID 1.11, report descriptor length)
    9, HID_DESC_TYPE_HID, U16_TO_U8S_LE(0x0111), 0, 1,
       HID_DESC_TYPE_REPORT, U16_TO_U8S_LE(sizeof(switch_pro_report_descriptor)),

    // Endpoint IN (0x81) - input reports
    7, TUSB_DESC_ENDPOINT, 0x81, TUSB_XFER_INTERRUPT, U16_TO_U8S_LE(64), 8,

    // Endpoint OUT (0x01) - rumble / subcommands
    7, TUSB_DESC_ENDPOINT, 0x01, TUSB_XFER_INTERRUPT, U16_TO_U8S_LE(64), 8,
};

// String descriptors (genuine Pro Controller identity)
#define SWITCH_PRO_MANUFACTURER  "Nintendo Co., Ltd."
#define SWITCH_PRO_PRODUCT       "Pro Controller"

#endif // SWITCH_PRO_DESCRIPTORS_H
