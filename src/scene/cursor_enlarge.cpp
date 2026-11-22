#include "scene/cursor_enlarge.h"

#include "config/config.h"
#include "core/log.h"
#include "output/output.h"
#include "server/server.h"
#include "wlr.h"

#include <algorithm>
#include <cmath>

namespace {

  constexpr const char* kDefaultShape = "default";

} // namespace

namespace umbriel {

  CursorEnlarge::CursorEnlarge(Server& server) : m_server(&server) { rebuildChairs(); }

  CursorEnlarge::~CursorEnlarge() {
    if (m_timer != nullptr) {
      wl_event_source_remove(m_timer);
    }
    releaseChairs();
    if (m_manager != nullptr) {
      wlr_xcursor_manager_destroy(m_manager);
    }
  }

  void CursorEnlarge::trigger() {
    if (m_active) {
      return;
    }
    const Config::Input::Cursor& settings = config().input.cursor;
    if (settings.zoomFactor <= 1.0 || settings.size <= 0) {
      return;
    }
    // The image is loaded once at a whole device pixel ratio: a fractional one
    // would hand the scene a larger image than the grown size can show.
    double scale = 1.0;
    for (const std::unique_ptr<Output>& output : m_server->outputs()) {
      scale = std::max(scale, std::floor(static_cast<double>(output->wlr()->scale)));
    }
    if (m_manager != nullptr) {
      wlr_xcursor_manager_destroy(m_manager);
    }
    m_manager = wlr_xcursor_manager_create(settings.theme.empty() ? nullptr : settings.theme.c_str(), settings.size);
    if (m_manager == nullptr) {
      return;
    }
    if (!wlr_xcursor_manager_load(m_manager, scale)) {
      wlr_xcursor_manager_destroy(m_manager);
      m_manager = nullptr;
      return;
    }
    m_scale = scale;
    m_zoom = settings.zoomFactor;
    m_active = true;
    logDebug("cursor enlarged by {}", m_zoom);
    applyCursorImage();
    armTimer();
  }

  void CursorEnlarge::armTimer() {
    if (m_timer == nullptr) {
      m_timer = wl_event_loop_add_timer(wl_display_get_event_loop(m_server->display()), onTimer, this);
      if (m_timer == nullptr) {
        return;
      }
    }
    // A hold of zero keeps the image out until a press puts it away.
    wl_event_source_timer_update(m_timer, config().input.cursor.enlargeHoldMs);
  }

  int CursorEnlarge::onTimer(void* data) {
    static_cast<CursorEnlarge*>(data)->dismiss();
    return 0;
  }

  void CursorEnlarge::dismiss() {
    if (m_timer != nullptr) {
      wl_event_source_timer_update(m_timer, 0);
    }
    if (!m_active) {
      return;
    }
    m_active = false;
    for (const std::unique_ptr<Chair>& chair : m_chairs) {
      hideImage(*chair);
    }
  }

  void CursorEnlarge::setScale(double scale) {
    if (m_scale == scale || m_manager == nullptr || !m_active) {
      m_scale = scale;
      return;
    }
    if (wlr_xcursor_manager_load(m_manager, scale)) {
      m_scale = scale;
      applyCursorImage();
    }
  }

  void CursorEnlarge::applyCursorImage() {
    for (const std::unique_ptr<Chair>& chair : m_chairs) {
      applyImage(*chair);
    }
  }

  void CursorEnlarge::setXcursor(const char* name) {
    m_shape = name != nullptr ? name : kDefaultShape;
    m_surface = nullptr;
    m_hotspotX = 0;
    m_hotspotY = 0;
    if (m_active) {
      applyCursorImage();
    }
  }

  void CursorEnlarge::setSurface(wlr_surface* surface, int32_t hotspotX, int32_t hotspotY) {
    m_surface = surface;
    m_hotspotX = hotspotX;
    m_hotspotY = hotspotY;
    if (m_active) {
      applyCursorImage();
    }
  }

  void CursorEnlarge::move(double x, double y) {
    m_x = x;
    m_y = y;
  }

