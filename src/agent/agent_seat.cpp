#include "agent/agent_seat.h"

#include "config/config.h"
#include "core/log.h"
#include "input/cursor.h"
#include "input/keyboard.h"
#include "input/seat.h"
#include "output/output.h"
#include "server/action_request.h"
#include "server/server.h"
#include "view/view.h"
#include "workspace/workspace.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <nlohmann/json.hpp>
#include <string>
#include <wayland-server-protocol.h>
#include <xkbcommon/xkbcommon.h>

namespace {

  // One wheel notch, in the pixels a client that ignores `value120` uses.
  constexpr double kNotchStep = 15.0;

  wlr_surface* focusedSurface(wlr_seat* seat) {
    return seat != nullptr ? seat->keyboard_state.focused_surface : nullptr;
  }

  bool isModifierKeysym(xkb_keysym_t sym) { return sym >= XKB_KEY_Shift_L && sym <= XKB_KEY_Hyper_R; }

} // namespace

namespace umbriel {

  AgentSeat::AgentSeat(Server& server) : m_server(&server) {
    // The agent seat is a seat of its own, so the person's pointer, keyboard
    // focus, and focus history are never involved: a client bound to it gets
    // this seat's enter, and nothing the agent does moves what the person sees.
    m_seat = wlr_seat_create(m_server->display(), "agent");
    if (m_seat == nullptr) {
      return;
    }

    // Taking a window back: a button the person presses over the agent's
    // window, or a key they type into it, is the person reaching for it. Watch
    // the devices themselves: the seat's own signals only report what a client
    // did, and this is about what the person did. Devices that arrive later are
    // picked up by the backend's new-input signal.
    for (const auto& keyboard : m_server->keyboards()) {
      watchKeyboard(keyboard->wlr());
    }
    m_newKeyboard.notify = onNewDevice;
    wl_signal_add(&m_server->backend()->events.new_input, &m_newKeyboard);
    m_keyboardFocus.notify = onKeyboardFocusChange;
    wl_signal_add(&m_seat->keyboard_state.events.focus_change, &m_keyboardFocus);
  }

  AgentSeat::~AgentSeat() {
    // Removing a listener needs the signal it was added to, and the devices are
    // still alive here, so each list is walked rather than cleared.
    for (const auto& entry : m_personKeyboards) {
      wl_list_remove(&entry->listener.link);
    }
    m_personKeyboards.clear();
    wl_list_remove(&m_newKeyboard.link);
    if (m_keyboardFocus.link.next != nullptr) {
      wl_list_remove(&m_keyboardFocus.link);
    }
    destroyVirtualDevices();
    release();
    removeOutput();
  }

  void AgentSeat::destroyVirtualDevices() {
    if (!m_virtual.has_value()) {
      return;
    }
    Virtual devices = *m_virtual;
    m_virtual.reset();
    // Destroying each resource tears its device down exactly as a client
    // disconnecting would, so no device outlives the seat that made it. The
    // destroy listeners stay in place for this: they are what clears m_virtual
    // when the teardown comes back around.
    m_server->destroyAgentDevices(devices.pointer, devices.keyboard);
  }

  View* AgentSeat::target() const { return m_target; }

  void AgentSeat::release() {
    // A window that was only moved for the agent goes back where it came from,
    // so letting go leaves the desktop as the person had it.
    if (View* target = m_target; target != nullptr && !m_previousOutput.empty() && m_output != m_previousOutput) {
      const Output* current = target->currentOutput();
      if (current != nullptr && current->wlr()->name != nullptr && m_output == current->wlr()->name) {
        std::string ignored;
        moveToOutput(*target, m_previousOutput, &ignored);
      }
    }
    m_previousOutput.clear();
    wlr_seat_keyboard_notify_clear_focus(m_seat);
    wlr_seat_pointer_clear_focus(m_seat);
    destroyVirtualDevices();
    m_target = nullptr;
    m_targetId.clear();
    m_takenOver = false;
    m_x = 0.0;
    m_y = 0.0;
    m_buttons = 0;
  }

  void AgentSeat::handBack() {
    if (m_target == nullptr || m_takenOver) {
      return;
    }
    m_takenOver = true;
    wlr_seat_keyboard_notify_clear_focus(m_seat);
    wlr_seat_pointer_clear_focus(m_seat);
    // Whatever the agent typed with the keyboard held must not outlive the
    // hand-off: a stuck Shift would reach the person's typing afterwards.
    m_buttons = 0;
  }

