@module mk

@block dem_chunk
float dem_elevation(vec2 uv, float size) {
  vec2 p = uv * size + 1.5;
  vec2 f = floor(p);
  vec2 t = p - f;
  ivec2 i = ivec2(f);
  float a = texelFetch(sampler2D(dem_tex, dem_smp), i, 0).r;
  float b = texelFetch(sampler2D(dem_tex, dem_smp), i + ivec2(1, 0), 0).r;
  float c = texelFetch(sampler2D(dem_tex, dem_smp), i + ivec2(0, 1), 0).r;
  float d = texelFetch(sampler2D(dem_tex, dem_smp), i + ivec2(1, 1), 0).r;
  return mix(mix(a, b, t.x), mix(c, d, t.x), t.y);
}
@end

@block paint
layout(binding=0) uniform texture2D paint_tex;
layout(binding=0) uniform sampler paint_smp;
vec4 paint_value(uint feature, float slot, float comp, vec4 fallback, vec4 info) {
  if (slot < 0.0) return fallback;
  int width = int(info.x);
  int index = int(feature) * int(info.y) + int(slot);
  vec4 a = texelFetch(sampler2D(paint_tex, paint_smp), ivec2(index % width, index / width), 0);
  if (comp < 0.5) return a;
  int next = index + 1;
  vec4 b = texelFetch(sampler2D(paint_tex, paint_smp), ivec2(next % width, next / width), 0);
  return mix(a, b, info.z);
}
@end

@vs solid_vs
layout(binding=0) uniform solid_params {
  mat4 u_matrix;
  vec4 u_color;
};
in vec2 a_pos;
out vec4 v_color;
void main() {
  v_color = u_color;
  gl_Position = u_matrix * vec4(a_pos, 0.0, 1.0);
}
@end

@fs solid_fs
in vec4 v_color;
out vec4 frag_color;
void main() {
  frag_color = v_color;
}
@end

@program solid solid_vs solid_fs

@vs fill_vs
layout(binding=0) uniform fill_params {
  mat4 u_matrix;
  vec4 u_color;
  vec4 u_slots;
  vec4 u_misc;
  vec4 u_paint;
};
@include_block paint
in ivec2 a_pos;
in uint a_feature;
out vec4 v_color;
out vec2 v_pos;
void main() {
  vec4 color = paint_value(a_feature, u_slots.x, u_slots.y, u_color, u_paint);
  float opacity = paint_value(a_feature, u_slots.z, u_slots.w, vec4(u_misc.x), u_paint).x;
  v_color = vec4(color.rgb * color.a, color.a) * opacity;
  v_pos = vec2(a_pos);
  gl_Position = u_matrix * vec4(vec2(a_pos) + u_misc.yz, 0.0, 1.0);
}
@end

@fs fill_fs
in vec4 v_color;
in vec2 v_pos;
out vec4 frag_color;
void main() {
  vec2 unused_pos = v_pos;
  frag_color = v_color;
}
@end

@program fill fill_vs fill_fs

@fs fill_clip_fs
layout(binding=2) uniform clip_params {
  vec4 u_clip;
};
in vec4 v_color;
in vec2 v_pos;
out vec4 frag_color;
void main() {
  if (v_pos.x < u_clip.x || v_pos.y < u_clip.y || v_pos.x >= u_clip.z || v_pos.y >= u_clip.w) discard;
  frag_color = v_color;
}
@end

@program fill_clip fill_vs fill_clip_fs

