@module mk

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
in vec2 a_pos;
in uint a_feature;
out vec4 v_color;
void main() {
  vec4 color = paint_value(a_feature, u_slots.x, u_slots.y, u_color, u_paint);
  float opacity = paint_value(a_feature, u_slots.z, u_slots.w, vec4(u_misc.x), u_paint).x;
  v_color = vec4(color.rgb * color.a, color.a) * opacity;
  gl_Position = u_matrix * vec4(a_pos + u_misc.yz, 0.0, 1.0);
}
@end

@fs fill_fs
in vec4 v_color;
out vec4 frag_color;
void main() {
  frag_color = v_color;
}
@end

@program fill fill_vs fill_fs

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
in vec2 a_pos;
in vec2 a_extrude;
in vec4 a_side;
in float a_dist;
in uint a_feature;
out vec2 v_side;
out vec4 v_shape;
out float v_dist;
out vec4 v_color;
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
  float steps = a_side.z < 0.0 ? -a_side.z - 1.0 : a_side.z;
  float turn = a_side.w * (3.14159265 / 128.0);
  vec2 shift = vec2(cos(turn), sin(turn)) * (steps / 126.0 * 4.0);
  v_side = a_side.xy * (1.0 / 127.0) * 2.0 * outset;
  v_shape = vec4(a_side.z < 0.0 ? 1.0 : 0.0, half_width, gap, edge);
  v_dist = a_dist / u_misc.x;
  vec2 pos = a_pos + u_values2.yz + (a_extrude * (1.0 / 2048.0) * outset + shift * offset) * u_misc.x;
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
out vec4 frag_color;
void main() {
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
      float len = u_dash[i / 4][i % 4];
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
in vec2 a_pos;
in vec4 a_corner;
in uint a_feature;
out vec2 v_offset;
out vec4 v_shape;
out vec4 v_fill;
out vec4 v_stroke;
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
  vec2 corner = a_corner.xy / 127.0;
  v_offset = corner * extent;
  v_shape = vec4(radius, stroke_width, edge, 0.0);
  v_fill = vec4(color.rgb * color.a, color.a) * opacity;
  v_stroke = vec4(stroke.rgb * stroke.a, stroke.a) * stroke_opacity;
  vec2 base = a_pos + u_values2.yz;
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
out vec4 frag_color;
void main() {
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
in vec2 a_pos;
in vec4 a_normal;
in uint a_feature;
out vec4 v_color;
void main() {
  vec4 color = paint_value(a_feature, u_slots.x, u_slots.y, u_color, u_paint);
  float height = max(paint_value(a_feature, u_slots.z, u_slots.w, vec4(u_values.x), u_paint).x, 0.0);
  float base = clamp(paint_value(a_feature, u_slots2.x, u_slots2.y, vec4(u_values.y), u_paint).x, 0.0, height);
  vec3 normal = a_normal.xyz / 127.0;
  bool top = a_normal.w > 63.0;
  float lambert = max(dot(normal, u_light.xyz), 0.0);
  float shade = normal.z > 0.5 ? 1.0 : (0.66 + 0.3 * lambert) * (top ? 1.0 : 0.86);
  v_color = vec4(color.rgb * shade * color.a, color.a) * u_values.z;
  float z = (top ? height : base) * u_values.w;
  gl_Position = u_matrix * vec4(a_pos, z, 1.0);
}
@end

@fs extrusion_fs
in vec4 v_color;
out vec4 frag_color;
void main() {
  frag_color = v_color;
}
@end

@program extrusion extrusion_vs extrusion_fs

@vs raster_vs
layout(binding=0) uniform raster_vs_params {
  mat4 u_matrix;
  vec4 u_uv;
};
in vec2 a_pos;
out vec2 v_uv;
void main() {
  v_uv = u_uv.xy + a_pos * u_uv.zw;
  gl_Position = u_matrix * vec4(a_pos, 0.0, 1.0);
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
  float average = (rgb.r + rgb.g + rgb.b) / 3.0;
  rgb += (average - rgb) * u_adjust.z;
  rgb = (rgb - 0.5) * u_adjust.w + 0.5;
  rgb = mix(vec3(u_adjust.x), vec3(u_adjust.y), clamp(rgb, 0.0, 1.0));
  frag_color = vec4(rgb * c.a, c.a) * u_adjust2.x;
}
@end

@program raster raster_vs raster_fs
