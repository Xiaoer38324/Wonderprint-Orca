#include "VirtualFilamentState.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>

#include "Model.hpp"
#include "PresetBundle.hpp"

namespace Slic3r {

namespace {

bool fits_definition(const MixedFilament &mixed, size_t physical_count)
{
    if (physical_count < 2)
        return false;
    if (mixed.component_a < 1 || mixed.component_a > physical_count ||
        mixed.component_b < 1 || mixed.component_b > physical_count)
        return false;
    for (unsigned int id : MixedFilamentManager::decode_gradient_component_ids(mixed.gradient_component_ids, 64))
        if (id < 1 || id > physical_count)
            return false;
    const std::string pattern = MixedFilamentManager::normalize_manual_pattern(mixed.manual_pattern);
    if (!pattern.empty()) {
        for (const std::string &group : MixedFilamentManager::split_pattern_groups(pattern)) {
            for (const std::string &token : MixedFilamentManager::split_pattern_group_to_tokens(group, 64)) {
                const unsigned int id = MixedFilamentManager::physical_filament_from_token(token, mixed, 64);
                if (id < 1 || id > physical_count)
                    return false;
            }
        }
    }
    return true;
}

bool uses_definition(const MixedFilament &mixed, unsigned int filament_id)
{
    if (mixed.component_a == filament_id || mixed.component_b == filament_id)
        return true;
    for (unsigned int id : MixedFilamentManager::decode_gradient_component_ids(mixed.gradient_component_ids, 64))
        if (id == filament_id)
            return true;
    const std::string pattern = MixedFilamentManager::normalize_manual_pattern(mixed.manual_pattern);
    if (!pattern.empty()) {
        for (const std::string &group : MixedFilamentManager::split_pattern_groups(pattern)) {
            for (const std::string &token : MixedFilamentManager::split_pattern_group_to_tokens(group, 64)) {
                if (MixedFilamentManager::physical_filament_from_token(token, mixed, 64) == filament_id)
                    return true;
            }
        }
    }
    return false;
}

// Physical filaments referenced by a row. Cycle/pattern rows encode the participating filaments
// entirely in the pattern; component_a/component_b are only the defaults for the "1"/"2" tokens.
std::vector<unsigned int> referenced_filaments_of(const MixedFilament &mixed, size_t physical_count)
{
    std::vector<unsigned int> ids;
    const auto push = [&ids, physical_count](unsigned int id) {
        if (id >= 1 && id <= physical_count && std::find(ids.begin(), ids.end(), id) == ids.end())
            ids.push_back(id);
    };

    const std::string pattern = MixedFilamentManager::normalize_manual_pattern(mixed.manual_pattern);
    if (!pattern.empty()) {
        for (const std::string &group : MixedFilamentManager::split_pattern_groups(pattern))
            for (const std::string &token : MixedFilamentManager::split_pattern_group_to_tokens(group, physical_count))
                push(MixedFilamentManager::physical_filament_from_token(token, mixed, physical_count));
        return ids;
    }

    push(mixed.component_a);
    push(mixed.component_b);
    for (unsigned int id : MixedFilamentManager::decode_gradient_component_ids(mixed.gradient_component_ids, 64))
        push(id);
    return ids;
}

void shift_physical_ids(MixedFilament &mixed, unsigned int deleted_id)
{
    if (mixed.component_a > deleted_id)
        --mixed.component_a;
    if (mixed.component_b > deleted_id)
        --mixed.component_b;

    std::vector<unsigned int> ids =
        MixedFilamentManager::decode_gradient_component_ids(mixed.gradient_component_ids, 64);
    for (unsigned int &id : ids)
        if (id > deleted_id)
            --id;
    mixed.gradient_component_ids = MixedFilamentManager::encode_gradient_component_ids(ids);
}

// Human readable composition, e.g. "F1*23%+F2*77%".
std::string display_text_of(const MixedFilament &mixed)
{
    if (mixed.gradient_enabled || mixed.ui_mode == 3)
        return "F" + std::to_string(mixed.component_a) + "->F" + std::to_string(mixed.component_b);

    std::vector<unsigned int> ids;
    std::vector<double>      weights;

    const std::string normalized = MixedFilamentManager::normalize_manual_pattern(mixed.manual_pattern);
    if (!normalized.empty()) {
        for (const std::string &group : MixedFilamentManager::split_pattern_groups(normalized)) {
            for (const std::string &token : MixedFilamentManager::split_pattern_group_to_tokens(group, 64)) {
                const unsigned int id = MixedFilamentManager::physical_filament_from_token(token, mixed, 64);
                if (id < 1)
                    continue;
                auto it = std::find(ids.begin(), ids.end(), id);
                if (it == ids.end()) {
                    ids.push_back(id);
                    weights.push_back(1.0);
                } else {
                    weights[size_t(it - ids.begin())] += 1.0;
                }
            }
        }
    } else {
        ids = MixedFilamentManager::decode_gradient_component_ids(mixed.gradient_component_ids, 0);
        if (ids.size() < 2)
            ids = {mixed.component_a, mixed.component_b};

        std::string token;
        for (char c : mixed.gradient_component_weights) {
            if (c >= '0' && c <= '9') {
                token.push_back(c);
            } else if (!token.empty()) {
                weights.push_back(double(std::max(1, std::atoi(token.c_str()))));
                token.clear();
            }
        }
        if (!token.empty())
            weights.push_back(double(std::max(1, std::atoi(token.c_str()))));

        if (weights.size() != ids.size()) {
            weights.assign(ids.size(), 1.0);
            if (ids.size() == 2 && mixed.mix_b_percent > 0) {
                weights[0] = double(100 - std::clamp(mixed.mix_b_percent, 0, 100));
                weights[1] = double(std::clamp(mixed.mix_b_percent, 0, 100));
            }
        }
    }

    if (ids.size() < 2)
        return {};

    double total = 0.0;
    for (double w : weights)
        total += std::max(0.0, w);
    if (total <= 0.0)
        return {};

    std::vector<int> percentages(ids.size(), 0);
    int              sum = 0;
    for (size_t i = 0; i < ids.size(); ++i) {
        percentages[i] = int(std::lround(100.0 * std::max(0.0, weights[i]) / total));
        sum += percentages[i];
    }
    percentages.back() += 100 - sum;

    std::string out;
    for (size_t i = 0; i < ids.size(); ++i) {
        if (i > 0)
            out += "+";
        out += "F" + std::to_string(ids[i]) + "*" + std::to_string(percentages[i]) + "%";
    }
    return out;
}

} // namespace

const VirtualFilamentRow *VirtualFilamentState::row(size_t index) const
{
    return index < m_rows.size() ? &m_rows[index] : nullptr;
}

int VirtualFilamentState::virtual_id(size_t index) const
{
    return index < m_rows.size() ? int(m_num_physical + index + 1) : 0;
}

int VirtualFilamentState::index_of_virtual_id(int filament_id) const
{
    if (filament_id <= int(m_num_physical) || filament_id > int(total_filaments()))
        return -1;
    return filament_id - int(m_num_physical) - 1;
}

bool VirtualFilamentState::is_virtual_id(int filament_id) const
{
    return filament_id > int(m_num_physical) && filament_id <= int(total_filaments());
}

std::vector<int> VirtualFilamentState::virtual_ids() const
{
    std::vector<int> ids;
    ids.reserve(m_rows.size());
    for (size_t index = 0; index < m_rows.size(); ++index)
        ids.push_back(virtual_id(index));
    return ids;
}

std::vector<unsigned int> VirtualFilamentState::referenced_filaments(size_t index) const
{
    return index < m_rows.size() ? referenced_filaments_of(m_rows[index].definition, m_num_physical)
                                 : std::vector<unsigned int>{};
}

std::string VirtualFilamentState::display_text(size_t index) const
{
    return index < m_rows.size() ? display_text_of(m_rows[index].definition) : std::string{};
}

bool VirtualFilamentState::fits_physical_filaments(size_t index) const
{
    return index < m_rows.size() && fits_definition(m_rows[index].definition, m_num_physical);
}

bool VirtualFilamentState::uses_physical_filament(size_t index, unsigned int filament_id) const
{
    return index < m_rows.size() && uses_definition(m_rows[index].definition, filament_id);
}

MixedFilamentManager VirtualFilamentState::make_manager() const
{
    MixedFilamentManager manager;
    manager.mixed_filaments().reserve(m_rows.size());
    for (const VirtualFilamentRow &row : m_rows) {
        MixedFilament mixed = row.definition;
        mixed.custom        = true;
        mixed.enabled       = true;
        mixed.deleted       = false;
        manager.mixed_filaments().push_back(std::move(mixed));
    }
    return manager;
}

void VirtualFilamentState::load_definitions(const std::string &definitions, const std::vector<std::string> &colours)
{
    m_rows.clear();
    if (definitions.empty())
        return;

    MixedFilamentManager manager;
    manager.load_custom_entries(definitions, colours);
    for (const MixedFilament &mixed : manager.mixed_filaments()) {
        if (!mixed.enabled || mixed.deleted || mixed.component_a == 0 || mixed.component_b == 0)
            continue;
        VirtualFilamentRow row;
        row.definition = mixed;
        m_rows.push_back(row);
    }
}

std::string VirtualFilamentState::normalize(const std::vector<std::string> &colours)
{
    // Same authoritative cadence as PrintApply::load_mixed_filaments(): the dialog only stores the
    // blend percentage, the manager derives the integer cycle from it here (minority = 1 layer,
    // majority = round(major/minor), e.g. 23%/77% -> 1:3 -> "3444").
    MixedFilamentManager manager = make_manager();
    manager.apply_gradient_settings(0, 0.04f, 0.16f, false);
    manager.refresh_display_colors(colours);
    for (size_t index = 0; index < m_rows.size() && index < manager.mixed_filaments().size(); ++index)
        m_rows[index].definition = manager.mixed_filaments()[index];
    return manager.serialize_custom_entries();
}

void VirtualFilamentState::publish(PresetBundle &preset_bundle, const std::vector<std::string> &colours) const
{
    MixedFilamentManager manager = make_manager();
    manager.apply_gradient_settings(0, 0.04f, 0.16f, false);
    manager.refresh_display_colors(colours);
    preset_bundle.mixed_filaments = std::move(manager);
    if (ConfigOptionString *definitions = preset_bundle.project_config.option<ConfigOptionString>("mixed_filament_definitions", true))
        definitions->value = preset_bundle.mixed_filaments.serialize_custom_entries();
}

std::string VirtualFilamentState::type_family(std::string filament_type)
{
    std::transform(filament_type.begin(), filament_type.end(), filament_type.begin(),
                   [](unsigned char c) { return char(std::toupper(c)); });
    if (const size_t pos = filament_type.find('-'); pos != std::string::npos)
        filament_type.resize(pos);
    if (filament_type == "PCTG")
        filament_type = "PETG";
    return filament_type;
}

void VirtualFilamentState::update_compatibility(const std::vector<std::string> &filament_types,
                                                const std::function<bool(const MixedFilament &)> &fallback)
{
    for (VirtualFilamentRow &row : m_rows) {
        bool incompatible = false;
        bool type_checked = false;

        // Prefer the printer's current per slot types: this also sees unsaved filament type edits,
        // unlike the preset based fallback.
        if (filament_types.size() >= 2) {
            std::string family;
            for (unsigned int id : referenced_filaments_of(row.definition, filament_types.size())) {
                const std::string &type = filament_types[id - 1];
                if (type.empty())
                    continue;
                type_checked = true;
                const std::string candidate = type_family(type);
                if (family.empty()) {
                    family = candidate;
                } else if (candidate != family) {
                    incompatible = true;
                    break;
                }
            }
        }

        if (!type_checked && fallback)
            incompatible = !fallback(row.definition);

        row.incompatible = incompatible;
    }
}

size_t VirtualFilamentState::on_row_added(const MixedFilament &definition)
{
    VirtualFilamentRow row;
    row.definition = definition;
    m_rows.push_back(row);
    return m_rows.size() - 1;
}

void VirtualFilamentState::on_row_edited(size_t index, const MixedFilament &definition)
{
    if (index < m_rows.size())
        m_rows[index].definition = definition;
}

VirtualFilamentTransition VirtualFilamentState::on_row_removed(size_t index, int replacement_filament)
{
    VirtualFilamentTransition transition;
    if (index >= m_rows.size())
        return transition;

    // A replacement has to be a physical filament, anything else falls back to the default one.
    const int replacement = (replacement_filament >= 1 && size_t(replacement_filament) <= m_num_physical)
                                ? replacement_filament
                                : 0;
    for (size_t row_index = 0; row_index < m_rows.size(); ++row_index) {
        const int old_id = int(m_num_physical + row_index + 1);
        if (row_index == index)
            transition.base_extruder_map[old_id] = replacement;
        else if (row_index > index)
            transition.base_extruder_map[old_id] = old_id - 1;
    }

    m_rows.erase(m_rows.begin() + index);
    transition.rows_changed = true;
    return transition;
}

VirtualFilamentTransition VirtualFilamentState::on_physical_count_changed(size_t new_physical_count)
{
    VirtualFilamentTransition transition;

    std::vector<bool>               survives(m_rows.size(), false);
    std::vector<VirtualFilamentRow> survivors;
    survivors.reserve(m_rows.size());
    for (size_t index = 0; index < m_rows.size(); ++index) {
        survives[index] = fits_definition(m_rows[index].definition, new_physical_count);
        if (survives[index])
            survivors.push_back(m_rows[index]);
    }

    size_t new_index = 0;
    for (size_t index = 0; index < m_rows.size(); ++index) {
        const int old_id = int(m_num_physical + index + 1);
        transition.base_extruder_map[old_id] = survives[index] ? int(new_physical_count + new_index++ + 1) : 0;
    }
    transition.rows_changed = !transition.base_extruder_map.empty();

    m_rows         = std::move(survivors);
    m_num_physical = new_physical_count;
    return transition;
}

VirtualFilamentTransition VirtualFilamentState::on_rows_pruned()
{
    VirtualFilamentTransition transition;

    std::vector<bool>               survives(m_rows.size(), false);
    std::vector<VirtualFilamentRow> survivors;
    survivors.reserve(m_rows.size());
    for (size_t index = 0; index < m_rows.size(); ++index) {
        survives[index] = fits_definition(m_rows[index].definition, m_num_physical);
        if (survives[index])
            survivors.push_back(m_rows[index]);
    }
    // Nothing to prune: report no change, so a repeated call cannot renumber anything.
    if (survivors.size() == m_rows.size())
        return transition;

    size_t new_index = 0;
    for (size_t index = 0; index < m_rows.size(); ++index) {
        const int old_id = int(m_num_physical + index + 1);
        transition.base_extruder_map[old_id] = survives[index] ? int(m_num_physical + new_index++ + 1) : 0;
    }
    transition.rows_changed = true;

    m_rows = std::move(survivors);
    return transition;
}

VirtualFilamentTransition VirtualFilamentState::on_physical_filament_deleted(unsigned int deleted_physical,
                                                                            size_t       new_physical_count)
{
    VirtualFilamentTransition transition;

    // 1) Decide which rows disappear together with the physical filament.
    std::vector<bool> survives(m_rows.size(), false);
    for (size_t index = 0; index < m_rows.size(); ++index)
        survives[index] = !uses_definition(m_rows[index].definition, deleted_physical);

    // 2) The caller has already shifted every id above the deleted filament by one, so the current
    //    virtual ids are new_physical_count + index + 1.
    size_t new_index = 0;
    for (size_t index = 0; index < m_rows.size(); ++index) {
        const int old_id = int(new_physical_count + index + 1);
        transition.base_extruder_map[old_id] = survives[index] ? int(new_physical_count + new_index++ + 1) : 0;
    }
    transition.rows_changed = !transition.base_extruder_map.empty();

    // 3) Keep the survivors, renumbering their physical components.
    std::vector<VirtualFilamentRow> survivors;
    survivors.reserve(m_rows.size());
    for (size_t index = 0; index < m_rows.size(); ++index) {
        if (!survives[index])
            continue;
        VirtualFilamentRow row = m_rows[index];
        shift_physical_ids(row.definition, deleted_physical);
        survivors.push_back(std::move(row));
    }

    m_rows         = std::move(survivors);
    m_num_physical = new_physical_count;
    return transition;
}

bool VirtualFilamentState::apply_base_extruder_map(Model &model, const std::map<int, int> &id_map)
{
    if (id_map.empty())
        return false;

    bool changed = false;
    // Orca: an object without an explicit "extruder" key (or with 0) uses filament 1, the slicer
    // default - exactly like ModelVolume::extruder_id() resolves it. Such an object has to be
    // remapped too, otherwise remapping filament 1 changes the painted facets but not the base
    // filament, and erasing the painting silently falls back to the old filament 1.
    // Volumes and layer height ranges without a key inherit from their object, which is remapped
    // here as well, so they must stay key-less.
    auto remap = [&id_map, &changed](ModelConfig &config, bool is_object) {
        const bool has_key   = config.has("extruder");
        const int  own       = has_key ? config.extruder() : 0;
        const int  effective = own > 0 ? own : (is_object ? 1 : 0);
        if (effective <= 0)
            return; // inherits from the object

        const auto it = id_map.find(effective);
        if (it == id_map.end())
            return;

        const int target = it->second;
        if (target > 0) {
            if (own == target)
                return;
            config.set_key_value("extruder", new ConfigOptionInt(target));
        } else if (is_object) {
            // The filament is gone: fall back to the default one.
            if (!has_key || own == 0 || own == 1)
                return; // already the default
            config.set_key_value("extruder", new ConfigOptionInt(1));
        } else {
            if (own == 0)
                return; // already inheriting from the object
            config.erase("extruder");
        }
        changed = true;
    };

    for (ModelObject *object : model.objects) {
        remap(object->config, true);
        for (ModelVolume *volume : object->volumes)
            remap(volume->config, false);
        for (auto &layer_range : object->layer_config_ranges)
            remap(layer_range.second, false);
    }
    return changed;
}

bool VirtualFilamentState::apply_painted_state_map(Model &model, const EnforcerBlockerStateMap &state_map,
                                                   const ModelObject *skip_object)
{
    bool changed = false;
    for (ModelObject *object : model.objects) {
        if (object == skip_object)
            continue;
        for (ModelVolume *volume : object->volumes)
            if (!volume->mmu_segmentation_facets.empty()) {
                volume->mmu_segmentation_facets.remap_enforcer_block_types(*volume, EnforcerBlockerType::ExtruderMax, state_map);
                changed = true;
            }
    }
    return changed;
}

EnforcerBlockerStateMap VirtualFilamentState::identity_painted_state_map()
{
    EnforcerBlockerStateMap state_map;
    for (size_t state = 0; state < state_map.size(); ++state)
        state_map[state] = EnforcerBlockerType(int(state));
    return state_map;
}

EnforcerBlockerStateMap VirtualFilamentState::painted_state_map_from_id_map(const std::map<int, int> &id_map)
{
    EnforcerBlockerStateMap state_map = identity_painted_state_map();
    for (const auto &entry : id_map) {
        const int old_id = entry.first;
        if (old_id <= 0 || size_t(old_id) >= state_map.size())
            continue;
        const int new_id = entry.second;
        state_map[size_t(old_id)] =
            (new_id > 0 && new_id <= int(EnforcerBlockerType::ExtruderMax)) ? EnforcerBlockerType(new_id)
                                                                           : EnforcerBlockerType(0);
    }
    return state_map;
}

std::vector<unsigned int> VirtualFilamentState::painted_virtual_states(const Model &model, size_t physical_count)
{
    std::vector<unsigned int> states;
    for (const ModelObject *object : model.objects)
        for (const ModelVolume *volume : object->volumes)
            for (int filament : volume->get_extruders()) {
                const unsigned int state = unsigned(std::max(0, filament));
                if (state > physical_count && std::find(states.begin(), states.end(), state) == states.end())
                    states.push_back(state);
            }
    return states;
}

std::vector<VirtualFilamentStateIssue> VirtualFilamentState::validate(const Model &model) const
{
    std::vector<VirtualFilamentStateIssue> issues;

    const int total        = int(total_filaments());
    const int max_paintable = int(EnforcerBlockerType::ExtruderMax);

    auto check_base = [&issues, total](const ModelConfig &config, int object_index, int volume_index) {
        if (!config.has("extruder"))
            return;
        const int filament_id = config.extruder();
        if (filament_id <= 0 || filament_id <= total)
            return; // 0 = inherit, everything within the list is fine
        VirtualFilamentStateIssue issue;
        issue.kind         = VirtualFilamentStateIssue::Kind::BaseExtruderOutOfRange;
        issue.object_index = object_index;
        issue.volume_index = volume_index;
        issue.filament_id  = filament_id;
        issues.push_back(issue);
    };

    for (size_t object_index = 0; object_index < model.objects.size(); ++object_index) {
        const ModelObject *object = model.objects[object_index];
        check_base(object->config, int(object_index), -1);
        for (size_t volume_index = 0; volume_index < object->volumes.size(); ++volume_index) {
            const ModelVolume *volume = object->volumes[volume_index];
            check_base(volume->config, int(object_index), int(volume_index));
            if (volume->mmu_segmentation_facets.empty())
                continue;
            // Only the *painted* states are checked: get_extruders() also appends the base filament,
            // which is validated above and may legitimately be a virtual id above ExtruderMax.
            const std::vector<bool> &used_states = volume->mmu_segmentation_facets.get_data().used_states;
            for (size_t state = size_t(EnforcerBlockerType::NONE) + 1; state < used_states.size(); ++state) {
                if (!used_states[state])
                    continue;
                // Two different problems: the state references a filament that is gone (stale paint)
                // or it is a valid filament id that simply cannot be stored in a facet state.
                VirtualFilamentStateIssue::Kind kind;
                if (int(state) > total)
                    kind = VirtualFilamentStateIssue::Kind::PaintedStateOutOfRange;
                else if (int(state) > max_paintable)
                    kind = VirtualFilamentStateIssue::Kind::PaintedStateNotPaintable;
                else
                    continue;
                VirtualFilamentStateIssue issue;
                issue.kind         = kind;
                issue.object_index = int(object_index);
                issue.volume_index = int(volume_index);
                issue.filament_id  = int(state);
                issues.push_back(issue);
            }
        }
        for (const auto &layer_range : object->layer_config_ranges)
            check_base(layer_range.second, int(object_index), -2);
    }

    for (size_t index = 0; index < m_rows.size(); ++index) {
        if (fits_definition(m_rows[index].definition, m_num_physical))
            continue;
        VirtualFilamentStateIssue issue;
        issue.kind        = VirtualFilamentStateIssue::Kind::RowComponentOutOfRange;
        issue.filament_id = int(m_rows[index].definition.component_a);
        issues.push_back(issue);
    }
    return issues;
}

std::vector<int> VirtualFilamentState::base_extruder_refs(const Model &model)
{
    std::vector<int> refs;
    auto             push = [&refs](const ModelConfig &config) {
        if (!config.has("extruder"))
            return;
        const int id = config.extruder();
        if (std::find(refs.begin(), refs.end(), id) == refs.end())
            refs.push_back(id);
    };

    for (const ModelObject *object : model.objects) {
        push(object->config);
        for (const ModelVolume *volume : object->volumes)
            push(volume->config);
        for (const auto &layer_range : object->layer_config_ranges)
            push(layer_range.second);
    }
    return refs;
}

} // namespace Slic3r