@vs line_vs
layout(binding=0) uniform line_params {
  mat4 u_matrix;
  vec4 u_color;
  vec4 u_values;
  vec4 u_values2;
  vec4 u_slots;
  vec4 u_slots2;
  vec4 u_slots3;
  vec4 u_misc;
  vec4 u_paint;
};
@include_block paint
in ivec2 a_pos;
in ivec2 a_extrude;
in ivec4 a_side;
in float a_dist;
in uint a_feature;
out vec2 v_side;
out vec4 v_shape;
out float v_dist;
out vec4 v_color;
out vec2 v_pos;
void main() {
  vec4 color = paint_value(a_feature, u_slots.x, u_slots.y, u_color, u_paint);
  float opacity = paint_value(a_feature, u_slots.z, u_slots.w, vec4(u_values.x), u_paint).x;
  float width = max(paint_value(a_feature, u_slots2.x, u_slots2.y, vec4(u_values.y), u_paint).x, u_misc.w);
  float blur = max(paint_value(a_feature, u_slots2.z, u_slots2.w, vec4(u_values.z), u_paint).x, 0.0);
  float offset = paint_value(a_feature, u_slots3.x, u_slots3.y, vec4(u_values.w), u_paint).x;
  float gap = max(paint_value(a_feature, u_slots3.z, u_slots3.w, vec4(u_values2.x), u_paint).x, 0.0) * 0.5;
  float aa = 1.0 / u_misc.z;
  float half_width = width * 0.5;
  float coverage = 1.0;
  if (width < aa) {
    coverage = width / aa;
    half_width = aa * 0.5;
  }
  float edge = blur + aa;
  float outset = (gap > 0.0 ? gap + half_width * 2.0 : half_width) + edge * 0.5;
  v_color = vec4(color.rgb * color.a, color.a) * opacity * coverage;
  float steps = vec4(a_side).z < 0.0 ? -vec4(a_side).z - 1.0 : vec4(a_side).z;
  float turn = vec4(a_side).w * (3.14159265 / 128.0);
  vec2 shift = vec2(cos(turn), sin(turn)) * (steps / 126.0 * 4.0);
  v_side = vec4(a_side).xy * (1.0 / 127.0) * 2.0 * outset;
  v_shape = vec4(vec4(a_side).z < 0.0 ? 1.0 : 0.0, half_width, gap, edge);
  v_dist = a_dist / u_misc.x;
  vec2 pos = vec2(a_pos) + u_values2.yz + (vec2(a_extrude) * (1.0 / 2048.0) * outset + shift * offset) * u_misc.x;
  v_pos = pos;
  gl_Position = u_matrix * vec4(pos, 0.0, 1.0);
}
@end

@fs line_fs
layout(binding=1) uniform line_dash {
  vec4 u_dash[2];
  vec4 u_dash_info;
};
in vec2 v_side;
in vec4 v_shape;
in float v_dist;
in vec4 v_color;
in vec2 v_pos;
out vec4 frag_color;
float dash_at(int i) {
  vec4 g = i < 4 ? u_dash[0] : u_dash[1];
  int k = i - (i / 4) * 4;
  return k == 0 ? g.x : k == 1 ? g.y : k == 2 ? g.z : g.w;
}
void main() {
  vec2 unused_pos = v_pos;
  float half_width = v_shape.y;
  float gap = v_shape.z;
  float edge = v_shape.w;
  float d = v_shape.x > 0.5 ? max(abs(v_side.x), abs(v_side.y)) : length(v_side);
  float band = gap > 0.0 ? abs(d - gap - half_width) : d;
  float alpha = clamp((half_width + edge * 0.5 - band) / edge, 0.0, 1.0);
  int count = int(u_dash_info.x);
  if (count > 0) {
    float unit = max(half_width * 2.0, 1.0);
    float t = mod(v_dist / unit, u_dash_info.y);
    float start = 0.0;
    float on = 0.0;
    for (int i = 0; i < 8; i++) {
      if (i >= count) break;
      float len = dash_at(i);
      if (t >= start && t < start + len) {
        float inside = min(t - start, start + len - t) * unit;
        on = (i % 2 == 0) ? clamp(inside + 0.5, 0.0, 1.0) : 1.0 - clamp(inside + 0.5, 0.0, 1.0);
        break;
      }
      start += len;
    }
    alpha *= on;
  }
  if (alpha <= 0.0) discard;
  frag_color = v_color * alpha;
}
@end

@program line line_vs line_fs

@fs line_clip_fs
layout(binding=1) uniform line_dash {
  vec4 u_dash[2];
  vec4 u_dash_info;
};
in vec2 v_side;
in vec4 v_shape;
in float v_dist;
in vec4 v_color;
in vec2 v_pos;
layout(binding=2) uniform clip_params {
  vec4 u_clip;
};
out vec4 frag_color;
float dash_at(int i) {
  vec4 g = i < 4 ? u_dash[0] : u_dash[1];
  int k = i - (i / 4) * 4;
  return k == 0 ? g.x : k == 1 ? g.y : k == 2 ? g.z : g.w;
}
void main() {
  if (v_pos.x < u_clip.x || v_pos.y < u_clip.y || v_pos.x >= u_clip.z || v_pos.y >= u_clip.w) discard;
  float half_width = v_shape.y;
  float gap = v_shape.z;
  float edge = v_shape.w;
  float d = v_shape.x > 0.5 ? max(abs(v_side.x), abs(v_side.y)) : length(v_side);
  float band = gap > 0.0 ? abs(d - gap - half_width) : d;
  float alpha = clamp((half_width + edge * 0.5 - band) / edge, 0.0, 1.0);
  int count = int(u_dash_info.x);
  if (count > 0) {
    float unit = max(half_width * 2.0, 1.0);
    float t = mod(v_dist / unit, u_dash_info.y);
    float start = 0.0;
    float on = 0.0;
    for (int i = 0; i < 8; i++) {
      if (i >= count) break;
      float len = dash_at(i);
      if (t >= start && t < start + len) {
        float inside = min(t - start, start + len - t) * unit;
        on = (i % 2 == 0) ? clamp(inside + 0.5, 0.0, 1.0) : 1.0 - clamp(inside + 0.5, 0.0, 1.0);
        break;
      }
      start += len;
    }
    alpha *= on;
  }
  if (alpha <= 0.0) discard;
  frag_color = v_color * alpha;
}
@end

