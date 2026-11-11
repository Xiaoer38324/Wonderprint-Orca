#ifndef slic3r_SnapmakerPathData_hpp_
#define slic3r_SnapmakerPathData_hpp_

// ============================================================================
// Snapmaker render pipeline - de-geometrized toolpath data
// ----------------------------------------------------------------------------
// Ported from Snapmaker OrcaSlicer (Snapmaker/OrcaSlicer, "Render engine build
// - v2.4.1", GUI/PathData.*). The Snapmaker pipeline never builds a vertex
// buffer for the toolpaths: it stores the raw path points plus small per-move
// attribute tables, and the vertex shader expands every segment into a prism
// at draw time. That is what removes the multi-gigabyte VBOs and the per-zoom
// CPU rebuilds of the classic GL vertex-buffer preview.
//
// Changes for Wonderprint-Orca:
//   * the class names are prefixed with Snapmaker* so the ported pipeline is
//     visible at every call site,
//   * layers can be built from GCodeProcessorResult::MoveVertex::layer_id so
//     the stack stays in 1:1 lockstep with the libvgcode viewer that owns the
//     sliders in this fork (the original Snapmaker build groups by Z and by
//     the ";Z:" endpoint table, which is not what libvgcode does here),
//   * the color attribute table now carries an explicit color source. This
//     keeps the colours identical to libvgcode's get_vertex_color() (roles,
//     tools, color-print palette, option colours, gradient views and the
//     grayed "layer contains pause/custom gcode" case), instead of the
//     Orca-specific palette rules used by Snapmaker's own viewer,
//   * the legacy render-path records used by Snapmaker only to compute the
//     bottom-slider endpoints were removed: libvgcode keeps owning the
//     sequential slider in Wonderprint-Orca.
// ============================================================================

#include "SnapmakerBufferTexture.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace Slic3r {
namespace GUI {

// View types of the Snapmaker pipeline. The numeric value is only used
// internally; the shaders receive an explicit per-move colour source instead
// of a view-type id, so adding a view does not require touching the shader.
enum class SnapmakerPathViewType : unsigned int
{
    Summary = 0,
    FeatureType,
    ColorPrint,
    Speed,
    ActualSpeed,
    Height,
    Width,
    VolumetricFlowRate,
    ActualVolumetricFlowRate,
    LayerTimeLinear,
    LayerTimeLogarithmic,
    FanSpeed,
    Temperature,
    PressureAdvance,
    Tool
};

// Colour source written into the per-move attribute table (attribute .w).
// The shader uses it to select the gradient LUT or one of the four palettes.
enum class SnapmakerColorSource : unsigned int
{
    Gradient = 0,      // value -> 256-texel gradient LUT
    ExtrusionRole = 1, // extrusion-role palette (index = ExtrusionRole)
    Tool = 2,          // tool palette (index = extruder id)
    ColorPrint = 3,    // color-print palette (index = cp_color_id)
    Option = 4,        // option palette (index = EMoveType)
    Constant = 5       // neutral / disabled gray
};

// One recorded toolpath point: world position plus the index of the move
// group it belongs to (the group index doubles as the per-move attribute
// index sampled by the shaders).
struct SnapmakerPathNode
{
    Vec3f position{ Vec3f::Zero() };
    uint32_t groupIndex{ 0 };
};

// All nodes produced by one move. A straight move contributes a single node;
// an arc move would contribute its interpolation-point chain followed by the
// end position. Wipe moves lift their nodes by half the wipe height.
// The nodes of a group are stored contiguously in the layer node array, so a
// group only needs its first node and its count (one vector per move would
// cost an allocation per move on multi-million-move gcode).
struct SnapmakerMoveNodeGroup
{
    uint32_t moveIndex{ 0 };  // index into GCodeProcessorResult::moves
    uint32_t firstNode{ 0 };  // index of the first node in the layer
    uint32_t nodeCount{ 0 };  // number of contiguous nodes owned by this move
};

// One drawable step of a path: the node groups it connects plus the
// attributes of its ending move. firstSid/secondSid are sequential ids in
// the same space as the libvgcode viewer's move indices (raw move indices
// with seam moves skipped).
struct SnapmakerPathStep
{
    uint32_t firstGroup{ 0 };
    uint32_t secondGroup{ 0 };
    uint32_t firstSid{ 0 };
    uint32_t secondSid{ 0 };
    EMoveType type{ EMoveType::Noop };
    ExtrusionRole role{ erNone };
    uint16_t extruderId{ 0 };
};

// Data tables of one printable layer: the raw path data (nodes, per-move
// groups, steps), the visibility-filtered GPU step records, and the buffer
// textures they get uploaded into. No geometry is ever generated here; the
// prism vertices are rebuilt in the vertex shader at draw time.
class SnapmakerPathLayerStack;

class SnapmakerPathLayerData
{
public:
    float Z() const { return _z; }
    uint32_t FirstSid() const { return _firstSid; }
    uint32_t LastSid() const { return _lastSid; }
    // Layer duration under the viewer's current time mode (Normal/Stealth),
    // pushed by the adapter from libvgcode's layer table.
    float LayerTime() const { return _layerTime; }

