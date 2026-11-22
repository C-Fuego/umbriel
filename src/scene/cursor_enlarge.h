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
    // An output appeared, moved or went away: rebuild the buffers and seats.
    void handleOutputLayoutChange();

  private:
    // One output that shows the grown image: a scene buffer on its overlay for
    // a client's own cursor, and a seat whose software cursor draws the theme's.
    struct Chair {
      Output* output = nullptr;
      wlr_scene_tree* tree = nullptr;
      wlr_scene_buffer* image = nullptr;
      // The overlay's origin in the layout, and the logical size of the output.
      int originX = 0;
      int originY = 0;
      int width = 0;
      int height = 0;
      wlr_cursor* cursor = nullptr;
      wl_listener destroy{};
      wl_listener surfaceCommit{};
      wl_listener surfaceDestroy{};
      wlr_surface* surface = nullptr;
      // Who attached the listeners above, which carry no data of their own.
      CursorEnlarge* owner = nullptr;
    };

    static void onOutputDestroy(wl_listener* listener, void* data);
    static void onChairSurfaceCommit(wl_listener* listener, void* data);
    static void onChairSurfaceDestroy(wl_listener* listener, void* data);

    Chair* chairAt(Output* output);
    Chair& chairForOutput(Output& output);
    void rebuildChairs();
    void releaseChairs();
    void detachSurface(Chair& chair);
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
