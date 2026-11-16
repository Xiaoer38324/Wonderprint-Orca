#include <catch2/catch_all.hpp>

#include "libslic3r/Model.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Slicing.hpp"
#include "libslic3r/VirtualFilamentState.hpp"

#include <vector>

using namespace Slic3r;

namespace {

// Four physical filaments, two Color Mixing rows:
//   F5 = F2 + F4, F6 = F1 + F2
MixedFilament make_mixed(unsigned int component_a, unsigned int component_b, int mix_b_percent = 50)
{
    MixedFilament mixed;
    mixed.component_a   = component_a;
    mixed.component_b   = component_b;
    mixed.mix_b_percent = mix_b_percent;
    mixed.custom        = true;
    mixed.enabled       = true;
    return mixed;
}

void set_extruder(ModelConfig &config, int extruder)
{
    config.set_key_value("extruder", new ConfigOptionInt(extruder));
}

VirtualFilamentState make_state(size_t num_physical)
{
    VirtualFilamentState state;
    state.set_num_physical(num_physical);
    state.on_row_added(make_mixed(2, 4));
    state.on_row_added(make_mixed(1, 2));
    return state;
}

std::vector<std::string> colours(size_t count)
{
    static const char *palette[] = {"#FF0000", "#00FF00", "#0000FF", "#FFFFFF", "#000000"};
    std::vector<std::string> result;
    for (size_t i = 0; i < count; ++i)
        result.push_back(palette[i % (sizeof(palette) / sizeof(palette[0]))]);
    return result;
}

} // namespace

TEST_CASE("Virtual filament numbering follows the physical filament count", "[VirtualFilament]")
{
    VirtualFilamentState state = make_state(4);

    REQUIRE(state.num_physical() == 4);
    REQUIRE(state.row_count() == 2);
    REQUIRE(state.total_filaments() == 6);

    REQUIRE(state.virtual_id(0) == 5);
    REQUIRE(state.virtual_id(1) == 6);
    REQUIRE(state.virtual_id(2) == 0);

    REQUIRE(state.index_of_virtual_id(5) == 0);
    REQUIRE(state.index_of_virtual_id(6) == 1);
    REQUIRE(state.index_of_virtual_id(4) == -1);
    REQUIRE(state.index_of_virtual_id(7) == -1);

    REQUIRE_FALSE(state.is_virtual_id(1));
    REQUIRE_FALSE(state.is_virtual_id(4));
    REQUIRE(state.is_virtual_id(5));
    REQUIRE(state.is_virtual_id(6));
    REQUIRE_FALSE(state.is_virtual_id(7));

    REQUIRE(state.virtual_ids() == std::vector<int>{5, 6});
}

TEST_CASE("Removing a Color Mixing row renumbers the later rows", "[VirtualFilament]")
{
    VirtualFilamentState state = make_state(4);

    // F5 = F2 + F4 is deleted: objects that used F6 move to F5, objects that used F5 fall back.
    const VirtualFilamentTransition transition = state.on_row_removed(0);

    REQUIRE(transition.rows_changed);
    REQUIRE(state.row_count() == 1);
    REQUIRE(state.virtual_id(0) == 5);
    REQUIRE(transition.base_extruder_map.size() == 2);
    REQUIRE(transition.base_extruder_map.at(5) == 0);
    REQUIRE(transition.base_extruder_map.at(6) == 5);

    // A physical replacement becomes the new base filament instead of the default one.
    VirtualFilamentState replaced = make_state(4);
    const VirtualFilamentTransition replace_transition = replaced.on_row_removed(1, 3);
    REQUIRE(replace_transition.base_extruder_map.size() == 1);
    REQUIRE(replace_transition.base_extruder_map.at(6) == 3);
    REQUIRE(replaced.row_count() == 1);
    REQUIRE(replaced.virtual_id(0) == 5);
}

