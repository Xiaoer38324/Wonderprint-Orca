#include "SnapmakerPathView.hpp"

#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/LibVGCode/LibVGCodeWrapper.hpp"

#include <boost/log/trivial.hpp>

#include <algorithm>
#include <array>
#include <cmath>

// ============================================================================
// Snapmaker render pipeline - libvgcode adapter
// ----------------------------------------------------------------------------
// The Snapmaker pipeline (ported from Snapmaker/OrcaSlicer render engine
// 2.4.1) is data-driven; this adapter is the Wonderprint-specific glue that
// maps the state of the existing libvgcode viewer onto it.
// ============================================================================

namespace Slic3r {
namespace GUI {

namespace {

constexpr uint32_t INVALID_SID = 0xFFFFFFFFu;

ColorRGBA NeutralColor()
{
    return ColorRGBA(64.0f / 255.0f, 64.0f / 255.0f, 64.0f / 255.0f, 1.0f);
}

struct ViewMapping
{
    SnapmakerPathViewType pipeline{ SnapmakerPathViewType::FeatureType };
    bool gradient{ false };
    libvgcode::EViewType rangeType{ libvgcode::EViewType::Height };
};

ViewMapping MapViewType(libvgcode::EViewType type)
{
    ViewMapping ret;
    switch (type) {
    case libvgcode::EViewType::Summary:
        ret.pipeline = SnapmakerPathViewType::Summary;
        break;
    case libvgcode::EViewType::FeatureType:
        ret.pipeline = SnapmakerPathViewType::FeatureType;
        break;
    case libvgcode::EViewType::ColorPrint:
        ret.pipeline = SnapmakerPathViewType::ColorPrint;
        break;
    case libvgcode::EViewType::Speed:
        ret.pipeline = SnapmakerPathViewType::Speed;
        ret.gradient = true;
        ret.rangeType = libvgcode::EViewType::Speed;
        break;
    case libvgcode::EViewType::ActualSpeed:
        ret.pipeline = SnapmakerPathViewType::ActualSpeed;
        ret.gradient = true;
        ret.rangeType = libvgcode::EViewType::ActualSpeed;
        break;
    case libvgcode::EViewType::Height:
        ret.pipeline = SnapmakerPathViewType::Height;
        ret.gradient = true;
        ret.rangeType = libvgcode::EViewType::Height;
        break;
    case libvgcode::EViewType::Width:
        ret.pipeline = SnapmakerPathViewType::Width;
        ret.gradient = true;
        ret.rangeType = libvgcode::EViewType::Width;
        break;
    case libvgcode::EViewType::VolumetricFlowRate:
        ret.pipeline = SnapmakerPathViewType::VolumetricFlowRate;
        ret.gradient = true;
        ret.rangeType = libvgcode::EViewType::VolumetricFlowRate;
        break;
    case libvgcode::EViewType::ActualVolumetricFlowRate:
        ret.pipeline = SnapmakerPathViewType::ActualVolumetricFlowRate;
        ret.gradient = true;
        ret.rangeType = libvgcode::EViewType::ActualVolumetricFlowRate;
        break;
    case libvgcode::EViewType::LayerTimeLinear:
        ret.pipeline = SnapmakerPathViewType::LayerTimeLinear;
        ret.gradient = true;
        ret.rangeType = libvgcode::EViewType::LayerTimeLinear;
        break;
    case libvgcode::EViewType::LayerTimeLogarithmic:
        ret.pipeline = SnapmakerPathViewType::LayerTimeLogarithmic;
        ret.gradient = true;
        ret.rangeType = libvgcode::EViewType::LayerTimeLogarithmic;
        break;
    case libvgcode::EViewType::FanSpeed:
        ret.pipeline = SnapmakerPathViewType::FanSpeed;
        ret.gradient = true;
        ret.rangeType = libvgcode::EViewType::FanSpeed;
        break;
    case libvgcode::EViewType::Temperature:
        ret.pipeline = SnapmakerPathViewType::Temperature;
        ret.gradient = true;
        ret.rangeType = libvgcode::EViewType::Temperature;
        break;
    case libvgcode::EViewType::PressureAdvance:
        ret.pipeline = SnapmakerPathViewType::PressureAdvance;
        ret.gradient = true;
        ret.rangeType = libvgcode::EViewType::PressureAdvance;
        break;
    case libvgcode::EViewType::Tool:
        ret.pipeline = SnapmakerPathViewType::Tool;
        break;
    default:
        ret.pipeline = SnapmakerPathViewType::FeatureType;
        break;
    }
    return ret;
}

} // namespace

SnapmakerPathView::SnapmakerPathView() = default;

SnapmakerPathView::~SnapmakerPathView() = default;

bool SnapmakerPathView::IsSupported()
{
    // The pipeline needs texture buffers (GL 3.1) and both shader programs.
    // The shader lookup comes first so an early call degrades to the
    // libvgcode preview instead of touching the GL info prematurely.
    if (wxGetApp().get_shader("snapmaker_gpu_path") == nullptr ||
        wxGetApp().get_shader("snapmaker_gpu_path_marker") == nullptr)
        return false;
    return wxGetApp().is_gl_version_greater_or_equal_to(3, 1);
}

void SnapmakerPathView::Load(const GCodeProcessorResult& result)
{
    _loaded = false;
    _layerMappingFailed = false;
    _layerTimesValid = false;
    _timeMode = -1;
    _moveGcodeIds.clear();
    _moveSids.clear();
    _settings = SnapmakerPathRenderer::ViewSettings();

    _stack.BuildFromResult(result, true);
    _loaded = _stack.LayerCount() > 0;
    if (!_loaded)
        return;

    // sid space used by the stack: raw move indices with seam moves skipped
    _moveGcodeIds.reserve(result.moves.size());
    _moveSids.reserve(result.moves.size());
    uint32_t sid = 0;
    for (const GCodeProcessorResult::MoveVertex& move : result.moves) {
        _moveGcodeIds.push_back(move.gcode_id);
        if (move.type == EMoveType::Seam)
            _moveSids.push_back(INVALID_SID);
        else {
            _moveSids.push_back(sid);
            ++sid;
        }
    }

    BOOST_LOG_TRIVIAL(info) << "Snapmaker GPU path pipeline: loaded " << _stack.LayerCount()
                            << " layers, " << result.moves.size() << " moves";
}

void SnapmakerPathView::Reset()
{
    _stack.Reset();
    _renderer.Reset();
    _settings = SnapmakerPathRenderer::ViewSettings();
    _loaded = false;
    _layerMappingFailed = false;
    _layerTimesValid = false;
    _timeMode = -1;
    _moveGcodeIds.clear();
    _moveSids.clear();
}

uint32_t SnapmakerPathView::SidOfGcodeId(uint32_t gcodeId) const
{
    if (_moveGcodeIds.empty())
        return 0;

    // gcode ids are non-decreasing in move order
    const auto it = std::upper_bound(_moveGcodeIds.begin(), _moveGcodeIds.end(), gcodeId);
    if (it == _moveGcodeIds.begin())
        return 0;

    size_t index = static_cast<size_t>(it - _moveGcodeIds.begin()) - 1;
    if (_moveSids[index] == INVALID_SID) {
        // the vertex is a seam: its marker belongs to the sid of the move
        // that follows it
        size_t next = index + 1;
        while (next < _moveSids.size() && _moveSids[next] == INVALID_SID)
            ++next;
        if (next < _moveSids.size())
            return _moveSids[next];

        while (index > 0 && _moveSids[index] == INVALID_SID)
            --index;
        if (_moveSids[index] != INVALID_SID)
            return _moveSids[index];
        return 0;
    }
    return _moveSids[index];
}

bool SnapmakerPathView::SyncFromViewer(libvgcode::Viewer& viewer)
{
    if (!_loaded || _layerMappingFailed)
        return false;

    // libvgcode normally performs its pending view/visibility/color updates
    // at the start of Viewer::render(). We skip those draw calls, so run the
    // update-only entry point first; otherwise the ranges and colors read
    // below would be one event behind (or never updated after a view or
    // visibility change).
    viewer.update();

    // The layer slider of this fork indexes libvgcode's Layers table, which
    // is fed from MoveVertex::layer_id. The Snapmaker stack was built from
    // the same ids, so a mismatch means the two views are out of sync and
    // the safe path is libvgcode's own renderer.
    if (viewer.get_layers_count() != _stack.LayerCount()) {
        BOOST_LOG_TRIVIAL(warning) << "Snapmaker GPU path pipeline: layer count mismatch (viewer "
                                   << viewer.get_layers_count() << ", pipeline " << _stack.LayerCount()
                                   << "), falling back to libvgcode rendering";
        _layerMappingFailed = true;
        return false;
    }

    const ViewMapping mapping = MapViewType(viewer.get_view_type());
    _settings.viewType = mapping.pipeline;
    _stack.SetViewType(mapping.pipeline);

    // ---- visibility -----------------------------------------------------
    // libvgcode exposes option visibility per EOptionType, the pipeline per
    // EMoveType. Extrusions are always enabled here and filtered by role.
    const auto set_move_visible = [this, &viewer](EMoveType type, libvgcode::EOptionType option) {
        _stack.SetMoveTypeVisible(type, viewer.is_option_visible(option));
    };
    _stack.SetMoveTypeVisible(EMoveType::Noop, false);
    _stack.SetMoveTypeVisible(EMoveType::Extrude, true);
    set_move_visible(EMoveType::Travel, libvgcode::EOptionType::Travels);
    set_move_visible(EMoveType::Wipe, libvgcode::EOptionType::Wipes);
    set_move_visible(EMoveType::Retract, libvgcode::EOptionType::Retractions);
    set_move_visible(EMoveType::Unretract, libvgcode::EOptionType::Unretractions);
    set_move_visible(EMoveType::Seam, libvgcode::EOptionType::Seams);
    set_move_visible(EMoveType::Tool_change, libvgcode::EOptionType::ToolChanges);
    set_move_visible(EMoveType::Color_change, libvgcode::EOptionType::ColorChanges);
    set_move_visible(EMoveType::Pause_Print, libvgcode::EOptionType::PausePrints);
    set_move_visible(EMoveType::Custom_GCode, libvgcode::EOptionType::CustomGCodes);

    uint32_t role_flags = 0;
    for (uint32_t role = 0; role < static_cast<uint32_t>(erCount); ++role) {
        if (viewer.is_extrusion_role_visible(libvgcode::convert(static_cast<ExtrusionRole>(role))))
            role_flags |= 1u << role;
    }
    _stack.SetRoleVisibilityFlags(role_flags);

    // ---- palettes -------------------------------------------------------
    _settings.roleColors.clear();
    for (uint32_t role = 0; role < static_cast<uint32_t>(erCount); ++role) {
        _settings.roleColors.push_back(libvgcode::convert(
            viewer.get_extrusion_role_color(libvgcode::convert(static_cast<ExtrusionRole>(role)))));
    }

    _settings.toolColors.clear();
    for (const libvgcode::Color& color : viewer.get_tool_colors())
        _settings.toolColors.push_back(libvgcode::convert(color));

    _settings.colorPrintColors.clear();
    for (const libvgcode::Color& color : viewer.get_color_print_colors())
        _settings.colorPrintColors.push_back(libvgcode::convert(color));

    // The option palette is indexed by EMoveType in the shaders (the marker
    // shader uses moveType directly), so fill every move type slot.
    _settings.optionColors.clear();
    for (uint32_t type = 0; type < static_cast<uint32_t>(EMoveType::Count); ++type) {
        const libvgcode::EOptionType option =
            libvgcode::move_type_to_option(libvgcode::convert(static_cast<EMoveType>(type)));
        _settings.optionColors.push_back(option == libvgcode::EOptionType::COUNT
            ? NeutralColor()
            : libvgcode::convert(viewer.get_option_color(option)));
    }

    // ---- active color range --------------------------------------------
    if (mapping.gradient) {
        const libvgcode::ColorRange& range = viewer.get_color_range(mapping.rangeType);
        const std::array<float, 2>& bounds = range.get_range();
        _settings.rangeLog = (range.get_type() == libvgcode::EColorRangeType::Logarithmic);
        _settings.rangeValid = bounds[0] <= bounds[1] && bounds[1] - bounds[0] > 1e-6f;
        if (_settings.rangeLog) {
            _settings.rangeValid = _settings.rangeValid && bounds[0] > 0.0f;
            _settings.rangeMin = std::log(std::max(bounds[0], 0.001f));
            _settings.rangeMax = std::log(std::max(bounds[1], 0.001f));
        }
        else {
            _settings.rangeMin = bounds[0];
            _settings.rangeMax = bounds[1];
        }

        _settings.rangePalette.clear();
        for (const libvgcode::Color& color : range.get_palette())
            _settings.rangePalette.push_back(libvgcode::convert(color));
        if (_settings.rangePalette.empty())
            _settings.rangePalette = { NeutralColor(), NeutralColor() };
    }
    else {
        _settings.rangeValid = false;
        _settings.rangeLog = false;
        _settings.rangeMin = 0.0f;
        _settings.rangeMax = 0.0f;
        _settings.rangePalette = { NeutralColor(), NeutralColor() };
    }

    // ---- layer times (current time mode) -------------------------------
    // LayerTime views must use the same per-layer durations as libvgcode,
    // including the Normal/Stealth switch. get_layers_estimated_times()
    // walks all layers, so only refetch it when the mode actually changes.
    const int time_mode = static_cast<int>(viewer.get_time_mode());
    if (!_layerTimesValid || time_mode != _timeMode) {
        _stack.SetLayerTimes(viewer.get_layers_estimated_times());
        _timeMode = time_mode;
        _layerTimesValid = true;
    }

    // ---- layer window / playback ---------------------------------------
    const libvgcode::Interval& layers_range = viewer.get_layers_view_range();
    _stack.SetLayerWindow(static_cast<uint32_t>(layers_range[0]), static_cast<uint32_t>(layers_range[1]));

    const libvgcode::Interval& full_range = viewer.get_view_full_range();
    const libvgcode::Interval& visible_range = viewer.get_view_visible_range();
    const bool playback_active = full_range[1] != visible_range[1];
    // libvgcode only grays the layers below the top one while the sequential
    // playback is running and the "top layer only" preference is enabled.
    _settings.topLayerOnly = viewer.is_top_layer_only_view_range() && playback_active;

    uint32_t sid_last = 0;
    if (visible_range[1] < viewer.get_vertices_count())
        sid_last = SidOfGcodeId(viewer.get_vertex_at(visible_range[1]).gcode_id);
    _stack.SetMoveWindow(0, sid_last);

    return true;
}

bool SnapmakerPathView::Render(const GCodeProcessorResult& result)
{
    if (!_loaded)
        return false;
    if (!_renderer.Render(_stack, result, _settings)) {
        _layerMappingFailed = true;
        return false;
    }
    return true;
}

} // namespace GUI
} // namespace Slic3r
