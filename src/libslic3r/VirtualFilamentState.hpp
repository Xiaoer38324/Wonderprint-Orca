#ifndef slic3r_VirtualFilamentState_hpp_
#define slic3r_VirtualFilamentState_hpp_

#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "MixedFilament.hpp"
#include "TriangleSelector.hpp"

namespace Slic3r {

class Model;
class PresetBundle;

// Orca: one Color Mixing ("virtual filament") row as pure data. The wxWidgets sidebar keeps only a
// *view* of this (colour square + label); every rule is implemented here.
struct VirtualFilamentRow
{
    MixedFilament definition;
    bool          incompatible = false; // the components are not printable together
};

// Orca: what a state transition changed in terms of filament numbering. The caller applies
// `base_extruder_map` to the scene and refreshes its widgets, it never decides anything itself.
struct VirtualFilamentTransition
{
    bool               rows_changed = false; // the row list and/or the numbering changed
    std::map<int, int> base_extruder_map;    // 1-based old filament id -> new id, 0 = filament gone
};

// Orca: one broken invariant of the scene / state pair, reported by VirtualFilamentState::validate().
struct VirtualFilamentStateIssue
{
    enum class Kind {
        BaseExtruderOutOfRange,   // an object / volume / layer range references a filament that does not exist
        PaintedStateOutOfRange,   // a painted state references a filament that does not exist
        PaintedStateNotPaintable, // a painted state is a valid filament id but does not fit into a facet state
        RowComponentOutOfRange,   // a Color Mixing row references a physical filament the printer does not have
    };

    Kind kind         = Kind::BaseExtruderOutOfRange;
    int  object_index = -1;
    int  volume_index = -1; // -1 = the object itself, -2 = a layer height range
    int  filament_id  = 0;
};

// Orca: pure state machine behind the Color Mixing section of the sidebar (Humble Object pattern).
// No wxWidgets and no GUI headers here: only standard containers and libslic3r data structures, so
// every rule (numbering, invalid row pruning, base filament remapping after a physical filament was
// deleted, ...) can be asserted from unit tests in seconds instead of clicking through the UI.
class VirtualFilamentState
{
public:
    VirtualFilamentState() = default;

    // ---- queries --------------------------------------------------------
    size_t num_physical() const { return m_num_physical; }
    size_t row_count() const { return m_rows.size(); }
    bool   empty() const { return m_rows.empty(); }
    size_t total_filaments() const { return m_num_physical + m_rows.size(); }
    const std::vector<VirtualFilamentRow> &rows() const { return m_rows; }

    const VirtualFilamentRow *row(size_t index) const;

    // 1-based id of the row, 0 when `index` is out of range. Virtual ids are laid out after the
    // physical filaments: num_physical + index + 1.
    int              virtual_id(size_t index) const;
    // Index of the row with virtual id `filament_id`, -1 when it is not an existing virtual id.
    int              index_of_virtual_id(int filament_id) const;
    bool             is_virtual_id(int filament_id) const;
    std::vector<int> virtual_ids() const;

    // Physical filaments referenced by the row (pattern aware).
    std::vector<unsigned int> referenced_filaments(size_t index) const;
    // Human readable composition, e.g. "F1*23%+F2*77%".
    std::string display_text(size_t index) const;
    // True when every referenced physical filament exists for the current physical count.
    bool fits_physical_filaments(size_t index) const;
    bool uses_physical_filament(size_t index, unsigned int filament_id) const;

    // ---- configuration --------------------------------------------------
    void set_num_physical(size_t num_physical) { m_num_physical = num_physical; }
    void set_rows(std::vector<VirtualFilamentRow> rows) { m_rows = std::move(rows); }
    void clear() { m_rows.clear(); }

    // Loads the project definitions (`mixed_filament_definitions`), dropping disabled/deleted rows.
    void load_definitions(const std::string &definitions, const std::vector<std::string> &colours);
    // Recomputes the derived ratios / display colours in place and returns the project definitions.
    std::string normalize(const std::vector<std::string> &colours);
    // Publishes the rows to the preset bundle and to the project config (thin wrapper).
    void publish(PresetBundle &preset_bundle, const std::vector<std::string> &colours) const;

