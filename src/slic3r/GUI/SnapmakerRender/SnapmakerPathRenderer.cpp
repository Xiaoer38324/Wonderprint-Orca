#include "SnapmakerPathRenderer.hpp"

#include "slic3r/GUI/GLShader.hpp"
#include "slic3r/GUI/GLModel.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/Camera.hpp"
#include "slic3r/GUI/3DScene.hpp" // glsafe

#include <boost/log/trivial.hpp>

#include <GL/glew.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

// ============================================================================
// Snapmaker render pipeline - GPU toolpath renderer
// Ported from Snapmaker/OrcaSlicer GUI/PathRenderer.cpp (render engine 2.4.1).
// See the header for the list of adaptations made for Wonderprint-Orca.
// ============================================================================

namespace Slic3r {
namespace GUI {

namespace {

// texture stages used by the snapmaker gpu path shaders
constexpr unsigned int STAGE_PALETTE = 0;
constexpr unsigned int STAGE_NODE_TABLE = 1;
constexpr unsigned int STAGE_WIDTH_HEIGHT_TABLE = 2;
constexpr unsigned int STAGE_ATTRIBUTE_TABLE = 3;
constexpr unsigned int STAGE_STEP_TABLE = 4;
constexpr unsigned int STAGE_GRADIENT = 5;

// texels of the linear color-gradient ramp
constexpr size_t GRADIENT_TEXELS = 256;

std::vector<float> BuildGradientData(const std::vector<ColorRGBA>& colors)
{
    std::vector<float> data(GRADIENT_TEXELS * 4, 0.0f);
    if (colors.empty())
        return data;

    for (size_t i = 0; i < GRADIENT_TEXELS; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(GRADIENT_TEXELS - 1);
        const float scaled = t * static_cast<float>(colors.size() - 1);
        const size_t low = std::clamp<size_t>(static_cast<size_t>(scaled), 0, colors.size() - 1);
        const size_t high = std::clamp<size_t>(low + 1, 0, colors.size() - 1);
        const float frac = scaled - static_cast<float>(low);
        const ColorRGBA& c0 = colors[low];
        const ColorRGBA& c1 = colors[high];
        data[i * 4 + 0] = c0.r() + (c1.r() - c0.r()) * frac;
        data[i * 4 + 1] = c0.g() + (c1.g() - c0.g()) * frac;
        data[i * 4 + 2] = c0.b() + (c1.b() - c0.b()) * frac;
        data[i * 4 + 3] = c0.a() + (c1.a() - c0.a()) * frac;
    }
    return data;
}

std::vector<float> BuildPaletteData(const std::vector<ColorRGBA>& colors)
{
    std::vector<float> data(colors.size() * 4, 0.0f);
    for (size_t i = 0; i < colors.size(); ++i) {
        data[i * 4 + 0] = colors[i].r();
        data[i * 4 + 1] = colors[i].g();
        data[i * 4 + 2] = colors[i].b();
        data[i * 4 + 3] = colors[i].a();
    }
    return data;
}

ColorRGBA NeutralColor()
{
    return ColorRGBA(64.0f / 255.0f, 64.0f / 255.0f, 64.0f / 255.0f, 1.0f);
}

} // namespace

// ----------------------------------------------------------------------------
// SnapmakerLutTexture
// ----------------------------------------------------------------------------

SnapmakerLutTexture::~SnapmakerLutTexture()
{
    Reset();
}

void SnapmakerLutTexture::Upload(const float* data, size_t texelCount, bool nearestFilter)
{
    if (data == nullptr || texelCount == 0)
        return;

    if (_textureId == 0)
        glsafe(::glGenTextures(1, &_textureId));
    glsafe(::glActiveTexture(GL_TEXTURE0));
    glsafe(::glBindTexture(GL_TEXTURE_2D, _textureId));
    glsafe(::glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, GLsizei(texelCount), 1, 0, GL_RGBA, GL_FLOAT, data));
    const GLint filter = nearestFilter ? GL_NEAREST : GL_LINEAR;
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE));
    glsafe(::glBindTexture(GL_TEXTURE_2D, 0));

    _texelCount = texelCount;
}

