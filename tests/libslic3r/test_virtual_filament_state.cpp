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