  void AgentSeat::notePersonInput(View* view) {
    if (m_target == nullptr || m_takenOver) {
      return;
    }
    if (view != nullptr && view == m_target) {
      handBack();
    }
  }

  bool AgentSeat::prepare(View* view, std::string* error) {
    if (!enabled()) {
      if (error != nullptr) {
        *error = "the agent seat is off";
      }
      return false;
    }
    if (m_server->sessionLocked()) {
      if (error != nullptr) {
        *error = "the session is locked";
      }
      return false;
    }
    if (m_takenOver) {
      if (error != nullptr) {
        *error = "the person took the window back; take a window again to continue";
      }
      return false;
    }
    if (view != nullptr && view != m_target) {
      m_target = view;
      m_targetId = view->extForeignIdentifier() != nullptr ? view->extForeignIdentifier() : "";
      m_x = 0.0;
      m_y = 0.0;
    }
    if (m_target == nullptr) {
      if (error != nullptr) {
        *error = "no window is taken; take one first";
      }
      return false;
    }
    if (!m_virtual.has_value() || m_virtual->keyboard == nullptr || m_virtual->pointer == nullptr) {
      // A device can go away while the target is still taken; make it again.
      destroyVirtualDevices();
      if (!createVirtualDevices(error)) {
        return false;
      }
    }
    if (m_virtual->keyboard != nullptr && m_virtual->keyboard->keyboard.keymap != nullptr) {
      // The server registers a virtual keyboard when its keymap arrives, which
      // may be after the client's create request returned. Until then it is not
      // in the server's keyboard list and typing would go nowhere.
      wlr_seat_set_keyboard(m_seat, &m_virtual->keyboard->keyboard);
    }
    enterTarget();
    return true;
  }

  bool AgentSeat::createVirtualDevices(std::string* error) {
    wlr_virtual_pointer_manager_v1* pointerManager = m_server->virtualPointerManager();
    wlr_virtual_keyboard_manager_v1* keyboardManager = m_server->virtualKeyboardManager();
    if (pointerManager == nullptr || keyboardManager == nullptr) {
      if (error != nullptr) {
        *error = "this compositor was built without virtual input";
      }
      return false;
    }

    // The server's own listeners see these devices before this call returns and
    // would attach them to the person's cursor and keyboard list. The claims
    // make it hand them here instead.
    Virtual devices;
    devices.pointer = m_server->makeVirtualPointer().release();
    if (devices.pointer == nullptr) {
      if (error != nullptr) {
        *error = "could not create a virtual pointer";
      }
      return false;
    }
    devices.keyboard = m_server->makeVirtualKeyboard().release();
    if (devices.keyboard == nullptr) {
      m_server->destroyAgentDevices(devices.pointer, nullptr);
      if (error != nullptr) {
        *error = "could not create a virtual keyboard";
      }
      return false;
    }

    // The keyboard types with the session's layout, so the agent's keys reach
    // the client as the same characters the person's keyboard would produce.
    if (xkb_keymap* keymap = m_server->compileKeymap(m_server->systemKeymap()); keymap != nullptr) {
      wlr_keyboard_set_keymap(&devices.keyboard->keyboard, keymap);
      xkb_keymap_unref(keymap);
    }
    wlr_keyboard_set_repeat_info(
        &devices.keyboard->keyboard, config().input.keyboard.repeatRate, config().input.keyboard.repeatDelay
    );

    m_virtual = devices;
    return true;
  }

  bool AgentSeat::moveToOutput(View& view, std::string_view output, std::string* error) {
    // Moving the window is the compositor's own transfer, the same one the
    // move-to-output action performs, so the agent's move behaves exactly like
    // a workspace move rather than a bespoke rearrangement.
    nlohmann::json request{{"cmd", "action"}, {"action", "workspace-move-to-output"}, {"destination", output}};
    if (view.workspace() == nullptr) {
      if (error != nullptr) {
        *error = "window is not on a workspace";
      }
      return false;
    }
    request["workspace"] = view.workspace()->id();
    std::string reason;
    const std::optional<Keybind> bind = actionFromRequest(request, reason);
    if (!bind) {
      if (error != nullptr) {
        *error = std::move(reason);
      }
      return false;
    }
    m_server->executeAction(*bind, &reason);
    if (!reason.empty()) {
      if (error != nullptr) {
        *error = std::move(reason);
      }
      return false;
    }
    return true;
  }

