# Driving Umbriel from a program

Umbriel has an **agent seat**: a second seat, separate from the one a person uses, with its own pointer, its own keyboard focus, its own virtual input devices, and its own view of the session. A program works one window on it, and reads back what happened after every step.

That is the difference from a general remote-control setup. A client that grabs the ordinary seat drives the desktop exactly as a person at the keyboard would: it moves the real cursor, it takes the real focus, and anything it types goes wherever the person's focus happens to be. The agent seat is for the other case — an assistant, a support session, a demo recorder, a test that clicks through an application — where the work should be visible, confined to one window, and unable to disturb the person.

```sh
umbriel agent                       # the seat, the window it holds, and every window to choose from
umbriel agent take <window-id>      # work this window
umbriel agent click left 240 120    # click 240,120 inside it
umbriel agent type hello
umbriel agent release
```

> An agent can still do whatever the application it drives lets it do: read documents, send mail, delete files. Take a window only for an application you would hand to it. The seat confines *where* the input goes, not *what* it can be used for.

## What the seat is

One `wl_seat` named `agent`, created when the compositor starts. It has:

- **Its own pointer.** A virtual pointer the compositor makes for the agent, carrying the agent seat's enter, motion, button and scroll. It does not move the person's cursor.
- **Its own keyboard focus.** Typing goes to the taken window, whether or not the person has it focused or visible.
- **Its own clipboard and drag state.** Copying in the agent's window does not replace what the person copied.
- **One taken window at a time.** `agent take` moves the agent's focus there; `agent release` lets go.

`[agent] off = true` in the config keeps the seat from existing at all. It is read at startup, because a seat is registered with clients when it is created; turning it on later only makes the requests report `the agent seat is off`.

## Choose the window

`umbriel agent` lists the seat's state as JSON with `--json`, and in human form without it. The reply carries three things: whether the seat exists, the window it holds (if any), and `windows`, every mapped window in the same shape as [`umbriel windows --json`](ipc.md#queries):

```sh
umbriel agent --json | jq -r '.ok.windows[] | "\(.id)\t\(.app_id)\t\(.title)"'
```

The ID is the window's ext-foreign-toplevel identifier, the same string `windows` reports, so a script that already picks windows from `windows` can hand the same ID to `agent take`.

`agent take` refuses a window the person is focused in (`is focused on the person's seat`): taking it would move the person's focus and their view. Windows on other workspaces, hidden tabs, pinned windows and the agent's own window are all fine.

### Give the agent its own output

`[agent] output = "AGENT-1"` makes the seat create a virtual output by that name at startup, and moves every taken window onto it. The person works on their own outputs; the agent's windows are never in their way and never scrolled out from under it. Releasing the window moves it back to the output it came from.

```toml
[agent]
output = "AGENT-1"
```

