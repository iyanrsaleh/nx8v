#include "bindings/window.h"
#include "bindings/window_gtk.h"

extern "C" int nx8v_gtk_run_window(
    const char* url, const char* title, int width, int height, uv_loop_t* loop,
    int x, int y, int min_w, int min_h, int show,
    int has_pos, int center, int frameless, int resizable,
    int single_instance, int devtools, const char* icon) {
  nx8v::bindings::Nx8vWindowOpts opts;
  opts.x = x;
  opts.y = y;
  opts.min_w = min_w;
  opts.min_h = min_h;
  opts.show = show;
  opts.has_pos = has_pos != 0;
  opts.center = center != 0;
  opts.frameless = frameless != 0;
  opts.resizable = resizable != 0;
  opts.single_instance = single_instance != 0;
  opts.devtools = devtools != 0;
  if (icon) opts.icon = icon;
  return nx8v::bindings::Nx8vGtkRunWindow(
      url ? url : "", title ? title : "", width, height, loop, opts);
}
