#ifndef slic3r_FilamentRemapPlan_hpp_
#define slic3r_FilamentRemapPlan_hpp_

#include <map>
#include <vector>

#include "TriangleSelector.hpp"

namespace Slic3r {

class ModelObject;

// Orca: a problem found while validating a "remap filaments" request.
// Positive validation: a plan is usable only when `issues()` is empty, so the caller never applies
// half a remap and the UI can tell the user exactly what is wrong instead of silently skipping it.
struct FilamentRemapIssue
{
    enum class Kind {
        NoFilaments,        // nothing to remap against (empty / inconsistent filament list)
        SourceOutOfRange,   // the source id is not an existing filament
        TargetOutOfRange,   // the target id is not an existing filament (e.g. a deleted virtual row)
        TargetNotPaintable, // the target cannot be stored in a painted state (above ExtruderMax)
    };

    Kind kind   = Kind::NoFilaments;
    int  source = 0; // 1-based, 0 when the issue is not tied to one assignment
    int  target = 0; // 1-based, 0 when unknown
};

// Orca: pure description of the painting tool's "Remap filaments" operation (Humble Object: no
// wxWidgets, no dialogs, no globals). The assignment vector is 1-based:
//   assignments[source - 1] == target,  0 (or a missing entry) means "keep this filament".
//
// The remap is PAINT ONLY. It never changes the base filament of an object, a volume or a layer
// height range - "remap 1 -> 5" means "paint everything that currently shows filament 1 with
// filament 5". Erasing the painting afterwards therefore goes back to the original base filament
// and an unpainted model, which is exactly what the painting tool must do.
//
//   * painted state s -> target;
//   * unpainted facets (state 0) become painted with the target when the volume's base filament is
//     the remapped source, so a fully unpainted model is covered as well.
class FilamentRemapPlan
{
public:
    FilamentRemapPlan();

    static FilamentRemapPlan build(const std::vector<int> &assignments,
                                   size_t                  num_physical,
                                   size_t                  total_filaments);

    bool valid() const { return m_issues.empty(); }
    bool any_change() const { return m_any_change; }
    const std::vector<FilamentRemapIssue> &issues() const { return m_issues; }

    // 1-based source -> target for every changed assignment (identity entries omitted).
    const std::map<int, int> &targets() const { return m_targets; }

    // Painted state map for one volume. `base_filament` is the volume's base filament
    // (ModelVolume::extruder_id()) and decides whether unpainted facets get painted with the target.
    EnforcerBlockerStateMap painted_state_map_for(int base_filament) const;
    // Painted state map of the painted facets only (state 0 stays unpainted). Diagnostics / tests.
    const EnforcerBlockerStateMap &painted_state_map() const { return m_painted_state_map; }

    // Applies the remap to the painted facets of one object (paint only, model parts). The painting
    // gizmo does not use this for the object being edited - there the live triangle selectors are
    // authoritative - but it is the natural entry point for the other objects and for tests.
    bool apply(ModelObject &object) const;

private:
    std::vector<FilamentRemapIssue> m_issues;
    EnforcerBlockerStateMap         m_painted_state_map;
    std::map<int, int>              m_targets;
    bool                            m_any_change = false;
};

} // namespace Slic3r

#endif // slic3r_FilamentRemapPlan_hpp_
