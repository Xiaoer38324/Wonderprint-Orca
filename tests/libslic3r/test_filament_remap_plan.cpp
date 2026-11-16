#include <catch2/catch_all.hpp>

#include "libslic3r/FilamentRemapPlan.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Slicing.hpp"

#include <vector>

using namespace Slic3r;

namespace {

void set_extruder(ModelConfig &config, int extruder)
{
    config.set_key_value("extruder", new ConfigOptionInt(extruder));
}

// 4 physical filaments + 2 virtual rows (F5, F6): total 6.
constexpr size_t kNumPhysical = 4;
constexpr size_t kTotal        = 6;

std::vector<int> assignment(int source, int target)
{
    std::vector<int> assignments(kTotal, 0);
    if (source >= 1 && source <= int(kTotal))
        assignments[size_t(source) - 1] = target;
    return assignments;
}

} // namespace

TEST_CASE("A filament remap is a painting operation", "[FilamentRemap]")
{
    const FilamentRemapPlan plan = FilamentRemapPlan::build(assignment(1, 5), kNumPhysical, kTotal);

    REQUIRE(plan.valid());
    REQUIRE(plan.any_change());
    REQUIRE(plan.targets().size() == 1);
    REQUIRE(plan.targets().at(1) == 5);

    // The painted state 1 becomes 5; unpainted facets only get painted when the volume's base
    // filament is the remapped source (so "remap the whole filament" also covers them).
    REQUIRE(int(plan.painted_state_map()[1]) == 5);
    REQUIRE(int(plan.painted_state_map()[0]) == 0);
    REQUIRE(int(plan.painted_state_map()[2]) == 2);

    REQUIRE(int(plan.painted_state_map_for(1)[0]) == 5); // base 1 -> unpainted faces become painted 5
    REQUIRE(int(plan.painted_state_map_for(5)[0]) == 0); // other bases stay untouched
    REQUIRE(int(plan.painted_state_map_for(0)[0]) == 0); // no base information

    // Identity assignments are not a change.
    const std::vector<int> identity = {1, 2, 3, 4, 5, 6};
    const FilamentRemapPlan noop    = FilamentRemapPlan::build(identity, kNumPhysical, kTotal);
    REQUIRE(noop.valid());
    REQUIRE_FALSE(noop.any_change());
    REQUIRE(noop.targets().empty());
}

TEST_CASE("A filament remap rejects ids that do not exist", "[FilamentRemap]")
{
    // Target 7 does not exist (4 physical + 2 virtual = 6).
    const FilamentRemapPlan out_of_range = FilamentRemapPlan::build(assignment(2, 7), kNumPhysical, kTotal);
    REQUIRE_FALSE(out_of_range.valid());
    REQUIRE(out_of_range.issues().front().kind == FilamentRemapIssue::Kind::TargetOutOfRange);
    REQUIRE(out_of_range.issues().front().source == 2);
    REQUIRE(out_of_range.issues().front().target == 7);

    // The assignment list is longer than the filament list (stale panel after a filament deletion).
    std::vector<int> stale(kTotal + 2, 0);
    stale[kTotal + 1] = 1; // source 8
    const FilamentRemapPlan stale_plan = FilamentRemapPlan::build(stale, kNumPhysical, kTotal);
    REQUIRE_FALSE(stale_plan.valid());
    REQUIRE(stale_plan.issues().front().kind == FilamentRemapIssue::Kind::SourceOutOfRange);

    // Nothing to remap against.
    const FilamentRemapPlan empty = FilamentRemapPlan::build({}, 0, 0);
    REQUIRE_FALSE(empty.valid());
    REQUIRE(empty.issues().front().kind == FilamentRemapIssue::Kind::NoFilaments);
}

TEST_CASE("A filament remap can only target paint-able filament ids", "[FilamentRemap]")
{
    // 4 physical + 13 virtual rows = 17 filaments: id 17 cannot be stored as a painted state
    // (EnforcerBlockerType::ExtruderMax == 16), so a remap onto it is rejected.
    std::vector<int> assignments(17, 0);
    assignments[0] = 17; // source 1 -> 17
    const FilamentRemapPlan plan = FilamentRemapPlan::build(assignments, 4, 17);
    REQUIRE_FALSE(plan.valid());
    REQUIRE(plan.issues().front().kind == FilamentRemapIssue::Kind::TargetNotPaintable);
    REQUIRE(plan.issues().front().source == 1);
    REQUIRE(plan.issues().front().target == 17);
}

TEST_CASE("A filament remap never touches the base filament", "[FilamentRemap]")
{
    // The object and its part have no "extruder" key at all: the effective base filament is 1
    // (ModelVolume::extruder_id() default), exactly like a freshly imported model.
    Model model;
    ModelObject *object = model.add_object();
    ModelVolume *part = object->add_volume(make_cube(10., 10., 10.), ModelVolumeType::MODEL_PART, false);
    ModelVolume *modifier = object->add_volume(make_cube(5., 5., 5.), ModelVolumeType::PARAMETER_MODIFIER, false);
    ModelConfig layer_range_config;
    set_extruder(layer_range_config, 1);
    object->layer_config_ranges[std::make_pair(coordf_t(0.0), coordf_t(1.0))] = layer_range_config;

    REQUIRE_FALSE(object->config.has("extruder"));
    REQUIRE(part->extruder_id() == 1);

    const FilamentRemapPlan plan = FilamentRemapPlan::build(assignment(1, 3), kNumPhysical, kTotal);
    REQUIRE(plan.valid());
    REQUIRE(plan.apply(*object));

    // Paint was created on the model part (all of it, because its base filament is the source) ...
    REQUIRE_FALSE(part->mmu_segmentation_facets.empty());
    const std::vector<bool> &used = part->mmu_segmentation_facets.get_data().used_states;
    REQUIRE(used[3]);
    REQUIRE_FALSE(used[0]);
    REQUIRE_FALSE(used[1]);

    // ... while every base filament stayed exactly where it was: the remap is paint only.
    REQUIRE_FALSE(object->config.has("extruder"));
    REQUIRE_FALSE(part->config.has("extruder"));
    REQUIRE_FALSE(modifier->config.has("extruder"));
    REQUIRE(part->extruder_id() == 1);
    REQUIRE(modifier->extruder_id() == 1);
    REQUIRE(object->layer_config_ranges.begin()->second.extruder() == 1);
    REQUIRE(modifier->mmu_segmentation_facets.empty()); // modifiers carry no paint

    // Erasing all painting (what the "erase all" button does) returns to the untouched base
    // filament: this is the behaviour the remap has to guarantee.
    part->mmu_segmentation_facets.reset();
    REQUIRE(part->mmu_segmentation_facets.empty());
    REQUIRE(part->extruder_id() == 1);
}
