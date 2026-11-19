#ifndef slic3r_GUI_MultiNozzleSettingsDialog_hpp_
#define slic3r_GUI_MultiNozzleSettingsDialog_hpp_

#include <vector>

#include <wx/scrolwin.h>
#include <wx/textctrl.h>

#include "GUI_Utils.hpp"
#include "Widgets/ComboBox.hpp"
#include "Widgets/DialogButtons.hpp"
#include "Widgets/Label.hpp"

namespace Slic3r {
namespace GUI {

// Nozzle settings page.
//
// One nozzle per extruder: the dialog edits the diameter and the flow type of every nozzle. There is no
// mode switch any more: the multi nozzle diameter mode follows the diameters (different sizes enable it,
// all equal sizes disable it) and a warning dialog explains the consequence of a change. Filaments do not
// choose a nozzle: filament N is printed with nozzle N.
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
    void build_extruder_rows(wxWindow *parent, const std::vector<double> &diameters, const std::vector<int> &flow_types);
    // Live status: the mode the current combo selection will lead to.
    void update_mode_status();
    void apply_to_config();

    wxScrolledWindow *m_content{ nullptr };
    wxBoxSizer       *m_content_sizer{ nullptr };
    Label            *m_mode_status{ nullptr };
    Label            *m_mode_detail{ nullptr };
    // Debug: JSON the printer server would return. Read by the "update from printer server" action.
    wxTextCtrl       *m_mock_server_text{ nullptr };

    std::vector<ExtruderRow> m_extruder_rows;
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_MultiNozzleSettingsDialog_hpp_