If an output already has that name — a real monitor, or one made earlier with `umbriel output-create` — that output is used and nothing is created. A window moved onto the agent's output is still an ordinary window: the person can focus it in the overview, and taking it back that way [ends the agent's hold](#the-person-takes-it-back).

## Point at what you mean

Coordinates are in the taken window, from its top-left corner, in logical pixels. `umbriel agent capture` answers the window's position and size for a screenshot region:

```sh
umbriel agent capture --json      # {"ok":{"x":100,"y":50,"width":800,"height":600}}
grim -g "$(umbriel agent capture | awk '{print $1" "$2}')" shot.png
```

A click at `x,y` is `agent click <button> x y`: the pointer moves there, presses, and releases. `agent pointer x y` moves without clicking, for hover states, tooltips and menus that open on hover. `agent scroll <notches> [horizontal]` turns the wheel; a notch is one click of a real wheel, positive scrolls down or right.

**Where to click.** There is no accessibility tree: Umbriel does not publish one and cannot see inside an application. A click target is either a coordinate known from the application's own layout (a toolbar slot, a list row index times row height) or one found on screen. When it is found on screen, capture first and look at the picture; a window's `app_id` and `title` say what it is, not where its buttons are.

## Type into it

`agent type <text>` types literal text, one key press per character, choosing modifiers from the live keymap: on a layout with dead keys and AltGr levels, the character still arrives as itself. A newline in the text is an Enter. It does not interpret escapes.

`agent key <keycode>` presses and releases one key by its evdev code — the number in `linux/input-event-codes.h`, so `30` is `KEY_A`, `28` is `KEY_ENTER`, `1` is `KEY_ESC`. Use it for keys that are not characters: Escape, Tab, arrows, function keys.

`agent type` cannot hold a modifier while it types, so `Ctrl+V` is `agent key 47` (KEY_V) only as a bare `V`. Combinations are the application's job through its own menus, or an application feature that does not need a chord:

```sh
umbriel agent type 'hello'          # literal text
umbriel agent key 28                # Enter
umbriel agent key 1                 # Escape
```

## Run an action

`agent act <action> [key=value...]` runs any [compositor action](actions.md) with the arguments the structured [request form](ipc.md#actions) takes. It is the same handler `umbriel msg` reaches, so an agent and a script do the same thing:

```sh
umbriel agent act window-move-to-workspace workspace=3
umbriel agent act workspace-switch workspace=2
umbriel agent act window-focus window="<window-id>"
umbriel agent act window-move-to-output destination=DP-1
```

`agent act` does not check whether the action concerns the taken window: `workspace-switch` changes what the person sees, exactly as it would from `msg`. Prefer the agent commands for what concerns its window, and `agent act` for arranging the desktop around it.

## Launch a program

`agent spawn <command...>` starts a program with a launch activation token — the same as `umbriel msg spawn` — and returns the seat state. A token is what tells a window apart from others the person may open at the same moment, so a script can find the new window without guessing from its title.

```sh
before=$(umbriel agent --json | jq '.ok.windows | length')
umbriel agent spawn kitty
while [ "$(umbriel agent --json | jq '.ok.windows | length')" -le "$before" ]; do sleep 0.1; done
umbriel agent take "$(umbriel agent --json | jq -r '.ok.windows[-1].id')"
```

The same race exists without the token; the token only makes the window findable. Poll until a window appears, then take it.

## Read the reply

Every agent command answers with `{"ok": <seat state>}` or `{"err": "<what went wrong>"}`. The seat state is:

| Field        | Meaning                                                                             |
| ------------ | ----------------------------------------------------------------------------------- |
| `on`         | The seat exists: the compositor started without `[agent] off`                       |
| `off`        | Only when `on` is false: the seat is off by configuration                           |
| `taken_over` | The person took the window back; the agent's input stops until it takes a window again |
| `window`     | The taken window, as a `windows` entry, or `null`                                   |
| `windows`    | Every mapped window, as in `umbriel windows --json`                                  |

A click or a key press is not a confirmation that anything happened. The application decides what to do with it, and only the next query shows whether it did: `windows` for state, `agent capture` for geometry, a fresh capture for the picture, an [event subscription](ipc.md#event-stream) for what changed.

The human-readable form of the same reply prints one line for the seat and one for each window, with the taken window's app ID and title:

```console
$ umbriel agent
agent seat: firefox "Inbox — Mozilla Firefox" [xdg-abc123]
* firefox  "Inbox — Mozilla Firefox"  [DP-1 0+0 800x600]
  kitty    "~/src"                    [DP-1 +840+0 700x600]
```

## When it says no

| Error                                                  | Why                                                                                     | What to do                                                                |
| ------------------------------------------------------ | --------------------------------------------------------------------------------------- | ------------------------------------------------------------------------- |
| `the agent seat is off`                                | `[agent] off = true` at startup, or the seat could not be created                       | Remove `off` and restart; the seat is created once, at startup            |
| `no window taken; take one first`                      | Input commands need a target                                                           | `umbriel agent take <window-id>`                                          |
| `window <id> is focused on the person's seat`          | The person is working in it                                                            | Pick another window, or ask them to switch away                           |
| `window is not on a workspace`                         | The window is minimized, in a hidden tab, or a scratchpad window not on screen         | Bring it back first: `umbriel agent act window-toggle-minimize` or focus it |
| `the session is locked`                                | The lock screen owns the session                                                        | Wait for it to unlock; agent input is never replayed across a lock       |
| `the person took the window back; take a window again to continue` | The person pressed a key or clicked in the agent's window                  | Take a window again, or stop                                              |
| `unknown window: <id>`                                 | The ID is not a mapped window                                                          | Read `umbriel agent --json` again; IDs change when a client restarts      |
| `output is not available: <name>` / `unknown output: <name>` | `[agent] output` names an output that does not exist                             | Create it with `umbriel output-create` or fix the name                    |
| `not a number: <text>` / `missing <name>`              | A coordinate or a count that is not a number                                           | Check the argument                                                       |
| `unknown action: <name>` / `invalid arguments for …`   | `agent act` got an action or fields the compositor does not know                       | See [Actions](actions.md) for the name and its fields                     |
| `no key on the agent keyboard produces this character` | The live keymap has no key for the character, such as an emoji                        | Use a keymap that has it, or paste instead                              |

Programs that report their own failure — an application that refuses a click on a disabled button, a dialog that opens instead of the expected screen — are not errors here. Every command reports only what the compositor did with the input, and the state that follows says the rest.

## The person takes it back

Any of these ends the agent's hold on the window:

- the person presses a key in the window on their own keyboard, holding Shift or Ctrl alone does not count;
- the person focuses another window with the agent seat's keyboard — a client that hands the focus away;
- the agent's window goes away (the client closed it, its output was removed).

A pointer click by the person anywhere does not by itself end the hold: the agent's window may be on an output the person is not looking at. The seat reports `taken_over` and refuses further input (`the person took the window back; take a window again to continue`) until it takes a window again.

Locking the session ends every agent request with `the session is locked`, and nothing typed while locked is replayed afterwards.

## What it will not do

These hold regardless of configuration. A setup that assumes otherwise fails somewhere it matters:

- **Nothing approves a permission prompt.** The input-capture, screencast and [global-shortcut](security.md#global-shortcut-approval) prompts take Enter, R or a click from a backend keyboard or pointer. The agent seat's virtual devices cannot approve, remember, or advance them. An agent cannot grant itself a screencast or [input capture](security.md#input-capture) session; a person does that.
- **The lock screen is never driven.** Locked, Umbriel shows the lock client's surface and hands it the input; agent input is refused rather than queued.
- **X11 applications see emulated input as XTest,** and the [XTest permission prompt](security.md#x11-desktop-input) needs a person.
- **There is no accessibility tree.** Umbriel cannot see inside an application. Point at coordinates, not at named controls.
- **No clipboard access.** The agent types; it cannot read or set the clipboard through the seat. Use the application's own facilities if it has any.

## Seeing the screen

Capture uses protocols Umbriel publishes: `wlr-screencopy-unstable-v1`, `ext-image-copy-capture-v1` and `ext-output-image-capture-source-v1`. Tools built on them work unchanged, and capture follows the same permission rules as any other client: where a prompt appears, [a person answers it](#what-it-will-not-do).

```sh
grim -o AGENT-1 shot.png                      # the agent's own output, by the name from `umbriel outputs`
grim -g "100,50 800x600" crop.png             # the taken window, from `agent capture`
```

Capturing the agent's own output needs no window arithmetic at all, which is the other reason to give it one.

## A round trip

Read the state, act, read again — and wait for the application between the two reads:

```sh
#!/bin/sh
set -eu

wait_window_count() {
    i=0
    while [ "$(umbriel agent --json | jq '.ok.windows | length')" -le "$1" ]; do
        [ "$i" -lt 50 ] || { echo "no window appeared" >&2; exit 1; }
        i=$((i + 1))
        sleep 0.1
    done
}

before=$(umbriel agent --json | jq '.ok.windows | length')
umbriel agent spawn kitty
wait_window_count "$before"
id=$(umbriel agent --json | jq -r '.ok.windows[-1].id')

umbriel agent take "$id"
umbriel agent capture --json         # x, y, width, height of the window

umbriel agent type 'echo agent was here'
umbriel agent key 28                 # Enter
umbriel agent capture --json         # the geometry is unchanged; the picture is what changed

umbriel agent release
```

Each step reads before it acts because a query answers what the compositor has already applied: `{"ok": …}` means the input was delivered, not that the application finished reacting. Polling the query is what waits for the application.

## What to prefer

- **Query, then act, then query.** State is cheap and exact; screenshots are expensive and need a person's consent.
- **Act through `umbriel agent act` where an action exists.** Workspace and layout changes through actions stay in the compositor's own ordering; synthesised keys race whichever window happens to hold focus.
- **Give the agent its own output.** A window there is out of the person's way, capturable as a whole output, and moved back when released.
- **Route with window rules, drive with actions.** A [window rule](window-rules.md) that sends an application to a known workspace shortens the search; the agent still moves windows with `agent act`, not by dragging them.
- **Take one window, not the desktop.** The seat confines the input, so a stray click lands in the agent's window instead of wherever the person's cursor was.

## See also

- [IPC](ipc.md) — the socket protocol: queries, actions, event stream.
- [Actions](actions.md) — names and arguments for `agent act`.
- [Outputs](outputs.md#virtual-outputs) — virtual outputs, including the one `[agent] output` makes.
- [Security](security.md) — permission prompts and what a person answers.
