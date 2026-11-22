---
name: umbriel-agent
description: Drive the Umbriel Wayland desktop through its agent seat — work one window with `umbriel agent`, keep the person's input untouched. Use when asked to operate a graphical application, click through a UI, type into an app, or verify what a window shows on Umbriel.
---

# Working an Umbriel window

Umbriel is a Wayland compositor with an **agent seat**: a second seat, apart from the person's, with its own pointer and its own keyboard focus. You work one window on it. Nothing you do moves the person's cursor or takes the person's focus, and the person can take the window back at any moment.

Read [Driving Umbriel from a program](https://github.com/noctalia-dev/umbriel/blob/main/docs/user/automation.md) for the full surface. This page is the working loop.

## Look for an interface first

A graphical interface is the last resort. Before clicking, check whether the job has a command:

| Task                 | Tool                                                                    |
| -------------------- | ----------------------------------------------------------------------- |
| Volume, mute         | `wpctl status`, `wpctl set-volume @DEFAULT_AUDIO_SINK@ 50%`             |
| Network              | `nmcli connection show`, `nmcli device wifi list`                       |
| Bluetooth            | `bluetoothctl devices`, `bluetoothctl connect <mac>`                     |
| Media                | `playerctl status`, `playerctl play-pause`                              |
| Brightness           | `brightnessctl set 50%`                                                 |
| Windows, workspaces  | `umbriel windows --json`, `umbriel msg window-focus <id>`                |
| Notifications        | `notify-send`, `makoctl list`                                           |

An application with a CLI, an API, a config file or a database is driven that way. Clicking is for what has no other door.

## Check the seat

```sh
umbriel agent --json
```

- `{"err": "the agent seat is off"}` — `[agent] off = true` at startup. The person must restart the compositor with it removed. Do not try to work around it.
- `{"ok": {"on": true, "taken_over": false, "window": null, "windows": [...]}}` — the seat is there; `windows` lists every mapped window with `id`, `app_id`, `title`, `workspace`, `output`, `x`, `y`, `w`, `h`, `focused`, `visible`.

The application must support a second Wayland seat: bind the `wl_seat` globals advertised by `wl_registry`, identify
`agent` through its `wl_seat.name` event, and create pointer and keyboard objects for it. Taking a window cannot add
multi-seat support to a toolkit that lacks it. A successful command is not proof of delivery; verify the result on screen. A window the person is focused in cannot be taken (`is focused on the person's seat`). That is deliberate: their view is not yours to move. Ask, or take another window.

## Take a window

```sh
umbriel agent take <window-id>
```

The ID is the ext-foreign-toplevel identifier from `windows` or from the seat's own listing — the same string in both. Taking a window focuses it on the agent seat and, with `[agent] output` configured, moves it onto the agent's own virtual output, out of the person's sight.

If you started the program yourself, use `umbriel agent spawn <command>` and poll `windows` until the new one appears; a launch activation token makes it findable even when the person is opening windows at the same moment.

## Point at what you mean

**There is no accessibility tree.** Umbriel cannot see inside an application. A click target is either a coordinate you know from the application's own structure (row index × row height, a toolbar slot) or one you found on screen.

```sh
umbriel agent capture --json     # {"ok":{"x":100,"y":50,"width":800,"height":600}}
grim -g "100,50 800x600" /tmp/agent.png     # the taken window, if a person has approved capture
```

Coordinates for `pointer`, `click` and `scroll` are **inside the window**, from its top-left corner, in logical pixels — not layout coordinates, not device pixels. A point at the window's centre is `width/2, height/2` from `agent capture`.

```sh
umbriel agent pointer 240 120            # hover: tooltips, menus that open on hover
umbriel agent click left 240 120         # move, press, release
umbriel agent click right 240 120
umbriel agent scroll 3                   # 3 notches down; `scroll -3`, `scroll 2 horizontal`
```

Screen capture needs a person's approval once per client. Where no picture is available, work from `windows` entries (`title`, `app_id`, geometry) and from what the application reports in its own files or logs.

## Type into it

```sh
umbriel agent type 'grep -r TODO src'    # literal text, one key per character
umbriel agent key 28                     # Enter — evdev keycode from linux/input-event-codes.h
umbriel agent key 1                      # Escape; 15 Tab; 103 Up, 108 Down, 105 Left, 106 Right
```

`type` produces each character through the live keymap, so a layout with dead keys and AltGr still delivers the character itself. A newline in the text is Enter. **Combinations are impossible**: there is no way to hold Ctrl while typing, so `Ctrl+V` cannot be produced by `type`. Use the application's menu, or an action the application exposes on its own.

## Run an action

`agent act` reaches the same handlers `umbriel msg` does, with the typed request form:

```sh
umbriel agent act workspace-switch workspace=2
umbriel agent act window-move-to-workspace workspace=3
umbriel agent act window-toggle-maximize
umbriel agent act window-close
```

Prefer an action over synthesised keys wherever one exists: actions run in the compositor's own ordering and take effect immediately, while keys race whatever the application decides to do with them.

## Read the receipt

Every command answers `{"ok": <seat state>}` or `{"err": "<reason>"}`. The seat state fills in on `agent`, `take`, `release`, `capture` and `spawn`; input commands answer with the unchanged state.

**A click or a key is not a confirmation.** `{"ok": ...}` means the input was delivered to the client, nothing more. The application decides what happens next, and only the following query shows it:

- `umbriel agent --json` — did the window change state, open, close, move workspace?
- `umbriel agent capture --json` — did the geometry change?
- a fresh `grim` capture — did the picture change?
- the application's own artefacts — a file written, a row added, a log line

Poll rather than sleep blindly: a query answers what the compositor has already applied, so looping on it is what waits for an application that is still starting.

## When it says no

| Reply                                                        | Meaning                                                       | Move                                                     |
| ------------------------------------------------------------ | ------------------------------------------------------------- | -------------------------------------------------------- |
| `the agent seat is off`                                      | `[agent] off = true`; the seat is a startup setting           | Stop and report; only a restart with the setting removed helps |
| `no window is taken; take one first`                         | An input command with no target                               | `umbriel agent take <window-id>`                          |
| `window <id> is focused on the person's seat`                | The person is working in it                                   | Take another window, or ask them to leave it              |
| `window is not on a workspace`                               | Minimized, a hidden tab, or a scratchpad window off screen    | `umbriel agent act window-toggle-minimize`, or focus it   |
| `the session is locked`                                      | The lock screen owns the session                              | Stop. Nothing is replayed after unlock                    |
| `the person took the window back; take a window again to continue` | The person pressed a key in your window                 | Take a window again if the work still applies, else stop  |
| `unknown window: <id>`                                       | The ID is stale — the client restarted                        | Read `umbriel agent --json` again; IDs are not stable across restarts |
| `output is not available: <name>`                            | `[agent] output` names an output that does not exist          | Report it; the config names an output the session lacks    |
| `not a number: <text>`, `missing <name>`                     | Malformed argument                                            | Fix the argument                                          |
| `unknown action: <name>`                                     | `agent act` got an action the compositor does not have        | `umbriel msg --help` lists the real ones                   |
| `no key on the agent keyboard produces this character`       | The keymap has no key for it (emoji, some symbols)            | Paste through the application's own means, or type ASCII  |

A refused command is a normal answer, not a crash: nothing changed, and the seat state in the previous query still holds. Report the reply rather than retrying it unchanged.

## Stay out of the person's way

- **Take one window.** Taking a second one is allowed and means you are done with the first; release explicitly when finished so the window returns where it came from.
- **Do not take a window the person is using.** The seat refuses it, and asking is better than working around it.
- **Leave the desktop as you found it.** Release the window, and close only windows you opened.
- **Release before anything destructive or hard to undo** — sending mail, deleting files, publishing — and let the person confirm. The seat confines where input goes, not what the application can do.
- **Never drive a permission prompt.** Input-capture, screencast and X11 (XTest) prompts take a real Enter, R, or click from the person's own devices; the agent seat cannot approve them. Ask the person.
