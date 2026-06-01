# SInput Output: Dynamic Capabilities

Design notes for `feature/sinput-dynamic-capabilities`.

## Problem

The current SInput output mode declares the dongle's capabilities (motion,
touchpad, rumble, LEDs, etc.) in the features response to the host. The host
(SDL via Steam) reads this response once during USB enumeration, caches it,
and does not re-read on its own.

The dongle's USB device side enumerates immediately on power-up — before any
controller has connected over Bluetooth. At that moment, `cached_has_motion`
and `cached_has_touch` are both false, so the features response advertises no
motion and no touchpad. By the time a motion-capable controller (e.g. DS5)
actually pairs and reports, the host has already cached the "no motion"
features response. SDL never registers motion sensors for the device, so
Steam's controller configuration UI never shows a gyro/accelerometer
calibration section.

Symptoms observed on Windows + Steam:
- Joypad-os SInput device: shows joysticks, rumble toggle, LED settings; **no
  gyro/accel section**, even with a DS5 actively paired and streaming motion.
- HHL `Pico-W-SInput-Example` (canonical reference, MIT-0): shows joysticks
  and gyro/accel section. Confirms SDL and Steam do correctly surface SInput
  motion when capability is advertised at enumeration time.

## Investigation summary (already ruled out)

Reading SDL's `src/joystick/hidapi/SDL_hidapi_sinput.c` and comparing
joypad-os's SInput output against HHL's reference:

- **VID/PID:** match (`0x2E8A:0x10C6`). SDL's SInput driver identifies by
  VID/PID only, not by HID descriptor inspection.
- **Features-response byte layout:** matches SDL's `ProcessSDLFeaturesResponse`
  field offsets (gamepad type at byte 4, sub_type at byte 5, polling rate at
  6-7, accel range at 8-9, gyro range at 10-11, etc.).
- **HID report descriptor:** differs from HHL's lib (joypad-os uses standard
  Generic Desktop usage codes for motion, lib uses Vendor Defined). Confirmed
  not the cause — SDL's SInput driver does not read the report descriptor.
- **Wire-format input report:** byte offsets match SDL's `SINPUT_REPORT_IDX_*`
  constants. Motion bytes flow correctly when sensors are enabled.
- **MS OS descriptors:** neither implements them. Not a differentiator.

## Root cause

Timing. Features-response query happens before motion-capable input has been
seen. Host caches the inaccurate response. No mechanism currently forces the
host to re-read.

## Proposed design

### USB device lifecycle bound to merged-event capabilities

1. **At boot / USB power-up:** dongle does **not** call `tud_connect()`
   immediately. The Bluetooth stack and other infrastructure come up, but the
   USB device side stays disconnected until there's something meaningful to
   advertise.

   Alternative (refined during design): enumerate immediately with **empty
   capabilities** (zero buttons, no sticks, no motion, no touch, no rumble, no
   LEDs). Steam sees a SInput device — confirms the dongle is alive — but
   nothing to configure. Honest at every moment.

2. **When the first controller pairs and reports:** `tud_disconnect()`, brief
   delay, `tud_connect()` with capabilities reflecting the connected
   controller. Steam re-enumerates the device and reads accurate capabilities.

3. **On controller swap or re-pair:** if the merged event's capability flags
   (`has_motion`, `has_touch`, `has_pressure`, button counts, etc.) change
   meaningfully vs. the last-advertised set, re-enumerate.

4. **On last controller disconnect:** return to empty/no-capability state via
   re-enumeration.

### Capability source: the router's merged event

The router (`src/core/router/router.c`) already combines capability flags
correctly across multiple connected controllers in MERGE_BLEND mode:
- `has_motion`: union — true if any connected device has motion
- `has_touch`: union — true if any connected device has touch
- Buttons: OR'd
- Sticks: furthest-from-center wins
- Triggers: max wins

So the SInput output mode does **not** need its own multi-controller capability
combination logic. It observes the merged event's `has_*` flags as authoritative
truth at any moment, and re-enumerates when those flags transition.

