#ifndef slic3r_GUI_NozzleRackPanel_hpp_
#define slic3r_GUI_NozzleRackPanel_hpp_

#include <vector>

#include <wx/panel.h>

#include "Widgets/ComboBox.hpp"
#include "Widgets/Label.hpp"

namespace Slic3r {
namespace GUI {

// Nozzle editor below the printer card in the sidebar.
//
// One tab per nozzle (nozzle 1 .. nozzle N, N = number of print heads), and under the tabs the diameter
// and flow type of the selected nozzle. A change is applied immediately through apply_nozzle_configuration(),
// so the multi nozzle diameter mode and its warnings behave exactly like the nozzle settings dialog.
class NozzleRackPanel : public wxPanel
{
public:
    explicit NozzleRackPanel(wxWindow *parent);

    // Re-read the nozzle configuration and rebuild the tabs when the number of nozzles changed.
    void reload();
    void msw_rescale();

private:
    struct Tab
    {
        wxPanel *panel{ nullptr };
        Label   *label{ nullptr };
    };

    void build_content();
    void rebuild_tabs(size_t nozzle_count);
    void update_tab_styles();
    void select_nozzle(size_t index);
    void refresh_content();
    void on_diameter_changed(wxCommandEvent &evt);
    void on_flow_changed(wxCommandEvent &evt);
    bool apply_edit();

    wxBoxSizer       *m_tab_sizer{ nullptr };
    std::vector<Tab>  m_tabs;
    wxPanel          *m_content_panel{ nullptr };
    Label            *m_diameter_label{ nullptr };
    Label            *m_flow_label{ nullptr };
    ComboBox         *m_diameter_combo{ nullptr };
    ComboBox         *m_flow_combo{ nullptr };

    std::vector<double> m_diameters;
    std::vector<int>    m_flow_types;
    size_t              m_selected{ 0 };
    // Guards the combo/label updates done by reload() from being mistaken for a user edit.
    bool                m_updating{ false };
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_NozzleRackPanel_hpp_