TEST_CASE("A printer change prunes the Color Mixing rows that no longer fit", "[VirtualFilament]")
{
    // Switching from a 4 to a 3 filament printer: F5 = F2 + F4 cannot be printed any more.
    VirtualFilamentState state = make_state(4);
    const VirtualFilamentTransition transition = state.on_physical_count_changed(3);

    REQUIRE(state.num_physical() == 3);
    REQUIRE(state.row_count() == 1);
    REQUIRE(state.virtual_id(0) == 4);
    REQUIRE(transition.base_extruder_map.at(5) == 0);
    REQUIRE(transition.base_extruder_map.at(6) == 4);

    // Growing the filament count keeps every row and renumbers all of them.
    VirtualFilamentState grown = make_state(2);
    const VirtualFilamentTransition grow_transition = grown.on_physical_count_changed(4);
    REQUIRE(grown.row_count() == 2);
    REQUIRE(grown.virtual_id(0) == 5);
    REQUIRE(grown.virtual_id(1) == 6);
    REQUIRE(grow_transition.base_extruder_map.at(3) == 5);
    REQUIRE(grow_transition.base_extruder_map.at(4) == 6);
}

TEST_CASE("Deleting a physical filament drops the rows that use it", "[VirtualFilament]")
{
    // Four physical filaments, then F1 is deleted: F6 = F1 + F2 disappears, F5 = F2 + F4 survives
    // and its components are renumbered (F2 -> F1, F4 -> F3).
    VirtualFilamentState state = make_state(4);
    const VirtualFilamentTransition transition = state.on_physical_filament_deleted(1, 3);

    REQUIRE(state.num_physical() == 3);
    REQUIRE(state.row_count() == 1);
    REQUIRE(state.virtual_id(0) == 4);

    // The object list has already shifted the ids above F1 down, so F5 became 4 and F6 became 5.
    REQUIRE(transition.base_extruder_map.at(4) == 4);
    REQUIRE(transition.base_extruder_map.at(5) == 0);

    const VirtualFilamentRow *survivor = state.row(0);
    REQUIRE(survivor != nullptr);
    REQUIRE(survivor->definition.component_a == 1);
    REQUIRE(survivor->definition.component_b == 3);

    // Deleting a filament that no row uses keeps every row.
    VirtualFilamentState untouched = make_state(4);
    const VirtualFilamentTransition untouched_transition = untouched.on_physical_filament_deleted(3, 3);
    REQUIRE(untouched.row_count() == 2);
    REQUIRE(untouched_transition.base_extruder_map.at(4) == 4);
    REQUIRE(untouched_transition.base_extruder_map.at(5) == 5);
}

TEST_CASE("Base filament references follow the Color Mixing rows", "[VirtualFilament]")
{
    Model model;
    ModelObject *object = model.add_object();
    set_extruder(object->config, 6);
    ModelVolume *volume = object->add_volume(make_cube(10., 10., 10.), ModelVolumeType::MODEL_PART, false);
    set_extruder(volume->config, 6);
    ModelConfig layer_range_config;
    set_extruder(layer_range_config, 6);
    object->layer_config_ranges[std::make_pair(coordf_t(0.0), coordf_t(1.0))] = layer_range_config;

    ModelObject *second = model.add_object();
    set_extruder(second->config, 5);

    ModelObject *physical = model.add_object();
    set_extruder(physical->config, 2);

    REQUIRE(VirtualFilamentState::base_extruder_refs(model) == std::vector<int>{6, 5, 2});

    VirtualFilamentState state = make_state(4);
    // Remove the last row (F6 = F1 + F2): F6 falls back, F5 keeps its id, physical filaments are
    // never touched.
    const VirtualFilamentTransition transition = state.on_row_removed(1);
    REQUIRE(VirtualFilamentState::apply_base_extruder_map(model, transition.base_extruder_map));

    REQUIRE(object->config.extruder() == 1);
    REQUIRE_FALSE(volume->config.has("extruder")); // inherits the object again
    REQUIRE_FALSE(object->layer_config_ranges.begin()->second.has("extruder")); // no hour range override
    REQUIRE(second->config.extruder() == 5);
    REQUIRE(physical->config.extruder() == 2);
}

