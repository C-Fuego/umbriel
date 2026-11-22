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
    unwatchSurface();
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
    // Asking the theme for a bigger cursor beats scaling a small one up: the
    // grown image is the shape the theme draws, at the size the zoom asks for.
    const int grownSize =
        std::max(1, static_cast<int>(std::lround(static_cast<double>(settings.size) * settings.zoomFactor)));
    m_manager = wlr_xcursor_manager_create(settings.theme.empty() ? nullptr : settings.theme.c_str(), grownSize);
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
    unwatchSurface();
    m_surface = nullptr;
    m_hotspotX = 0;
    m_hotspotY = 0;
    if (m_active) {
      applyCursorImage();
    }
  }

  void CursorEnlarge::setSurface(wlr_surface* surface, int32_t hotspotX, int32_t hotspotY) {
    if (surface != m_surface) {
      if (surface != nullptr) {
        watchSurface(surface);
      } else {
        unwatchSurface();
      }
    }
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
    if (m_active) {
      for (const std::unique_ptr<Chair>& chair : m_chairs) {
        placeImage(*chair);
      }
    }
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
          chair.image->point_accepts_input = [](wlr_scene_buffer*, double*, double*) -> bool { return false; };
          wlr_scene_node_set_enabled(&chair.image->node, false);
        }
      }
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
      if (chair->destroy.link.next != nullptr) {
        wl_list_remove(&chair->destroy.link);
        chair->destroy.link.next = nullptr;
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

  void CursorEnlarge::watchSurface(wlr_surface* surface) {
    unwatchSurface();
    m_surfaceCommit.notify = onSurfaceCommit;
    wl_signal_add(&surface->events.commit, &m_surfaceCommit);
    m_surfaceDestroy.notify = onSurfaceDestroy;
    wl_signal_add(&surface->events.destroy, &m_surfaceDestroy);
  }

  void CursorEnlarge::unwatchSurface() {
    if (m_surfaceCommit.link.next != nullptr) {
      wl_list_remove(&m_surfaceCommit.link);
      m_surfaceCommit.link.next = nullptr;
    }
    if (m_surfaceDestroy.link.next != nullptr) {
      wl_list_remove(&m_surfaceDestroy.link);
      m_surfaceDestroy.link.next = nullptr;
    }
  }

  void CursorEnlarge::hideImage(Chair& chair) {
    chair.destWidth = 0;
    chair.destHeight = 0;
    if (chair.image != nullptr) {
      wlr_scene_node_set_enabled(&chair.image->node, false);
    }
  }

  void CursorEnlarge::applyImage(Chair& chair) {
    hideImage(chair);
    if (!m_active || m_hidden || chair.image == nullptr || chair.output == nullptr) {
      return;
    }
    const double scale = std::max(1.0, static_cast<double>(chair.output->wlr()->scale));
    // The grown image is drawn by the scene in both cases: a client's cursor
    // surface as it is, and the theme's own image, copied out of the xcursor at
    // the size the theme was asked for. A software cursor would be invisible on
    // an output with no cursor plane, which is every headless one.
    if (m_surface != nullptr) {
      // A client that draws its own cursor image keeps it: the scene buffer
      // scales the very same surface, which no seat gets to see.
      if (m_surface->buffer == nullptr) {
        return;
      }
      const wlr_buffer* buffer = &m_surface->buffer->base;
      const double factor = m_zoom / scale;
      chair.destWidth = static_cast<int>(std::lround(static_cast<double>(buffer->width) * factor));
      chair.destHeight = static_cast<int>(std::lround(static_cast<double>(buffer->height) * factor));
      chair.hotspotX = static_cast<double>(m_hotspotX);
      chair.hotspotY = static_cast<double>(m_hotspotY);
      wlr_scene_buffer_set_buffer(chair.image, &m_surface->buffer->base);
      placeImage(chair);
      return;
    }
    wlr_xcursor* xcursor =
        m_manager != nullptr ? wlr_xcursor_manager_get_xcursor(m_manager, m_shape.c_str(), m_scale) : nullptr;
    if (xcursor == nullptr || xcursor->image_count == 0) {
      return;
    }
    wlr_xcursor_image* image = xcursor->images[0];
    // wlroots hands out the theme's own image as a buffer, which the scene keeps
    // alive by its own lock.
    wlr_buffer* buffer = wlr_xcursor_image_get_buffer(image);
    if (buffer == nullptr) {
      return;
    }
    chair.destWidth = static_cast<int>(std::lround(static_cast<double>(buffer->width) / scale));
    chair.destHeight = static_cast<int>(std::lround(static_cast<double>(buffer->height) / scale));
    chair.hotspotX = static_cast<double>(image->hotspot_x) / scale;
    chair.hotspotY = static_cast<double>(image->hotspot_y) / scale;
    wlr_scene_buffer_set_buffer(chair.image, buffer);
    placeImage(chair);
  }

  void CursorEnlarge::placeImage(Chair& chair) {
    if (chair.image == nullptr || chair.output == nullptr) {
      return;
    }
    const bool visible = m_active
        && !m_hidden
        && (m_surface == nullptr || m_surface->buffer != nullptr)
        && chair.destWidth > 0
        && chair.destHeight > 0
        && m_x >= chair.originX
        && m_y >= chair.originY
        && m_x < chair.originX + chair.width
        && m_y < chair.originY + chair.height;
    wlr_scene_node_set_enabled(&chair.image->node, visible);
    if (!visible) {
      return;
    }
    // The image is drawn at the size the zoom asks for, and set back from the
    // pointer by its own hotspot so the shape covers what the real cursor does.
    wlr_scene_buffer_set_dest_size(chair.image, chair.destWidth, chair.destHeight);
    wlr_scene_node_set_position(
        &chair.image->node, static_cast<int>(std::lround(m_x - chair.hotspotX)) - chair.originX,
        static_cast<int>(std::lround(m_y - chair.hotspotY)) - chair.originY
    );
  }

  void CursorEnlarge::onOutputDestroy(wl_listener* listener, void* /*data*/) {
    Chair* chair;
    chair = wl_container_of(listener, chair, destroy);
    chair->output = nullptr;
    wl_list_remove(&chair->destroy.link);
    chair->destroy.link.next = nullptr;
    chair->owner->hideImage(*chair);
  }

  void CursorEnlarge::onSurfaceCommit(wl_listener* listener, void* /*data*/) {
    CursorEnlarge* self;
    self = wl_container_of(listener, self, m_surfaceCommit);
    // The client drew its cursor again: every chair shows the new buffer at the
    // size the zoom asks for.
    self->applyCursorImage();
  }

  void CursorEnlarge::onSurfaceDestroy(wl_listener* listener, void* /*data*/) {
    CursorEnlarge* self;
    self = wl_container_of(listener, self, m_surfaceDestroy);
    self->unwatchSurface();
    self->m_surface = nullptr;
    // Nothing is left to grow: the theme's shape takes over on the next move.
    for (const std::unique_ptr<Chair>& chair : self->m_chairs) {
      self->hideImage(*chair);
    }
  }

} // namespace umbriel
