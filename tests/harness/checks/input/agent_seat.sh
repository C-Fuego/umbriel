#!/usr/bin/env bash
# The agent seat drives one window with its own pointer and keyboard, apart from the person's, and the person typing in
# that window takes it back.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"

# The target binds the agent seat by name: the person's seat stays the one every other check drives.
LOG=$UMBRIEL_RUNTIME_DIR/agent-target.log
SEAT_NAME=agent "$UMBRIEL_SEAT_LOG_CLIENT" agent-target > "$LOG" 2>&1 &
await_window agent-target

# Something for the person to be looking at: the seat will not take the window the person has focused.
"$UMBRIEL_SEAT_LOG_CLIENT" person-window > "$UMBRIEL_RUNTIME_DIR/person-window.log" 2>&1 &
await_window person-window

id=$(window_by_title agent-target | jq -r .id)

# A seat with nothing taken says so, and refuses what needs a window.
"$UMBRIEL" agent | grep -qx 'agent seat: no window taken'
if "$UMBRIEL" agent pointer 10 10 2> "$UMBRIEL_RUNTIME_DIR/refused.txt"; then
  echo "the seat pointed with no window taken"
  exit 1
fi
grep -q 'no window is taken; take one first' "$UMBRIEL_RUNTIME_DIR/refused.txt"

"$UMBRIEL" agent take "$id" > "$UMBRIEL_RUNTIME_DIR/taken.txt"
grep -q "agent-target\" \[$id\]" "$UMBRIEL_RUNTIME_DIR/taken.txt"

# The agent pointer enters the taken window, and a click carries the position inside it.
"$UMBRIEL" agent pointer 100 80
await_events "$LOG" 'pointer-enter' 1
"$UMBRIEL" agent click left
await_events "$LOG" 'pointer-button code=272 state=pressed' 1
grep -qx 'press-position x=100 y=80' "$LOG"

# The agent keyboard reaches the same window: a raw keycode, then text through the live keymap.
"$UMBRIEL" agent key 28
await_events "$LOG" 'keyboard-key code=28 state=pressed' 1
"$UMBRIEL" agent type hi
await_events "$LOG" 'keyboard-key code=35 state=pressed' 1
await_events "$LOG" 'keyboard-key code=23 state=pressed' 1

# The captured region is the window's own. The listing names the layout slot the
# window is being arranged into while capture names where it is on screen this
# frame, so the two only agree once the window has finished moving there.
captured=
for _ in $(seq 60); do
  read -r wx wy ww wh < <(window_by_title agent-target | jq -r '"\(.x) \(.y) \(.w) \(.h)"')
  captured=$("$UMBRIEL" agent capture)
  [[ $captured == "$wx,$wy ${ww}x${wh}" ]] && break
  sleep 0.05
done
[[ $captured == "$wx,$wy ${ww}x${wh}" ]] || {
  echo "agent capture was '$captured', the window is at $wx,$wy ${ww}x${wh}"
  exit 1
}

# The person clicks the taken window and types in it: that is the window being taken back, not the agent's typing.
"$UMBRIEL_POINTER_CLIENT" 1280 720 move "$((wx + ww / 2))" "$((wy + wh / 2))" click 272 > /dev/null
"$UMBRIEL_POINTER_CLIENT" 1280 720 tap 30 > /dev/null
for _ in $(seq 40); do
  "$UMBRIEL" agent | grep -q '\[taken over\]' && break
  sleep 0.05
done
"$UMBRIEL" agent | grep -q '\[taken over\]'
if "$UMBRIEL" agent pointer 20 20 2> "$UMBRIEL_RUNTIME_DIR/refused.txt"; then
  echo "the seat kept pointing after the person took the window back"
  exit 1
fi
grep -q 'the person took the window back' "$UMBRIEL_RUNTIME_DIR/refused.txt"

# The person goes back to their own window, and the agent takes the target again: taking it is only refused while the
# person is focused in it.
person=$(window_by_title person-window)
"$UMBRIEL_POINTER_CLIENT" 1280 720 move "$(jq -r '.x + .w / 2 | floor' <<< "$person")" \
  "$(jq -r '.y + .h / 2 | floor' <<< "$person")" click 272 > /dev/null
"$UMBRIEL" agent take "$id" > /dev/null
"$UMBRIEL" agent pointer 20 20
await_events "$LOG" 'pointer-enter' 2

# Releasing clears focus. The same client must receive a fresh enter on the next take.
"$UMBRIEL" agent release
"$UMBRIEL" agent | grep -qx 'agent seat: no window taken'
"$UMBRIEL" agent take "$id" > /dev/null
"$UMBRIEL" agent pointer 30 30
await_events "$LOG" 'pointer-enter' 3
"$UMBRIEL" agent release