  void AgentSeat::createOutput() {
    m_output = config().agent.output;
    if (m_output.empty()) {
      return;
    }
    // An output with that name may already be there, a real monitor included.
    // Then that output is the agent's, and nothing more is created.
    std::string error;
    if (m_server->createHeadlessOutput(m_output, std::nullopt, &error) != m_output) {
      logWarn("agent output: {}", error);
    }
    for (const auto& output : m_server->outputs()) {
      if (output->wlr() != nullptr && output->wlr()->name != nullptr && m_output == output->wlr()->name) {
        m_outputWlr = output->wlr();
        break;
      }
    }
  }

  void AgentSeat::removeOutput() {
    if (m_outputWlr == nullptr) {
      return;
    }
    std::string error;
    m_server->destroyOutput(m_output, &error);
    m_outputWlr = nullptr;
    m_output.clear();
  }

  bool AgentSeat::take(View& view, std::string* error) {
    if (!enabled()) {
      if (error != nullptr) {
        *error = "the agent seat is off";
      }
      return false;
    }
    if (m_server->sessionLocked()) {
      if (error != nullptr) {
        *error = "the session is locked";
      }
      return false;
    }
    // A window the person is focused in is theirs: taking it would move their
    // focus and their view. Windows on other workspaces, pinned windows, and the
    // window already taken are all possible.
    wlr_surface* personFocus = focusedSurface(m_server->seat()->wlr());
    View* personView = personFocus != nullptr ? View::fromSurface(personFocus) : nullptr;
    if (personView != nullptr && personView == &view && &view != m_target) {
      if (error != nullptr) {
        *error = "window "
            + std::string(view.extForeignIdentifier() != nullptr ? view.extForeignIdentifier() : "")
            + " is focused on the person's seat";
      }
      return false;
    }
    if (view.workspace() == nullptr) {
      if (error != nullptr) {
        *error = "window is not on a workspace";
      }
      return false;
    }
    // With an output of its own, the agent works where the person is not: the
    // window is moved there, and moved back when it is released.
    const std::string previousOutput = view.currentOutput() != nullptr && view.currentOutput()->wlr()->name != nullptr
        ? view.currentOutput()->wlr()->name
        : "";
    if (!m_output.empty() && previousOutput != m_output) {
      if (!moveToOutput(view, m_output, error)) {
        return false;
      }
    }
    m_target = &view;
    m_targetId = view.extForeignIdentifier() != nullptr ? view.extForeignIdentifier() : "";
    m_previousOutput = previousOutput;
    m_takenOver = false;
    m_x = 0.0;
    m_y = 0.0;
    m_buttons = 0;
    return prepare(&view, error);
  }

  bool AgentSeat::geometry(wlr_box* box) const {
    if (m_target == nullptr || box == nullptr || m_takenOver) {
      return false;
    }
    const wlr_box content = m_target->committedContentBox();
    box->x = m_target->layoutX();
    box->y = m_target->layoutY();
    box->width = content.width;
    box->height = content.height;
    return true;
  }

  bool AgentSeat::focusTarget() {
    if (m_target == nullptr || !m_virtual.has_value() || m_virtual->keyboard == nullptr) {
      return false;
    }
    wlr_surface* surface = m_target->rootSurface();
    if (surface == nullptr || !surface->mapped) {
      return false;
    }
    wlr_keyboard* keyboard = &m_virtual->keyboard->keyboard;
    wlr_seat_keyboard_notify_enter(m_seat, surface, keyboard->keycodes, keyboard->num_keycodes, &keyboard->modifiers);
    return true;
  }

  void AgentSeat::enterTarget() {
    focusTarget();
    if (wlr_surface* surface = m_target != nullptr ? m_target->rootSurface() : nullptr;
        surface != nullptr && surface->mapped) {
      wlr_seat_pointer_notify_enter(m_seat, surface, m_x, m_y);
      wlr_seat_pointer_notify_motion(m_seat, 0, m_x, m_y);
      wlr_seat_pointer_notify_frame(m_seat);
    }
  }

  bool AgentSeat::movePointer(double x, double y, std::string* error) {
    if (!prepare(m_target, error)) {
      return false;
    }
    m_x = x;
    m_y = y;
    enterTarget();
    wlr_seat_pointer_notify_frame(m_seat);
    return true;
  }