    // ---- compatibility ---------------------------------------------------
    // `filament_types` are the printer's current per slot types. `fallback` is used for rows whose
    // slots have no type at all (the caller knows the preset based rule).
    void update_compatibility(const std::vector<std::string> &filament_types,
                              const std::function<bool(const MixedFilament &)> &fallback = {});

    static std::string type_family(std::string filament_type);

    // ---- transitions ----------------------------------------------------
    // Appends a row and returns its index.
    size_t on_row_added(const MixedFilament &definition);
    void   on_row_edited(size_t index, const MixedFilament &definition);
    // Removes one row. `replacement_filament` (a physical id) becomes the new base filament of the
    // objects that used this row; 0 means "fall back to the default filament".
    VirtualFilamentTransition on_row_removed(size_t index, int replacement_filament = 0);
    // A physical filament count change (printer switch, add/delete): rows that no longer fit are
    // dropped and the surviving ones are renumbered.
    VirtualFilamentTransition on_physical_count_changed(size_t new_physical_count);
    // A physical filament was deleted together with the rows that referenced it.
    // The returned `base_extruder_map` is keyed on the virtual ids as they are *after* the caller
    // has already shifted every id above the deleted filament by one (the object list does that).
    VirtualFilamentTransition on_physical_filament_deleted(unsigned int deleted_physical,
                                                           size_t       new_physical_count);
    // Drops the rows that no longer fit the *current* physical count. Unlike
    // on_physical_count_changed() the count itself does not change, so calling this repeatedly is a
    // no-op (idempotent) and never renumbers a surviving row.
    VirtualFilamentTransition on_rows_pruned();

    // ---- scene synchronisation (libslic3r Model only) -------------------
    // Applies an id map to the base filament of every object, volume and layer range. Missing keys
    // resolve like ModelVolume::extruder_id(): an object without a key uses filament 1, while volumes
    // and layer ranges inherit from the object (and are left key-less). Objects fall back to
    // filament 1 when their filament is gone; volumes and layer ranges inherit again.
    static bool apply_base_extruder_map(Model &model, const std::map<int, int> &id_map);
    // Applies a painted state map to all painted facets of the scene. `skip_object` is used by the
    // painting gizmo: the selected object paints through its live triangle selectors, so its facets
    // must not be touched here (they are written back from the selectors afterwards).
    static bool apply_painted_state_map(Model &model, const EnforcerBlockerStateMap &state_map,
                                        const ModelObject *skip_object = nullptr);

    static EnforcerBlockerStateMap identity_painted_state_map();
    // Painted counterpart of a `base_extruder_map`: painted state ids use the same numbering, so a
    // map that renumbers base filaments renumbers the painted facets as well (0 = unpaint). This is
    // the single entry point for every row change: the transition maps already compose the physical
    // count shift with the pruning/renumbering of the rows.
    static EnforcerBlockerStateMap painted_state_map_from_id_map(const std::map<int, int> &id_map);

    // Painted virtual states used by the scene (1-based ids above `physical_count`).
    static std::vector<unsigned int> painted_virtual_states(const Model &model, size_t physical_count);
    // Base filament ids referenced by the objects / volumes / layer ranges of `model`.
    static std::vector<int> base_extruder_refs(const Model &model);

    // Orca: positive validation of the scene / state pair. An empty result means every reference of
    // the scene points at an existing filament and every row fits the current physical count, so
    // tests can assert the invariant after any operation instead of hunting symptoms.
    std::vector<VirtualFilamentStateIssue> validate(const Model &model) const;

private:
    MixedFilamentManager make_manager() const;

    size_t                          m_num_physical = 0;
    std::vector<VirtualFilamentRow> m_rows;
};

} // namespace Slic3r

#endif // slic3r_VirtualFilamentState_hpp_
