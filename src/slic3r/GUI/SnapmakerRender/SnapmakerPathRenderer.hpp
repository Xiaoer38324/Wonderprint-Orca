#ifndef slic3r_SnapmakerPathRenderer_hpp_
#define slic3r_SnapmakerPathRenderer_hpp_

// ============================================================================
// Snapmaker render pipeline - GPU toolpath renderer
// ----------------------------------------------------------------------------
// Ported from Snapmaker OrcaSlicer (Snapmaker/OrcaSlicer, "Render engine build
// - v2.4.1", GUI/PathRenderer.* + resources/shaders/140/gpu_path.*).
//
// The pipeline draws one instanced prism per path step and expands the prism
// in the vertex shader from the data tables. The CPU only maintains the
// visibility tables and the small colour LUTs, so zooming into a dense model
// no longer re-tessellates or re-uploads toolpath geometry.
//
// Changes for Wonderprint-Orca:
//   * class names prefixed with Snapmaker*,
//   * the palette is a combined LUT built from libvgcode's role / tool /
//     color-print / option colors, so the GPU preview matches the existing
//     legend while libvgcode keeps owning the data model and the sliders,
//   * the instance meshes own a VAO (required by the core profile contexts
//     this fork requests),
//   * gradient LUTs are built from the libvgcode ColorRange palette and its
//     linear/logarithmic mode.
// ============================================================================

#include "SnapmakerPathData.hpp"
#include "libslic3r/Color.hpp"

#include <array>
#include <cstddef>
#include <vector>

namespace Slic3r {
class GLShaderProgram;

namespace GUI {

// 1D lookup texture (stored as 1 x N RGBA32F) used for the color ramp of the
// active view: value gradients use a 256-texel linear-filtered ramp, indexed
// palettes (roles / tools) use one nearest-filtered texel per color.
class SnapmakerLutTexture
{
public:
    SnapmakerLutTexture() = default;
    ~SnapmakerLutTexture();

    SnapmakerLutTexture(const SnapmakerLutTexture&) = delete;
    SnapmakerLutTexture& operator=(const SnapmakerLutTexture&) = delete;
    SnapmakerLutTexture(SnapmakerLutTexture&&) = delete;
    SnapmakerLutTexture& operator=(SnapmakerLutTexture&&) = delete;

    void Upload(const float* data, size_t texelCount, bool nearestFilter);
    void Upload(const std::vector<float>& data, size_t texelCount, bool nearestFilter);
    void Bind(unsigned int stage) const;
    void Reset();

    bool IsValid() const { return _textureId != 0; }
    size_t TexelCount() const { return _texelCount; }

private:
    unsigned int _textureId{ 0 };
    size_t _texelCount{ 0 };
};

// Instance mesh used by the de-geometrized pipeline: the actual vertex
// positions are generated in the vertex shader from gl_VertexID and the data
// tables, only the index pattern of this template is relevant.
class SnapmakerInstanceMesh
{
public:
    SnapmakerInstanceMesh() = default;
    ~SnapmakerInstanceMesh();

    SnapmakerInstanceMesh(const SnapmakerInstanceMesh&) = delete;
    SnapmakerInstanceMesh& operator=(const SnapmakerInstanceMesh&) = delete;
    SnapmakerInstanceMesh(SnapmakerInstanceMesh&&) = delete;
    SnapmakerInstanceMesh& operator=(SnapmakerInstanceMesh&&) = delete;

    // builds the 10-vertex / 48-index unit prism (2 diamonds + miter fill);
    // the vertices are irrelevant (all zeros), the real positions come from
    // gl_VertexID and the data tables
    void InitUnitPrism();
    // builds the marker template mesh (position + normal attributes, e.g.
    // the diamond used for option markers)
    void InitDiamond(unsigned int resolution);
    // draws the mesh `instanceCount` times (one instance per path step or
    // marker); must be called with the matching shader in use.
    // positionId/normalId are the shader attribute locations; when omitted
    // (-1), they are queried from the currently bound program. The marker
    // pass passes them in once per frame to avoid a glGetIntegerv pipeline
    // stall for every layer.
    void DrawInstanced(unsigned int instanceCount, int positionId = -1, int normalId = -1);
    void Reset();