  bool AgentSeat::button(uint32_t button, bool pressed, std::string* error) {
    if (button < 1 || button > 5) {
      if (error != nullptr) {
        *error = "button must be 1 (left), 2 (middle), 3 (right), 4, or 5";
      }
      return false;
    }
    if (!prepare(m_target, error)) {
      return false;
    }
    // wl_pointer numbers buttons 8 above their Linux code, so a client reading
    // a 1-3 click sees what it would from a real mouse.
    const uint32_t protocolButton = button + 8;
    wlr_seat_pointer_notify_button(
        m_seat, 0, protocolButton, pressed ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED
    );
    wlr_seat_pointer_notify_frame(m_seat);
    if (pressed) {
      m_buttons |= 1U << (button - 1);
    } else {
      m_buttons &= ~(1U << (button - 1));
    }
    return true;
  }

  bool AgentSeat::scroll(double notches, bool horizontal, std::string* error) {
    if (!prepare(m_target, error)) {
      return false;
    }
    if (notches == 0.0) {
      return true;
    }
    wlr_seat_pointer_notify_axis(
        m_seat, static_cast<uint32_t>(m_server->uptimeMs()),
        horizontal ? WL_POINTER_AXIS_HORIZONTAL_SCROLL : WL_POINTER_AXIS_VERTICAL_SCROLL, notches * kNotchStep,
        static_cast<int32_t>(std::lround(notches * kNotchStep)), WL_POINTER_AXIS_SOURCE_WHEEL,
        WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL
    );
    wlr_seat_pointer_notify_frame(m_seat);
    return true;
  }

  bool AgentSeat::sendKey(uint32_t keycode, bool pressed) {
    if (!m_virtual || m_virtual->keyboard == nullptr) {
      return false;
    }
    wlr_keyboard* keyboard = &m_virtual->keyboard->keyboard;
    wlr_keyboard_key_event event{};
    event.time_msec = static_cast<uint32_t>(m_server->uptimeMs());
    event.keycode = keycode;
    event.update_state = true;
    event.state = pressed ? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED;
    wlr_seat_set_keyboard(m_seat, keyboard);
    wlr_keyboard_notify_key(keyboard, &event);
    return true;
  }

  bool AgentSeat::key(uint32_t keycode, std::string* error) {
    if (keycode == 0) {
      if (error != nullptr) {
        *error = "key needs an evdev keycode, the number linux/input-event-codes.h gives the key";
      }
      return false;
    }
    if (!prepare(m_target, error)) {
      return false;
    }
    if (!sendKey(keycode, true)) {
      if (error != nullptr) {
        *error = "the agent keyboard is gone";
      }
      return false;
    }
    sendKey(keycode, false);
    return true;
  }

  bool AgentSeat::type(std::string_view text, std::string* error) {
    if (!prepare(m_target, error)) {
      return false;
    }
    if (!m_virtual || m_virtual->keyboard == nullptr || m_virtual->keyboard->keyboard.keymap == nullptr) {
      if (error != nullptr) {
        *error = "the agent keyboard has no keymap";
      }
      return false;
    }
    if (text.empty()) {
      return true;
    }
    // xkb's layout queries want a mutable keymap, and wlr's accessor hands out
    // the keyboard's const one; the lookups below only read it.
    auto* keymap = const_cast<xkb_keymap*>(m_virtual->keyboard->keyboard.keymap);
    const xkb_mod_index_t shift = xkb_keymap_mod_get_index(keymap, XKB_MOD_NAME_SHIFT);
    const uint32_t shiftCode = 42;
    const uint32_t altGrCode = 100;

    for (const char32_t character : text) {
      if (character == U'\n' || character == U'\r') {
        sendKey(28, true);
        sendKey(28, false);
        continue;
      }
      // Walk the keyboard's own keymap for a key and level that produce the
      // character, so the agent types what this layout types, non-Latin
      // layouts included, instead of assuming a US keyboard.
      bool found = false;
      uint32_t code = 0;
      unsigned level = 0;
      for (uint32_t candidate = 0; candidate < 256 && !found; ++candidate) {
        const xkb_layout_index_t layouts = xkb_keymap_num_layouts_for_key(keymap, candidate);
        for (xkb_layout_index_t layout = 0; layout < layouts && !found; ++layout) {
          const xkb_level_index_t levels = xkb_keymap_num_levels_for_key(keymap, candidate, layout);
          for (xkb_level_index_t candidateLevel = 0; candidateLevel < levels && !found; ++candidateLevel) {
            const xkb_keysym_t* syms = nullptr;
            const int count = xkb_keymap_key_get_syms_by_level(keymap, candidate, layout, candidateLevel, &syms);
            if (count != 1 || syms == nullptr || syms[0] > 0x10FFFFU || xkb_keysym_to_utf32(syms[0]) != character) {
              continue;
            }
            found = true;
            code = candidate;
            level = candidateLevel;
          }
        }
      }
      if (!found) {
        if (error != nullptr) {
          *error = "no key on the agent keyboard produces this character";
        }
        return false;
      }
      // The level a keysym sits at is what its modifiers are: level 1 comes from
      // Shift, and the levels above 2 from AltGr (Mod5) on the layouts that have
      // one, which is where most non-Latin characters live.
      std::array<uint32_t, 2> held{};
      size_t heldCount = 0;
      if (level % 2 == 1 && shift != XKB_MOD_INVALID) {
        held[heldCount++] = shiftCode;
      }
      if (level >= 2) {
        held[heldCount++] = altGrCode;
      }
      for (size_t index = 0; index < heldCount; ++index) {
        sendKey(held[index], true);
      }
      sendKey(code, true);
      sendKey(code, false);
      for (size_t index = heldCount; index > 0; --index) {
        sendKey(held[index - 1], false);
      }
    }
    return true;
  }

