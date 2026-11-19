#ifndef slic3r_GUI_NozzleConfig_hpp_
#define slic3r_GUI_NozzleConfig_hpp_

#include <vector>

#include <wx/string.h>

class wxWindow;

namespace Slic3r {
namespace GUI {

// One nozzle per extruder. A nozzle is described by its diameter and its flow type; both are edited in
// the nozzle settings dialog and in the nozzle editor below the printer card in the sidebar.
struct NozzleConfiguration
{
    std::vector<double> diameters;
    std::vector<int>    flow_types;
};

// The four sizes every machine of this family ships with.
const std::vector<double> &nozzle_size_choices();

// Number of nozzles the selected printer carries (nozzle_diameter.size(), at least 1).
int nozzle_count();

// Current per-extruder configuration: diameters from the edited printer preset, flow types from the
// project (they are a project scope option in this fork) with the printer preset as fallback.
NozzleConfiguration current_nozzle_configuration();

bool nozzles_all_equal(const std::vector<double> &diameters);

// True when the plate is sliced for mixed nozzle diameters. Derived from the nozzle sizes, not from the
// stored mode flag: all equal sizes mean single diameter slicing.
bool multi_nozzle_mode_active();

// Status lines of the nozzle settings dialog.
wxString multi_nozzle_mode_status_text();
wxString multi_nozzle_mode_detail_text();

// Apply per-nozzle diameters and flow types. The multi nozzle diameter mode follows the diameters: a set
// of different sizes enables it, a set of equal sizes disables it. A change of the mode shows a warning
// dialog (which can be suppressed with "Do not ask me again"). The printer preset keeps the diameters,
// the project keeps the flow types and the mode flag.
void apply_nozzle_configuration(const NozzleConfiguration &configuration, wxWindow *parent);

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_NozzleConfig_hpp_