void SnapmakerLutTexture::Upload(const std::vector<float>& data, size_t texelCount, bool nearestFilter)
{
    Upload(data.data(), texelCount, nearestFilter);
}

void SnapmakerLutTexture::Bind(unsigned int stage) const
{
    if (_textureId == 0)
        return;

    glsafe(::glActiveTexture(GL_TEXTURE0 + stage));
    glsafe(::glBindTexture(GL_TEXTURE_2D, _textureId));
}

void SnapmakerLutTexture::Reset()
{
    if (_textureId != 0) {
        glsafe(::glDeleteTextures(1, &_textureId));
        _textureId = 0;
    }
    _texelCount = 0;
}

// ----------------------------------------------------------------------------
// SnapmakerInstanceMesh
// ----------------------------------------------------------------------------

SnapmakerInstanceMesh::~SnapmakerInstanceMesh()
{
    Reset();
}

void SnapmakerInstanceMesh::InitUnitPrism()
{
    if (IsInitialized())
        return;

    // 10 zero vertices: the shader computes the real positions from
    // gl_VertexID; only the index pattern matters here
    constexpr size_t VERTEX_COUNT = 10;
    constexpr float VERTICES[VERTEX_COUNT * 3] = { 0.0f };

    // start diamond v0..v3 (top/right/bottom/left), end diamond v4..v7,
    // miter fill v8..v9
    const unsigned int INDICES[] = {
        // start cap
        0, 2, 1,   0, 3, 2,
        // sides
        0, 1, 5,   0, 5, 4,
        1, 2, 6,   1, 6, 5,
        2, 3, 7,   2, 7, 6,
        3, 0, 4,   3, 4, 7,
        // end cap
        4, 5, 6,   4, 6, 7,
        // miter fill
        0, 3, 8,   8, 3, 2,
        0, 9, 1,   1, 9, 2,
    };
    _indexCount = sizeof(INDICES) / sizeof(INDICES[0]);
    _hasAttributes = false;

    // A VAO is required by the core-profile contexts requested by this fork.
    glsafe(::glGenVertexArrays(1, &_vertexArrayId));
    glsafe(::glBindVertexArray(_vertexArrayId));

    glsafe(::glGenBuffers(1, &_vertexBufferId));
    glsafe(::glBindBuffer(GL_ARRAY_BUFFER, _vertexBufferId));
    glsafe(::glBufferData(GL_ARRAY_BUFFER, sizeof(VERTICES), VERTICES, GL_STATIC_DRAW));
    glsafe(::glBindBuffer(GL_ARRAY_BUFFER, 0));

    glsafe(::glGenBuffers(1, &_indexBufferId));
    glsafe(::glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, _indexBufferId));
    glsafe(::glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(INDICES), INDICES, GL_STATIC_DRAW));

    glsafe(::glBindVertexArray(0));
}

void SnapmakerInstanceMesh::InitDiamond(unsigned int resolution)
{
    if (IsInitialized())
        return;

    GLModel::Geometry geometry = diamond(resolution);

    _indexCount = geometry.indices_count();
    _hasAttributes = true;
    _vertexStrideBytes = GLModel::Geometry::vertex_stride_bytes(geometry.format);
    _positionOffsetBytes = GLModel::Geometry::position_offset_bytes(geometry.format);
    _normalOffsetBytes = GLModel::Geometry::normal_offset_bytes(geometry.format);

    glsafe(::glGenVertexArrays(1, &_vertexArrayId));
    glsafe(::glBindVertexArray(_vertexArrayId));

    glsafe(::glGenBuffers(1, &_vertexBufferId));
    glsafe(::glBindBuffer(GL_ARRAY_BUFFER, _vertexBufferId));
    glsafe(::glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(geometry.vertices.size() * sizeof(float)),
        geometry.vertices.data(), GL_STATIC_DRAW));
    glsafe(::glBindBuffer(GL_ARRAY_BUFFER, 0));

    glsafe(::glGenBuffers(1, &_indexBufferId));
    glsafe(::glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, _indexBufferId));
    glsafe(::glBufferData(GL_ELEMENT_ARRAY_BUFFER, GLsizeiptr(geometry.indices.size() * sizeof(unsigned int)),
        geometry.indices.data(), GL_STATIC_DRAW));

    glsafe(::glBindVertexArray(0));
}

