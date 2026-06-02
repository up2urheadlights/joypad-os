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

## Verification

Diagnostic test on `feature/sinput-dynamic-capabilities`: temporarily set
`cached_has_motion = true` at module init in `sinput_mode.c` and prevented
per-event overwriting. Flashed to dongle, plugged into Windows with no
controller paired. **Steam's controller config showed the gyro/accel
calibration menu**, confirming that the host honors motion-capability
declaration in the features response when it's set at the moment of host
query. The bug is purely timing, not byte layout or descriptor structure.
Diagnostic reverted; real implementation requires re-enumeration on
capability change.

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

## Implementation order (staged)

Each step is a small commit, each is testable on hardware, each builds on
the previous. The discipline is: verify on hardware before moving to the next
step. If something doesn't work as expected at a given step, the design may
need revisiting before further investment.

### Step 1: TinyUSB re-enumeration proof-of-concept

Verify that `tud_disconnect()` + delay + `tud_connect()` produces a clean
re-enumeration on the Pico 2 W with joypad-os's existing TinyUSB
configuration, and that Steam recognizes the device coming back without
manual intervention.

Implementation: trigger re-enumeration from a testable signal — easiest
option is "after N seconds of running, re-enumerate once," logged over UART.
Flash, plug into PC, observe Steam.

Outcomes:
- Clean re-enumeration with Steam re-finding the device: green light, proceed.
- Device hangs, host shows error, or Steam loses the device permanently:
  red light, redesign required.

This is throwaway code — do not commit beyond this step. Once the
proof-of-concept is verified or fails, revert and proceed (or revisit design).

### Step 2: Empty-state initial enumeration

Make the dongle enumerate with empty capabilities at boot: no buttons, no
axes, no motion, no touch, no rumble, no LEDs. Steam should see the SInput
device but no configurable menus.

Implementation: modify `sinput_mode.c`'s features-response builder so that
when no controller has connected (`cached_*` flags all false and
`last_dev_addr == -1`), it emits a zeroed capability set. Verify by flashing
and plugging in with no controller paired.

### Step 3: Capability tracking + re-enumeration on first connect

Add the state machine that observes merged-event capabilities (motion,
touch, pressure, button count, etc.), records the last-advertised set, and
triggers re-enumeration when the current set differs.

Initial test: plug in dongle (empty state), pair a DS5, watch Steam refresh
to show the gyro menu.

### Step 4: Re-enumeration on controller swap

Verify that swapping controllers — DS5 (motion + touch) → Switch Pro 1
(motion only) → Xbox Series (none) — produces correct Steam UI updates at
each transition.

### Step 5: Return to empty on last controller disconnect

When all controllers disconnect, capabilities should revert to empty and the
device should re-enumerate. Steam should drop the configurable menus.

### Step 6: Debouncing

Add a stability window (suggested: 200-500 ms) before triggering
re-enumeration on capability change, to avoid thrashing on brief BT flaps.

### Step 7: README documentation

Document the user-facing behavior:
- Dongle appears in Steam immediately on plug-in.
- Configurable capabilities appear after a controller is paired and active.
- Brief reconnect occurs when controllers are swapped.

## Observed side-effects of the step 3 implementation

The step 3 implementation produces one user-visible change that wasn't part
of the stated goal but is worth recording:

- **Controller-specific graphics in Steam.** With the dongle alone (no
  controller paired), Steam shows generic input UI. After pairing a DS5,
  the re-enumeration causes Steam to read an updated features response in
  which `cached_face_style` and `cached_gamepad_type` reflect the connected
  controller (Sony face / PS5 type for DS5). Steam then displays the
  correct DualSense graphics and button labels. On stock joypad-os (no
  dynamic capabilities), the device identifies as generic for the entire
  session because the features response is only read once at boot, before
  any controller has connected — so Steam falls back to Xbox-style
  graphics.

  This is a side-effect of the same mechanism that surfaces the gyro menu:
  the features response is re-read after a controller pairs, so Steam sees
  the controller-specific identity in addition to the controller-specific
  capabilities.

  Verified for DS5 only. Other controllers (DS4, Switch Pro, Wii U Pro,
  etc.) should be tested as their drivers get the same synthetic-event
  treatment in follow-up work.

  **Note on persistence after disconnect**: After the controller
  disconnects, Steam continues to show the controller-specific graphics
  (e.g. DualSense art persists). Step 5 (return-to-empty on last
  disconnect) will need to reset `cached_face_style`, `cached_gamepad_type`,
  `cached_has_motion`, `cached_has_touch`, and `last_dev_addr` so the
  features response builder takes the empty branch again. Currently those
  values persist through the unpair re-enumeration.

## Known button-mapping issues observed during DS5 testing

These were observed during step 3 verification but are pre-existing bugs in
joypad-os main (verified by flashing the most recent upstream release).
Listed here as documentation; only the face-button swap is fixed in this
PR. The remaining issues are scoped for follow-up work.

1. **Face buttons swapped** — Cross↔Circle and Square↔Triangle. SInput
   descriptor byte 0 bit order in joypad-os doesn't match SInput-LIB's
   authoritative struct (south=bit 0, east=bit 1, west=bit 2, north=bit 3).
   Fixed in this PR (separate commit).

2. **Touchpad clicks fire the mic-mute event** instead of touchpad-click.

3. **Mic-mute button on DS5 produces no event.**

4. **Touchpad clicks appear in Steam's controller config as unlabeled
   binary inputs** (true/false, no label string).

5. **Macro Button 2 / Macro Button 3** (Steam-side labels) appear active
   when L2/R2 triggers are held — unclear whether this is a SInput
   descriptor mismapping or a Steam-side rendering of trigger-as-button.

## Future step: extend router's attached-transport exception to USB

Step 3's cleanup added `INPUT_TRANSPORT_BT_CLASSIC` and
`INPUT_TRANSPORT_BT_BLE` to the router's "attached device" exception
(the exception that lets a device register a player without input
activity, alongside the pre-existing `INPUT_TRANSPORT_NATIVE` and
`INPUT_TRANSPORT_GPIO`). `INPUT_TRANSPORT_USB` was deliberately left
out: it has the same logical property (a USB-host wired controller is
physically present from the moment it enumerates), but joypad-os
mainline ships no test rig where a wired USB-host controller could be
exercised, and we don't have hardware to verify it on. Adding it
speculatively would be an unverified change to a hot path.

The follow-up: wire a USB-A female breakout to the Pico's GP2/GP3
(default PIO USB pins), add Pico-PIO-USB host support to a test build,
plug in a wired DS5, and verify the same dynamic-capabilities re-enum
fires on plug-in (and Steam shows correct controller graphics) the
same way it does for BT. If yes, extend the `attached` exception in
both router.c sites to include `INPUT_TRANSPORT_USB`.

Reference test firmware: github.com/fhoedemakers/pico-pio-usb-gamepad-test.

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