  void AgentSeat::watchKeyboard(wlr_keyboard* keyboard) {
    // The agent's own keyboard types on the agent seat; a virtual keyboard from
    // someone else is still the person's.
    if (keyboard == nullptr) {
      return;
    }
    auto entry = std::make_unique<PersonKeyListener>();
    entry->seat = this;
    entry->keyboard = keyboard;
    entry->listener.notify = onPersonKey;
    wl_signal_add(&keyboard->events.key, &entry->listener);
    m_personKeyboards.push_back(std::move(entry));
  }

  void AgentSeat::onNewDevice(wl_listener* listener, void* data) {
    AgentSeat* self;
    self = wl_container_of(listener, self, m_newKeyboard);
    auto* device = static_cast<wlr_input_device*>(data);
    if (device->type == WLR_INPUT_DEVICE_KEYBOARD) {
      self->watchKeyboard(wlr_keyboard_from_input_device(device));
    }
  }

  void AgentSeat::onPersonKey(wl_listener* listener, void* data) {
    PersonKeyListener* entry;
    entry = wl_container_of(listener, entry, listener);
    entry->seat->handlePersonKey(data);
  }

  void AgentSeat::onKeyboardFocusChange(wl_listener* listener, void* /*data*/) {
    AgentSeat* self;
    self = wl_container_of(listener, self, m_keyboardFocus);
    self->handleKeyboardFocusChange();
  }

  void AgentSeat::handlePersonKey(void* data) {
    auto* event = static_cast<wlr_keyboard_key_event*>(data);
    if (event->state != WL_KEYBOARD_KEY_STATE_PRESSED) {
      return;
    }
    if (m_target == nullptr || m_takenOver) {
      return;
    }
    wlr_keyboard* keyboard = wlr_seat_get_keyboard(m_server->seat()->wlr());
    if (keyboard == nullptr || keyboard->xkb_state == nullptr) {
      return;
    }
    // The person's own keyboard, not the agent's: a key the agent sends must
    // not count as the person reaching in.
    if (m_virtual.has_value() && m_virtual->keyboard != nullptr && keyboard == &m_virtual->keyboard->keyboard) {
      return;
    }
    // Holding Shift on the way to somewhere else is not typing here.
    if (isModifierKeysym(xkb_state_key_get_one_sym(keyboard->xkb_state, event->keycode + 8))) {
      return;
    }
    notePersonInput(View::fromSurface(focusedSurface(m_server->seat()->wlr())));
  }

  void AgentSeat::handleKeyboardFocusChange() {
    if (m_target == nullptr || m_takenOver) {
      return;
    }
    wlr_surface* surface = focusedSurface(m_seat);
    if (surface == nullptr) {
      // The client withdrew focus itself, for example by closing. The target is
      // still taken; the next request enters it again.
      return;
    }
    // A submenu or popup inside the target is the same window; a focus that
    // moved to a different one means the client handed the seat elsewhere.
    if (View* view = View::fromSurface(surface); view != nullptr && view != m_target) {
      handBack();
    }
  }

  void AgentSeat::keyboardDestroyed() {
    if (m_virtual) {
      m_virtual->keyboard = nullptr;
    }
  }

  void AgentSeat::handleOutputRemoved(wlr_output* output) {
    if (m_target == nullptr || output == nullptr) {
      return;
    }
    // A target that is only on the output going away has nowhere to be shown,
    // and the person cannot take back a window they cannot see: hand it back
    // before the output pointer stops being valid.
    if (const Output* current = m_target->currentOutput(); current != nullptr && current->wlr() == output) {
      m_takenOver = true;
      release();
    }
  }

} // namespace umbriel