This works across all router modes (MERGE_ALL, MERGE_BLEND, MERGE_PRIORITY,
SIMPLE multi-output) because each operates on the same `input_event_t` shape
with the same `has_*` semantics.

## Open design questions

1. **TinyUSB lifecycle viability.** Does `tud_disconnect()` / `tud_connect()`
   work cleanly mid-runtime on the Pico's TinyUSB stack? Worth verifying with
   a small proof-of-concept (e.g., a button-triggered re-enumeration) before
   committing to the design. If TinyUSB doesn't support clean re-enumeration,
   we need an alternative (e.g., software reset of just the USB device side,
   or a full firmware reboot — neither ideal).

2. **Debouncing.** Capability changes during a brief BT flap (controller drops
   for a second, comes back) should not cause re-enumeration. Suggested: wait
   200-500ms after a capability change to confirm stability before triggering
   re-enumerate.

3. **State preservation across re-enumeration.** Ongoing rumble commands, LED
   state, etc. — do they survive the brief disconnect? Likely Steam re-reads
   and re-sends; verify.

4. **Bond persistence.** Re-enumeration is USB-only; BT bonds should not be
   affected. Verify.

5. **Scope: sinput-only or generalize?** joypad-os has many output modes
   (DualShock 3, Switch Pro, Xbox, dreamcast, etc.). Other modes may or may
   not have the same capability-caching issue with their respective hosts.
   Recommended initial scope: sinput-only. Generalizing is a separate concern
   once sinput is proven to work.

## User-facing behavior to document in README

- Dongle appears in Steam immediately on plug-in (confirms it's alive).
- Configurable capabilities (motion, etc.) only appear after a controller is
  paired and active.
- Switching controllers causes a brief reconnect in Steam as the dongle
  re-advertises with the new controller's capabilities.

Precedent: ds5dongle uses a related model (no enumeration until paired). This
design is friendlier — device visible from boot, capabilities update on pair.

## Suggested implementation order

1. **Proof-of-concept:** verify `tud_disconnect()` + delay + `tud_connect()`
   works cleanly on Pico 2 W with TinyUSB. Add a UART command or button
   trigger; observe Steam's reaction.
2. **Capability tracking:** add to `sinput_mode.c` a `last_advertised_caps`
   struct holding the capability flags that were active at the last
   enumeration. Compare against current merged-event flags on each event.
3. **Debounced re-enumeration trigger:** when current ≠ last for N consecutive
   events spanning at least M ms, schedule re-enumeration.
4. **Re-enumeration mechanics:** `tud_disconnect()`, sleep N ms, update
   advertised capabilities to current merged state, `tud_connect()`.
5. **Empty-state on no controllers:** when last controller disconnects, drive
   capabilities to empty state and re-enumerate.
6. **README documentation:** add user-facing behavior section.
7. **Test matrix:**
   - DS5 only (motion + touch + rumble)
   - Switch Pro 1 only (motion + rumble, no touch)
   - DS3 only (limited motion, pressure-sensitive buttons)
   - Xbox Series only (no motion)
   - DS5 → swap to Switch Pro 1 → swap back
   - DS5 + Switch Pro 1 simultaneously (MERGE_BLEND)
   - DS5 disconnects mid-session, reconnects
   - All controllers disconnect, Steam should show capability changes

## References

- HHL canonical reference firmware: `Pico-W-SInput-Example` (MIT-0). Confirmed
  working in Steam: joysticks + gyro/accel calibration visible.
- HHL SInput library: `SINPUT-LIB-HID` (MIT-0). Source of features-response
  byte layout, HID descriptor for reference (not required).
- SDL's SInput driver: `src/joystick/hidapi/SDL_hidapi_sinput.c`.
  Identifies devices by VID/PID; parses features response at byte offsets
  documented above; calls `SDL_PrivateJoystickAddSensor` only when
  `accelerometer_supported` / `gyroscope_supported` are true in the
  features response.
- ds5dongle: precedent for "no USB device until controller paired"
  user-facing behavior.
- Investigation: see conversation history (`/mnt/transcripts/`).