    const std::vector<SnapmakerPathNode>& Nodes() const { return _nodes; }
    const std::vector<SnapmakerMoveNodeGroup>& Groups() const { return _groups; }
    const std::vector<SnapmakerPathStep>& Steps() const { return _steps; }

    // Number of path / marker step records currently stored (one vec4 each).
    size_t PathStepRecordCount() const { return _pathStepRecords.size() / 4; }
    size_t MarkerStepRecordCount() const { return _markerStepRecords.size() / 4; }
    // Number of path step records whose sid is <= sidLast. The records are
    // ordered by sid, so this is the instance count for the sequential
    // playback window (a binary search, no table rebuild).
    unsigned int PathStepCountUpTo(uint32_t sidLast) const;
    unsigned int MarkerStepCountUpTo(uint32_t sidLast) const;

    const SnapmakerBufferTexture& NodeTable() const { return _nodeTable; }
    const SnapmakerBufferTexture& WidthHeightTable() const { return _widthHeightTable; }
    const SnapmakerBufferTexture& AttributeTable() const { return _attributeTable; }
    const SnapmakerBufferTexture& PathStepTable() const { return _pathStepTable; }
    const SnapmakerBufferTexture& MarkerStepTable() const { return _markerStepTable; }

    // Rebuilds the visible step list and the GPU step records for the given
    // visibility state (move types, roles, view type).
    void RefreshVisibleSteps(const SnapmakerPathLayerStack& stack);
    // Rebuilds the per-move attribute table for the current view type.
    void RefreshMoveAttributes(const GCodeProcessorResult& result, SnapmakerPathViewType viewType);
    // Uploads dirty tables; must be called with an active GL context.
    void UploadTables(const GCodeProcessorResult& result);

    void Reset();

private:
    // only the stack assembles layer content
    friend class SnapmakerPathLayerStack;

    uint32_t AddMoveNodeGroup(uint32_t moveIndex, const GCodeProcessorResult::MoveVertex& move);
    void AppendStep(const SnapmakerPathStep& step) { _steps.push_back(step); }
    uint32_t AddNode(const Vec3f& position, float zOffset, SnapmakerMoveNodeGroup& group);

    float _z{ 0.0f };
    float _layerTime{ 0.0f };
    uint32_t _firstSid{ 0 };
    uint32_t _lastSid{ 0 };

    std::vector<SnapmakerPathNode> _nodes;
    std::vector<SnapmakerMoveNodeGroup> _groups;
    std::vector<SnapmakerPathStep> _steps;

    // visibility-filtered step records, packed as vec4s:
    // path:   (startNode, endNode, hasPrevStep, prevStartNode)
    // marker: (node, node, 0, 0)
    std::vector<float> _pathStepRecords;
    std::vector<float> _markerStepRecords;
    // sid of each step record (records are ordered by sid), used to clip the
    // instanced draws to the sequential playback window
    std::vector<uint32_t> _pathRecordSids;
    std::vector<uint32_t> _markerRecordSids;
    // per-group attributes, packed as vec4:
    // (moveType, gradientValue, paletteIndex, colorSource)
    std::vector<float> _attributeRecords;

    SnapmakerBufferTexture _nodeTable;        // RGBA32F: xyz + group index
    SnapmakerBufferTexture _widthHeightTable; // RG32F: extrusion width + height, per group
    SnapmakerBufferTexture _attributeTable;   // RGBA32F: per-move attributes, per group
    SnapmakerBufferTexture _pathStepTable;    // RGBA32F: path step records
    SnapmakerBufferTexture _markerStepTable;  // RGBA32F: marker step records

    bool _nodesDirty{ true };
    bool _widthHeightDirty{ true };
    bool _attributesDirty{ true };
    bool _stepTablesDirty{ true };
};

// The layers of one print with the current visibility windows and dirty
// flags. Owns the per-layer data tables and provides the sequential-playback
// move window (in sid space) used to clip the top layer.
class SnapmakerPathLayerStack
{
public:
    enum class EDirtyFlag : unsigned char
    {
        Layers     = 1 << 0, // layer set rebuilt or layer window changed
        Visibility = 1 << 1, // move-type / role visibility changed
        MoveWindow = 1 << 2, // sequential move range changed (top layer clip)
        ViewType   = 1 << 3  // view type changed (attribute tables rebuild)
    };

    SnapmakerPathLayerStack() = default;

    SnapmakerPathLayerStack(const SnapmakerPathLayerStack&) = delete;
    SnapmakerPathLayerStack& operator=(const SnapmakerPathLayerStack&) = delete;