@program line_clip line_vs line_clip_fs

@vs circle_vs
layout(binding=0) uniform circle_params {
  mat4 u_matrix;
  vec4 u_color;
  vec4 u_stroke_color;
  vec4 u_values;
  vec4 u_values2;
  vec4 u_slots;
  vec4 u_slots2;
  vec4 u_slots3;
  vec4 u_slots4;
  vec4 u_misc;
  vec4 u_misc2;
  vec4 u_paint;
};
@include_block paint
in ivec2 a_pos;
in ivec4 a_corner;
in uint a_feature;
out vec2 v_offset;
out vec4 v_shape;
out vec4 v_fill;
out vec4 v_stroke;
flat out vec2 v_pos;
void main() {
  vec4 color = paint_value(a_feature, u_slots.x, u_slots.y, u_color, u_paint);
  vec4 stroke = paint_value(a_feature, u_slots.z, u_slots.w, u_stroke_color, u_paint);
  float opacity = paint_value(a_feature, u_slots2.x, u_slots2.y, vec4(u_values.x), u_paint).x;
  float radius = max(paint_value(a_feature, u_slots2.z, u_slots2.w, vec4(u_values.y), u_paint).x, 0.0);
  float stroke_width = max(paint_value(a_feature, u_slots3.x, u_slots3.y, vec4(u_values.z), u_paint).x, 0.0);
  float blur = max(paint_value(a_feature, u_slots3.z, u_slots3.w, vec4(u_values.w), u_paint).x, 0.0);
  float stroke_opacity = paint_value(a_feature, u_slots4.x, u_slots4.y, vec4(u_values2.x), u_paint).x;
  float edge = blur + 1.0 / u_misc.z;
  float extent = radius + stroke_width + edge;
  vec2 corner = vec4(a_corner).xy / 127.0;
  v_offset = corner * extent;
  v_shape = vec4(radius, stroke_width, edge, 0.0);
  v_fill = vec4(color.rgb * color.a, color.a) * opacity;
  v_stroke = vec4(stroke.rgb * stroke.a, stroke.a) * stroke_opacity;
  vec2 base = vec2(a_pos) + u_values2.yz;
  v_pos = vec2(a_pos);
  if (u_misc2.x > 0.5) {
    gl_Position = u_matrix * vec4(base + corner * extent * u_misc.w, 0.0, 1.0);
  } else {
    vec4 center = u_matrix * vec4(base, 0.0, 1.0);
    float w = u_misc2.y > 0.5 ? u_misc2.z : center.w;
    gl_Position = center + vec4(corner * extent * 2.0 / u_misc.xy * w, 0.0, 0.0);
  }
}
@end

@fs circle_fs
in vec2 v_offset;
in vec4 v_shape;
in vec4 v_fill;
in vec4 v_stroke;
flat in vec2 v_pos;
out vec4 frag_color;
void main() {
  vec2 unused_pos = v_pos;
  float radius = v_shape.x;
  float stroke = v_shape.y;
  float edge = v_shape.z;
  float d = length(v_offset);
  float alpha = clamp((radius + stroke + edge * 0.5 - d) / edge, 0.0, 1.0);
  if (alpha <= 0.0) discard;
  float t = stroke > 0.0 ? clamp((d - radius + edge * 0.5) / edge, 0.0, 1.0) : 0.0;
  frag_color = mix(v_fill, v_stroke, t) * alpha;
}
@end

@program circle circle_vs circle_fs