  void CursorEnlarge::setHidden(bool hidden) {
    if (m_hidden == hidden) {
      return;
    }
    m_hidden = hidden;
    if (m_active) {
      applyCursorImage();
    }
  }

  void CursorEnlarge::handleOutputLayoutChange() { rebuildChairs(); }

  CursorEnlarge::Chair* CursorEnlarge::chairAt(Output* output) {
    for (const std::unique_ptr<Chair>& chair : m_chairs) {
      if (chair->output == output) {
        return chair.get();
      }
    }
    return nullptr;
  }

  CursorEnlarge::Chair& CursorEnlarge::chairForOutput(Output& output) {
    if (Chair* chair = chairAt(&output)) {
      return *chair;
    }
    auto owned = std::make_unique<Chair>();
    owned->output = &output;
    m_chairs.push_back(std::move(owned));
    Chair& chair = *m_chairs.back();
    wlr_output* wlrOutput = output.wlr();
    const wlr_box layout = output.layoutBox();
    chair.originX = layout.x;
    chair.originY = layout.y;
    int width = 0;
    int height = 0;
    wlr_output_effective_resolution(wlrOutput, &width, &height);
    chair.width = width;
    chair.height = height;
    // Above the layer surfaces, so the grown image is not covered by a panel.
    if (wlr_scene_tree* overlay = output.layerTree(ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY)) {
      chair.tree = wlr_scene_tree_create(overlay->node.parent);
      if (chair.tree != nullptr) {
        wlr_scene_node_set_position(&chair.tree->node, layout.x, layout.y);
        chair.image = wlr_scene_buffer_create(chair.tree, nullptr);
        if (chair.image != nullptr) {
          wlr_scene_node_set_enabled(&chair.image->node, false);
        }
      }
    }
    // An Xcursor image is drawn by a software cursor, which needs a seat. One
    // without capabilities takes nothing from the real pointer, and a cursor
    // with no surface commits an empty one until it is enlarged.
    wlr_seat_create(m_server->display(), wlrOutput->name);
    chair.cursor = wlr_cursor_create();
    if (chair.cursor != nullptr) {
      wlr_cursor_set_surface(chair.cursor, nullptr, 0, 0);
    }
    chair.owner = this;
    chair.destroy.notify = onOutputDestroy;
    wl_signal_add(&wlrOutput->events.destroy, &chair.destroy);
    return chair;
  }

  void CursorEnlarge::rebuildChairs() {
    releaseChairs();
    for (const std::unique_ptr<Output>& output : m_server->outputs()) {
      if (output->wlr()->enabled) {
        chairForOutput(*output);
      }
    }
  }

  void CursorEnlarge::releaseChairs() {
    for (const std::unique_ptr<Chair>& chair : m_chairs) {
      detachSurface(*chair);
      if (chair->destroy.link.next != nullptr) {
        wl_list_remove(&chair->destroy.link);
        chair->destroy.link.next = nullptr;
      }
      if (chair->cursor != nullptr) {
        wlr_cursor_destroy(chair->cursor);
        chair->cursor = nullptr;
      }
      if (chair->tree != nullptr) {
        wlr_scene_node_destroy(&chair->tree->node);
        chair->tree = nullptr;
        chair->image = nullptr;
      }
    }
    m_chairs.clear();
    m_active = false;
  }

  void CursorEnlarge::detachSurface(Chair& chair) {
    if (chair.surfaceCommit.link.next != nullptr) {
      wl_list_remove(&chair.surfaceCommit.link);
      chair.surfaceCommit.link.next = nullptr;
    }
    if (chair.surfaceDestroy.link.next != nullptr) {
      wl_list_remove(&chair.surfaceDestroy.link);
      chair.surfaceDestroy.link.next = nullptr;
    }
    chair.surface = nullptr;
  }

  void CursorEnlarge::hideImage(Chair& chair) {
    if (chair.image != nullptr) {
      wlr_scene_node_set_enabled(&chair.image->node, false);
    }
    if (chair.cursor != nullptr) {
      wlr_cursor_set_surface(chair.cursor, nullptr, 0, 0);
    }
  }

