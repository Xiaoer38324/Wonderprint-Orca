#ifndef slic3r_GUI_NozzleConfig_hpp_
#define slic3r_GUI_NozzleConfig_hpp_

#include <string>
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

// ---------------------------------------------------------------------------------------------------
// Printer server nozzle data (debug)
// ---------------------------------------------------------------------------------------------------

// JSON the "Mock Server" box of the debug panel contains. Persisted in the application config so it
// survives a restart while the real printer server request is not implemented yet.
std::string mock_server_json();
void        set_mock_server_json(const std::string &json_text);

// Fetch the nozzle configuration JSON from the printer server.
// MOCK: there is no network request yet. This returns the text typed into the debug panel's "Mock
// Server" box. Only this function body has to be replaced by the real request later; every caller and
// the validation below can stay unchanged. Returns false and fills `error` when there is nothing to
// fetch.
bool fetch_nozzle_config_json(std::string &json_text, wxString &error);

// Parse and validate a firmware response. `nozzle_diameter` must be an array of supported nozzle sizes
// (0.2 / 0.4 / 0.6 / 0.8 mm) and its length must match the number of nozzles the printer carries.
// Values may be JSON numbers or numeric strings. Returns false and fills `error` with the reason on any
// malformed, out of range or mismatching input.
bool parse_nozzle_diameters(const std::string &json_text, std::vector<double> &diameters, wxString &error);

// "Update from printer server" button: fetch the JSON, validate it and apply the reported nozzle
// diameters to the current printer. Shows an error dialog with the reason and keeps the configuration
// untouched on failure. Returns true when the configuration was updated.
bool update_nozzle_configuration_from_server(wxWindow *parent);

// Check before printing that the configured nozzle diameters equal the ones the printer server reports.
// Shows an error dialog and returns false when they differ or when the server JSON cannot be validated.
// An empty Mock Server box means the mock is not configured and the check is skipped, so normal printing
// is not blocked while the real request is missing.
bool validate_nozzle_configuration_against_server(wxWindow *parent);

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_NozzleConfig_hpp_
