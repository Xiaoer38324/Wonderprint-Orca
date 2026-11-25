#include "NozzleConfig.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <numeric>

#include <wx/sizer.h>

#include <nlohmann/json.hpp>

#include "GUI_App.hpp"
#include "GUI_Utils.hpp"
#include "I18N.hpp"
#include "MainFrame.hpp"
#include "MsgDialog.hpp"
#include "Plater.hpp"
#include "Tab.hpp"
#include "Widgets/CheckBox.hpp"
#include "Widgets/DialogButtons.hpp"
#include "Widgets/Label.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"

namespace Slic3r {
namespace GUI {

namespace {

const char *const APP_CONFIG_SECTION       = "multi_nozzle";
const char *const APP_CONFIG_NO_WARN_ON    = "no_warn_enable";
const char *const APP_CONFIG_NO_WARN_OFF   = "no_warn_disable";
const char *const APP_CONFIG_MOCK_JSON     = "mock_server_json";
constexpr double  NOZZLE_DIAMETER_EPSILON  = 1e-4;
// The firmware never reports more nozzles than a machine can carry; a longer array is a broken response.
constexpr size_t  MAX_SERVER_NOZZLES       = 16;

// Fallback when the application config is not available (for example very early in the startup).
std::string g_mock_server_json;

// Human readable diameter list for error messages: "0.4, 0.4, 0.8, 0.4".
wxString diameters_to_text(const std::vector<double> &diameters)
{
    wxString text;
    for (size_t i = 0; i < diameters.size(); ++i) {
        if (i > 0)
            text += ", ";
        text += wxString::FromUTF8(format_nozzle_diameter_to_str(diameters[i], 1).c_str());
    }
    return text;
}

void show_nozzle_server_error(wxWindow *parent, const wxString &message)
{
    wxWindow *dialog_parent = parent != nullptr ? parent : static_cast<wxWindow *>(wxGetApp().mainframe);
    MessageDialog dialog(dialog_parent, message, _L("Multi-head mixed printing server data error"), wxOK | wxICON_ERROR);
    dialog.ShowModal();
}

// Strict text to number conversion: the complete string has to be a number, no suffix or unit.
bool parse_strict_double(const std::string &text, double &value)
{
    if (text.empty())
        return false;
    char        *end   = nullptr;
    const double parsed = std::strtod(text.c_str(), &end);
    if (end == text.c_str())
        return false;
    while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')
        ++end;
    if (*end != '\0')
        return false;
    value = parsed;
    return true;
}

const DynamicPrintConfig &edited_printer_config()
{
    static const DynamicPrintConfig empty;
    PresetBundle                  *bundle = wxGetApp().preset_bundle;
    return bundle != nullptr ? bundle->printers.get_edited_preset().config : empty;
}

// The warning is a plain notice: the mode follows the nozzle sizes, the user is not asked to confirm it,
// only informed about the consequence of the size change.
class NozzleModeWarningDialog : public DPIDialog
{
public:
    NozzleModeWarningDialog(wxWindow *parent, bool enabling, const std::vector<double> &diameters)
        : DPIDialog(parent, wxID_ANY, _L("Nozzle diameter changed"), wxDefaultPosition, wxDefaultSize,
                    wxCAPTION | wxCLOSE_BOX)
    {
        SetBackgroundColour(*wxWHITE);
        SetFont(Label::Body_14);

        auto *main_sizer = new wxBoxSizer(wxVERTICAL);

        auto *title = new Label(this, enabling ? _L("Mixed nozzle diameters") : _L("Nozzle diameters are equal"));
        title->SetFont(Label::Head_14);
        main_sizer->Add(title, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(20));

        auto *message = new Label(this, enabling ?
            _L("The nozzle diameters are not all the same. Multi nozzle diameter mode will be enabled: the plate "
               "is sliced on the layer grid of the smallest nozzle and every nozzle merges the layers it covers "
               "into one thicker extrusion.") :
            _L("All nozzle diameters are the same now. Multi nozzle diameter mode will be disabled and the plate "
               "is sliced with a single nozzle diameter again."));
        message->SetFont(Label::Body_14);
        message->Wrap(FromDIP(460));
        main_sizer->Add(message, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(20));

        wxString sizes;
        for (size_t i = 0; i < diameters.size(); ++i) {
            if (i > 0)
                sizes += "\n";
            sizes += wxString::Format(_L("Nozzle %d"), int(i) + 1) + ": " +
                     wxString::FromUTF8(format_nozzle_diameter_to_str(diameters[i], 1).c_str()) + " mm";
        }
        if (!sizes.empty()) {
            auto *size_label = new Label(this, sizes);
            size_label->SetFont(Label::Body_12);
            size_label->SetForegroundColour(wxColour(107, 107, 107));
            main_sizer->Add(size_label, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(20));
        }

        auto *dont_ask_sizer = new wxBoxSizer(wxHORIZONTAL);
        m_dont_ask           = new ::CheckBox(this);
        auto *dont_ask_label = new Label(this, _L("Do not ask me again"));
        dont_ask_label->SetFont(Label::Body_14);
        dont_ask_sizer->Add(m_dont_ask, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
        dont_ask_sizer->Add(dont_ask_label, 0, wxALIGN_CENTER_VERTICAL);
        main_sizer->Add(dont_ask_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(20));

        auto *buttons = new DialogButtons(this, { "OK" });
        buttons->GetOK()->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { EndModal(wxID_OK); });
        main_sizer->Add(buttons, 0, wxEXPAND | wxALL, FromDIP(10));

        SetSizer(main_sizer);
        main_sizer->Fit(this);
        SetMinSize(wxSize(FromDIP(520), -1));
        CentreOnScreen();
    }