TEST_CASE("Painted virtual states are renumbered with the rows", "[VirtualFilament]")
{
    // Removing a row unpaints its state and shifts the later virtual state down. The transition map
    // is directly usable for the painted facets.
    VirtualFilamentState removed_state = make_state(4);
    const VirtualFilamentTransition removed_transition = removed_state.on_row_removed(0);
    const EnforcerBlockerStateMap removed =
        VirtualFilamentState::painted_state_map_from_id_map(removed_transition.base_extruder_map);
    REQUIRE(int(removed[5]) == 0);
    REQUIRE(int(removed[6]) == 5);
    REQUIRE(int(removed[1]) == 1);

    // "Delete and replaced with F3": the removed state is repainted with the replacement filament.
    VirtualFilamentState replaced_state = make_state(4);
    const EnforcerBlockerStateMap replaced = VirtualFilamentState::painted_state_map_from_id_map(
        replaced_state.on_row_removed(1, 3).base_extruder_map);
    REQUIRE(int(replaced[6]) == 3);
    REQUIRE(int(replaced[5]) == 5);

    // A physical count change renumbers the whole virtual range. The transition map is the single
    // source for both painted states and base filaments.
    VirtualFilamentState state = make_state(4);
    const VirtualFilamentTransition grow_transition = state.on_physical_count_changed(5);
    REQUIRE(grow_transition.base_extruder_map.at(5) == 6);
    REQUIRE(grow_transition.base_extruder_map.at(6) == 7);
    const EnforcerBlockerStateMap grown =
        VirtualFilamentState::painted_state_map_from_id_map(grow_transition.base_extruder_map);
    REQUIRE(int(grown[5]) == 6);
    REQUIRE(int(grown[6]) == 7);
    REQUIRE(int(grown[4]) == 4);
}

TEST_CASE("Color Mixing definitions survive a normalize/load round trip", "[VirtualFilament]")
{
    const std::vector<std::string> filament_colours = colours(4);

    VirtualFilamentState state;
    state.set_num_physical(4);
    state.on_row_added(make_mixed(2, 4, 50));
    state.on_row_added(make_mixed(1, 3, 30));
    const std::string definitions = state.normalize(filament_colours);
    REQUIRE_FALSE(definitions.empty());

    VirtualFilamentState reloaded;
    reloaded.set_num_physical(4);
    reloaded.load_definitions(definitions, filament_colours);

    REQUIRE(reloaded.row_count() == 2);
    REQUIRE(reloaded.virtual_id(0) == 5);
    REQUIRE(reloaded.virtual_id(1) == 6);
    REQUIRE(reloaded.row(0)->definition.component_a == 2);
    REQUIRE(reloaded.row(0)->definition.component_b == 4);
    REQUIRE(reloaded.row(1)->definition.component_a == 1);
    REQUIRE(reloaded.row(1)->definition.component_b == 3);
}

TEST_CASE("Color Mixing rows report whether they still fit the printer", "[VirtualFilament]")
{
    VirtualFilamentState state;
    state.set_num_physical(4);
    state.on_row_added(make_mixed(2, 4));
    state.on_row_added(make_mixed(2, 5)); // 5 is not a physical filament

    REQUIRE(state.fits_physical_filaments(0));
    REQUIRE_FALSE(state.fits_physical_filaments(1));
    REQUIRE(state.uses_physical_filament(0, 4));
    REQUIRE_FALSE(state.uses_physical_filament(0, 1));
    REQUIRE(state.uses_physical_filament(1, 5));

    // Pruning keeps the valid row only and drops the base reference of the invalid one.
    const VirtualFilamentTransition transition = state.on_physical_count_changed(4);
    REQUIRE(state.row_count() == 1);
    REQUIRE(state.virtual_id(0) == 5);
    REQUIRE(transition.base_extruder_map.at(5) == 5);
    REQUIRE(transition.base_extruder_map.at(6) == 0);
}