    bool IsInitialized() const { return _indexBufferId != 0; }

private:
    unsigned int _vertexArrayId{ 0 };
    unsigned int _vertexBufferId{ 0 };
    unsigned int _indexBufferId{ 0 };
    size_t _indexCount{ 0 };
    // meshes with real vertex attributes (markers) need pointer setup at
    // draw time; attribute-less templates (the prism) only need the indices
    bool _hasAttributes{ false };
    size_t _vertexStrideBytes{ 0 };
    size_t _positionOffsetBytes{ 0 };
    size_t _normalOffsetBytes{ 0 };
};

// Draws the toolpaths of a SnapmakerPathLayerStack with GPU-generated
// geometry. Per frame, the CPU only maintains the visibility tables and
// decides which layers/steps are drawn; every vertex and every color lookup
// happens in the shaders.
class SnapmakerPathRenderer
{
public:
    // View-dependent inputs computed by the adapter (the same values the
    // libvgcode viewer uses for its legend), kept in a plain struct to avoid
    // a header dependency on the viewer.
    struct ViewSettings
    {
        SnapmakerPathViewType viewType{ SnapmakerPathViewType::FeatureType };
        bool topLayerOnly{ false }; // dim the layers below the top one
        bool rangeValid{ false };
        bool rangeLog{ false };
        float rangeMin{ 0.0f };
        float rangeMax{ 0.0f };
        // palette of the active libvgcode ColorRange (used to build the
        // 256-texel gradient LUT)
        std::vector<ColorRGBA> rangePalette;
        // palettes, in the same order as the shader's colour sources
        std::vector<ColorRGBA> roleColors;
        std::vector<ColorRGBA> toolColors;
        std::vector<ColorRGBA> colorPrintColors;
        std::vector<ColorRGBA> optionColors; // indexed by EMoveType
    };

    SnapmakerPathRenderer() = default;
    ~SnapmakerPathRenderer();

    SnapmakerPathRenderer(const SnapmakerPathRenderer&) = delete;
    SnapmakerPathRenderer& operator=(const SnapmakerPathRenderer&) = delete;

    // Renders all layers inside the stack's layer window. Must be called
    // with an active GL context (e.g. from GCodeViewer::render_toolpaths()).
    // Returns false only when the shaders are unavailable (the caller then
    // falls back to libvgcode).
    bool Render(SnapmakerPathLayerStack& stack, const GCodeProcessorResult& result, const ViewSettings& settings);

    void Reset();

private:
    void EnsureMeshes();
    void UpdateColorRamps(const ViewSettings& settings);
    void RenderLayers(SnapmakerPathLayerStack& stack, const GCodeProcessorResult& result,
                      GLShaderProgram& shader, bool draw_top_down);
    void RenderMarkers(SnapmakerPathLayerStack& stack, const GCodeProcessorResult& result,
                       GLShaderProgram& shader, bool draw_top_down);

    SnapmakerInstanceMesh _prismMesh;
    SnapmakerInstanceMesh _markerMesh;
    SnapmakerLutTexture _palette;   // combined role / tool / color-print / option LUT
    SnapmakerLutTexture _gradient;  // value-gradient LUT

    // caches used to skip redundant uploads
    std::vector<ColorRGBA> _paletteColors;
    std::array<float, 4> _paletteOffsets{ 0.0f, 0.0f, 0.0f, 0.0f };
    std::array<float, 4> _paletteCounts{ 0.0f, 0.0f, 0.0f, 0.0f };
    float _paletteTotal{ 1.0f };
    std::vector<ColorRGBA> _gradientPalette;
    std::array<float, 4> _gradientKey{ 0.0f, 0.0f, 0.0f, 0.0f };
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_SnapmakerPathRenderer_hpp_
