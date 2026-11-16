#include "FilamentRemapPlan.hpp"

#include <algorithm>

#include "Model.hpp"

namespace Slic3r {

namespace {

int max_paintable_state()
{
    return int(EnforcerBlockerType::ExtruderMax);
}

EnforcerBlockerStateMap make_identity_state_map()
{
    EnforcerBlockerStateMap state_map;
    for (size_t state = 0; state < state_map.size(); ++state)
        state_map[state] = EnforcerBlockerType(int(state));
    return state_map;
}

void add_issue(std::vector<FilamentRemapIssue> &issues, FilamentRemapIssue::Kind kind, int source, int target)
{
    FilamentRemapIssue issue;
    issue.kind   = kind;
    issue.source = source;
    issue.target = target;
    issues.push_back(issue);
}

} // namespace

FilamentRemapPlan::FilamentRemapPlan()
    : m_painted_state_map(make_identity_state_map())
{
}

FilamentRemapPlan FilamentRemapPlan::build(const std::vector<int> &assignments,
                                           size_t                  num_physical,
                                           size_t                  total_filaments)
{
    FilamentRemapPlan plan;
    if (num_physical == 0 || total_filaments == 0 || num_physical > total_filaments) {
        add_issue(plan.m_issues, FilamentRemapIssue::Kind::NoFilaments, 0, 0);
        return plan;
    }

    const int    max_paintable   = max_paintable_state();
    const size_t state_capacity  = plan.m_painted_state_map.size();
    const int    num_filaments   = int(total_filaments);

    for (size_t index = 0; index < assignments.size(); ++index) {
        const int source = int(index) + 1;
        const int target = assignments[index];
        if (target <= 0 || target == source)
            continue; // "keep this filament"

        if (source > num_filaments) {
            add_issue(plan.m_issues, FilamentRemapIssue::Kind::SourceOutOfRange, source, target);
            continue;
        }
        if (target > num_filaments) {
            add_issue(plan.m_issues, FilamentRemapIssue::Kind::TargetOutOfRange, source, target);
            continue;
        }
        // The remap is a paint operation: the target has to be stored in a painted state.
        if (target > max_paintable) {
            add_issue(plan.m_issues, FilamentRemapIssue::Kind::TargetNotPaintable, source, target);
            continue;
        }

        plan.m_any_change               = true;
        plan.m_targets[source]          = target;
        if (size_t(source) < state_capacity)
            plan.m_painted_state_map[size_t(source)] = EnforcerBlockerType(target);
    }
    return plan;
}

EnforcerBlockerStateMap FilamentRemapPlan::painted_state_map_for(int base_filament) const
{
    EnforcerBlockerStateMap state_map = m_painted_state_map;
    if (base_filament <= 0)
        return state_map;

    const auto it = m_targets.find(base_filament);
    if (it != m_targets.end() && it->second > 0 && it->second <= max_paintable_state())
        state_map[0] = EnforcerBlockerType(it->second);
    return state_map;
}

bool FilamentRemapPlan::apply(ModelObject &object) const
{
    if (!valid() || !m_any_change)
        return false;

    bool changed = false;
    for (ModelVolume *volume : object.volumes) {
        // Painting only exists on model parts; modifiers and support volumes have no facets.
        if (!volume->is_model_part())
            continue;
        const TriangleSelector::TriangleSplittingData before = volume->mmu_segmentation_facets.get_data();
        const EnforcerBlockerStateMap state_map = painted_state_map_for(volume->extruder_id());
        // A volume without any facets gets one built from the mesh here, so the "paint the unpainted
        // facets too" rule also covers a completely unpainted model part.
        volume->mmu_segmentation_facets.remap_enforcer_block_types(*volume, EnforcerBlockerType::ExtruderMax, state_map);
        if (!(volume->mmu_segmentation_facets.get_data() == before))
            changed = true;
    }
    return changed;
}

} // namespace Slic3r
