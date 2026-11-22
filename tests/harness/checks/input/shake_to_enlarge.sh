#!/usr/bin/env bash
# harness: outputs=2
# Shaking the pointer grows the cursor under it. The grown image is drawn by the scene, so it shows on a headless
# output, where the real cursor is invisible: nothing bright is on screen before a shake, and the shake puts a bright
# image there. A jump across the screen and a straight drag are not shakes, the image goes away at enlarge_hold_ms, a
# hold of zero keeps it out until a press, the output the pointer is on gets it and the other one does not, and
# shake_to_enlarge = false never grows anything.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"

readonly OUTPUT_W=1280
readonly OUTPUT_H=720
readonly POINTER="${UMBRIEL_POINTER_CLIENT:-./build-debug/tests/pointer-client}"
readonly SHOT="$UMBRIEL_RUNTIME_DIR/cursor-enlarge.png"
# A theme cursor is white and the windowless backdrop is dark, so a colour threshold counts the grown image and
# nothing else.
readonly BRIGHT='r>0.85&&g>0.85&&b>0.85'

if [[ ! -x $POINTER ]]; then
  echo "the pointer client is not available"
  exit 1
fi

pointer() { "$POINTER" "$OUTPUT_W" "$OUTPUT_H" "$@"; }

# The cursor settings live in their own table, so the whole table is replaced rather than appended to: a second
# [input.cursor] table would make the file invalid and the reload would be dropped.
set_cursor() {
  python3 - "$UMBRIEL_CONFIG" "$1" "$2" "$3" <<'PY'
import re, sys

path, shake, zoom, hold = sys.argv[1:5]
text = open(path).read()
text = re.sub(r'\[input\.cursor\][^\[]*', '', text)
text += f"""
[input.cursor]
shake_to_enlarge = {shake}
enlarge_zoom = {zoom}
enlarge_threshold = 400
enlarge_hold_ms = {hold}
"""
open(path, 'w').write(text)
PY
  "$UMBRIEL" msg config-reload > /dev/null
  "$UMBRIEL" settle
}

# Bright pixels in a region of the whole layout, and their bounds.
cursor_pixels() {
  grim "$SHOT"
  "$UMBRIEL_PIXEL_PROBE" "$SHOT" count "$BRIGHT" "${1:-$((OUTPUT_W * 2))x${OUTPUT_H}+0+0}"
}

cursor_bounds() {
  grim "$SHOT"
  "$UMBRIEL_PIXEL_PROBE" "$SHOT" bbox "$BRIGHT" "${1:-$((OUTPUT_W * 2))x${OUTPUT_H}+0+0}"
}

# Five short swings either way: travel with a direction change in it, and less than enlarge_threshold on its own.
shake() {
  pointer relative 100 0 relative -100 0 relative 100 0 relative -100 0 relative 100 0 relative -100 0 > /dev/null
  "$UMBRIEL" settle
}

# Waits for the grown image to be gone or to stay, without a guessed delay.
await_pixels() {
  local want=$1 region=${2:-$((OUTPUT_W * 2))x${OUTPUT_H}+0+0}
  local attempt count
  for ((attempt = 0; attempt < 100; attempt += 1)); do
    count=$(cursor_pixels "$region")
    if [[ $want == none && $count == 0 ]]; then
      return 0
    fi
    if [[ $want != none && $count > 0 ]]; then
      return 0
    fi
    sleep 0.05
  done
  echo "the grown cursor stayed in the wrong state: $count bright pixels, wanted $want"
  return 1
}

set_cursor true 3.0 300
pointer move 400 300 > /dev/null
"$UMBRIEL" settle
if (( $(cursor_pixels) != 0 )); then
  echo "bright pixels on a quiet screen: $(cursor_pixels)"
  exit 1
fi