@fs circle_clip_fs
in vec2 v_offset;
in vec4 v_shape;
in vec4 v_fill;
in vec4 v_stroke;
flat in vec2 v_pos;
layout(binding=2) uniform clip_params {
  vec4 u_clip;
};
out vec4 frag_color;
void main() {
  if (v_pos.x < u_clip.x || v_pos.y < u_clip.y || v_pos.x >= u_clip.z || v_pos.y >= u_clip.w) discard;
  float radius = v_shape.x;
  float stroke = v_shape.y;
  float edge = v_shape.z;
  float d = length(v_offset);
  float alpha = clamp((radius + stroke + edge * 0.5 - d) / edge, 0.0, 1.0);
  if (alpha <= 0.0) discard;
  float t = stroke > 0.0 ? clamp((d - radius + edge * 0.5) / edge, 0.0, 1.0) : 0.0;
  frag_color = mix(v_fill, v_stroke, t) * alpha;
}
@end

@program circle_clip circle_vs circle_clip_fs

@vs extrusion_vs
layout(binding=0) uniform extrusion_params {
  mat4 u_matrix;
  vec4 u_color;
  vec4 u_values;
  vec4 u_slots;
  vec4 u_slots2;
  vec4 u_light;
  vec4 u_paint;
};
@include_block paint
layout(binding=3) uniform extrusion_dem {
  vec4 u_dem_rect;
  vec4 u_dem_info;
  vec4 u_rows;
};
layout(binding=2) uniform texture2D dem_tex;
layout(binding=2) uniform sampler dem_smp;
@include_block dem_chunk
in ivec2 a_pos;
in ivec4 a_normal;
in uint a_feature;
out vec4 v_color;
out vec2 v_pos;
void main() {
  vec4 color = paint_value(a_feature, u_slots.x, u_slots.y, u_color, u_paint);
  float height = max(paint_value(a_feature, u_slots.z, u_slots.w, vec4(u_values.x), u_paint).x, 0.0);
  float base = clamp(paint_value(a_feature, u_slots2.x, u_slots2.y, vec4(u_values.y), u_paint).x, 0.0, height);
  vec3 normal = vec4(a_normal).xyz / 127.0;
  bool top = vec4(a_normal).w > 63.0;
  float lambert = max(dot(normal, u_light.xyz), 0.0);
  float shade = normal.z > 0.5 ? 1.0 : (0.66 + 0.3 * lambert) * (top ? 1.0 : 0.86);
  v_color = vec4(color.rgb * shade * color.a, color.a) * u_values.z;
  float z = (top ? height : base) * u_values.w;
  if (u_dem_info.y > 0.5) {
    vec2 g = clamp(vec2(a_pos) / u_dem_info.w, 0.0, 1.0);
    float y = mix(u_rows.x, u_rows.y, g.y);
    float ppm = u_rows.z * cosh(3.14159265 * (1.0 - 2.0 * y)) / 40075016.686;
    z += dem_elevation(u_dem_rect.xy + g * u_dem_rect.zw, u_dem_info.x) * u_dem_info.z * ppm;
  }
  v_pos = vec2(a_pos);
  gl_Position = u_matrix * vec4(vec2(a_pos), z, 1.0);
}
@end

@fs extrusion_fs
in vec4 v_color;
in vec2 v_pos;
out vec4 frag_color;
void main() {
  vec2 unused_pos = v_pos;
  frag_color = v_color;
}
@end

@program extrusion extrusion_vs extrusion_fs

@fs extrusion_clip_fs
layout(binding=2) uniform clip_params {
  vec4 u_clip;
};
in vec4 v_color;
in vec2 v_pos;
out vec4 frag_color;
void main() {
  if (v_pos.x < u_clip.x || v_pos.y < u_clip.y || v_pos.x >= u_clip.z || v_pos.y >= u_clip.w) discard;
  frag_color = v_color;
}
@end

@program extrusion_clip extrusion_vs extrusion_clip_fs

@vs raster_vs
layout(binding=0) uniform raster_vs_params {
  mat4 u_matrix;
  vec4 u_clip;
};
in vec2 a_pos;
out vec2 v_uv;
void main() {
  vec2 p = mix(u_clip.xy, u_clip.zw, a_pos);
  v_uv = p;
  gl_Position = u_matrix * vec4(p, 0.0, 1.0);
}
@end