    bool dont_ask_again() const { return m_dont_ask != nullptr && m_dont_ask->GetValue(); }

private:
    void on_dpi_changed(const wxRect &) override {}

    ::CheckBox *m_dont_ask{ nullptr };
};

bool warning_suppressed(bool enabling)
{
    AppConfig *config = wxGetApp().app_config;
    return config != nullptr && config->get_bool(APP_CONFIG_SECTION, enabling ? APP_CONFIG_NO_WARN_ON : APP_CONFIG_NO_WARN_OFF);
}

void suppress_warning(bool enabling)
{
    if (AppConfig *config = wxGetApp().app_config; config != nullptr)
        config->set(APP_CONFIG_SECTION, enabling ? APP_CONFIG_NO_WARN_ON : APP_CONFIG_NO_WARN_OFF, true);
}

} // namespace

const std::vector<double> &nozzle_size_choices()
{
    static const std::vector<double> choices{ 0.2, 0.4, 0.6, 0.8 };
    return choices;
}

int nozzle_count()
{
    PresetBundle *bundle = wxGetApp().preset_bundle;
    if (bundle == nullptr)
        return 1;
    return std::max(1, bundle->get_printer_extruder_count());
}

NozzleConfiguration current_nozzle_configuration()
{
    NozzleConfiguration result;

    PresetBundle *bundle = wxGetApp().preset_bundle;
    if (bundle == nullptr) {
        result.diameters.push_back(0.4);
        result.flow_types.push_back(int(NozzleVolumeType::nvtStandard));
        return result;
    }

    const DynamicPrintConfig &printer_config = edited_printer_config();
    if (const ConfigOptionFloats *opt = printer_config.option<ConfigOptionFloats>("nozzle_diameter"))
        result.diameters = opt->values;
    if (result.diameters.empty())
        result.diameters.push_back(0.4);

    // nozzle_volume_type is a project scope option in this fork; the printer preset may still carry the
    // machine default, which is the fallback.
    if (const ConfigOptionEnumsGeneric *opt = bundle->project_config.option<ConfigOptionEnumsGeneric>("nozzle_volume_type"))
        result.flow_types = opt->values;
    if (result.flow_types.empty())
        if (const ConfigOptionEnumsGeneric *opt = printer_config.option<ConfigOptionEnumsGeneric>("nozzle_volume_type"))
            result.flow_types = opt->values;
    if (result.flow_types.empty())
        if (const ConfigOptionEnumsGeneric *opt = printer_config.option<ConfigOptionEnumsGeneric>("default_nozzle_volume_type"))
            result.flow_types = opt->values;

    const int default_flow = result.flow_types.empty() ? int(NozzleVolumeType::nvtStandard) : result.flow_types.front();
    result.flow_types.resize(result.diameters.size(), default_flow);
    return result;
}

bool nozzles_all_equal(const std::vector<double> &diameters)
{
    double first = 0.;
    for (double diameter : diameters) {
        if (diameter <= NOZZLE_DIAMETER_EPSILON)
            continue;
        if (first <= NOZZLE_DIAMETER_EPSILON)
            first = diameter;
        else if (std::abs(diameter - first) > NOZZLE_DIAMETER_EPSILON)
            return false;
    }
    return true;
}

bool multi_nozzle_mode_active()
{
    return !nozzles_all_equal(current_nozzle_configuration().diameters);
}

wxString multi_nozzle_mode_status_text()
{
    return multi_nozzle_mode_active() ? _L("Multi nozzle diameter mode: ON") : _L("Multi nozzle diameter mode: OFF");
}

wxString multi_nozzle_mode_detail_text()
{
    return multi_nozzle_mode_active() ?
        _L("The nozzles have different diameters. The plate is sliced for mixed nozzle diameters: each nozzle "
           "merges the layer grid at its own diameter.") :
        _L("All nozzles have the same diameter. The plate is sliced with a single nozzle diameter.");
}

void apply_nozzle_configuration(const NozzleConfiguration &configuration, wxWindow *parent)
{
    PresetBundle *bundle = wxGetApp().preset_bundle;
    if (bundle == nullptr)
        return;

    const int count = nozzle_count();
    std::vector<double> diameters = configuration.diameters;
    std::vector<int>    flow_types = configuration.flow_types;
    diameters.resize(size_t(count), diameters.empty() ? 0.4 : diameters.front());
    flow_types.resize(size_t(count), flow_types.empty() ? int(NozzleVolumeType::nvtStandard) : flow_types.front());

    const bool want_mode = !nozzles_all_equal(diameters);
    const bool mode_now  = multi_nozzle_diameter_mode_enabled(bundle->project_config);

    if (want_mode != mode_now && !warning_suppressed(want_mode)) {
        NozzleModeWarningDialog dialog(parent != nullptr ? parent : static_cast<wxWindow *>(wxGetApp().mainframe),
                                       want_mode, diameters);
        dialog.ShowModal();
        if (dialog.dont_ask_again())
            suppress_warning(want_mode);
    }

    // Printer scope: the nozzle size of every extruder. One nozzle per extruder, so the diameter list has
    // exactly one entry per extruder. This is the list nozzle_diameter_for_filament() reads.
    DynamicPrintConfig printer_config;
    printer_config.set_key_value("nozzle_diameter", new ConfigOptionFloats(diameters));
    bundle->printers.get_edited_preset().config.apply(printer_config);
    if (Tab *tab = wxGetApp().get_tab(Preset::TYPE_PRINTER)) {
        tab->update_dirty();
        tab->reload_config();
    }

    // Project scope: the mode switch, the flow type of every extruder, and the mirrored one-slot-per-extruder
    // description used by the ported multi-diameter helpers.
    DynamicPrintConfig project_config;
    project_config.set_key_value("multi_nozzle_diameter_mode", new ConfigOptionBool(want_mode));
    project_config.set_key_value("extruder_nozzle_count", new ConfigOptionInts(std::vector<int>(size_t(count), 1)));
    project_config.set_key_value("extruder_nozzle_diameter", new ConfigOptionFloats(diameters));
    std::vector<int> slots;
    slots.resize(size_t(count));
    std::iota(slots.begin(), slots.end(), 0);
    project_config.set_key_value("extruder_nozzle_slot", new ConfigOptionInts(slots));

    auto *nozzle_volume_types      = new ConfigOptionEnumsGeneric(flow_types);
    nozzle_volume_types->keys_map  = &ConfigOptionEnum<NozzleVolumeType>::get_enum_values();
    project_config.set_key_value("nozzle_volume_type", nozzle_volume_types);

    // Filament N prints with nozzle N. The legacy per-filament override is cleared so it cannot shadow the
    // nozzle the filament index points at.
    size_t filament_count = bundle->filament_presets.size();
    if (const ConfigOptionStrings *existing = bundle->project_config.option<ConfigOptionStrings>("filament_diameter_map"))
        filament_count = std::max(filament_count, existing->values.size());
    std::vector<std::string> cleared_filament_map(filament_count);
    project_config.set_key_value("filament_diameter_map", new ConfigOptionStrings(cleared_filament_map));

    bundle->project_config.apply(project_config);

    if (Plater *plater = wxGetApp().plater()) {
        // Push the complete merged config. auto_reslice=false: invalidate the plate and mark it dirty,
        // but leave the actual slicing to the user's Slice button.
        const DynamicPrintConfig full_config = bundle->full_config();
        plater->on_config_change(full_config, false);
        plater->set_plater_dirty(true);
    }
}

std::string mock_server_json()
{
    if (AppConfig *config = wxGetApp().app_config)
        return config->get(APP_CONFIG_SECTION, APP_CONFIG_MOCK_JSON);
    return g_mock_server_json;
}

void set_mock_server_json(const std::string &json_text)
{
    g_mock_server_json = json_text;
    if (AppConfig *config = wxGetApp().app_config)
        config->set(APP_CONFIG_SECTION, APP_CONFIG_MOCK_JSON, json_text);
}

bool fetch_nozzle_config_json(std::string &json_text, wxString &error)
{
    // MOCK: there is no printer server request yet. The JSON typed into the debug panel's "Mock Server"
    // box is returned as if the server had answered. Replace only the body of this function with the real
    // request later; the validation below and all callers stay unchanged.
    json_text = mock_server_json();
    if (json_text.find_first_not_of(" \t\r\n") == std::string::npos) {
        error = _L("Mock Server is empty. Enter the JSON the printer server would return in the Mock Server box of "
                   "the Multi-head mixed printing Debug panel.");
        return false;
    }
    return true;
}

bool parse_nozzle_diameters(const std::string &json_text, std::vector<double> &diameters, wxString &error)
{
    diameters.clear();

    nlohmann::json document;
    try {
        document = nlohmann::json::parse(json_text);
    } catch (const std::exception &exception) {
        error = _L("Cannot parse the  Server JSON: ") + wxString::FromUTF8(exception.what());
        return false;
    }

    if (! document.is_object()) {
        error = _L("The server response must be a JSON object that contains a nozzle_diameter array.");
        return false;
    }

    const auto nozzle_it = document.find("nozzle_diameter");
    if (nozzle_it == document.end()) {
        error = _L("The server response has no nozzle_diameter field.");
        return false;
    }
    if (! nozzle_it->is_array()) {
        error = _L("nozzle_diameter must be an array, for example [0.4, 0.4, 0.8, 0.4].");
        return false;
    }
    if (nozzle_it->empty()) {
        error = _L("The nozzle_diameter array is empty; no nozzle diameter can be applied.");
        return false;
    }
    if (nozzle_it->size() > MAX_SERVER_NOZZLES) {
        error = wxString::Format(_L("The nozzle_diameter array has %d entries, more than the %d nozzles a machine can carry."),
                                 int(nozzle_it->size()), int(MAX_SERVER_NOZZLES));
        return false;
    }

    const std::vector<double> &allowed = nozzle_size_choices();
    for (size_t index = 0; index < nozzle_it->size(); ++index) {
        const nlohmann::json &item = (*nozzle_it)[index];
        double                value = 0.;
        if (item.is_number()) {
            value = item.get<double>();
        } else if (item.is_string()) {
            if (! parse_strict_double(item.get<std::string>(), value)) {
                error = wxString::Format(_L("nozzle_diameter entry %d (\""), int(index) + 1) +
                        wxString::FromUTF8(item.get<std::string>().c_str()) + _L("\") is not a valid number.");
                return false;
            }
        } else {
            error = wxString::Format(_L("nozzle_diameter entry %d is not a number."), int(index) + 1);
            return false;
        }

        if (! std::isfinite(value) || value <= 0. || value > 10.) {
            error = wxString::Format(_L("nozzle_diameter entry %d value %.4g is out of range."), int(index) + 1, value);
            return false;
        }

        // Only the four sizes the machine family ships with are accepted; the reported value is snapped to
        // the exact supported size so a firmware rounding (0.40000001) does not poison the comparison.
        double snapped = 0.;
        bool   supported = false;
        for (double size : allowed) {
            if (std::abs(value - size) <= NOZZLE_DIAMETER_EPSILON) {
                snapped   = size;
                supported = true;
                break;
            }
        }
        if (! supported) {
            error = wxString::Format(
                _L("nozzle_diameter entry %d value %.4g mm is not a supported nozzle diameter (only 0.2 / 0.4 / 0.6 / 0.8 mm)."),
                int(index) + 1, value);
            return false;
        }
        diameters.push_back(snapped);
    }

    const int expected_count = nozzle_count();
    if (int(diameters.size()) != expected_count) {
        error = wxString::Format(_L("The server reports %d nozzles but the printer carries %d; the counts do not match."),
                                 int(diameters.size()), expected_count);
        return false;
    }
    return true;
}

bool update_nozzle_configuration_from_server(wxWindow *parent)
{
    std::string json_text;
    wxString    error;
    if (! fetch_nozzle_config_json(json_text, error)) {
        show_nozzle_server_error(parent, error);
        return false;
    }

    std::vector<double> diameters;
    if (! parse_nozzle_diameters(json_text, diameters, error)) {
        show_nozzle_server_error(parent, error);
        return false;
    }

    NozzleConfiguration configuration = current_nozzle_configuration();
    configuration.diameters = diameters;
    const int default_flow  = configuration.flow_types.empty() ? int(NozzleVolumeType::nvtStandard) :
                                                                 configuration.flow_types.front();
    configuration.flow_types.resize(diameters.size(), default_flow);
    apply_nozzle_configuration(configuration, parent);
    return true;
}

bool validate_nozzle_configuration_against_server(wxWindow *parent)
{
    // An empty Mock Server box means the mock is not set up. Normal printing must not be blocked until the
    // real printer-server request exists, so the check is skipped in that case.
    if (mock_server_json().find_first_not_of(" \t\r\n") == std::string::npos)
        return true;

    std::string json_text;
    wxString    error;
    if (! fetch_nozzle_config_json(json_text, error)) {
        show_nozzle_server_error(parent, error);
        return false;
    }

    std::vector<double> server_diameters;
    if (! parse_nozzle_diameters(json_text, server_diameters, error)) {
        show_nozzle_server_error(parent, error);
        return false;
    }

    const NozzleConfiguration current = current_nozzle_configuration();
    if (current.diameters.size() != server_diameters.size()) {
        error = wxString::Format(_L("Nozzle configuration does not match the server: the server reports %d nozzles ("),
                                 int(server_diameters.size())) +
                diameters_to_text(server_diameters) + _L("), the current configuration has ") +
                wxString::Format("%d", int(current.diameters.size())) + _L(" nozzles (") +
                diameters_to_text(current.diameters) + _L(").");
        show_nozzle_server_error(parent, error);
        return false;
    }

    for (size_t index = 0; index < server_diameters.size(); ++index) {
        if (std::abs(server_diameters[index] - current.diameters[index]) > NOZZLE_DIAMETER_EPSILON) {
            error = _L("Nozzle configuration does not match the server; printing was blocked.\n\nServer nozzles: ") +
                    diameters_to_text(server_diameters) + _L("\nCurrent configuration: ") +
                    diameters_to_text(current.diameters) +
                    _L("\n\nClick \"Update from printer server\" to synchronize the nozzle data, or correct the nozzle "
                       "diameters manually before printing.");
            show_nozzle_server_error(parent, error);
            return false;
        }
    }
    return true;
}

} // namespace GUI
} // namespace Slic3r