    // Splits the gcode result into layers and assembles nodes/groups/steps.
    // use_move_layer_ids == true groups the moves by
    // GCodeProcessorResult::MoveVertex::layer_id, which is the layer space
    // libvgcode uses in Wonderprint-Orca (the viewer's layer slider indexes
    // exactly these layers). false keeps Snapmaker's original Z-scan
    // behaviour.
    void BuildFromResult(const GCodeProcessorResult& result, bool use_move_layer_ids);
    void Reset();

    size_t LayerCount() const { return _layers.size(); }
    const SnapmakerPathLayerData& Layer(size_t index) const { return *_layers[index]; }
    SnapmakerPathLayerData& Layer(size_t index) { return *_layers[index]; }
    // The sequential playback window only clips the top layer of the visible
    // range; lower layers always render in full (and are dimmed by the
    // shader when the viewer asks for it).
    bool IsTopLayer(const SnapmakerPathLayerData& layer) const {
        return !_layers.empty() && &layer == _layers[_layerWindow.second].get();
    }

    // visibility windows
    void SetLayerWindow(uint32_t first, uint32_t last);
    void SetMoveWindow(uint32_t first, uint32_t last);
    std::pair<uint32_t, uint32_t> LayerWindow() const { return _layerWindow; }
    std::pair<uint32_t, uint32_t> MoveWindow() const { return _moveWindow; }

    // move-type visibility (defaults: Extrude and Seam visible, rest hidden,
    // matching libvgcode's default option visibility)
    void SetMoveTypeVisible(EMoveType type, bool visible);
    bool IsMoveTypeVisible(EMoveType type) const;
    // extrusion-role visibility flags (all bits set = all visible)
    void SetRoleVisibilityFlags(uint32_t flags);
    uint32_t RoleVisibilityFlags() const { return _roleVisibilityFlags; }
    bool IsRoleVisible(ExtrusionRole role) const;

    // view type (a SnapmakerPathViewType value)
    void SetViewType(SnapmakerPathViewType viewType);
    SnapmakerPathViewType ViewType() const { return _viewType; }

    // Per-layer durations of the viewer's current time mode (indexed like
    // the layer slider). The LayerTime* views sample these, exactly like
    // libvgcode does, instead of the Normal-mode move.layer_duration.
    void SetLayerTimes(const std::vector<float>& times);

    // Rebuilds the visible step lists of all layers when visibility or view
    // type changed. Cheap no-op otherwise.
    void RefreshVisibleSteps();
    // True when the last RefreshVisibleSteps() rebuild left at least one
    // visible path or marker record (feeds the viewer's no-render-path flag).
    bool AnyVisibleSteps() const { return _anyVisibleSteps; }
    // Rebuilds the per-move attribute tables of all layers when the view
    // type changed. Cheap no-op otherwise.
    void RefreshMoveAttributes(const GCodeProcessorResult& result);

    bool IsDirty(EDirtyFlag flag) const { return (_dirtyMask & static_cast<unsigned char>(flag)) != 0; }
    void MarkDirty(EDirtyFlag flag) { _dirtyMask |= static_cast<unsigned char>(flag); }
    void ClearDirty(EDirtyFlag flag) { _dirtyMask &= ~static_cast<unsigned char>(flag); }

    // raw move index of a sid (0 when out of range)
    uint32_t MoveIndexOfSid(uint32_t sid) const {
        return (sid < _sidToMoveIndex.size()) ? _sidToMoveIndex[sid] : 0;
    }

    // seam instances: (sid of the following move, raw move index), sorted by
    // sid
    const std::vector<std::pair<uint32_t, uint32_t>>& SeamMoves() const { return _seamMovesBySid; }

private:
    void BuildLayers(const GCodeProcessorResult& result, bool use_move_layer_ids);
    void AssembleSteps(const GCodeProcessorResult& result);

    std::vector<std::unique_ptr<SnapmakerPathLayerData>> _layers;
    std::pair<uint32_t, uint32_t> _layerWindow{ 0, 0 };
    std::pair<uint32_t, uint32_t> _moveWindow{ 0, 0 };

    uint32_t _moveTypeVisibilityFlags{ 0 };
    uint32_t _roleVisibilityFlags{ 0 };

    SnapmakerPathViewType _viewType{ SnapmakerPathViewType::FeatureType };
    unsigned char _dirtyMask{ 0xff };
    // recomputed inside RefreshVisibleSteps() when a rebuild runs
    bool _anyVisibleSteps{ false };
    // number of sids (move indices with seams skipped) in the current result
    uint32_t _sidCount{ 1 };

    // sid -> move index, and the seam moves sorted by their sid
    std::vector<uint32_t> _sidToMoveIndex;
    std::vector<std::pair<uint32_t, uint32_t>> _seamMovesBySid;
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_SnapmakerPathData_hpp_