@fs raster_fs
layout(binding=1) uniform raster_fs_params {
  vec4 u_adjust;
  vec4 u_adjust2;
  vec4 u_hue0;
  vec4 u_hue1;
  vec4 u_hue2;
};
layout(binding=1) uniform texture2D raster_tex;
layout(binding=1) uniform sampler raster_smp;
in vec2 v_uv;
out vec4 frag_color;
void main() {
  vec4 c = texture(sampler2D(raster_tex, raster_smp), v_uv);
  vec3 rgb = c.a > 0.0 ? c.rgb / c.a : vec3(0.0);
  rgb = vec3(dot(u_hue0.xyz, rgb), dot(u_hue1.xyz, rgb), dot(u_hue2.xyz, rgb));
  float lum = dot(rgb, vec3(0.2126, 0.7152, 0.0722));
  rgb = mix(vec3(lum), rgb, 1.0 + u_adjust.z);
  rgb = (rgb - 0.5) * u_adjust.w + 0.5;
  rgb = mix(vec3(u_adjust.x), vec3(u_adjust.y), clamp(rgb, 0.0, 1.0));
  frag_color = vec4(rgb * c.a, c.a) * u_adjust2.x;
}
@end

@program raster raster_vs raster_fs

@block globe_chunk
vec3 surface_point(vec2 rel, float meters, float pole, vec4 globe, vec4 center) {
  float y = center.x + rel.y;
  float m = 3.14159265 * (1.0 - 2.0 * y);
  float cosh_m = cosh(m);
  vec3 flat_point = vec3(rel * globe.z, meters * globe.z * cosh_m / 40075016.686);
  float sin_lat = tanh(m);
  float cos_lat = 1.0 / cosh_m;
  if (pole != 0.0) {
    sin_lat = pole;
    cos_lat = 0.0;
  }
  float lng = 6.28318531 * (rel.x - floor(rel.x + 0.5));
  float cos_lng = cos(lng);
  float east = cos_lat * sin(lng);
  float north = sin_lat * center.z - cos_lat * center.y * cos_lng;
  float up = sin_lat * center.y + cos_lat * center.z * cos_lng;
  float r = globe.y + meters * globe.z * globe.w / 40075016.686;
  vec3 sphere = vec3(r * east, -r * north, r * up - globe.y);
  return mix(flat_point, sphere, globe.x);
}
@end


@block relief_chunk
vec3 relief(vec3 color, vec2 uv, vec4 sun, vec4 grad, vec4 rows, vec4 rect) {
  if (sun.w <= 0.0 || rows.w < 0.5) return color;
  vec2 dem = rect.xy + uv * rect.zw;
  vec2 g = texture(sampler2D(gradient_tex, gradient_smp), dem * grad.x + grad.y).rg;
  float y = mix(rows.x, rows.y, uv.y);
  float k = cosh(3.14159265 * (1.0 - 2.0 * y));
  vec3 n = normalize(vec3(-g * (grad.z * k * rows.z), 1.0));
  float d = dot(n, sun.xyz) - sun.z;
  return color * clamp(1.0 + d * sun.w, 0.45, 1.3);
}
@end

@vs terrain_vs
layout(binding=0) uniform terrain_vs_params {
  mat4 u_matrix;
  vec4 u_dem_rect;
  vec4 u_dem_info;
  vec4 u_rows;
};
layout(binding=2) uniform texture2D dem_tex;
layout(binding=2) uniform sampler dem_smp;
@image_sample_type dem_tex unfilterable_float
@sampler_type dem_smp nonfiltering
@include_block dem_chunk
in ivec4 a_pos;
out vec2 v_uv;
void main() {
  vec4 pos = vec4(a_pos);
  vec2 uv = pos.xy / 64.0;
  v_uv = uv;
  float z = 0.0;
  if (u_dem_info.y > 0.5) {
    float y = mix(u_rows.x, u_rows.y, uv.y);
    float ppm = u_rows.z * cosh(3.14159265 * (1.0 - 2.0 * y)) / 40075016.686;
    z = dem_elevation(u_dem_rect.xy + uv * u_dem_rect.zw, u_dem_info.x) * u_dem_info.z * ppm;
  }
  z -= pos.z * u_dem_info.w;
  gl_Position = u_matrix * vec4(uv, z, 1.0);
}
@end

