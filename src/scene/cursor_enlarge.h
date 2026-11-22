#pragma once

#include "wlr.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace umbriel {

  class Output;
  class Server;
  class CursorEnlarge;

  // Shake the pointer and the cursor grows, so it is easy to find. The grown
  // image is a scene buffer of its own that plays the image the real cursor
  // shows, so the effect stands on its own and reaches no client.
  class CursorEnlarge {
  public:
    explicit CursorEnlarge(Server& server);
    ~CursorEnlarge();

    CursorEnlarge(const CursorEnlarge&) = delete;
    CursorEnlarge& operator=(const CursorEnlarge&) = delete;

    bool active() const { return m_active; }
    // Starts the growth, or keeps an enlargement that is already out.
    void trigger();

    // Stops one that is out, such as when a pointer button goes down.
    void dismiss();

    // The image the real cursor shows, in either of its two forms.
    void setXcursor(const char* name);
    void setSurface(wlr_surface* surface, int32_t hotspotX, int32_t hotspotY);
    // The pointer's position in the layout, read when it moves.
    void move(double x, double y);
    // The device pixel ratio an Xcursor image is loaded at.
    void setScale(double scale);
    void setHidden(bool hidden);
    // An output appeared, moved or went away: rebuild the buffers.
    void handleOutputLayoutChange();

  private:
    // One output that shows the grown image: a scene buffer of its own on the
    // output's overlay. The image is drawn by the scene rather than by a
    // software cursor, which is what makes the growth show on an output with no
    // cursor plane. It needs no seat of its own, so cursor rendering does not
    // add extra seat globals for clients to bind.
    struct Chair {
      Output* output = nullptr;
      wlr_scene_tree* tree = nullptr;
      wlr_scene_buffer* image = nullptr;
      // The overlay's origin in the layout, and the logical size of the output.
      int originX = 0;
      int originY = 0;
      int width = 0;
      int height = 0;
      // The grown image: the size it is drawn at, in logical pixels, and where
      // its hotspot sits inside it, so the arrow stays under the pointer.
      int destWidth = 0;
      int destHeight = 0;
      double hotspotX = 0.0;
      double hotspotY = 0.0;
      wl_listener destroy{};
      // Who attached the listener above, which carries no data of its own.
      CursorEnlarge* owner = nullptr;
    };

    static void onOutputDestroy(wl_listener* listener, void* data);
    static void onSurfaceCommit(wl_listener* listener, void* data);
    static void onSurfaceDestroy(wl_listener* listener, void* data);

    Chair* chairAt(Output* output);
    Chair& chairForOutput(Output& output);
    void rebuildChairs();
    void releaseChairs();
    void watchSurface(wlr_surface* surface);
    void unwatchSurface();
    void hideImage(Chair& chair);
    void placeImage(Chair& chair);
    void applyImage(Chair& chair);
    void applyCursorImage();
    void armTimer();

    Server* m_server;
    wlr_xcursor_manager* m_manager = nullptr;
    double m_zoom = 1.0;
    std::string m_shape;
    wlr_surface* m_surface = nullptr;
    // The client cursor surface's own commits and its going away, watched once
    // for every chair: each of them shows the same surface.
    wl_listener m_surfaceCommit{};
    wl_listener m_surfaceDestroy{};
    int32_t m_hotspotX = 0;
    int32_t m_hotspotY = 0;
    double m_scale = 1.0;
    double m_x = 0;
    double m_y = 0;
    bool m_hidden = false;
    bool m_active = false;
    // The timer that lets the enlargement go once its hold time is up.
    wl_event_source* m_timer = nullptr;
    static int onTimer(void* data);
    std::vector<std::unique_ptr<Chair>> m_chairs;
  };

} // namespace umbriel