TEST_CASE("Pruning invalid rows is idempotent", "[VirtualFilament]")
{
    VirtualFilamentState state;
    state.set_num_physical(4);
    state.on_row_added(make_mixed(2, 4));
    state.on_row_added(make_mixed(2, 5)); // 5 is not a physical filament

    const VirtualFilamentTransition first = state.on_rows_pruned();
    REQUIRE(first.rows_changed);
    REQUIRE(first.base_extruder_map.size() == 2);
    REQUIRE(first.base_extruder_map.at(5) == 5);
    REQUIRE(first.base_extruder_map.at(6) == 0);
    REQUIRE(state.row_count() == 1);

    // Nothing left to prune: a repeated call must not renumber (or reset) the survivor, which is
    // what makes prune_invalid_filament_mixes() safe to call after on_filament_count_change().
    const VirtualFilamentTransition second = state.on_rows_pruned();
    REQUIRE_FALSE(second.rows_changed);
    REQUIRE(second.base_extruder_map.empty());
    REQUIRE(state.row_count() == 1);
    REQUIRE(state.virtual_id(0) == 5);
}

TEST_CASE("Painted states follow the base filament map", "[VirtualFilament]")
{
    // A surviving row keeps its id, a removed row unpaints its state and a renumbered row moves.
    const EnforcerBlockerStateMap state_map =
        VirtualFilamentState::painted_state_map_from_id_map({{4, 4}, {5, 0}, {6, 5}});
    REQUIRE(int(state_map[4]) == 4);
    REQUIRE(int(state_map[5]) == 0);
    REQUIRE(int(state_map[6]) == 5);
    REQUIRE(int(state_map[3]) == 3); // physical filaments are not touched
}

TEST_CASE("Scene validation reports every dangling filament reference", "[VirtualFilament]")
{
    VirtualFilamentState state = make_state(4); // total = 6 filaments

    SECTION("A legal scene has no issues")
    {
        Model model;
        ModelObject *object = model.add_object();
        set_extruder(object->config, 6);
        ModelVolume *part = object->add_volume(make_cube(10., 10., 10.), ModelVolumeType::MODEL_PART, false);
        set_extruder(part->config, 5);
        REQUIRE(state.validate(model).empty());
    }

    SECTION("Object, volume and layer range references are checked")
    {
        Model model;
        ModelObject *object = model.add_object();
        set_extruder(object->config, 7); // one past the last filament
        ModelVolume *part = object->add_volume(make_cube(10., 10., 10.), ModelVolumeType::MODEL_PART, false);
        set_extruder(part->config, 8);
        ModelConfig layer_range_config;
        set_extruder(layer_range_config, 9);
        object->layer_config_ranges[std::make_pair(coordf_t(0.0), coordf_t(1.0))] = layer_range_config;

        const std::vector<VirtualFilamentStateIssue> issues = state.validate(model);
        REQUIRE(issues.size() == 3);
        REQUIRE(issues[0].kind == VirtualFilamentStateIssue::Kind::BaseExtruderOutOfRange);
        REQUIRE(issues[0].volume_index == -1); // the object itself
        REQUIRE(issues[1].volume_index == 0);  // the model part
        REQUIRE(issues[2].volume_index == -2); // the layer height range
    }

    SECTION("A Color Mixing row that does not fit the physical filaments is checked")
    {
        Model model; // nothing else to check
        VirtualFilamentState broken;
        broken.set_num_physical(3);
        broken.on_row_added(make_mixed(2, 5)); // 5 is not a physical filament

        const std::vector<VirtualFilamentStateIssue> issues = broken.validate(model);
        REQUIRE(issues.size() == 1);
        REQUIRE(issues.front().kind == VirtualFilamentStateIssue::Kind::RowComponentOutOfRange);
    }
}