void SnapmakerInstanceMesh::DrawInstanced(unsigned int instanceCount, int positionId, int normalId)
{
    if (!IsInitialized() || instanceCount == 0)
        return;

    glsafe(::glBindVertexArray(_vertexArrayId));

    if (_hasAttributes) {
        // attribute binding follows the GLModel convention: query the
        // locations from the shader currently in use when the caller did
        // not provide them
        if (positionId == -1 || normalId == -1) {
            GLShaderProgram* shader = wxGetApp().get_current_shader();
            if (shader == nullptr) {
                glsafe(::glBindVertexArray(0));
                return;
            }
            if (positionId == -1)
                positionId = shader->get_attrib_location("v_position");
            if (normalId == -1)
                normalId = shader->get_attrib_location("v_normal");
        }
        if (positionId == -1) {
            glsafe(::glBindVertexArray(0));
            return;
        }

        glsafe(::glBindBuffer(GL_ARRAY_BUFFER, _vertexBufferId));
        if (normalId != -1) {
            glsafe(::glVertexAttribPointer(normalId, 3, GL_FLOAT, GL_FALSE, GLsizei(_vertexStrideBytes),
                reinterpret_cast<const void*>(static_cast<uintptr_t>(_normalOffsetBytes))));
            glsafe(::glEnableVertexAttribArray(normalId));
        }
        glsafe(::glVertexAttribPointer(positionId, 3, GL_FLOAT, GL_FALSE, GLsizei(_vertexStrideBytes),
            reinterpret_cast<const void*>(static_cast<uintptr_t>(_positionOffsetBytes))));
        glsafe(::glEnableVertexAttribArray(positionId));
    }

    // the prism template has no attributes at all: the geometry comes from
    // gl_VertexID
    glsafe(::glDrawElementsInstanced(GL_TRIANGLES, GLsizei(_indexCount), GL_UNSIGNED_INT, nullptr, instanceCount));

    if (_hasAttributes) {
        if (normalId != -1)
            glsafe(::glDisableVertexAttribArray(normalId));
        glsafe(::glDisableVertexAttribArray(positionId));
        glsafe(::glBindBuffer(GL_ARRAY_BUFFER, 0));
    }

    glsafe(::glBindVertexArray(0));
}

void SnapmakerInstanceMesh::Reset()
{
    if (_vertexArrayId != 0) {
        glsafe(::glDeleteVertexArrays(1, &_vertexArrayId));
        _vertexArrayId = 0;
    }
    if (_vertexBufferId != 0) {
        glsafe(::glDeleteBuffers(1, &_vertexBufferId));
        _vertexBufferId = 0;
    }
    if (_indexBufferId != 0) {
        glsafe(::glDeleteBuffers(1, &_indexBufferId));
        _indexBufferId = 0;
    }
    _indexCount = 0;
    _vertexStrideBytes = 0;
    _positionOffsetBytes = 0;
    _normalOffsetBytes = 0;
}

// ----------------------------------------------------------------------------
// SnapmakerPathRenderer
// ----------------------------------------------------------------------------

SnapmakerPathRenderer::~SnapmakerPathRenderer()
{
    Reset();
}

void SnapmakerPathRenderer::EnsureMeshes()
{
    if (!_prismMesh.IsInitialized())
        _prismMesh.InitUnitPrism();
    if (!_markerMesh.IsInitialized())
        _markerMesh.InitDiamond(16);
}

