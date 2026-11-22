#!/usr/bin/env bash
# harness: outputs=2
# Shaking the pointer grows the cursor and leaves it under the pointer: the crop around the hotspot carries more
# cursor pixels than the plain cursor does. The grown image goes back at enlarge_hold_ms, a press puts it away early,
# a hold of zero keeps it until the press, the output the pointer is on shows it too, and shake_to_enlarge = false
# never grows it.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"

readonly OUTPUT_W=1280
readonly OUTPUT_H=720
readonly BTN_LEFT=272
readonly POINTER="${UMBRIEL_POINTER_CLIENT:-./build-debug/tests/pointer-client}"
readonly SHOT="$UMBRIEL_RUNTIME_DIR/cursor-enlarge.png"
readonly CROP=48x48
# Where the pointer rests on each output, and the same spot in that output's own image.
readonly FIRST_X=400
readonly FIRST_Y=300
readonly SECOND_X=350
readonly SECOND_Y=200

if [[ ! -x $POINTER ]]; then
  echo "the pointer client is not available"
  exit 1
fi

pointer() { "$POINTER" "$OUTPUT_W" "$OUTPUT_H" "$@"; }

# Cursor pixels in a CROP window at x,y: how many differ from the pixel under the hotspot. A black backdrop and an
# arrow cursor make that the cursor's own footprint, which a grown cursor widens.
cursor_pixels() {
  local predicate
  grim -s 1 -o "$1" "$SHOT"
  predicate=$("$UMBRIEL_PIXEL_PROBE" "$SHOT" pixel "$2" "$3" | awk '{
    printf "r < %.4f || r > %.4f || g < %.4f || g > %.4f || b < %.4f || b > %.4f",
      ($1 - 0.5) / 255, ($1 + 0.5) / 255, ($2 - 0.5) / 255, ($2 + 0.5) / 255, ($3 - 0.5) / 255, ($3 + 0.5) / 255
  }')
  "$UMBRIEL_PIXEL_PROBE" "$SHOT" count "$predicate" "${CROP}+$2+$3"
}

# A shake: five short sideways swings of 100 px, less than enlarge_threshold of 400 on their own.
shake() {
  pointer relative 100 0 relative -100 0 relative 100 0 relative -100 0 relative 100 0 > /dev/null
  "$UMBRIEL" settle
}

cat >> "$UMBRIEL_CONFIG" <<'EOF'
[animation]
enabled = false
[colors]
backdrop = "#000000FF"
[output."HEADLESS-1"]
position = [0, 0]
[output."HEADLESS-2"]
position = [1280, 0]
[input.cursor]
shake_to_enlarge = true
enlarge_zoom = 3.0
enlarge_threshold = 400
enlarge_hold_ms = 1500
EOF
"$UMBRIEL" msg config-reload > /dev/null
"$UMBRIEL" settle

# A jump past kShakeMaxStep is a warp, not a shake: on its own it never grows the cursor.
pointer warp "$FIRST_X" "$FIRST_Y" > /dev/null
"$UMBRIEL" settle
plain=$(cursor_pixels HEADLESS-1 "$FIRST_X" "$FIRST_Y")
if ((plain == 0)); then
  echo "positive control found no cursor at $FIRST_X,$FIRST_Y"
  exit 1
fi
pointer warp "$((FIRST_X + 120))" "$((FIRST_Y + 60))" > /dev/null
"$UMBRIEL" settle
warped=$(cursor_pixels HEADLESS-1 "$((FIRST_X + 120))" "$((FIRST_Y + 60))")
if ((warped != plain)); then
  echo "an unchecked pointer warp grew the cursor: $warped cursor pixels against $plain"
  exit 1
fi
pointer warp "$FIRST_X" "$FIRST_Y" > /dev/null
"$UMBRIEL" settle

shake
grown=$(cursor_pixels HEADLESS-1 "$FIRST_X" "$FIRST_Y")
if ((grown <= plain)); then
  echo "the shake did not grow the cursor: $plain cursor pixels against $grown"
  exit 1
fi

sleep 1.7 # real time: enlarge_hold_ms = 1500
"$UMBRIEL" settle
after=$(cursor_pixels HEADLESS-1 "$FIRST_X" "$FIRST_Y")
if ((after != plain)); then
  echo "the grown cursor stayed past enlarge_hold_ms: $after cursor pixels against $plain"
  exit 1
fi

sed -i 's/^enlarge_hold_ms = 1500$/enlarge_hold_ms = 0/' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
shake
sleep 1.7 # real time: a zero hold must not expire on its own
"$UMBRIEL" settle
held=$(cursor_pixels HEADLESS-1 "$FIRST_X" "$FIRST_Y")
if ((held != grown)); then
  echo "enlarge_hold_ms = 0 did not keep the grown cursor: $held cursor pixels against $grown"
  exit 1
fi
pointer click "$BTN_LEFT" > /dev/null
"$UMBRIEL" settle
pressed=$(cursor_pixels HEADLESS-1 "$FIRST_X" "$FIRST_Y")
if ((pressed != plain)); then
  echo "a press did not put the grown cursor away: $pressed cursor pixels against $plain"
  exit 1
fi

pointer move 1280 "$SECOND_X" "$SECOND_Y" > /dev/null
"$UMBRIEL" settle
plain_second=$(cursor_pixels HEADLESS-2 "$SECOND_X" "$SECOND_Y")
if ((plain_second == 0)); then
  echo "positive control found no cursor on HEADLESS-2 at $SECOND_X,$SECOND_Y"
  exit 1
fi
shake
grown_second=$(cursor_pixels HEADLESS-2 "$SECOND_X" "$SECOND_Y")
if ((grown_second <= plain_second)); then
  echo "a shake on HEADLESS-2 did not grow the cursor: $plain_second cursor pixels against $grown_second"
  exit 1
fi

sed -i 's/^shake_to_enlarge = true$/shake_to_enlarge = false/' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
pointer move "$FIRST_X" "$FIRST_Y" > /dev/null
"$UMBRIEL" settle
shake
off=$(cursor_pixels HEADLESS-1 "$FIRST_X" "$FIRST_Y")
if ((off != plain)); then
  echo "shake_to_enlarge = false still grew the cursor: $off cursor pixels against $plain"
  exit 1
fi

echo "shake to enlarge grows the cursor under the pointer, holds, releases on a press or the hold time, and stops when off"
