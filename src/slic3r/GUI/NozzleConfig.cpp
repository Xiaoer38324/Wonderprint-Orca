#include "NozzleConfig.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>

#include <wx/sizer.h>

#include "GUI_App.hpp"
#include "GUI_Utils.hpp"
#include "I18N.hpp"
#include "MainFrame.hpp"
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
constexpr double  NOZZLE_DIAMETER_EPSILON  = 1e-4;

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

} // namespace GUI
} // namespace Slic3r