@fs terrain_fs
layout(binding=1) uniform terrain_fs_params {
  vec4 u_drape_rect;
  vec4 u_sun;
  vec4 u_grad;
  vec4 u_rows2;
  vec4 u_dem_rect2;
};
layout(binding=0) uniform texture2D drape_tex;
layout(binding=0) uniform sampler drape_smp;
layout(binding=3) uniform texture2D gradient_tex;
layout(binding=3) uniform sampler gradient_smp;
@include_block relief_chunk
in vec2 v_uv;
out vec4 frag_color;
void main() {
  vec2 uv = u_drape_rect.xy + v_uv * u_drape_rect.zw;
  if (u_grad.w > 0.5) uv.y = 1.0 - uv.y;
  vec4 color = texture(sampler2D(drape_tex, drape_smp), uv);
  color.rgb = relief(color.rgb, v_uv, u_sun, u_grad, u_rows2, u_dem_rect2);
  frag_color = color;
}
@end

@program terrain terrain_vs terrain_fs

@fs hillshade_fs
layout(binding=1) uniform hillshade_params {
  vec4 u_texel_rows;
  vec4 u_light;
  vec4 u_values;
  vec4 u_values2;
  vec4 u_shadow;
  vec4 u_highlight;
  vec4 u_accent;
};
layout(binding=3) uniform texture2D gradient_tex;
layout(binding=3) uniform sampler gradient_smp;
layout(binding=4) uniform texture2D mask_tex;
layout(binding=4) uniform sampler mask_smp;
in vec2 v_uv;
out vec4 frag_color;
void main() {
  const float PI = 3.14159265;
  vec2 g = texture(sampler2D(gradient_tex, gradient_smp), v_uv * u_texel_rows.x + u_texel_rows.y).rg;
  float k = cosh(PI * (1.0 - 2.0 * mix(u_texel_rows.z, u_texel_rows.w, v_uv.y)));
  float mask = texture(sampler2D(mask_tex, mask_smp), v_uv).r;
  if (u_values.w > 0.5) {
    vec2 deriv = clamp(g * u_values2.x, -4.0, 4.0) * k;
    float slope = atan(0.625 * length(deriv));
    float aspect = deriv.x != 0.0 ? atan(deriv.y, -deriv.x) : PI / 2.0 * (deriv.y > 0.0 ? 1.0 : -1.0);
    float intensity = u_values2.z;
    float base = 1.875 - intensity * 1.75;
    float steep = intensity != 0.5 ? (pow(base, slope) - 1.0) / (pow(base, PI / 2.0) - 1.0) * (PI / 2.0) : slope;
    float strength = clamp(intensity * 2.0, 0.0, 1.0);
    float side = abs(mod((aspect + u_values2.y + PI) / PI + 0.5, 2.0) - 1.0);
    vec4 lit = mix(u_shadow, u_highlight, side) * sin(steep) * strength;
    vec4 edge = (1.0 - cos(steep)) * u_accent * strength;
    frag_color = (edge * (1.0 - lit.a) + lit) * (mask * u_values.z);
    return;
  }
  vec3 n = normalize(vec3(-g * (u_values.x * k), 1.0));
  float d = dot(n, u_light.xyz) - u_light.w;
  vec4 shade = d < 0.0 ? u_shadow * min(1.0, -d * u_values.y) : u_highlight * min(1.0, d * u_values.y);
  vec4 accent = u_accent * min(1.0, (1.0 - n.z) * u_values.y);
  vec4 color = shade + accent * (1.0 - shade.a);
  frag_color = color * (mask * u_values.z);
}
@end

@program hillshade raster_vs hillshade_fs

@vs globe_vs
layout(binding=0) uniform globe_vs_params {
  mat4 u_matrix;
  vec4 u_globe;
  vec4 u_center;
  vec4 u_tile;
  vec4 u_tile_world;
  vec4 u_dem_rect;
  vec4 u_dem_info;
};
@include_block globe_chunk
layout(binding=2) uniform texture2D dem_tex;
layout(binding=2) uniform sampler dem_smp;
@include_block dem_chunk
in ivec4 a_pos;
out vec2 v_uv;
out float v_pole;
out vec2 v_world;
out vec3 v_local;
void main() {
  vec4 pos = vec4(a_pos);
  vec2 uv = pos.xy / 64.0;
  v_uv = uv;
  v_pole = pos.w;
  v_world = u_tile_world.xy + uv * u_tile_world.z;
  float meters = u_dem_info.y > 0.5 ? dem_elevation(u_dem_rect.xy + uv * u_dem_rect.zw, u_dem_info.x) * u_dem_info.z : 0.0;
  meters -= pos.z * u_dem_info.w;
  float pole = pos.w > 0.5 ? (uv.y < 0.5 ? 1.0 : -1.0) : 0.0;
  v_local = surface_point(u_tile.xy + uv * u_tile.z, meters, pole, u_globe, u_center);
  gl_Position = u_matrix * vec4(v_local, 1.0);
}
@end

