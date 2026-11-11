#ifndef slic3r_SnapmakerPathView_hpp_
#define slic3r_SnapmakerPathView_hpp_

// ============================================================================
// Snapmaker render pipeline - libvgcode adapter
// ----------------------------------------------------------------------------
// Wonderprint-Orca keeps libvgcode as its preview data model / slider owner.
// This adapter builds the Snapmaker de-geometrized layer stack from the same
// GCodeProcessorResult and mirrors the libvgcode view state (view type,
// layer window, playback position, role/option visibility, palette and
// color ranges) into the Snapmaker renderer every frame.
//
// The adapter is the only file in this folder that depends on libvgcode; the
// ported Snapmaker pipeline itself (PathData / PathRenderer / shaders) stays
// independent from it.
// ============================================================================

#include "SnapmakerPathRenderer.hpp"

#include <cstdint>
#include <vector>

// libvgcode lives in the global namespace.
namespace libvgcode {
class Viewer;
}

namespace Slic3r {
class GCodeProcessorResult;

namespace GUI {

class SnapmakerPathView
{
public:
    SnapmakerPathView();
    ~SnapmakerPathView();

    SnapmakerPathView(const SnapmakerPathView&) = delete;
    SnapmakerPathView& operator=(const SnapmakerPathView&) = delete;

    // True when the GL context and the snapmaker_gpu_path shaders are usable.
    // The caller can still disable the pipeline through the
    // "gcode_preview_gpu_accel" app preference.
    static bool IsSupported();

    // Builds the de-geometrized layer stack for the given gcode result.
    // Must only be called for toolpath data (not for Print previews).
    void Load(const GCodeProcessorResult& result);
    void Reset();

    bool HasData() const { return _loaded && _stack.LayerCount() > 0; }

    // Mirrors the libvgcode viewer state into the Snapmaker stack/settings.
    // Returns false when the viewer state cannot be mapped (layer count
    // mismatch / no data), in which case the caller must fall back to
    // libvgcode's own renderer.
    bool SyncFromViewer(libvgcode::Viewer& viewer);

    // Draws the toolpaths with the Snapmaker pipeline. SyncFromViewer() must
    // have returned true for this frame. Returns false when the shaders are
    // unavailable, so the caller can fall back to libvgcode immediately.
    bool Render(const GCodeProcessorResult& result);

private:
    // sid of the move carrying the given gcode line id (sids skip seams).
    // Used to clip the top layer to the bottom-slider playback position.
    uint32_t SidOfGcodeId(uint32_t gcodeId) const;

    SnapmakerPathLayerStack _stack;
    SnapmakerPathRenderer _renderer;
    SnapmakerPathRenderer::ViewSettings _settings;

    bool _loaded{ false };
    bool _layerMappingFailed{ false };
    bool _layerTimesValid{ false };
    int _timeMode{ -1 };

    // raw move index -> gcode line id, and raw move index -> sid
    std::vector<uint32_t> _moveGcodeIds;
    std::vector<uint32_t> _moveSids;
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_SnapmakerPathView_hpp_
