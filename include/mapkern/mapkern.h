#ifndef MAPKERN_H
#define MAPKERN_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
#if defined(MAPKERN_BUILDING)
#define MK_API __declspec(dllexport)
#else
#define MK_API __declspec(dllimport)
#endif
#else
#define MK_API __attribute__((visibility("default")))
#endif

typedef struct mk_map mk_map;
typedef struct mk_view mk_view;

enum {
  MK_FORMAT_DEFAULT = 0,
  MK_FORMAT_NONE = 1,
  MK_FORMAT_RGBA8 = 2,
  MK_FORMAT_BGRA8 = 3,
  MK_FORMAT_DEPTH = 4,
  MK_FORMAT_DEPTH_STENCIL = 5
};

typedef struct mk_options {
  const char* style;
  double lng, lat, zoom, bearing, pitch;
  double min_zoom, max_zoom, max_pitch;
  int globe;
  double pixel_ratio;
  const char* cache_directory;
  uint64_t disk_cache_bytes;
  int offline;
  int controls;
  int touch_controls;
  const char* view;
} mk_options;

typedef struct mk_graphics {
  const void* metal_device;
  const void* d3d11_device;
  const void* d3d11_device_context;
  int color_format;
  int depth_format;
  int sample_count;
} mk_graphics;

typedef struct mk_surface {
  int width, height, sample_count;
  int color_format, depth_format;
  const void* metal_current_drawable;
  const void* metal_depth_stencil_texture;
  const void* metal_msaa_color_texture;
  const void* d3d11_render_view;
  const void* d3d11_resolve_view;
  const void* d3d11_depth_stencil_view;
  uint32_t gl_framebuffer;
} mk_surface;

typedef struct mk_camera {
  double lng, lat, zoom, bearing, pitch;
} mk_camera;

typedef void (*mk_invalidate_fn)(void* user);
typedef void (*mk_event_fn)(void* user, const char* type, const char* message, const char* source_id);
typedef void (*mk_progress_fn)(void* user, size_t done, size_t total);

MK_API const char* mk_version(void);
MK_API void mk_options_default(mk_options* options);

MK_API mk_map* mk_map_create(const mk_options* options);
MK_API void mk_map_destroy(mk_map* map);
MK_API void mk_map_set_invalidate(mk_map* map, mk_invalidate_fn fn, void* user);
MK_API void mk_map_set_events(mk_map* map, mk_event_fn fn, void* user);

MK_API void mk_map_init_graphics(mk_map* map, const mk_graphics* graphics);
MK_API void mk_map_shutdown_graphics(mk_map* map);
MK_API int mk_map_render(mk_map* map, const mk_surface* surface);
MK_API int mk_map_needs_render(const mk_map* map);
MK_API void mk_map_resize(mk_map* map, double width, double height, double pixel_ratio);

MK_API void mk_map_set_style(mk_map* map, const char* url_or_json);
MK_API int mk_map_style_loaded(const mk_map* map);
MK_API void mk_map_add_source(mk_map* map, const char* id, const char* spec_json);
MK_API void mk_map_remove_source(mk_map* map, const char* id);
MK_API void mk_map_add_layer(mk_map* map, const char* spec_json, const char* before_id);
MK_API void mk_map_remove_layer(mk_map* map, const char* id);
MK_API void mk_map_set_paint_property(mk_map* map, const char* layer_id, const char* name, const char* value_json);
MK_API void mk_map_set_layout_property(mk_map* map, const char* layer_id, const char* name, const char* value_json);
MK_API void mk_map_set_filter(mk_map* map, const char* layer_id, const char* filter_json);
MK_API void mk_map_set_layer_visible(mk_map* map, const char* layer_id, int visible);
MK_API size_t mk_map_attribution(const mk_map* map, char* buffer, size_t size);

MK_API void mk_map_get_camera(const mk_map* map, mk_camera* camera);
MK_API void mk_map_jump_to(mk_map* map, const mk_camera* camera);
MK_API void mk_map_ease_to(mk_map* map, const mk_camera* camera, double duration_ms);
MK_API void mk_map_fly_to(mk_map* map, const mk_camera* camera, double duration_ms);
MK_API void mk_map_stop(mk_map* map);
MK_API void mk_map_set_projection(mk_map* map, int globe);
MK_API int mk_map_globe(const mk_map* map);
MK_API void mk_map_set_terrain(mk_map* map, const char* source_id, double exaggeration, double relief);
MK_API void mk_map_clear_terrain(mk_map* map);
MK_API int mk_map_elevation_at(const mk_map* map, double lng, double lat, double* meters);
MK_API int mk_map_unproject(const mk_map* map, double x, double y, double* lng, double* lat);
MK_API void mk_map_project(const mk_map* map, double lng, double lat, double* x, double* y);

MK_API void mk_map_pointer_down(mk_map* map, int id, double x, double y);
MK_API void mk_map_pointer_move(mk_map* map, int id, double x, double y);
MK_API void mk_map_pointer_up(mk_map* map, int id, double x, double y);
MK_API void mk_map_wheel(mk_map* map, double delta, double x, double y);
MK_API void mk_map_double_tap(mk_map* map, double x, double y);
MK_API void mk_map_zoom_by(mk_map* map, double delta, double x, double y, int animate);
MK_API void mk_map_rotate_by(mk_map* map, double degrees);
MK_API void mk_map_pitch_by(mk_map* map, double degrees);

MK_API void mk_map_set_offline(mk_map* map, int offline);
MK_API int mk_map_offline(const mk_map* map);
MK_API void mk_map_clear_disk_cache(mk_map* map);
MK_API void mk_map_download_region(mk_map* map, double west, double south, double east, double north, int min_zoom, int max_zoom,
                                   mk_progress_fn progress, void* user);
MK_API void mk_set_network_reachable(int reachable);

MK_API mk_view* mk_view_create_hwnd(void* hwnd, const mk_options* options);
MK_API mk_map* mk_view_map(mk_view* view);
MK_API void mk_view_resize(mk_view* view, int pixel_width, int pixel_height, double pixel_ratio);
MK_API int mk_view_render(mk_view* view);
MK_API void mk_view_destroy(mk_view* view);

#ifdef __cplusplus
}
#endif

#endif
