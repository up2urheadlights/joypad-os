#!/usr/bin/env python3
"""joypad-bot — VLM plays NES games through a controller-only tool schema.

v1 architecture (this file): nes-py emulator + OpenAI vision API, pure
software loop. The model sees a frame and emits a single press() tool call
holding a list of controller buttons; the effector translates that into the
emulator's next step. No game-specific logic, no per-game prompt — change
the ROM and the same code plays a different game.

Eventual v2: swap the NESPyEffector for a JoypadLiveEffector (HTTP POSTs
into the joypad-live REST bridge) — same agent code, same tool schema, real
adapter driving any console. Eventual v3: same loop, real console + HDMI
capture replacing the emulator frame source. The press() abstraction is
deliberately the universal controller-side interface for all three paths.

Usage:
    pip install -r requirements.txt
    export OPENAI_API_KEY=sk-...
    python3 bot.py --rom /Users/robert/Downloads/tetris.nes --render
"""

import argparse
import base64
import io
import json
import os
import sys
import time
from pathlib import Path

from nes_py import NESEnv
from openai import OpenAI
from PIL import Image


# --- Controller -------------------------------------------------------------
# Game-agnostic NES controller schema. The agent never sees Tetris-specific
# concepts; it only knows these eight buttons exist.

NES_BUTTON_BITS = {
    "A":          1 << 0,
    "B":          1 << 1,
    "SELECT":     1 << 2,
    "START":      1 << 3,
    "DPAD_UP":    1 << 4,
    "DPAD_DOWN":  1 << 5,
    "DPAD_LEFT":  1 << 6,
    "DPAD_RIGHT": 1 << 7,
}


def buttons_to_action(buttons):
    """Translate a list of controller buttons into nes-py's 8-bit action int."""
    a = 0
    for b in buttons:
        a |= NES_BUTTON_BITS.get(b, 0)
    return a


# --- Tool schema ------------------------------------------------------------
# Single tool, controller-only. The model is told nothing about what game it
# is playing — it has to recognize the screen and figure out what to press.

SYSTEM_PROMPT = """You are playing a video game on an NES emulator. The image shows the current frame. \
You control the game with a standard NES controller.

Available buttons: DPAD_UP, DPAD_DOWN, DPAD_LEFT, DPAD_RIGHT, A, B, START, SELECT.

Each turn, look at the screen and emit ONE press() tool call listing the buttons \
to hold for the next few frames. Pass an empty list to release everything. The game \
will advance, and you'll see the next frame.

You will be told your previous action and whether it produced a visible change on \
the screen. If your last press had no visible effect, the button you pressed almost \
certainly did nothing useful in this context — try a different button or direction. \
Cursor/highlight selection menus typically respond to LEFT/RIGHT or UP/DOWN; \
confirmation is usually A or START. Experiment.

Play to make progress: clear menus, start the game, then play it well. Keep `reason` \
short — under 20 words. Don't analyze; play."""


TOOLS = [{
    "type": "function",
    "function": {
        "name": "press",
        "description": (
            "Hold the listed buttons for the next ~15 emulator frames, "
            "then release. Empty list = release everything."
        ),
        "parameters": {
            "type": "object",
            "properties": {
                "buttons": {
                    "type": "array",
                    "items": {
                        "type": "string",
                        "enum": list(NES_BUTTON_BITS.keys()),
                    },
                },
                "reason": {
                    "type": "string",
                    "description": "Brief rationale (< 20 words).",
                },
            },
            "required": ["buttons"],
        },
    },
}]


# --- Frame encoding ---------------------------------------------------------

def frame_to_jpeg_b64(rgb_array, max_side=384, quality=85):
    """RGB numpy frame -> base64 JPEG suitable for image_url content."""
    img = Image.fromarray(rgb_array)
    if max(img.size) > max_side:
        img.thumbnail((max_side, max_side))
    buf = io.BytesIO()
    img.save(buf, format="JPEG", quality=quality)
    return base64.b64encode(buf.getvalue()).decode("ascii")


# --- Main loop --------------------------------------------------------------

