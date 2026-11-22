#pragma once

#include "config/config.h"
#include "wlr.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <wayland-server-core.h>

namespace umbriel {

  class Server;
  class View;

  // A second seat for a program that drives the desktop, kept apart from the
  // seat a person uses: its own pointer, its own keyboard focus, its own
  // pointer position, its own virtual devices. Nothing it does moves the
  // person's cursor, takes the person's keyboard focus, or reaches the
  // person's clipboard.
  //
  // The person can take a window back at any time. Pressing a pointer button
  // or typing in a window the agent has open returns that window to the
  // person's seat and stops the agent's input until it asks again.
  class AgentSeat {
  public:
    explicit AgentSeat(Server& server);
    ~AgentSeat();

    AgentSeat(const AgentSeat&) = delete;
    AgentSeat& operator=(const AgentSeat&) = delete;

    // The window the agent is working with. Taking one focuses it on the
    // agent seat and, when the window is not where the agent may see it, moves
    // the agent's view there. A window the person is focused in is refused.
    bool take(View& view, std::string* error);
    // The taken window, or null.
    View* target() const;
    // Whether `[agent] off` is set right now. The seat itself, once created,
    // stays; this only refuses requests, so reloading the config takes effect
    // without restarting the compositor.
    static bool enabled() { return !config().agent.off; }
    // True when this seat exists: the compositor started without `[agent] off`.
    bool isOn() const { return m_seat != nullptr; }
    // True once the person has taken the target over; the agent's input stops.
    bool takenOver() const { return m_takenOver; }
    // Forgets the target window; a later take starts clean.
    void release();

    // Geometry the agent works in: where the target window sits in the layout,
    // with the origin at its top-left, in logical pixels. False when no window
    // is taken.
    bool geometry(wlr_box* box) const;
    // Move the agent's pointer to a position in the target window.
    bool movePointer(double x, double y, std::string* error);
    // Press or release a pointer button by the Linux evdev code of a mouse
    // button: 1 left, 2 middle, 3 right.
    bool button(uint32_t button, bool pressed, std::string* error);
    // Turn the scroll wheel `notches` steps, positive down or right.
    bool scroll(double notches, bool horizontal, std::string* error);
    // Press and release one key by an evdev keycode (the number in
    // `linux/input-event-codes.h`, so 30 is KEY_A).
    bool key(uint32_t keycode, std::string* error);
    // Type text on the agent's keyboard, one key per character, and let the
    // keymap produce each character's modifiers.
    bool type(std::string_view text, std::string* error);

    // The agent's virtual keyboard is being torn down (called on the way out,
    // before the device is freed).
    void keyboardDestroyed();

    // Where the agent works. With `[agent] output` naming an output, that
    // output is the agent's own virtual one and a taken window is moved onto
    // it, out of the person's sight, and moved back when released. Empty keeps
    // the window where the person put it.
    const std::string& output() const { return m_output; }
    // Creates the agent's virtual output from the configuration, and removes it
    // again. Called once during setup and teardown.
    void createOutput();
    void removeOutput();
    // An output is going away; a window that was only there becomes the
    // person's again.
    void handleOutputRemoved(wlr_output* output);

  private:
    struct Virtual {
      wlr_virtual_pointer_v1* pointer = nullptr;
      wlr_virtual_keyboard_v1* keyboard = nullptr;
    };

    // Attaches the agent's pointer and keyboard to the agent seat when the
    // first request needs them, and records the target's window as an enter on
    // the agent seat. False with `error` when the seat is off, locked, or the
    // window is gone.
    bool prepare(View* view, std::string* error);
    // The person used the seat on the target, or the client withdrew what the
    // agent holds: hand the window back.
    void handBack();
    // A key press on the person's seat landed in the target window.
    void notePersonInput(View* view);

    // Moves the window onto `output` through the compositor's own transfer.
    bool moveToOutput(View& view, std::string_view output, std::string* error);
    // Creates the agent's own virtual pointer and keyboard, the first request
    // that needs them. They join the agent seat, never the person's.
    bool createVirtualDevices(std::string* error);
    void destroyVirtualDevices();
    // Gives the target the agent seat's keyboard and pointer enter.
    void enterTarget();
    bool focusTarget();

    static void onPersonKey(wl_listener* listener, void* data);
    static void onKeyboardFocusChange(wl_listener* listener, void* /*data*/);
    static void onNewDevice(wl_listener* listener, void* data);

    void watchKeyboard(wlr_keyboard* keyboard);

    void handlePersonKey(void* data);
    void handleKeyboardFocusChange();

    bool sendKey(uint32_t keycode, bool pressed);

    Server* m_server = nullptr;
    wlr_seat* m_seat = nullptr;
    // The agent's own output, when `[agent] output` is set, and the wlr_output
    // it was made from so a destroyed output can be noticed.
    std::string m_output;
    wlr_output* m_outputWlr = nullptr;
    // Where the taken window was before it moved to the agent's output, so
    // releasing it puts it back.
    std::string m_previousOutput;
    std::optional<Virtual> m_virtual;
    // The taken window, by its foreign-toplevel identifier, which survives a
    // mapping that comes back after the client's surface was recreated.
    View* m_target = nullptr;
    std::string m_targetId;
    bool m_takenOver = false;
    // Pointer position, held in the target window's coordinates so the cursor
    // a client draws from them keeps its place when the window moves.
    double m_x = 0.0;
    double m_y = 0.0;
    uint32_t m_buttons = 0;

    // The person's devices, watched for the input that takes a window back.
    // One per keyboard the person can type on. The listener carries a pointer
    // back to the seat, so the handler is a plain C callback.
    struct PersonKeyListener {
      wl_listener listener{};
      AgentSeat* seat = nullptr;
      wlr_keyboard* keyboard = nullptr;
    };
    std::vector<std::unique_ptr<PersonKeyListener>> m_personKeyboards;
    wl_listener m_newKeyboard{};
    wl_listener m_keyboardFocus{};
  };

} // namespace umbriel