void SnapmakerPathRenderer::UpdateColorRamps(const ViewSettings& settings)
{
    // combined nearest-filtered palette: roles | tools | color-print | options
    std::vector<ColorRGBA> palette;
    palette.reserve(settings.roleColors.size() + settings.toolColors.size()
        + settings.colorPrintColors.size() + settings.optionColors.size() + 4);

    const auto append = [&palette](const std::vector<ColorRGBA>& colors) {
        if (colors.empty())
            palette.push_back(NeutralColor());
        else
            palette.insert(palette.end(), colors.begin(), colors.end());
    };
    append(settings.roleColors);
    std::array<float, 4> offsets{ 0.0f, 0.0f, 0.0f, 0.0f };
    std::array<float, 4> counts{ 0.0f, 0.0f, 0.0f, 0.0f };

    offsets[0] = 0.0f;
    counts[0] = static_cast<float>(settings.roleColors.empty() ? 1 : settings.roleColors.size());
    append(settings.toolColors);
    offsets[1] = counts[0];
    counts[1] = static_cast<float>(settings.toolColors.empty() ? 1 : settings.toolColors.size());
    append(settings.colorPrintColors);
    offsets[2] = offsets[1] + counts[1];
    counts[2] = static_cast<float>(settings.colorPrintColors.empty() ? 1 : settings.colorPrintColors.size());
    append(settings.optionColors);
    offsets[3] = offsets[2] + counts[2];
    counts[3] = static_cast<float>(settings.optionColors.empty() ? 1 : settings.optionColors.size());

    const float total = static_cast<float>(palette.size());
    if (palette != _paletteColors || offsets != _paletteOffsets || counts != _paletteCounts) {
        _palette.Upload(BuildPaletteData(palette), palette.size(), true);
        _paletteColors = std::move(palette);
        _paletteOffsets = offsets;
        _paletteCounts = counts;
        _paletteTotal = total;
    }

    // gradient ramp of the active libvgcode ColorRange (built once per
    // view/range change; the shader applies the linear or logarithmic
    // normalization)
    const std::array<float, 4> key{
        settings.rangeMin,
        settings.rangeMax,
        settings.rangeLog ? 1.0f : 0.0f,
        settings.rangeValid ? 1.0f : 0.0f };
    if (_gradientPalette != settings.rangePalette || key != _gradientKey) {
        _gradient.Upload(BuildGradientData(settings.rangePalette), GRADIENT_TEXELS, false);
        _gradientPalette = settings.rangePalette;
        _gradientKey = key;
    }
}

