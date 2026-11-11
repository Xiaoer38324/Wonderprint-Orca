#version 140

// ============================================================================
// Snapmaker render pipeline - option marker shading
// ----------------------------------------------------------------------------
// Ported from Snapmaker/OrcaSlicer resources/shaders/140/gpu_path_marker.fs
// ("Render engine build - v2.4.1").
//
// The colour comes from the option palette, indexed by the move type stored
// in the attribute table. During sequential playback markers on the layers
// below the top one are dimmed, matching libvgcode's colored-preview
// behaviour.
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

// 1.0 while sequential playback is active (playhead below the slider end)
uniform float u_top_layer_only;
// 1.0 when this layer is the top layer of the visible window
uniform float u_is_top_layer;

uniform sampler2D s_color_palette;    // combined palette (option part)
uniform float u_option_offset;        // first texel of the option palette
uniform float u_palette_total;

uniform samplerBuffer s_attribute_table; // RGBA32F: moveType, ...

in float moveGroup;
in vec3 fragNormal;
in vec3 fragPos;

out vec4 fragColor;

void main()
{
    // palette index by move type: Retract = 1 .. Custom_GCode = 7
    float moveType = texelFetch(s_attribute_table, int(moveGroup)).r;
    vec4 baseColor = texture(s_color_palette,
        vec2((u_option_offset + moveType + 0.5) / u_palette_total, 0.5));

    // sequential playback: dim the markers of already-printed (non-top)
    // layers like the paths are dimmed in snapmaker_gpu_path.fs
    if (u_top_layer_only > 0.5 && u_is_top_layer < 0.5)
        baseColor = vec4(0.25, 0.25, 0.25, 1.0); // Neutral_Color

    // same two-light shading as the legacy gouraud_light shader
    vec3 norm = normalize(normal_matrix * normalize(fragNormal));

    float NdotL = max(dot(norm, LIGHT_TOP_DIR), 0.0);
    float tainted = INTENSITY_AMBIENT + NdotL * LIGHT_TOP_DIFFUSE;
    float specular = LIGHT_TOP_SPECULAR * pow(max(dot(-normalize(fragPos), reflect(-LIGHT_TOP_DIR, norm)), 0.0), LIGHT_TOP_SHININESS);

    NdotL = max(dot(norm, LIGHT_FRONT_DIR), 0.0);
    tainted += NdotL * LIGHT_FRONT_DIFFUSE;

    fragColor = vec4(vec3(specular) + baseColor.rgb * (tainted + emission_factor), baseColor.a);
}