@fs globe_fs
layout(binding=1) uniform globe_fs_params {
  vec4 u_drape_rect;
  vec4 u_base;
  vec4 u_atmo;
  vec4 u_misc;
  vec4 u_sun;
  vec4 u_grad;
  vec4 u_rows2;
  vec4 u_dem_rect2;
};
layout(binding=0) uniform texture2D drape_tex;
layout(binding=0) uniform sampler drape_smp;
layout(binding=1) uniform texture2D pole_tex;
layout(binding=1) uniform sampler pole_smp;
layout(binding=3) uniform texture2D gradient_tex;
layout(binding=3) uniform sampler gradient_smp;
@include_block relief_chunk
in vec2 v_uv;
in float v_pole;
in vec2 v_world;
in vec3 v_local;
out vec4 frag_color;
void main() {
  vec2 uv = u_drape_rect.xy + v_uv * u_drape_rect.zw;
  if (u_misc.z > 0.5) uv.y = 1.0 - uv.y;
  vec4 color;
  if (u_misc.x > 0.5) {
    vec4 edge = texture(sampler2D(drape_tex, drape_smp), uv);
    float row = u_misc.z > 0.5 ? 1.0 - uv.y : uv.y;
    vec4 pole = texture(sampler2D(pole_tex, pole_smp), vec2(row < 0.5 ? 0.25 : 0.75, 0.5));
    color = mix(edge, pole, smoothstep(0.08, 0.7, v_pole));
  } else {
    color = texture(sampler2D(drape_tex, drape_smp), uv);
  }
  color += u_base * (1.0 - color.a);
  if (u_misc.x < 0.5) color.rgb = relief(color.rgb, v_uv, u_sun, u_grad, u_rows2, u_dem_rect2);
  if (u_atmo.w > 0.0) {
    vec3 normal = normalize(v_local + vec3(0.0, 0.0, u_misc.y));
    float facing = clamp(dot(normal, normalize(u_atmo.xyz - v_local)), 0.0, 1.0);
    float haze = (pow(1.0 - facing, 2.6) * 0.8 + 0.04) * u_atmo.w;
    color.rgb = mix(color.rgb, vec3(0.56, 0.74, 1.0) * color.a, haze);
  }
  frag_color = color;
}
@end

@program globe globe_vs globe_fs

@vs atmosphere_vs
in vec2 a_pos;
out vec2 v_ndc;
void main() {
  v_ndc = a_pos * 2.0 - 1.0;
  gl_Position = vec4(v_ndc, 0.0, 1.0);
}
@end

@fs atmosphere_fs
layout(binding=1) uniform atmosphere_params {
  mat4 u_inverse;
  vec4 u_camera;
  vec4 u_space;
  vec4 u_sky;
  vec4 u_sun;
  vec4 u_misc;
};
in vec2 v_ndc;
out vec4 frag_color;
void main() {
  vec4 far = u_inverse * vec4(v_ndc, 1.0, 1.0);
  vec3 dir = normalize(far.xyz / far.w - u_camera.xyz);
  float radius = u_camera.w;
  vec3 oc = u_camera.xyz + vec3(0.0, 0.0, radius);
  float b = dot(oc, dir);
  vec3 closest = oc - dir * min(b, 0.0);
  float r = length(closest);
  float scale = 0.011 * radius;
  float height = max(r - radius, 0.0);
  float depth = b < 0.0 ? exp(-height / scale) * sqrt(r / scale) * 0.09 : 0.0;
  float lit = 1.0;
  vec3 tint = vec3(1.0);
  if (u_sun.w > 0.5) {
    float facing = dot(closest / r, u_sun.xyz);
    lit = smoothstep(-0.32, 0.12, facing);
    tint = mix(vec3(1.0, 0.62, 0.38), vec3(1.0), smoothstep(-0.12, 0.3, facing));
  }
  vec3 scatter = (1.0 - exp(-depth * vec3(0.38, 0.62, 1.0) * 2.4)) * vec3(0.62, 0.8, 1.0) * tint * lit;
  float up = clamp(dir.z * 0.5 + 0.5, 0.0, 1.0);
  vec4 space = mix(u_space, u_sky, u_misc.x * smoothstep(0.35, 0.75, up));
  frag_color = vec4(min(space.rgb + scatter * (1.0 - 0.6 * u_misc.x), vec3(1.0)), 1.0);
}
@end

@program atmosphere atmosphere_vs atmosphere_fs