bool SnapmakerPathRenderer::Render(SnapmakerPathLayerStack& stack, const GCodeProcessorResult& result,
                                   const ViewSettings& settings)
{
    if (stack.LayerCount() == 0)
        return true;

    GLShaderProgram* shader = wxGetApp().get_shader("snapmaker_gpu_path");
    if (shader == nullptr) {
        BOOST_LOG_TRIVIAL(error) << "SnapmakerPathRenderer::Render: snapmaker_gpu_path shader not found";
        return false;
    }

    EnsureMeshes();

    // The main scene pass may leave back-face culling and blending enabled.
    // Toolpath prisms are closed shapes with faces of both windings and are
    // fully opaque, so render them like libvgcode does: no culling, no
    // blending (the previous state is restored on exit).
    const GLboolean cull_face = glIsEnabled(GL_CULL_FACE);
    const GLboolean blend = glIsEnabled(GL_BLEND);
    glsafe(::glDisable(GL_CULL_FACE));
    glsafe(::glDisable(GL_BLEND));

    // rebuild the visibility lists / per-move attributes if anything changed
    stack.RefreshVisibleSteps();
    stack.RefreshMoveAttributes(result);

    UpdateColorRamps(settings);

    Camera& camera = wxGetApp().plater()->get_camera();
    const Transform3d& viewMatrix = camera.get_view_matrix();

    // travel/wipe stay thin prisms exactly like the Snapmaker pipeline
    // (travel forced to 0.1mm in the tables); markers keep a small fixed
    // safety floor for zero-width retraction moves
    const float minLineSize = 0.0f;
    const float markerMinSize = 0.3f;

    shader->start_using();
    shader->set_uniform("view_model_matrix", viewMatrix);
    shader->set_uniform("projection_matrix", camera.get_projection_matrix());
    shader->set_uniform("normal_matrix", (Matrix3d)viewMatrix.matrix().block(0, 0, 3, 3).inverse().transpose());
    shader->set_uniform("emission_factor", 0.25f);
    shader->set_uniform("u_view_config", std::array<float, 4>{
        0.0f,
        settings.rangeValid ? 1.0f : 0.0f,
        settings.topLayerOnly ? 1.0f : 0.0f,
        0.0f });
    shader->set_uniform("u_range_bounds", std::array<float, 2>{ settings.rangeMin, settings.rangeMax });
    shader->set_uniform("u_range_is_log", settings.rangeLog ? 1.0f : 0.0f);
    shader->set_uniform("u_min_line_size", minLineSize);
    shader->set_uniform("u_palette_offsets", _paletteOffsets);
    shader->set_uniform("u_palette_counts", _paletteCounts);
    shader->set_uniform("u_palette_total", _paletteTotal);

    _palette.Bind(STAGE_PALETTE);
    shader->set_uniform("s_color_palette", int(STAGE_PALETTE));
    _gradient.Bind(STAGE_GRADIENT);
    shader->set_uniform("s_gradient", int(STAGE_GRADIENT));
    // sampler uniforms are program state, not per-layer state
    shader->set_uniform("s_node_table", int(STAGE_NODE_TABLE));
    shader->set_uniform("s_width_height_table", int(STAGE_WIDTH_HEIGHT_TABLE));
    shader->set_uniform("s_attribute_table", int(STAGE_ATTRIBUTE_TABLE));
    shader->set_uniform("s_step_table", int(STAGE_STEP_TABLE));

    // Draw the layer nearest to the camera first, so the depth test can
    // reject the layers behind it early. The classic top-down order is only
    // correct while the camera is above the printed layers: when the camera
    // is moved below the build plate, the bed no longer occludes the preview
    // and the top-down order makes every layer render fully before the
    // nearest (bottom) layer is reached - the "looking up through the bed"
    // overdraw stall.
    const auto window = stack.LayerWindow();
    const double top_z = stack.Layer(window.second).Z();
    const double bottom_z = stack.Layer(window.first).Z();
    const bool draw_top_down = camera.get_position().z() >= 0.5 * (top_z + bottom_z);

    RenderLayers(stack, result, *shader, draw_top_down);

    shader->stop_using();

    // option markers (seams, retractions, tool changes, ...) pass
    GLShaderProgram* markerShader = wxGetApp().get_shader("snapmaker_gpu_path_marker");
    if (markerShader != nullptr) {
        markerShader->start_using();
        markerShader->set_uniform("view_model_matrix", viewMatrix);
        markerShader->set_uniform("projection_matrix", camera.get_projection_matrix());
        markerShader->set_uniform("normal_matrix", (Matrix3d)viewMatrix.matrix().block(0, 0, 3, 3).inverse().transpose());
        markerShader->set_uniform("emission_factor", 0.25f);
        markerShader->set_uniform("u_min_marker_size", markerMinSize);
        markerShader->set_uniform("u_top_layer_only", settings.topLayerOnly ? 1.0f : 0.0f);
        markerShader->set_uniform("u_option_offset", _paletteOffsets[3]);
        markerShader->set_uniform("u_palette_total", _paletteTotal);

        _palette.Bind(STAGE_PALETTE);
        markerShader->set_uniform("s_color_palette", int(STAGE_PALETTE));
        markerShader->set_uniform("s_node_table", int(STAGE_NODE_TABLE));
        markerShader->set_uniform("s_width_height_table", int(STAGE_WIDTH_HEIGHT_TABLE));
        markerShader->set_uniform("s_attribute_table", int(STAGE_ATTRIBUTE_TABLE));
        markerShader->set_uniform("s_marker_table", int(STAGE_STEP_TABLE));

        RenderMarkers(stack, result, *markerShader, draw_top_down);

        markerShader->stop_using();
    }

    if (blend)
        glsafe(::glEnable(GL_BLEND));
    if (cull_face)
        glsafe(::glEnable(GL_CULL_FACE));

    return true;
}

