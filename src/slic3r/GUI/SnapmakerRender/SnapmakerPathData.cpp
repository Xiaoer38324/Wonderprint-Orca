#include "SnapmakerPathData.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

// ============================================================================
// Snapmaker render pipeline - de-geometrized toolpath data
// Ported from Snapmaker/OrcaSlicer GUI/PathData.cpp (render engine 2.4.1).
// See the header for the list of adaptations made for Wonderprint-Orca.
// ============================================================================

namespace Slic3r {
namespace GUI {

namespace {

constexpr uint32_t INVALID_INDEX = 0xFFFFFFFFu;
// z tolerance used to detect layer boundaries (Snapmaker's original Z-scan)
constexpr float LAYER_Z_EPSILON = 1e-4f;
// travel moves carry no real width/height; keep a minimal size so the
// generated prism stays visible
constexpr float TRAVEL_SIZE = 0.1f;
// neutral gray used by libvgcode for hidden / disabled / dummy vertices
constexpr float NEUTRAL_GRAY = 64.0f / 255.0f;

// moves rendered as diamond markers (options), everything else as prisms
bool IsOptionMove(EMoveType type)
{
    switch (type) {
    case EMoveType::Tool_change:
    case EMoveType::Color_change:
    case EMoveType::Pause_Print:
    case EMoveType::Custom_GCode:
    case EMoveType::Retract:
    case EMoveType::Unretract:
    case EMoveType::Seam:
        return true;
    default:
        return false;
    }
}

// Value sampled by the active gradient view (Snapmaker's ViewValueOfMove,
// extended with the views libvgcode exposes in Wonderprint-Orca).
float GradientValueOfMove(SnapmakerPathViewType viewType, const GCodeProcessorResult::MoveVertex& move, float layerTime)
{
    switch (viewType) {
    case SnapmakerPathViewType::Speed:                   return move.feedrate;
    case SnapmakerPathViewType::ActualSpeed:             return move.actual_feedrate;
    case SnapmakerPathViewType::Height:                  return move.height;
    case SnapmakerPathViewType::Width:                   return move.width;
    case SnapmakerPathViewType::VolumetricFlowRate:      return move.volumetric_rate();
    case SnapmakerPathViewType::ActualVolumetricFlowRate:return move.actual_volumetric_rate();
    case SnapmakerPathViewType::LayerTimeLinear:
    case SnapmakerPathViewType::LayerTimeLogarithmic:    return layerTime;
    case SnapmakerPathViewType::FanSpeed:                return move.fan_speed;
    case SnapmakerPathViewType::Temperature:             return move.temperature;
    case SnapmakerPathViewType::PressureAdvance:         return move.pressure_advance;
    default:                                             return 0.0f;
    }
}

struct ColorAttrib
{
    SnapmakerColorSource source{ SnapmakerColorSource::Constant };
    float value{ 0.0f };         // gradient value (source == Gradient)
    float paletteIndex{ 0.0f };  // palette index (other sources)
};

ColorAttrib GradientAttrib(SnapmakerPathViewType viewType, const GCodeProcessorResult::MoveVertex& move, float layerTime)
{
    ColorAttrib ret;
    ret.source = SnapmakerColorSource::Gradient;
    ret.value = GradientValueOfMove(viewType, move, layerTime);
    return ret;
}

ColorAttrib PaletteAttrib(SnapmakerColorSource source, float index)
{
    ColorAttrib ret;
    ret.source = source;
    ret.paletteIndex = index;
    return ret;
}

// Replicates libvgcode::ViewerImpl::get_vertex_color() for the active view so
// the GPU toolpaths and the libvgcode legend keep exactly the same colours.
ColorAttrib ColorAttribOfMove(SnapmakerPathViewType viewType, const GCodeProcessorResult::MoveVertex& move,
    bool layerHasOptionMove, float layerTime)
{
    // libvgcode draws Noop vertices with DUMMY_COLOR
    if (move.type == EMoveType::Noop)
        return PaletteAttrib(SnapmakerColorSource::Constant, NEUTRAL_GRAY);

    // Options always use the option palette, in every view
    if (IsOptionMove(move.type))
        return PaletteAttrib(SnapmakerColorSource::Option, static_cast<float>(move.type));

    // Wipes use the option palette, except in the speed views where libvgcode
    // falls through to the feedrate gradient
    if (move.type == EMoveType::Wipe) {
        if (viewType == SnapmakerPathViewType::Speed || viewType == SnapmakerPathViewType::ActualSpeed)
            return GradientAttrib(viewType, move, layerTime);
        return PaletteAttrib(SnapmakerColorSource::Option, static_cast<float>(EMoveType::Wipe));
    }

    if (move.type == EMoveType::Travel) {
        switch (viewType) {
        case SnapmakerPathViewType::Speed:
        case SnapmakerPathViewType::ActualSpeed:
            return GradientAttrib(viewType, move, layerTime);
        case SnapmakerPathViewType::Tool:
            return PaletteAttrib(SnapmakerColorSource::Tool, static_cast<float>(move.extruder_id));
        case SnapmakerPathViewType::Summary:
        case SnapmakerPathViewType::ColorPrint:
            // Summary colors travels with the color-print palette; ColorPrint
            // keeps the travel option colour
            if (viewType == SnapmakerPathViewType::Summary) {
                if (layerHasOptionMove)
                    return PaletteAttrib(SnapmakerColorSource::Constant, NEUTRAL_GRAY);
                return PaletteAttrib(SnapmakerColorSource::ColorPrint, static_cast<float>(move.cp_color_id));
            }
            return PaletteAttrib(SnapmakerColorSource::Option, static_cast<float>(EMoveType::Travel));
        default:
            return PaletteAttrib(SnapmakerColorSource::Option, static_cast<float>(EMoveType::Travel));
        }
    }

    // Extrusions
    switch (viewType) {
    case SnapmakerPathViewType::FeatureType:
        return PaletteAttrib(SnapmakerColorSource::ExtrusionRole, static_cast<float>(move.extrusion_role));
    case SnapmakerPathViewType::Summary:
    case SnapmakerPathViewType::ColorPrint:
        if (layerHasOptionMove)
            return PaletteAttrib(SnapmakerColorSource::Constant, NEUTRAL_GRAY);
        return PaletteAttrib(SnapmakerColorSource::ColorPrint, static_cast<float>(move.cp_color_id));
    case SnapmakerPathViewType::Tool:
        return PaletteAttrib(SnapmakerColorSource::Tool, static_cast<float>(move.extruder_id));
    case SnapmakerPathViewType::Speed:
    case SnapmakerPathViewType::ActualSpeed:
    case SnapmakerPathViewType::Height:
    case SnapmakerPathViewType::Width:
    case SnapmakerPathViewType::VolumetricFlowRate:
    case SnapmakerPathViewType::ActualVolumetricFlowRate:
    case SnapmakerPathViewType::LayerTimeLinear:
    case SnapmakerPathViewType::LayerTimeLogarithmic:
    case SnapmakerPathViewType::FanSpeed:
    case SnapmakerPathViewType::Temperature:
    case SnapmakerPathViewType::PressureAdvance:
        return GradientAttrib(viewType, move, layerTime);
    default:
        return PaletteAttrib(SnapmakerColorSource::Constant, NEUTRAL_GRAY);
    }
}

// Appends the GPU step records of one path step. Consecutive connected body
// steps share their junction node, so the previous record's end node equals
// the current record's start node; that links the pair for the miter fill
// computed in the vertex shader. Arc chains additionally emit one record per
// interpolation segment.
void AppendPathStepRecords(const SnapmakerPathStep& step,
                           const std::vector<SnapmakerMoveNodeGroup>& groups,
                           std::vector<float>& records,
                           std::vector<uint32_t>& recordSids,
                           uint32_t& prevRecordIndex)
{
    const auto& firstGroup = groups[step.firstGroup];
    const auto& secondGroup = groups[step.secondGroup];
    const uint32_t firstNode = firstGroup.firstNode + firstGroup.nodeCount - 1;

    bool hasPrev = false;
    uint32_t prevFirstNode = 0;
    if (prevRecordIndex != INVALID_INDEX) {
        const uint32_t prevEndNode = static_cast<uint32_t>(records[prevRecordIndex * 4 + 1]);
        if (prevEndNode == firstNode) {
            hasPrev = true;
            prevFirstNode = static_cast<uint32_t>(records[prevRecordIndex * 4 + 0]);
        }
    }

    const auto appendRecord = [&](uint32_t startNode, uint32_t endNode, bool linked, uint32_t prevNode) {
        records.push_back(float(startNode));
        records.push_back(float(endNode));
        records.push_back(linked ? 1.0f : 0.0f);
        records.push_back(float(prevNode));
        recordSids.push_back(step.secondSid);
    };

    appendRecord(firstNode, secondGroup.firstNode, hasPrev, prevFirstNode);
    prevRecordIndex = static_cast<uint32_t>(records.size() / 4 - 1);

    for (uint32_t k = 1; k < secondGroup.nodeCount; ++k) {
        const uint32_t prevRecordFirst = static_cast<uint32_t>(records[prevRecordIndex * 4 + 0]);
        appendRecord(secondGroup.firstNode + k - 1, secondGroup.firstNode + k, hasPrev, prevRecordFirst);
        ++prevRecordIndex;
    }
}

void AppendMarkerStepRecords(const SnapmakerPathStep& step,
                             const std::vector<SnapmakerMoveNodeGroup>& groups,
                             std::vector<float>& records,
                             std::vector<uint32_t>& recordSids)
{
    const uint32_t node = groups[step.secondGroup].firstNode;
    records.push_back(float(node));
    records.push_back(float(node));
    records.push_back(0.0f);
    records.push_back(0.0f);
    recordSids.push_back(step.secondSid);
}

} // namespace

// ----------------------------------------------------------------------------
// SnapmakerPathLayerData
// ----------------------------------------------------------------------------

uint32_t SnapmakerPathLayerData::AddNode(const Vec3f& position, float zOffset, SnapmakerMoveNodeGroup& group)
{
    SnapmakerPathNode node;
    node.position = position;
    node.position.z() += zOffset;
    node.groupIndex = static_cast<uint32_t>(_groups.size());
    _nodes.push_back(node);
    ++group.nodeCount;
    return group.firstNode + group.nodeCount - 1;
}

uint32_t SnapmakerPathLayerData::AddMoveNodeGroup(uint32_t moveIndex, const GCodeProcessorResult::MoveVertex& move)
{
    SnapmakerMoveNodeGroup group;
    group.moveIndex = moveIndex;
    group.firstNode = static_cast<uint32_t>(_nodes.size());
    group.nodeCount = 0;

    // wipe blobs are lifted above the path by half their height
    const float zOffset = (move.type == EMoveType::Wipe) ? 0.5f * GCodeProcessor::Wipe_Height : 0.0f;

    // NOTE: arc moves are not stored with interpolation points in this
    // GCodeProcessor version, so a move always contributes one node.
    AddNode(move.position, zOffset, group);

    _groups.push_back(group);
    return static_cast<uint32_t>(_groups.size() - 1);
}

void SnapmakerPathLayerData::RefreshVisibleSteps(const SnapmakerPathLayerStack& stack)
{
    _pathStepRecords.clear();
    _markerStepRecords.clear();
    _pathRecordSids.clear();
    _markerRecordSids.clear();
    uint32_t prevRecordIndex = INVALID_INDEX;

    for (const SnapmakerPathStep& step : _steps) {
        if (!stack.IsMoveTypeVisible(step.type))
            continue;
        if (step.type == EMoveType::Extrude && !stack.IsRoleVisible(step.role))
            continue;

        if (IsOptionMove(step.type))
            AppendMarkerStepRecords(step, _groups, _markerStepRecords, _markerRecordSids);
        else
            AppendPathStepRecords(step, _groups, _pathStepRecords, _pathRecordSids, prevRecordIndex);
    }
    _stepTablesDirty = true;
}

unsigned int SnapmakerPathLayerData::PathStepCountUpTo(uint32_t sidLast) const
{
    // records are ordered by sid: count the prefix up to sidLast
    const auto it = std::upper_bound(_pathRecordSids.begin(), _pathRecordSids.end(), sidLast);
    return static_cast<unsigned int>(it - _pathRecordSids.begin());
}

unsigned int SnapmakerPathLayerData::MarkerStepCountUpTo(uint32_t sidLast) const
{
    const auto it = std::upper_bound(_markerRecordSids.begin(), _markerRecordSids.end(), sidLast);
    return static_cast<unsigned int>(it - _markerRecordSids.begin());
}

void SnapmakerPathLayerData::RefreshMoveAttributes(const GCodeProcessorResult& result, SnapmakerPathViewType viewType)
{
    _attributeRecords.assign(_groups.size() * 4, 0.0f);

    // ColorPrint / Summary: libvgcode turns the whole layer gray when it
    // contains a pause or custom-gcode option (the classic "color change"
    // visualization). Detect it once per layer, not once per move.
    bool layerHasOptionMove = false;
    if (viewType == SnapmakerPathViewType::Summary || viewType == SnapmakerPathViewType::ColorPrint) {
        for (const SnapmakerMoveNodeGroup& group : _groups) {
            const EMoveType type = result.moves[group.moveIndex].type;
            if (type == EMoveType::Pause_Print || type == EMoveType::Custom_GCode) {
                layerHasOptionMove = true;
                break;
            }
        }
    }

    for (size_t groupIndex = 0; groupIndex < _groups.size(); ++groupIndex) {
        const GCodeProcessorResult::MoveVertex& move = result.moves[_groups[groupIndex].moveIndex];
        const ColorAttrib attrib = ColorAttribOfMove(viewType, move, layerHasOptionMove, _layerTime);

        float* record = &_attributeRecords[groupIndex * 4];
        record[0] = float(move.type);
        record[1] = attrib.value;
        record[2] = attrib.paletteIndex;
        record[3] = float(static_cast<unsigned int>(attrib.source));
    }
    _attributesDirty = true;
}

void SnapmakerPathLayerData::UploadTables(const GCodeProcessorResult& result)
{
    // node table: xyz + group index (the group index is what the shaders use
    // to sample the width/height and attribute tables)
    if (_nodesDirty && !_nodes.empty()) {
        std::vector<float> data;
        data.reserve(_nodes.size() * 4);
        for (const SnapmakerPathNode& node : _nodes) {
            data.push_back(node.position.x());
            data.push_back(node.position.y());
            data.push_back(node.position.z());
            data.push_back(float(node.groupIndex));
        }
        _nodeTable.Upload(data, SnapmakerBufferTexture::EFormat::RGBA32F);
        _nodesDirty = false;
    }

    // width/height table, per group
    if (_widthHeightDirty && !_groups.empty()) {
        std::vector<float> data;
        data.reserve(_groups.size() * 2);
        for (const SnapmakerMoveNodeGroup& group : _groups) {
            const GCodeProcessorResult::MoveVertex& move = result.moves[group.moveIndex];
            const bool isTravel = (move.type == EMoveType::Travel);
            data.push_back(isTravel ? TRAVEL_SIZE : move.width);
            data.push_back(isTravel ? TRAVEL_SIZE : move.height);
        }
        _widthHeightTable.Upload(data, SnapmakerBufferTexture::EFormat::RG32F);
        _widthHeightDirty = false;
    }

    if (_attributesDirty && !_attributeRecords.empty()) {
        _attributeTable.Upload(_attributeRecords, SnapmakerBufferTexture::EFormat::RGBA32F);
        _attributesDirty = false;
    }

    if (_stepTablesDirty) {
        if (!_pathStepRecords.empty())
            _pathStepTable.Upload(_pathStepRecords, SnapmakerBufferTexture::EFormat::RGBA32F);
        if (!_markerStepRecords.empty())
            _markerStepTable.Upload(_markerStepRecords, SnapmakerBufferTexture::EFormat::RGBA32F);
        _stepTablesDirty = false;
    }
}

void SnapmakerPathLayerData::Reset()
{
    _z = 0.0f;
    _layerTime = 0.0f;
    _firstSid = 0;
    _lastSid = 0;
    _nodes.clear();
    _groups.clear();
    _steps.clear();
    _pathStepRecords.clear();
    _markerStepRecords.clear();
    _pathRecordSids.clear();
    _markerRecordSids.clear();
    _attributeRecords.clear();
    _nodeTable.Reset();
    _widthHeightTable.Reset();
    _attributeTable.Reset();
    _pathStepTable.Reset();
    _markerStepTable.Reset();
    _nodesDirty = true;
    _widthHeightDirty = true;
    _attributesDirty = true;
    _stepTablesDirty = true;
}

// ----------------------------------------------------------------------------
// SnapmakerPathLayerStack
// ----------------------------------------------------------------------------

void SnapmakerPathLayerStack::BuildFromResult(const GCodeProcessorResult& result, bool use_move_layer_ids)
{
    Reset();
    BuildLayers(result, use_move_layer_ids);
    if (_layers.empty())
        return;

    AssembleSteps(result);

    _layerWindow = { 0, static_cast<uint32_t>(_layers.size() - 1) };
    _moveWindow = { 0, static_cast<uint32_t>(_sidCount - 1) };
}

void SnapmakerPathLayerStack::BuildLayers(const GCodeProcessorResult& result, bool use_move_layer_ids)
{
    const size_t moveCount = result.moves.size();
    if (moveCount == 0)
        return;

    // sids are move indices with seam moves skipped, matching the space of
    // the libvgcode viewer ranges this fork uses
    _sidToMoveIndex.clear();
    _sidToMoveIndex.reserve(moveCount);
    // compact seam list: (sid, raw move index), naturally sorted by sid
    // (a per-move vector-of-vectors would allocate ~24 bytes per move)
    _seamMovesBySid.clear();
    _sidCount = 0;

    uint32_t seamsCount = 0;
    uint32_t lastTravelSid = 0;
    uint32_t currentLayerId = INVALID_INDEX;

    for (size_t i = 0; i < moveCount; ++i) {
        const GCodeProcessorResult::MoveVertex& move = result.moves[i];

        if (move.type == EMoveType::Seam) {
            // a seam attaches to the sid of the move that follows it
            ++seamsCount;
            const uint32_t seamSid = (i >= seamsCount) ? static_cast<uint32_t>(i - seamsCount) : 0u;
            _seamMovesBySid.emplace_back(seamSid, static_cast<uint32_t>(i));
            continue;
        }

        const uint32_t sid = static_cast<uint32_t>(i - seamsCount);
        _sidToMoveIndex.push_back(static_cast<uint32_t>(i));
        _sidCount = sid + 1;

        if (use_move_layer_ids) {
            // Layer-id mode: libvgcode's Layers table is fed from
            // MoveVertex::layer_id, so grouping by the same id guarantees
            // that the viewer's layer slider indexes exactly the layers of
            // this stack.
            if (_layers.empty() || move.layer_id != currentLayerId) {
                currentLayerId = move.layer_id;
                auto layer = std::make_unique<SnapmakerPathLayerData>();
                layer->_z = move.position.z();
                layer->_firstSid = sid;
                layer->_lastSid = sid;
                _layers.push_back(std::move(layer));
            }
            else {
                _layers.back()->_lastSid = sid;
                // the layer z is the extrusion z (travels may still be a
                // z-hop from the previous layer)
                if (move.type == EMoveType::Extrude)
                    _layers.back()->_z = move.position.z();
            }
            continue;
        }

        // Snapmaker's original Z-scan grouping, kept for completeness
        if (move.type == EMoveType::Extrude) {
            const float z = move.position.z();
            const bool needsNewLayer = _layers.empty()
                || std::abs(_layers.back()->_z - z) > LAYER_Z_EPSILON;
            if (needsNewLayer) {
                auto layer = std::make_unique<SnapmakerPathLayerData>();
                layer->_z = z;
                layer->_firstSid = lastTravelSid;
                layer->_lastSid = sid;
                _layers.push_back(std::move(layer));
            }
            else {
                _layers.back()->_lastSid = sid;
            }
        }
        else if (move.type == EMoveType::Travel) {
            // every travel extends the layer; trailing travel chains
            // (e.g. z-hop: lift, xy move, lower) belong to the layer
            if (!_layers.empty())
                _layers.back()->_lastSid = sid;
            lastTravelSid = sid;
        }
    }

    if (use_move_layer_ids)
        return;

    // Snapmaker's original Z-scan grouping (used when layer ids are not
    // available). This GCodeProcessor version has no layer-endpoint table,
    // so the endpoint override of the upstream render engine is not needed.
}

void SnapmakerPathLayerStack::AssembleSteps(const GCodeProcessorResult& result)
{
    for (auto& layer : _layers) {
        if (layer->_lastSid < layer->_firstSid || layer->_lastSid >= _sidToMoveIndex.size())
            continue;

        const uint32_t sidSpan = layer->_lastSid - layer->_firstSid + 1;
        std::vector<uint32_t> sidToGroup(sidSpan, INVALID_INDEX);
        std::vector<std::vector<uint32_t>> seamGroupsOfSid(sidSpan);

        // the compact (sid, move) seam list is sorted: walk it with a cursor
        // while the sids ascend
        size_t seamCursor = std::lower_bound(_seamMovesBySid.begin(), _seamMovesBySid.end(),
            std::make_pair(layer->_firstSid, 0u)) - _seamMovesBySid.begin();

        // node groups, in sid order; the seam groups of a sid follow its own
        for (uint32_t sid = layer->_firstSid, offset = 0; sid <= layer->_lastSid; ++sid, ++offset) {
            const uint32_t moveIndex = _sidToMoveIndex[sid];
            sidToGroup[offset] = layer->AddMoveNodeGroup(moveIndex, result.moves[moveIndex]);
            while (seamCursor < _seamMovesBySid.size() && _seamMovesBySid[seamCursor].first == sid) {
                const uint32_t seamMoveIndex = _seamMovesBySid[seamCursor].second;
                const GCodeProcessorResult::MoveVertex& seamMove = result.moves[seamMoveIndex];
                seamGroupsOfSid[offset].push_back(layer->AddMoveNodeGroup(seamMoveIndex, seamMove));
                ++seamCursor;
            }
        }

        // steps connect consecutive sids; the step takes the attributes of
        // its ending move. The seam steps of a sid follow the step ending at
        // that sid.
        for (uint32_t sid = layer->_firstSid + 1, offset = 1; sid <= layer->_lastSid; ++sid, ++offset) {
            const uint32_t endGroupIndex = sidToGroup[offset];
            const GCodeProcessorResult::MoveVertex& move = result.moves[layer->_groups[endGroupIndex].moveIndex];

            SnapmakerPathStep step;
            step.firstGroup = sidToGroup[offset - 1];
            step.secondGroup = endGroupIndex;
            step.firstSid = sid - 1;
            step.secondSid = sid;
            step.type = move.type;
            step.role = move.extrusion_role;
            step.extruderId = move.extruder_id;
            layer->AppendStep(step);

            for (const uint32_t seamGroupIndex : seamGroupsOfSid[offset]) {
                const GCodeProcessorResult::MoveVertex& seamMove = result.moves[layer->_groups[seamGroupIndex].moveIndex];
                SnapmakerPathStep seamStep;
                seamStep.firstGroup = seamGroupIndex;
                seamStep.secondGroup = seamGroupIndex;
                seamStep.firstSid = sid;
                seamStep.secondSid = sid;
                seamStep.type = seamMove.type;
                seamStep.role = seamMove.extrusion_role;
                seamStep.extruderId = seamMove.extruder_id;
                layer->AppendStep(seamStep);
            }
        }

        layer->_nodesDirty = true;
        layer->_widthHeightDirty = true;
        layer->_attributesDirty = true;
        layer->_stepTablesDirty = true;
    }
}

void SnapmakerPathLayerStack::Reset()
{
    _layers.clear();
    _layerWindow = { 0, 0 };
    _moveWindow = { 0, 0 };
    _sidCount = 1;
    _anyVisibleSteps = false;

    // defaults matching libvgcode's default option visibility: extrusions
    // and seams visible, travel/wipe/options hidden; all roles visible
    _moveTypeVisibilityFlags = 0;
    _moveTypeVisibilityFlags |= 1u << static_cast<uint32_t>(EMoveType::Extrude);
    _moveTypeVisibilityFlags |= 1u << static_cast<uint32_t>(EMoveType::Seam);
    _roleVisibilityFlags = 0;
    for (uint32_t role = 0; role < static_cast<uint32_t>(erCount); ++role)
        _roleVisibilityFlags |= 1u << role;
    _viewType = SnapmakerPathViewType::FeatureType;
    _dirtyMask = 0xff;
    _sidToMoveIndex.clear();
    _seamMovesBySid.clear();
}

void SnapmakerPathLayerStack::SetLayerWindow(uint32_t first, uint32_t last)
{
    if (_layers.empty()) {
        _layerWindow = { 0, 0 };
        return;
    }
    first = std::min(first, static_cast<uint32_t>(_layers.size() - 1));
    last = std::min(last, static_cast<uint32_t>(_layers.size() - 1));
    if (first > last)
        std::swap(first, last);
    // pure window change: the render loop only iterates the window, no
    // per-layer table needs rebuilding
    _layerWindow = { first, last };
}

void SnapmakerPathLayerStack::SetMoveWindow(uint32_t first, uint32_t last)
{
    if (first > last)
        std::swap(first, last);
    if (_moveWindow != std::make_pair(first, last)) {
        _moveWindow = { first, last };
        MarkDirty(EDirtyFlag::MoveWindow);
    }
}

void SnapmakerPathLayerStack::SetMoveTypeVisible(EMoveType type, bool visible)
{
    const uint32_t bit = 1u << static_cast<uint32_t>(type);
    const bool changed = ((_moveTypeVisibilityFlags & bit) != 0) != visible;
    if (visible)
        _moveTypeVisibilityFlags |= bit;
    else
        _moveTypeVisibilityFlags &= ~bit;
    if (changed)
        MarkDirty(EDirtyFlag::Visibility);
}

bool SnapmakerPathLayerStack::IsMoveTypeVisible(EMoveType type) const
{
    return (_moveTypeVisibilityFlags & (1u << static_cast<uint32_t>(type))) != 0;
}

void SnapmakerPathLayerStack::SetRoleVisibilityFlags(uint32_t flags)
{
    if (_roleVisibilityFlags != flags) {
        _roleVisibilityFlags = flags;
        MarkDirty(EDirtyFlag::Visibility);
    }
}

bool SnapmakerPathLayerStack::IsRoleVisible(ExtrusionRole role) const
{
    return role < erCount && (_roleVisibilityFlags & (1u << static_cast<uint32_t>(role))) != 0;
}

void SnapmakerPathLayerStack::SetViewType(SnapmakerPathViewType viewType)
{
    if (_viewType != viewType) {
        _viewType = viewType;
        MarkDirty(EDirtyFlag::ViewType);
    }
}

void SnapmakerPathLayerStack::SetLayerTimes(const std::vector<float>& times)
{
    bool changed = false;
    for (size_t i = 0; i < _layers.size(); ++i) {
        const float time = (i < times.size()) ? times[i] : 0.0f;
        if (_layers[i]->_layerTime != time) {
            _layers[i]->_layerTime = time;
            changed = true;
        }
    }
    if (changed)
        MarkDirty(EDirtyFlag::ViewType);
}

void SnapmakerPathLayerStack::RefreshVisibleSteps()
{
    const bool needsRefresh = IsDirty(EDirtyFlag::Layers)
        || IsDirty(EDirtyFlag::Visibility)
        || IsDirty(EDirtyFlag::ViewType);
    if (!needsRefresh)
        return;

    for (auto& layer : _layers)
        layer->RefreshVisibleSteps(*this);

    // maintained here (inside the dirty guard) so it costs nothing per
    // frame: "any layer holds a visible path or marker record"
    _anyVisibleSteps = false;
    for (const auto& layer : _layers) {
        if (layer->PathStepRecordCount() > 0 || layer->MarkerStepRecordCount() > 0) {
            _anyVisibleSteps = true;
            break;
        }
    }

    ClearDirty(EDirtyFlag::Layers);
    ClearDirty(EDirtyFlag::Visibility);
    ClearDirty(EDirtyFlag::MoveWindow);
    // ViewType stays set until RefreshMoveAttributes() consumed it
}

void SnapmakerPathLayerStack::RefreshMoveAttributes(const GCodeProcessorResult& result)
{
    if (!IsDirty(EDirtyFlag::ViewType))
        return;

    for (auto& layer : _layers)
        layer->RefreshMoveAttributes(result, _viewType);

    ClearDirty(EDirtyFlag::ViewType);
}

} // namespace GUI
} // namespace Slic3r