@vs pole_vs
layout(binding=0) uniform pole_vs_params {
  vec4 u_target;
};
in vec2 a_pos;
void main() {
  vec2 p = vec2(u_target.x, 0.0) + (a_pos - 0.5) * vec2(0.9, 1.9);
  gl_Position = vec4(p, 0.0, 1.0);
}
@end

@fs pole_fs
layout(binding=1) uniform pole_fs_params {
  vec4 u_rect;
  vec4 u_info;
};
layout(binding=0) uniform texture2D drape_tex;
layout(binding=0) uniform sampler drape_smp;
out vec4 frag_color;
void main() {
  vec4 sum = vec4(0.0);
  for (int i = 0; i < 8; i++) {
    vec2 uv = u_rect.xy + vec2((float(i) + 0.5) / 8.0, u_info.x) * u_rect.zw;
    if (u_info.z > 0.5) uv.y = 1.0 - uv.y;
    sum += texture(sampler2D(drape_tex, drape_smp), uv);
  }
  frag_color = sum * (u_info.y / 8.0);
}
@end

@program pole pole_vs pole_fs

@vs label_vs
layout(binding=0) uniform label_vs_params {
  vec4 u_view;
};
in vec2 a_pos;
in vec2 a_uv;
in vec4 a_color;
in vec4 a_halo;
in vec4 a_params;
out vec2 v_uv;
out vec4 v_color;
out vec4 v_halo;
out vec4 v_params;
void main() {
  v_uv = a_uv / u_view.zw;
  v_color = a_color;
  v_halo = a_halo;
  v_params = a_params;
  gl_Position = vec4(a_pos.x / u_view.x * 2.0 - 1.0, 1.0 - a_pos.y / u_view.y * 2.0, 0.0, 1.0);
}
@end

@fs text_fs
layout(binding=1) uniform label_fs_params {
  vec4 u_info;
};
layout(binding=0) uniform texture2D atlas_tex;
layout(binding=0) uniform sampler atlas_smp;
in vec2 v_uv;
in vec4 v_color;
in vec4 v_halo;
in vec4 v_params;
out vec4 frag_color;
void main() {
  float d = texture(sampler2D(atlas_tex, atlas_smp), v_uv).r;
  float opacity = v_params.x;
  float scale = max(v_params.y, 0.05);
  float radius = u_info.z;
  float gamma = 0.8 / (radius * scale * u_info.y);
  float edge = 0.75;
  if (u_info.x < 0.5) {
    float halo = v_params.z;
    if (halo <= 0.0 || v_halo.a <= 0.0) discard;
    float halo_edge = max(0.05, edge - halo / (radius * scale));
    float blur = v_params.w / (radius * scale);
    float a = smoothstep(halo_edge - gamma - blur, halo_edge + gamma, d);
    if (a <= 0.0) discard;
    frag_color = v_halo * a * opacity;
  } else {
    float a = smoothstep(edge - gamma, edge + gamma, d);
    if (a <= 0.0) discard;
    frag_color = v_color * a * opacity;
  }
}
@end

@program text label_vs text_fs

@fs icon_fs
layout(binding=1) uniform label_fs_params {
  vec4 u_info;
};
layout(binding=0) uniform texture2D atlas_tex;
layout(binding=0) uniform sampler atlas_smp;
in vec2 v_uv;
in vec4 v_color;
in vec4 v_halo;
in vec4 v_params;
out vec4 frag_color;
void main() {
  vec4 texel = texture(sampler2D(atlas_tex, atlas_smp), v_uv);
  float opacity = v_params.x;
  if (v_params.w < 0.5) {
    if (u_info.x < 0.5) discard;
    frag_color = texel * opacity;
    return;
  }
  float d = texel.a;
  float scale = max(v_params.y, 0.05);
  float radius = u_info.z;
  float gamma = 0.8 / (radius * scale * u_info.y);
  float edge = 0.75;
  if (u_info.x < 0.5) {
    float halo = v_params.z;
    if (halo <= 0.0 || v_halo.a <= 0.0) discard;
    float halo_edge = max(0.05, edge - halo / (radius * scale));
    float a = smoothstep(halo_edge - gamma, halo_edge + gamma, d);
    if (a <= 0.0) discard;
    frag_color = v_halo * a * opacity;
  } else {
    float a = smoothstep(edge - gamma, edge + gamma, d);
    if (a <= 0.0) discard;
    frag_color = v_color * a * opacity;
  }
}
@end

@program icon label_vs icon_fs
