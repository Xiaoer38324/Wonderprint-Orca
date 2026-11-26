#pragma once

// Standalone color-mixing match engine.
//
// This class is the single home of the matching algorithm that used to live inside
// MixedFilamentBatchDialog::launch_background_match(): for a set of base filament
// colors it searches, per painted model color, the best in-gamut recipe (two- or
// three-component mix) and reports the resulting ΔE.
//
// It also implements the automatic mode used by the "Auto Mixing Match" preference:
//   * 2..4 available base filaments -> all of them are used;
//   * more than 4 base filaments    -> every C(N,4) subset is evaluated and the subset
//                                      with the lowest model-area-weighted ΔE wins;
//   * a single filament             -> no mapping at all (the caller skips it).
//
// Weighted ΔE of one subset:
//   Σ (area_i / Σ area) * ΔE_i
// where area_i is the surface area (mm²) covered by model color i. Areas do not depend on the
// subset, so the caller computes them once and reuses them for every subset.
//
// The engine is pure computation: it never touches the preset bundle, the model or any
// wx UI state, so it can run off the UI thread.

#include "MixedColorMatchHelpers.hpp"

#include <atomic>
#include <limits>
#include <map>
#include <string>
#include <vector>

namespace Slic3r {
class Model;
} // namespace Slic3r

namespace Slic3r { namespace GUI {

// One physical filament usable as a mixing ingredient.
struct MixBaseFilament
{
    unsigned int filament_id { 0 }; // 1-based project filament id
    std::string  hex;
};

// One distinct model color, with the surface area it covers.
struct MixModelColor
{
    unsigned int              color_index { 0 }; // 1-based
    wxColour                  color;
    std::string               hex;
    std::vector<unsigned int> extruder_ids;      // filament ids that use this color (paint states and/or volume assignments)
    double                    area { 0.0 };      // mm² of surface with this color
};

// One model color -> recipe mapping produced by the engine.
struct MixColorResult
{
    MixModelColor               sample;
    MixedColorMatchRecipeResult recipe; // component ids are project filament ids
    wxColour                    matched_color;
    double                      delta_e { 0.0 };
    bool                        is_pure { false };
};

// Result of one full (subset) matching run.
struct MixMatchResult
{
    bool                         success { false };
    bool                         cancelled { false };
    std::string                  error;
    std::vector<MixBaseFilament> bases;
    std::vector<MixColorResult>  colors;
    double                       total_area { 0.0 };
    double                       weighted_delta_e { std::numeric_limits<double>::infinity() };
    double                       avg_delta_e { std::numeric_limits<double>::infinity() };
};

class MixedColorMatchEngine
{
public:
    static constexpr size_t kMixingBaseCount = 4;

    // Upper bound on the number of base filaments considered by the C(N,4) search:
    // C(12,4) = 495 subsets keeps the one-shot automatic match fast. With more filaments
    // loaded, the ones covering the least painted area are dropped (they cannot matter as
    // mixing ingredients anyway); the dropped count is logged.
    static constexpr size_t kMaxEnumerationBases = 12;

    MixedColorMatchEngine(int min_component_percent = 0, int max_component_percent = 100);

    // Manual-matching semantics: `bases` is the complete ingredient set (2..4 colors).
    MixMatchResult run(const std::vector<MixBaseFilament>& bases,
                       const std::vector<MixModelColor>& colors,
                       std::atomic<bool>*                cancel = nullptr) const;

    // Automatic mode: uses all bases when there are at most 4, otherwise evaluates every
    // C(N,4) subset (bounded by kMaxEnumerationBases) and returns the best one.
    MixMatchResult run_best(const std::vector<MixBaseFilament>& bases,
                            const std::vector<MixModelColor>& colors,
                            std::atomic<bool>*                cancel = nullptr) const;

private:
    int m_min_component_percent { 0 };
    int m_max_component_percent { 100 };
};

// Surface area (mm²) per filament id.
//
// With `include_base_colors == false` only facet paint states count (the classic painted
// model). With `include_base_colors == true` the volume's own filament assignment is added
// as well, which is how multi-part models store their colors: one MODEL_PART volume per
// color, each naming its filament through the volume's "extruder" option. The base color
// covers the unpainted remainder of a painted volume, or the whole surface of a volume that
// carries no paint at all; the painted part is subtracted so no area is counted twice.
// States above EnforcerBlockerType::ExtruderMax cannot be represented and are ignored.
std::map<unsigned int, double> painted_area_per_extruder(const Model& model, bool include_base_colors = false);

// Distinct model colors with their accumulated surface area, sorted by area (descending)
// and capped to `max_colors`.
// `physical_colors` are the colors the extruder ids 1..N referred to when the model was
// saved (NOT necessarily the colors currently loaded). `extra_virtual_colors[i]` is the
// display color of the original virtual filament N+i+1; pass an empty vector when the
// project has no Color Mixing rows. This lets the caller match the original 3MF colors
// against a completely different filament configuration.
// `include_base_colors` also folds in per-volume filament assignments, see
// painted_area_per_extruder().
std::vector<MixModelColor> collect_model_colors_with_area(const Model&                    model,
                                                          const std::vector<std::string>& physical_colors,
                                                          const std::vector<std::string>& extra_virtual_colors,
                                                          bool                            include_base_colors = false,
                                                          size_t                          max_colors = 64);

}} // namespace Slic3r::GUI
