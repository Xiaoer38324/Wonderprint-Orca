#include "MixedColorMatchEngine.hpp"

#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleSelector.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

#include <boost/log/trivial.hpp>

namespace Slic3r { namespace GUI {

namespace {

// Largest state that can be stored in the paint stream; states above it cannot be
// represented as painted facets (see EnforcerBlockerType).
constexpr size_t kMaxPaintState = static_cast<size_t>(EnforcerBlockerType::ExtruderMax);

// Surface area of an indexed triangle set, in model units squared (mm²).
double its_surface_area(const indexed_triangle_set& its)
{
    double area = 0.0;
    for (size_t i = 0; i < its.indices.size(); ++i)
        area += double(its.facet_area(static_cast<int>(i)));
    return area;
}

// Translate a recipe component index (1-based position inside the engine's base palette)
// into the project filament id it refers to.
unsigned int project_component_id(unsigned int palette_index, const std::vector<unsigned int>& base_ids)
{
    return (palette_index >= 1 && palette_index <= base_ids.size()) ? base_ids[palette_index - 1] : 0u;
}

} // namespace

MixedColorMatchEngine::MixedColorMatchEngine(int min_component_percent, int max_component_percent)
    : m_min_component_percent(std::clamp(min_component_percent, 0, 50)),
      m_max_component_percent(std::clamp(max_component_percent, 50, 100))
{
}

// ---------------------------------------------------------------------------
// Painted area / model color collection
// ---------------------------------------------------------------------------

std::map<unsigned int, double> painted_area_per_extruder(const Model& model, bool include_base_colors)
{
    std::map<unsigned int, double> area;
    for (const ModelObject* mo : model.objects) {
        if (mo == nullptr)
            continue;
        for (const ModelVolume* mv : mo->volumes) {
            if (mv == nullptr || mv->type() != ModelVolumeType::MODEL_PART)
                continue;

            // Painted facet states. The unpainted remainder of the volume is not a paint
            // state; it prints in the volume's own filament (see below).
            double painted_area = 0.0;
            if (mv->is_mm_painted()) {
                std::vector<indexed_triangle_set> per_type;
                mv->mmu_segmentation_facets.get_facets(*mv, per_type);
                for (size_t state = 1; state < per_type.size() && state <= kMaxPaintState; ++state) {
                    const double state_area = its_surface_area(per_type[state]);
                    if (state_area > 0.0) {
                        area[static_cast<unsigned int>(state)] += state_area;
                        painted_area += state_area;
                    }
                }
            }

            // Per-volume filament assignment (multi-part models store one color per part).
            // `extruder_id()` falls back to the object's extruder when the volume has none,
            // so a shared base filament is attributed once per volume that prints with it.
            // Only the unpainted part of a painted volume belongs to that base filament.
            if (! include_base_colors)
                continue;
            const int base = mv->extruder_id();
            if (base <= 0)
                continue;
            const double surface   = its_surface_area(mv->mesh().its);
            const double base_area = mv->is_mm_painted() ? surface - painted_area : surface;
            if (base_area > 0.0)
                area[static_cast<unsigned int>(base)] += base_area;
        }
    }
    return area;
}

std::vector<MixModelColor> collect_model_colors_with_area(const Model&                    model,
                                                          const std::vector<std::string>& physical_colors,
                                                          const std::vector<std::string>& extra_virtual_colors,
                                                          bool                            include_base_colors,
                                                          size_t                          max_colors)
{
    std::vector<MixModelColor> colors;
    const auto area_by_extruder = painted_area_per_extruder(model, include_base_colors);
    if (area_by_extruder.empty() || max_colors == 0)
        return colors;

    struct Accumulator
    {
        std::string               hex;
        std::vector<unsigned int> extruder_ids;
        double                    area { 0.0 };
    };

    std::vector<Accumulator> accumulators;
    const size_t             num_physical = physical_colors.size();

    for (const auto& [extruder_id, area] : area_by_extruder) {
        if (area <= 0.0)
            continue;

        std::string hex;
        if (extruder_id >= 1 && extruder_id <= num_physical)
            hex = physical_colors[extruder_id - 1];
        else if (extruder_id > num_physical && extruder_id - num_physical - 1 < extra_virtual_colors.size())
            hex = extra_virtual_colors[extruder_id - num_physical - 1];
        if (hex.empty())
            continue;

        wxColour parsed;
        if (!try_parse_color_match_hex(wxString::FromUTF8(hex.c_str()), parsed))
            continue;

        const std::string normalized = normalize_color_match_hex(wxString::FromUTF8(hex.c_str())).ToStdString();
        auto              it = std::find_if(accumulators.begin(), accumulators.end(),
                                           [&normalized](const Accumulator& acc) { return acc.hex == normalized; });
        if (it == accumulators.end()) {
            Accumulator acc;
            acc.hex = normalized;
            acc.extruder_ids.push_back(extruder_id);
            acc.area = area;
            accumulators.push_back(std::move(acc));
        } else {
            it->extruder_ids.push_back(extruder_id);
            it->area += area;
        }
    }

    // Biggest surfaces first so the per-run color cap keeps what matters most.
    std::stable_sort(accumulators.begin(), accumulators.end(),
                     [](const Accumulator& lhs, const Accumulator& rhs) { return lhs.area > rhs.area; });
    if (accumulators.size() > max_colors)
        accumulators.resize(max_colors);

    colors.reserve(accumulators.size());
    for (size_t i = 0; i < accumulators.size(); ++i) {
        MixModelColor color;
        color.color_index = static_cast<unsigned int>(i + 1);
        color.hex         = accumulators[i].hex;
        if (!try_parse_color_match_hex(wxString::FromUTF8(color.hex.c_str()), color.color))
            continue;
        color.extruder_ids = accumulators[i].extruder_ids;
        color.area         = accumulators[i].area;
        colors.push_back(std::move(color));
    }
    return colors;
}

// ---------------------------------------------------------------------------
// Matching
// ---------------------------------------------------------------------------

MixMatchResult MixedColorMatchEngine::run(const std::vector<MixBaseFilament>& bases,
                                          const std::vector<MixModelColor>& colors,
                                          std::atomic<bool>*                cancel) const
{
    MixMatchResult result;
    if (bases.size() < 2) {
        result.error = "At least two base filaments are required";
        return result;
    }
    if (colors.empty()) {
        result.error = "No painted model colors to match";
        return result;
    }

    std::vector<unsigned int> base_ids;
    std::vector<std::string>  base_hex;
    std::vector<wxColour>     base_colors;
    base_ids.reserve(bases.size());
    base_hex.reserve(bases.size());
    base_colors.reserve(bases.size());
    for (const MixBaseFilament& base : bases) {
        wxColour parsed;
        if (!try_parse_color_match_hex(wxString::FromUTF8(base.hex.c_str()), parsed))
            continue;
        base_ids.push_back(base.filament_id);
        base_hex.push_back(base.hex);
        base_colors.push_back(parsed);
    }
    if (base_ids.size() < 2) {
        result.error = "Fewer than two valid base filament colors";
        return result;
    }

    result.bases = bases;
    result.colors.reserve(colors.size());

    double total_area   = 0.0;
    double weighted_sum = 0.0;
    double delta_e_sum  = 0.0;

    for (const MixModelColor& sample : colors) {
        if (cancel != nullptr && cancel->load()) {
            MixMatchResult cancelled;
            cancelled.cancelled = true;
            return cancelled;
        }

        MixedColorMatchRecipeResult recipe;
        bool                        is_pure = false;

        // Pure mapping: a model color that is (almost) one of the base filaments is
        // mapped directly to that filament instead of a degenerate 99/1 recipe.
        double best_delta_e = std::numeric_limits<double>::infinity();
        size_t best_index   = 0;
        for (size_t i = 0; i < base_colors.size(); ++i) {
            const double delta_e = color_delta_e00(sample.color, base_colors[i]);
            if (delta_e < best_delta_e) {
                best_delta_e = delta_e;
                best_index   = i;
            }
        }

        if (best_delta_e < 1.0) {
            recipe.valid         = true;
            recipe.component_a   = base_ids[best_index];
            recipe.component_b   = base_ids[best_index];
            recipe.mix_b_percent = 0;
            recipe.preview_color = base_colors[best_index];
            recipe.delta_e       = best_delta_e;
            is_pure              = true;
        } else {
            recipe = build_best_color_match_recipe(base_hex, sample.color, m_min_component_percent,
                                                   m_max_component_percent, /*check_compatible=*/false);
            if (!recipe.valid) {
                BOOST_LOG_TRIVIAL(debug) << "MixedColorMatchEngine: no recipe for painted color " << sample.hex
                                         << " (bases=" << base_hex.size() << ")";
                MixMatchResult failed;
                failed.error = "No valid recipe for a painted color";
                return failed;
            }

            // The recipe addresses the engine palette by position; re-address it with the
            // project filament ids so the result can be applied directly.
            recipe.component_a = project_component_id(recipe.component_a, base_ids);
            recipe.component_b = project_component_id(recipe.component_b, base_ids);
            if (!recipe.gradient_component_ids.empty()) {
                std::vector<unsigned int> component_ids =
                    MixedFilamentManager::decode_gradient_component_ids(recipe.gradient_component_ids);
                for (unsigned int& component_id : component_ids)
                    component_id = project_component_id(component_id, base_ids);
                recipe.gradient_component_ids = MixedFilamentManager::encode_gradient_component_ids(component_ids);
            }
            // Re-score on the final preview color so every subset is compared with the same
            // ΔE00 definition (same normalization as the recipe search itself).
            recipe.delta_e = color_delta_e00(sample.color, recipe.preview_color);
        }

        MixColorResult color_result;
        color_result.sample        = sample;
        color_result.recipe        = recipe;
        color_result.matched_color = recipe.preview_color;
        color_result.delta_e       = recipe.delta_e;
        color_result.is_pure       = is_pure;

        total_area += sample.area;
        weighted_sum += sample.area * recipe.delta_e;
        delta_e_sum += recipe.delta_e;
        result.colors.push_back(std::move(color_result));
    }

    result.success    = !result.colors.empty();
    result.total_area = total_area;
    result.avg_delta_e = delta_e_sum / double(result.colors.size());
    result.weighted_delta_e = total_area > 0.0 ? weighted_sum / total_area : result.avg_delta_e;
    return result;
}

MixMatchResult MixedColorMatchEngine::run_best(const std::vector<MixBaseFilament>& bases,
                                               const std::vector<MixModelColor>& colors,
                                               std::atomic<bool>*                cancel) const
{
    BOOST_LOG_TRIVIAL(warning) << "MixedColorMatchEngine: run_best start (bases=" << bases.size()
                               << ", painted colors=" << colors.size() << ")";
    // 2..4 filaments: every filament is an ingredient, no enumeration needed.
    if (bases.size() <= kMixingBaseCount) {
        MixMatchResult direct = run(bases, colors, cancel);
        if (!direct.success)
            BOOST_LOG_TRIVIAL(warning) << "MixedColorMatchEngine: direct run failed: " << direct.error
                                       << " (cancelled=" << direct.cancelled << ")";
        return direct;
    }

    // Rank the filaments by the painted area they directly own. Filaments that are never
    // painted cannot help as base colors, and keeping the search on the most relevant ones
    // bounds C(N,4) for large spool counts.
    std::map<unsigned int, double> usage;
    for (const MixModelColor& color : colors)
        for (unsigned int extruder_id : color.extruder_ids)
            usage[extruder_id] += color.area;

    std::vector<MixBaseFilament> ranked = bases;
    std::stable_sort(ranked.begin(), ranked.end(), [&usage](const MixBaseFilament& lhs, const MixBaseFilament& rhs) {
        const double lhs_usage = usage.count(lhs.filament_id) ? usage.at(lhs.filament_id) : 0.0;
        const double rhs_usage = usage.count(rhs.filament_id) ? usage.at(rhs.filament_id) : 0.0;
        if (lhs_usage != rhs_usage)
            return lhs_usage > rhs_usage;
        return lhs.filament_id < rhs.filament_id;
    });
    if (ranked.size() > kMaxEnumerationBases) {
        BOOST_LOG_TRIVIAL(warning) << "MixedColorMatchEngine: " << ranked.size()
                                   << " base filaments, enumerating only the " << kMaxEnumerationBases
                                   << " with the largest painted area";
        ranked.resize(kMaxEnumerationBases);
    }
    // Deterministic enumeration order regardless of the area ranking.
    std::sort(ranked.begin(), ranked.end(), [](const MixBaseFilament& lhs, const MixBaseFilament& rhs) {
        return lhs.filament_id < rhs.filament_id;
    });

    MixMatchResult best;
    size_t         evaluated = 0;
    for (size_t a = 0; a + 3 < ranked.size(); ++a) {
        for (size_t b = a + 1; b + 2 < ranked.size(); ++b) {
            for (size_t c = b + 1; c + 1 < ranked.size(); ++c) {
                for (size_t d = c + 1; d < ranked.size(); ++d) {
                    if (cancel != nullptr && cancel->load()) {
                        MixMatchResult cancelled;
                        cancelled.cancelled = true;
                        return cancelled;
                    }
                    ++evaluated;
                    const std::vector<MixBaseFilament> subset = { ranked[a], ranked[b], ranked[c], ranked[d] };
                    MixMatchResult                    candidate = run(subset, colors, cancel);
                    if (!candidate.success)
                        continue;
                    if (!best.success || candidate.weighted_delta_e < best.weighted_delta_e)
                        best = std::move(candidate);
                }
            }
        }
    }

    if (!best.success) {
        best.error = "No base subset produced a valid match";
        BOOST_LOG_TRIVIAL(warning) << "MixedColorMatchEngine: no base subset produced a valid match";
    }
    BOOST_LOG_TRIVIAL(warning) << "MixedColorMatchEngine: evaluated " << evaluated << " base subsets"
                               << (best.success ? (", best weighted ΔE=" + std::to_string(best.weighted_delta_e))
                                                : std::string(", no valid result"));
    return best;
}

}} // namespace Slic3r::GUI