void SnapmakerPathRenderer::RenderLayers(SnapmakerPathLayerStack& stack, const GCodeProcessorResult& result,
                                         GLShaderProgram& shader, bool draw_top_down)
{
    const auto window = stack.LayerWindow();
    // sequential playback end: the bottom slider is a playback of the
    // CURRENT layer only — it clips the top layer of the window and never
    // affects the layers below (they stay fully drawn, dimmed by the shader
    // while playback is active)
    const uint32_t sidLast = stack.MoveWindow().second;

    // Draw front-to-back: top-down while the camera is above the plate,
    // bottom-up once it is below it (see Render()).
    const uint32_t layerCount = window.second - window.first + 1;
    for (uint32_t i = 0; i < layerCount; ++i) {
        const uint32_t li = draw_top_down ? (window.second - i) : (window.first + i);
        SnapmakerPathLayerData& layer = stack.Layer(li);
        const bool isTop = (li == window.second);
        const unsigned int stepCount = isTop
            ? layer.PathStepCountUpTo(sidLast)
            : static_cast<unsigned int>(layer.PathStepRecordCount());
        if (stepCount == 0)
            continue;

        layer.UploadTables(result);

        shader.set_uniform("u_is_top_layer", isTop ? 1.0f : 0.0f);

        layer.NodeTable().Bind(STAGE_NODE_TABLE);
        layer.WidthHeightTable().Bind(STAGE_WIDTH_HEIGHT_TABLE);
        layer.AttributeTable().Bind(STAGE_ATTRIBUTE_TABLE);
        layer.PathStepTable().Bind(STAGE_STEP_TABLE);

        _prismMesh.DrawInstanced(stepCount);
    }

    // restore the active texture unit: later passes (imgui, shells) bind
    // their textures without switching the active unit
    glsafe(::glActiveTexture(GL_TEXTURE0));
}

void SnapmakerPathRenderer::RenderMarkers(SnapmakerPathLayerStack& stack, const GCodeProcessorResult& result,
                                          GLShaderProgram& shader, bool draw_top_down)
{
    const auto window = stack.LayerWindow();
    // like the paths, the playback window only clips the markers of the
    // current (top) layer
    const uint32_t sidLast = stack.MoveWindow().second;

    // The marker shader is the only program used by this mesh; resolve its
    // attributes once instead of doing a glGetIntegerv per layer.
    const int positionId = shader.get_attrib_location("v_position");
    const int normalId = shader.get_attrib_location("v_normal");

    const uint32_t layerCount = window.second - window.first + 1;
    for (uint32_t i = 0; i < layerCount; ++i) {
        const uint32_t li = draw_top_down ? (window.second - i) : (window.first + i);
        SnapmakerPathLayerData& layer = stack.Layer(li);
        const bool isTop = (li == window.second);
        const unsigned int markerCount = isTop
            ? layer.MarkerStepCountUpTo(sidLast)
            : static_cast<unsigned int>(layer.MarkerStepRecordCount());
        if (markerCount == 0)
            continue;

        layer.UploadTables(result);

        shader.set_uniform("u_is_top_layer", isTop ? 1.0f : 0.0f);

        layer.NodeTable().Bind(STAGE_NODE_TABLE);
        layer.WidthHeightTable().Bind(STAGE_WIDTH_HEIGHT_TABLE);
        layer.AttributeTable().Bind(STAGE_ATTRIBUTE_TABLE);
        layer.MarkerStepTable().Bind(STAGE_STEP_TABLE);

        _markerMesh.DrawInstanced(markerCount, positionId, normalId);
    }

    glsafe(::glActiveTexture(GL_TEXTURE0));
}

void SnapmakerPathRenderer::Reset()
{
    _prismMesh.Reset();
    _markerMesh.Reset();
    _palette.Reset();
    _gradient.Reset();
    _paletteColors.clear();
    _paletteOffsets = { 0.0f, 0.0f, 0.0f, 0.0f };
    _paletteCounts = { 0.0f, 0.0f, 0.0f, 0.0f };
    _paletteTotal = 1.0f;
    _gradientPalette.clear();
    _gradientKey = { 0.0f, 0.0f, 0.0f, 0.0f };
}

} // namespace GUI
} // namespace Slic3r
