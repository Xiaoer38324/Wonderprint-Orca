#version 140

// ============================================================================
// Snapmaker render pipeline - toolpath shading
// ----------------------------------------------------------------------------
// Ported from Snapmaker/OrcaSlicer resources/shaders/140/gpu_path.fs
// ("Render engine build - v2.4.1").
//
// The CPU stores an explicit per-move colour source in the attribute table,
// so this shader keeps the exact colour semantics of the libvgcode viewer
// that still owns the legend in Wonderprint-Orca:
//   source 0 -> value gradient (height / width / speed / flow / layer time...)
//   source 1 -> extrusion-role palette
//   source 2 -> tool palette
//   source 3 -> color-print palette
//   source 4 -> option palette (travel / wipe / retract / seam ...)
//   source 5 -> neutral gray (dummy / pause-custom layer)
// ============================================================================

#define INTENSITY_CORRECTION 0.6

// normalized values for (-0.6/1.31, 0.6/1.31, 1./1.31)
const vec3 LIGHT_TOP_DIR = vec3(-0.4574957, 0.4574957, 0.7624929);
#define LIGHT_TOP_DIFFUSE    (0.8 * INTENSITY_CORRECTION)
#define LIGHT_TOP_SPECULAR   (0.125 * INTENSITY_CORRECTION)
#define LIGHT_TOP_SHININESS  20.0

// normalized values for (1./1.43, 0.2/1.43, 1./1.43)
const vec3 LIGHT_FRONT_DIR = vec3(0.6985074, 0.1397015, 0.6985074);
#define LIGHT_FRONT_DIFFUSE  (0.3 * INTENSITY_CORRECTION)

#define INTENSITY_AMBIENT    0.3

uniform mat3 normal_matrix;
uniform float emission_factor;

// x = unused, y = isRangeValid, z = topLayerOnly, w = unused
uniform vec4 u_view_config;
// min/max of the active value range (pre-logged for logarithmic ranges)
uniform vec2 u_range_bounds;
// nonzero for logarithmic ranges (libvgcode LayerTimeLogarithmic)
uniform float u_range_is_log;
// 1.0 when this layer is the top layer of the visible window
uniform float u_is_top_layer;

// combined nearest-filtered palette: roles | tools | color-print | options
uniform sampler2D s_color_palette;
uniform vec4 u_palette_offsets; // role, tool, color-print, option offsets
uniform vec4 u_palette_counts;  // number of colors of each palette
uniform float u_palette_total;

// 256-texel linear-filtered gradient ramp
uniform sampler2D s_gradient;

uniform samplerBuffer s_attribute_table; // RGBA32F: moveType, value, paletteIndex, colorSource

in float moveGroup;
in vec3 fragNormal;
in vec3 fragPos;

out vec4 fragColor;

vec4 paletteColor(float source, float index)
{
    float offset = u_palette_offsets.x;
    float count = u_palette_counts.x;
    if (source > 3.5) {
        offset = u_palette_offsets.w;
        count = u_palette_counts.w;
    } else if (source > 2.5) {
        offset = u_palette_offsets.z;
        count = u_palette_counts.z;
    } else if (source > 1.5) {
        offset = u_palette_offsets.y;
        count = u_palette_counts.y;
    }

    int idx = int(index + 0.5);
    int count_int = max(int(count + 0.5), 1);
    // libvgcode wraps the color-print palette (color_id % palette size)
    if (source > 2.5 && source < 3.5)
        idx = idx % count_int;
    idx = clamp(idx, 0, count_int - 1);

    return texture(s_color_palette, vec2((offset + float(idx) + 0.5) / u_palette_total, 0.5));
}

vec4 gradientColor(float value)
{
    // invalid range keeps the first ramp color, like libvgcode's
    // ColorRange::get_color_at() with a zero-sized range
    if (u_view_config.y < 0.5)
        return texture(s_gradient, vec2(0.0, 0.5));

    float v = (u_range_is_log > 0.5) ? log(max(value, 1e-6)) : value;
    float u = clamp((v - u_range_bounds.x) / max(u_range_bounds.y - u_range_bounds.x, 1e-6), 0.0, 1.0);
    return texture(s_gradient, vec2(u, 0.5));
}

void main()
{
    vec4 attr = texelFetch(s_attribute_table, int(moveGroup));
    float source = attr.w;

    vec4 color;
    if (source > 4.5)
        color = vec4(64.0 / 255.0, 64.0 / 255.0, 64.0 / 255.0, 1.0); // DUMMY_COLOR
    else if (source < 0.5)
        color = gradientColor(attr.y);
    else
        color = paletteColor(source, attr.z);

    // sequential playback: dim the layers below the top one, exactly like
    // libvgcode's update_colors_texture()
    if (u_view_config.z > 0.5 && u_is_top_layer < 0.5)
        color = vec4(0.25, 0.25, 0.25, 1.0); // Neutral_Color

    // same two-light shading as the legacy gouraud_light shader
    vec3 norm = normalize(normal_matrix * normalize(fragNormal));

    float NdotL = max(dot(norm, LIGHT_TOP_DIR), 0.0);
    float tainted = INTENSITY_AMBIENT + NdotL * LIGHT_TOP_DIFFUSE;
    float specular = LIGHT_TOP_SPECULAR * pow(max(dot(-normalize(fragPos), reflect(-LIGHT_TOP_DIR, norm)), 0.0), LIGHT_TOP_SHININESS);

    NdotL = max(dot(norm, LIGHT_FRONT_DIR), 0.0);
    tainted += NdotL * LIGHT_FRONT_DIFFUSE;

    fragColor = vec4(vec3(specular) + color.rgb * (tainted + emission_factor), color.a);
}