  void CursorEnlarge::applyImage(Chair& chair) {
    hideImage(chair);
    if (!m_active || m_hidden || chair.cursor == nullptr) {
      return;
    }
    if (m_surface != nullptr) {
      // A client that draws its own cursor image keeps it: the scene buffer
      // scales the very same surface, which no seat gets to see.
      if (chair.surface != m_surface) {
        detachSurface(chair);
        chair.surface = m_surface;
        chair.surfaceCommit.notify = onChairSurfaceCommit;
        wl_signal_add(&m_surface->events.commit, &chair.surfaceCommit);
        chair.surfaceDestroy.notify = onChairSurfaceDestroy;
        wl_signal_add(&m_surface->events.destroy, &chair.surfaceDestroy);
      }
      if (chair.image != nullptr) {
        wlr_scene_buffer_set_buffer(chair.image, &m_surface->buffer->base);
      }
      placeImage(chair);
      return;
    }
    detachSurface(chair);
    if (m_manager != nullptr) {
      wlr_cursor_set_xcursor(chair.cursor, m_manager, m_shape.c_str());
    }
  }

  void CursorEnlarge::placeImage(Chair& chair) {
    if (chair.image == nullptr || chair.output == nullptr) {
      return;
    }
    wlr_buffer* buffer =
        chair.surface != nullptr && chair.surface->buffer != nullptr ? &chair.surface->buffer->base : nullptr;
    if (buffer == nullptr) {
      wlr_scene_node_set_enabled(&chair.image->node, false);
      return;
    }
    // The buffer is in device pixels, the scene draws in logical ones: the
    // grown size is the image times the factor, less the device scale.
    const double scale = std::max(1.0, static_cast<double>(chair.output->wlr()->scale));
    const double factor = m_zoom / scale;
    const double left = m_x - static_cast<double>(m_hotspotX);
    const double top = m_y - static_cast<double>(m_hotspotY);
    const bool visible = m_x >= chair.originX
        && m_y >= chair.originY
        && m_x < chair.originX + chair.width
        && m_y < chair.originY + chair.height;
    wlr_scene_node_set_enabled(&chair.image->node, visible);
    if (!visible) {
      return;
    }
    wlr_scene_buffer_set_dest_size(
        chair.image, static_cast<int>(std::lround(static_cast<double>(buffer->width) * factor)),
        static_cast<int>(std::lround(static_cast<double>(buffer->height) * factor))
    );
    wlr_scene_node_set_position(
        &chair.image->node, static_cast<int>(std::lround(left)) - chair.originX,
        static_cast<int>(std::lround(top)) - chair.originY
    );
  }

  void CursorEnlarge::onOutputDestroy(wl_listener* listener, void* /*data*/) {
    Chair* chair;
    chair = wl_container_of(listener, chair, destroy);
    chair->output = nullptr;
    chair->destroy.link.next = nullptr;
    chair->owner->hideImage(*chair);
    chair->owner->detachSurface(*chair);
  }

  void CursorEnlarge::onChairSurfaceCommit(wl_listener* listener, void* /*data*/) {
    Chair* chair;
    chair = wl_container_of(listener, chair, surfaceCommit);
    if (chair->image == nullptr || chair->surface == nullptr || chair->surface->buffer == nullptr) {
      return;
    }
    wlr_scene_buffer_set_buffer(chair->image, &chair->surface->buffer->base);
    chair->owner->placeImage(*chair);
  }

  void CursorEnlarge::onChairSurfaceDestroy(wl_listener* listener, void* /*data*/) {
    Chair* chair;
    chair = wl_container_of(listener, chair, surfaceDestroy);
    chair->surfaceCommit.link.next = nullptr;
    chair->surfaceDestroy.link.next = nullptr;
    chair->surface = nullptr;
    chair->owner->hideImage(*chair);
  }

} // namespace umbriel
