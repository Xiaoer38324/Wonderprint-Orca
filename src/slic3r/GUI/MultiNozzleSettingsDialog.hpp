#ifndef slic3r_GUI_MultiNozzleSettingsDialog_hpp_
#define slic3r_GUI_MultiNozzleSettingsDialog_hpp_

#include <vector>

#include <wx/scrolwin.h>

#include "GUI_Utils.hpp"
#include "Widgets/CheckBox.hpp"
#include "Widgets/ComboBox.hpp"
#include "Widgets/DialogButtons.hpp"
#include "Widgets/Label.hpp"

namespace Slic3r {
namespace GUI {

// Multi nozzle diameter configuration.
//
// Every extruder carries exactly one nozzle. The two parameters of that nozzle are its size (0.2 / 0.4 /
// 0.6 / 0.8 mm) and its flow type (Standard / High Flow):
//   nozzle_diameter     - one value per extruder, stored in the edited printer preset
//   nozzle_volume_type  - one NozzleVolumeType per extruder, a project option in this fork
// The project also keeps the mode switch, a mirrored one-slot-per-extruder description
// (extruder_nozzle_count / extruder_nozzle_diameter) for the ported multi-diameter helpers, and the optional
// per-filament size override (filament_diameter_map: empty = Auto = the mapped extruder's size). Which
// extruder a filament prints with is decided by the filament map.
class MultiNozzleSettingsDialog : public DPIDialog
{
public:
    explicit MultiNozzleSettingsDialog(wxWindow *parent = nullptr);
    ~MultiNozzleSettingsDialog() override = default;

private:
    struct ExtruderRow
    {
        ComboBox *diameter{ nullptr };
        ComboBox *flow{ nullptr };
    };

    void on_dpi_changed(const wxRect &suggested_rect) override {}

    void on_ok(wxCommandEvent &evt);

    void build_ui();
    void build_extruder_rows(wxWindow *parent, int extruder_count, const std::vector<double> &diameters,
                             const std::vector<int> &flow_types);
    // Per filament nozzle size override. Auto (the first entry) uses the nozzle size of the extruder the
    // filament is mapped to; any other entry overrides it for that filament only.
    void build_filament_rows(wxWindow *parent, const std::vector<std::string> &stored_map,
                             const std::vector<double> &extruder_diameters, const std::vector<int> &filament_map);
    void apply_to_config();

    wxScrolledWindow *m_content{ nullptr };
    wxBoxSizer       *m_content_sizer{ nullptr };
    CheckBox         *m_mode_checkbox{ nullptr };

    std::vector<ExtruderRow> m_extruder_rows;
    std::vector<ComboBox *>  m_filament_size_combos;
    // Nozzle sizes of the extruders when the dialog opened. Used to carry a per-filament override over
    // when its extruder's size is changed, instead of leaving the stale size behind.
    std::vector<double>      m_original_extruder_diameters;
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_MultiNozzleSettingsDialog_hpp_