def main():
    p = argparse.ArgumentParser()
    p.add_argument("--rom", required=True, help="Path to an .nes ROM.")
    p.add_argument("--model", default="gpt-4o-mini",
                   help="OpenAI model with vision + tool calling.")
    p.add_argument("--frames-per-decision", type=int, default=15,
                   help="Emulator frames advanced per agent decision (60fps base).")
    p.add_argument("--max-decisions", type=int, default=200,
                   help="Stop after this many agent calls.")
    p.add_argument("--render", action="store_true",
                   help="Open an nes-py viewer window (pyglet).")
    p.add_argument("--save-frames", default=None,
                   help="Directory to save (frame, decision) pairs for inspection.")
    p.add_argument("--max-side", type=int, default=384,
                   help="Resize frame to this max side before sending (latency win).")
    args = p.parse_args()

    if not os.environ.get("OPENAI_API_KEY"):
        sys.exit("error: OPENAI_API_KEY env var is required")
    if not Path(args.rom).is_file():
        sys.exit(f"error: ROM not found: {args.rom}")

    client = OpenAI()
    env = NESEnv(args.rom)
    # nes-py follows the new Gymnasium API: reset() -> (state, info),
    # step() -> (state, reward, terminated, truncated, info). Unpack
    # defensively in case a future version reverts to the old shape.
    r = env.reset()
    state = r[0] if isinstance(r, tuple) else r

    save_dir = Path(args.save_frames) if args.save_frames else None
    if save_dir:
        save_dir.mkdir(parents=True, exist_ok=True)

    action = 0  # currently-held button bitmask
    history = []
    prev_state = None         # raw RGB frame from the previous turn
    prev_call = None          # the previous tool call's {"buttons", "reason"}

    print(f"[bot] {args.model} playing {Path(args.rom).name} — "
          f"{args.frames_per_decision} frames/decision, "
          f"max {args.max_decisions} decisions")

    try:
        for i in range(args.max_decisions):
            # Advance N frames with the previously-chosen action held.
            done = False
            for _ in range(args.frames_per_decision):
                step = env.step(action)
                # New gym API: (state, reward, terminated, truncated, info).
                # Old API: (state, reward, done, info).
                if len(step) == 5:
                    state, _, term, trunc, _ = step
                    done = term or trunc
                else:
                    state, _, done, _ = step
                if args.render:
                    env.render()
                if done:
                    break
            if done:
                print(f"[bot] env done at decision {i}; resetting.")
                r = env.reset()
                state = r[0] if isinstance(r, tuple) else r
                action = 0
                continue

            # Diff signal: did the previous action visibly change anything?
            # Compares the current frame to the one we showed the model last
            # turn; a clean equality is heuristic but cheap and surprisingly
            # informative for menu navigation (cursor moves change pixels).
            if prev_state is not None and prev_state.shape == state.shape:
                unchanged = bool((state == prev_state).all())
            else:
                unchanged = False

            # Build the user-message context. Send only the current frame as
            # image (cheap) and describe the previous turn in text so the
            # model has memory without paying for a second image upload.
            if prev_call is not None:
                prev_btns = ",".join(prev_call["buttons"]) or "nothing"
                effect = "the screen did NOT visibly change" if unchanged \
                         else "the screen changed"
                ctx = (f"Previous action: press({prev_btns}). After it, "
                       f"{effect}. Frame {i}. What should I press now?")
            else:
                ctx = f"Frame {i}. What should I press?"

            img_b64 = frame_to_jpeg_b64(state, max_side=args.max_side)
            t0 = time.time()
            resp = client.chat.completions.create(
                model=args.model,
                messages=[
                    {"role": "system", "content": SYSTEM_PROMPT},
                    {"role": "user", "content": [
                        {"type": "image_url",
                         "image_url": {
                            "url": f"data:image/jpeg;base64,{img_b64}",
                            "detail": "low",
                         }},
                        {"type": "text", "text": ctx},
                    ]},
                ],
                tools=TOOLS,
                tool_choice={"type": "function", "function": {"name": "press"}},
                max_tokens=120,
            )
            elapsed_ms = int((time.time() - t0) * 1000)

            tc = resp.choices[0].message.tool_calls[0]
            call_args = json.loads(tc.function.arguments)
            buttons = call_args.get("buttons", [])
            reason = call_args.get("reason", "")[:80]
            action = buttons_to_action(buttons)

            buttons_str = ",".join(buttons) if buttons else "-"
            diff_tag = "  [no-change]" if (prev_call is not None and unchanged) else ""
            print(f"[{i:03d}] {elapsed_ms:4d}ms  press({buttons_str:<48})  {reason}{diff_tag}")
            history.append({
                "decision": i,
                "buttons": buttons,
                "reason": reason,
                "ms": elapsed_ms,
                "prev_action_had_effect": (prev_call is not None and not unchanged) if prev_call else None,
            })

            if save_dir:
                Image.fromarray(state).save(save_dir / f"{i:04d}.png")

            # Track for next turn's diff + memory.
            prev_state = state.copy()
            prev_call = {"buttons": buttons, "reason": reason}
    except KeyboardInterrupt:
        print("\n[bot] interrupted.")
    finally:
        env.close()

    if save_dir and history:
        with (save_dir / "history.json").open("w") as f:
            json.dump(history, f, indent=2)

    if history:
        n = len(history)
        mean_ms = sum(h["ms"] for h in history) / n
        p95_ms = sorted(h["ms"] for h in history)[int(n * 0.95)]
        print(f"\n[bot] {n} decisions  mean {mean_ms:.0f} ms  p95 {p95_ms} ms")


if __name__ == "__main__":
    main()