# A jump past the largest step a shake may take starts a new shake instead of counting as one, and a drag in one
# direction has no direction change, so neither grows the cursor.
pointer move 500 360 > /dev/null
"$UMBRIEL" settle
if (( $(cursor_pixels) != 0 )); then
  echo "a pointer jump grew the cursor: $(cursor_pixels) bright pixels"
  exit 1
fi
pointer relative 120 0 relative 120 0 relative 120 0 relative 120 0 > /dev/null
"$UMBRIEL" settle
if (( $(cursor_pixels) != 0 )); then
  echo "a straight drag grew the cursor: $(cursor_pixels) bright pixels"
  exit 1
fi

# The shake itself: an image appears, and it is the held one, twice the size the zoom asks for at zoom 3 against 1.5.
pointer move 400 300 > /dev/null
"$UMBRIEL" settle
shake
await_pixels yes
read -r zoomed_left zoomed_top zoomed_width zoomed_height <<< "$(cursor_bounds)"
if (( zoomed_width <= 0 || zoomed_height <= 0 )); then
  echo "the grown cursor has no size: $zoomed_left $zoomed_top $zoomed_width $zoomed_height"
  exit 1
fi
if [[ $zoomed_left -lt 0 || $zoomed_top -lt 0 ]]; then
  echo "the grown cursor sits outside the layout: $zoomed_left $zoomed_top"
  exit 1
fi

# It goes away on its own once enlarge_hold_ms is up.
await_pixels none

set_cursor true 1.5 300
pointer move 400 300 > /dev/null
"$UMBRIEL" settle
shake
await_pixels yes
read -r _ _ half_width half_height <<< "$(cursor_bounds)"
if (( half_width == 0 || half_height == 0 )); then
  echo "the grown cursor has no size at zoom 1.5: $half_width $half_height"
  exit 1
fi
if (( zoomed_width <= half_width )); then
  echo "zoom 3 drew ${zoomed_width}x${zoomed_height}, no bigger than zoom 1.5's ${half_width}x${half_height}"
  exit 1
fi
await_pixels none

# A hold of zero keeps the image out until a press, and the press puts it away.
set_cursor true 3.0 0
pointer move 400 300 > /dev/null
"$UMBRIEL" settle
shake
await_pixels yes
sleep 1.0  # real time: a hold of zero means no timer, so a second of wall clock has to leave it alone
if (( $(cursor_pixels) == 0 )); then
  echo "a hold of zero put the grown cursor away on its own"
  exit 1
fi
pointer press 272 > /dev/null
"$UMBRIEL" settle
await_pixels none
pointer release 272 > /dev/null
"$UMBRIEL" settle

# The image follows the pointer to the second output, and the first output keeps none.
set_cursor true 3.0 300
pointer move 1280 200 > /dev/null
"$UMBRIEL" settle
shake
await_pixels yes "${OUTPUT_W}x${OUTPUT_H}+${OUTPUT_W}+0"
read -r second_left _ _ _ <<< "$(cursor_bounds "${OUTPUT_W}x${OUTPUT_H}+${OUTPUT_W}+0")"
if (( second_left < OUTPUT_W )); then
  echo "the grown cursor is not on the second output: $second_left"
  exit 1
fi
if (( $(cursor_pixels "${OUTPUT_W}x${OUTPUT_H}+0+0") != 0 )); then
  echo "the first output kept a grown cursor: $(cursor_pixels "${OUTPUT_W}x${OUTPUT_H}+0+0") bright pixels"
  exit 1
fi
await_pixels none

# Off: a shake leaves the screen alone.
set_cursor false 3.0 300
pointer move 400 300 > /dev/null
"$UMBRIEL" settle
shake
if (( $(cursor_pixels) != 0 )); then
  echo "a shake grew the cursor with shake_to_enlarge = false: $(cursor_pixels) bright pixels"
  exit 1
fi

echo "a shake grows the cursor under the pointer, a drag or a jump does not, the hold and the press put it away, " \
  "the zoom sets its size and the output under the pointer shows it"