// The Color Mixing deletions renumber base filaments (a different operation than the painting
// tool's paint-only "remap filaments", which never touches the base filament at all).
TEST_CASE("A base filament map follows the implicit default filament", "[VirtualFilament]")
{
    // Freshly imported objects often have no "extruder" key at all; the effective filament is 1.
    Model model;
    ModelObject *object = model.add_object();
    ModelVolume *part = object->add_volume(make_cube(10., 10., 10.), ModelVolumeType::MODEL_PART, false);
    ModelVolume *inherit = object->add_volume(make_cube(5., 5., 5.), ModelVolumeType::PARAMETER_MODIFIER, false);
    set_extruder(inherit->config, 0); // explicit "inherit"
    object->layer_config_ranges[std::make_pair(coordf_t(0.0), coordf_t(1.0))] = ModelConfig{};

    REQUIRE_FALSE(object->config.has("extruder"));
    REQUIRE(part->extruder_id() == 1);

    // 1 -> 5 must materialize the object key. Without it the painted facets move to 5 but the base
    // filament stays at the implicit 1, so erasing the painting would silently go back to F1.
    std::map<int, int> id_map;
    id_map[1] = 5;
    REQUIRE(VirtualFilamentState::apply_base_extruder_map(model, id_map));

    REQUIRE(object->config.extruder() == 5);
    REQUIRE(part->extruder_id() == 5);
    REQUIRE_FALSE(part->config.has("extruder"));      // still inherits from the object
    REQUIRE(inherit->config.extruder() == 0);         // explicit "inherit" is preserved
    REQUIRE(inherit->extruder_id() == 5);             // ... and now follows the remapped object
    REQUIRE_FALSE(object->layer_config_ranges.begin()->second.has("extruder"));

    // A map that does not touch filament 1 must not materialize anything.
    Model untouched;
    ModelObject *plain = untouched.add_object();
    std::map<int, int> other_map;
    other_map[3] = 4;
    REQUIRE_FALSE(VirtualFilamentState::apply_base_extruder_map(untouched, other_map));
    REQUIRE_FALSE(plain->config.has("extruder"));
}

TEST_CASE("Scene validation separates stale and unpaintable painted states", "[VirtualFilament]")
{
    // A cube painted with state 3.
    Model model;
    ModelObject *object = model.add_object();
    ModelVolume *part = object->add_volume(make_cube(10., 10., 10.), ModelVolumeType::MODEL_PART, false);
    EnforcerBlockerStateMap paint = VirtualFilamentState::identity_painted_state_map();
    paint[0] = EnforcerBlockerType(3);
    part->mmu_segmentation_facets.remap_enforcer_block_types(*part, EnforcerBlockerType::ExtruderMax, paint);
    REQUIRE_FALSE(part->mmu_segmentation_facets.empty());

    // 6 filaments: the painted state 3 exists and is paintable.
    VirtualFilamentState state = make_state(4); // total = 6
    REQUIRE(state.validate(model).empty());

    // 2 filaments: the painted state 3 no longer exists -> stale paint (not "unpaintable").
    VirtualFilamentState shrunk;
    shrunk.set_num_physical(2);
    const std::vector<VirtualFilamentStateIssue> stale = shrunk.validate(model);
    REQUIRE(stale.size() == 1);
    REQUIRE(stale.front().kind == VirtualFilamentStateIssue::Kind::PaintedStateOutOfRange);
    REQUIRE(stale.front().filament_id == 3);
    REQUIRE(stale.front().volume_index == 0);
}

TEST_CASE("Compatibility flags follow the filament types", "[VirtualFilament]")
{
    VirtualFilamentState state = make_state(4);

    // PLA everywhere: both rows are printable.
    state.update_compatibility({"PLA", "PLA", "PLA", "PLA"});
    REQUIRE_FALSE(state.row(0)->incompatible);
    REQUIRE_FALSE(state.row(1)->incompatible);

    // F2 is PETG and the row mixes it with PLA -> incompatible; F6 mixes F1 (PLA) with F2 (PETG).
    state.update_compatibility({"PLA", "PETG", "PLA", "PLA"});
    REQUIRE(state.row(0)->incompatible);
    REQUIRE(state.row(1)->incompatible);

    // A row whose slots have no type at all uses the caller's fallback rule.
    state.update_compatibility({"", ""}, [](const MixedFilament &) { return false; });
    REQUIRE(state.row(0)->incompatible);
    REQUIRE(state.row(1)->incompatible);

    state.update_compatibility({"", ""}, [](const MixedFilament &) { return true; });
    REQUIRE_FALSE(state.row(0)->incompatible);
    REQUIRE_FALSE(state.row(1)->incompatible);
}
