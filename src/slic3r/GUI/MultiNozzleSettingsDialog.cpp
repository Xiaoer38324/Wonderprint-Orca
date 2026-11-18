#include "MultiNozzleSettingsDialog.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

#include <wx/sizer.h>

#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MainFrame.hpp"
#include "Plater.hpp"
#include "Tab.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"

namespace Slic3r {
namespace GUI {

namespace {

// One nozzle per extruder: only these four sizes are offered.
const std::vector<double> &nozzle_size_choices()
{
    static const std::vector<double> choices{ 0.2, 0.4, 0.6, 0.8 };
    return choices;
}

wxString nozzle_size_label(double diameter)
{
    return wxString::FromUTF8(format_nozzle_diameter_to_str(diameter, 1).c_str());
}

// Map an arbitrary preset diameter (for example a legacy 0.5) onto the closest offered size.
int nearest_nozzle_size_index(double diameter)
{
    const std::vector<double> &choices = nozzle_size_choices();
    int                        best    = 0;
    for (size_t i = 1; i < choices.size(); ++i)
        if (std::abs(choices[i] - diameter) < std::abs(choices[best] - diameter))
            best = int(i);
    return best;
}

const DynamicPrintConfig &empty_config()
{
    static const DynamicPrintConfig config;
    return config;
}

const DynamicPrintConfig &edited_printer_config()
{
    PresetBundle *bundle = wxGetApp().preset_bundle;
    return bundle != nullptr ? bundle->printers.get_edited_preset().config : empty_config();
}

const DynamicPrintConfig &edited_project_config()
{
    PresetBundle *bundle = wxGetApp().preset_bundle;
    return bundle != nullptr ? bundle->project_config : empty_config();
}

} // namespace

MultiNozzleSettingsDialog::MultiNozzleSettingsDialog(wxWindow *parent)
    : DPIDialog(parent ? parent : static_cast<wxWindow *>(wxGetApp().mainframe), wxID_ANY,
                _L("Multi nozzle diameter settings"), wxDefaultPosition, wxDefaultSize, wxCAPTION | wxCLOSE_BOX)
{
    SetBackgroundColour(*wxWHITE);
    SetFont(Label::Body_14);
    build_ui();
    SetMinSize(wxSize(FromDIP(560), FromDIP(360)));
    // One row per extruder plus the switch and the hints; the dialog is not meant to be very tall.
    SetSize(wxSize(FromDIP(660), FromDIP(520)));
    Layout();
    // Opened from the sidebar / printer settings tab, whose client areas are narrower than the dialog.
    CentreOnScreen();
}

void MultiNozzleSettingsDialog::build_ui()
{
    PresetBundle *bundle = wxGetApp().preset_bundle;
    const int     extruder_count = bundle != nullptr ? std::max(1, bundle->get_printer_extruder_count()) : 1;

    std::vector<double> diameters;
    std::vector<int>    flow_types;
    const DynamicPrintConfig &printer_config = edited_printer_config();
    if (const ConfigOptionFloats *opt = printer_config.option<ConfigOptionFloats>("nozzle_diameter"))
        diameters = opt->values;
    m_original_extruder_diameters = diameters;
    // nozzle_volume_type is a project scope option in this fork (PresetBundle::project_config), the printer
    // preset only carries extruder_type. Read the project value first and fall back to the printer preset.
    if (const ConfigOptionEnumsGeneric *opt = edited_project_config().option<ConfigOptionEnumsGeneric>("nozzle_volume_type"))
        flow_types = opt->values;
    if (flow_types.empty())
        if (const ConfigOptionEnumsGeneric *opt = printer_config.option<ConfigOptionEnumsGeneric>("nozzle_volume_type"))
            flow_types = opt->values;
    if (flow_types.empty())
        if (const ConfigOptionEnumsGeneric *opt = printer_config.option<ConfigOptionEnumsGeneric>("default_nozzle_volume_type"))
            flow_types = opt->values;

    auto *main_sizer = new wxBoxSizer(wxVERTICAL);

    auto *mode_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_mode_checkbox  = new ::CheckBox(this);
    m_mode_checkbox->SetValue(multi_nozzle_diameter_mode_enabled(edited_project_config()));
    auto *mode_label = new Label(this, _L("Enable multi nozzle diameter mode"));
    mode_label->SetFont(Label::Body_14);
    mode_sizer->Add(m_mode_checkbox, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
    mode_sizer->Add(mode_label, 0, wxALIGN_CENTER_VERTICAL);
    main_sizer->Add(mode_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(15));

    auto *mode_hint = new Label(
        this, _L("Each extruder prints with a single nozzle. Its size is stored in the printer preset, its flow "
                 "type in the project; which extruder a filament uses is set by the filament mapping."));
    mode_hint->SetFont(Label::Body_12);
    mode_hint->SetForegroundColour(wxColour(107, 107, 107));
    mode_hint->Wrap(FromDIP(600));
    main_sizer->Add(mode_hint, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(15));

    m_content = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(620), FromDIP(300)), wxVSCROLL);
    m_content->SetScrollRate(0, FromDIP(10));
    m_content->SetBackgroundColour(*wxWHITE);
    m_content_sizer = new wxBoxSizer(wxVERTICAL);

    auto *rack_label = new Label(m_content, _L("Nozzle of every extruder"));
    rack_label->SetFont(Label::Body_14);
    m_content_sizer->Add(rack_label, 0, wxALIGN_LEFT | wxLEFT | wxRIGHT | wxTOP, FromDIP(10));

    auto *rack_hint = new Label(m_content,
                                _L("One nozzle per extruder: choose its size (0.2 / 0.4 / 0.6 / 0.8 mm) and its flow type."));
    rack_hint->SetFont(Label::Body_12);
    rack_hint->SetForegroundColour(wxColour(107, 107, 107));
    rack_hint->Wrap(FromDIP(580));
    m_content_sizer->Add(rack_hint, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(10));

    build_extruder_rows(m_content, extruder_count, diameters, flow_types);

    // Per filament override. Auto keeps the nozzle size of the extruder the filament is mapped to; a size
    // entered here overrides it for that filament only.
    auto *filament_label = new Label(m_content, _L("Nozzle size per filament"));
    filament_label->SetFont(Label::Body_14);
    m_content_sizer->Add(filament_label, 0, wxALIGN_LEFT | wxLEFT | wxRIGHT | wxTOP, FromDIP(10));

    auto *filament_hint = new Label(
        m_content, _L("Auto uses the nozzle size of the extruder the filament is mapped to. Select a size to override it "
                      "for this filament."));
    filament_hint->SetFont(Label::Body_12);
    filament_hint->SetForegroundColour(wxColour(107, 107, 107));
    filament_hint->Wrap(FromDIP(580));
    m_content_sizer->Add(filament_hint, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(10));

    std::vector<std::string> stored_filament_map;
    if (const ConfigOptionStrings *map = edited_project_config().option<ConfigOptionStrings>("filament_diameter_map"))
        stored_filament_map = map->values;
    std::vector<int> filament_map;
    if (const ConfigOptionInts *map = edited_project_config().option<ConfigOptionInts>("filament_map"))
        filament_map = map->values;

    // The Auto label shows the nozzle size currently selected for the mapped extruder.
    std::vector<double> editable_diameters;
    editable_diameters.reserve(m_extruder_rows.size());
    for (const ExtruderRow &row : m_extruder_rows) {
        const int selection = row.diameter != nullptr ? row.diameter->GetSelection() : 0;
        editable_diameters.push_back(selection >= 0 && size_t(selection) < nozzle_size_choices().size() ?
                                         nozzle_size_choices()[size_t(selection)] :
                                         0.4);
    }
    build_filament_rows(m_content, stored_filament_map, editable_diameters, filament_map);

    m_content->SetSizer(m_content_sizer);
    m_content->Layout();
    m_content->FitInside();
    main_sizer->Add(m_content, 1, wxEXPAND | wxALL, FromDIP(10));

    auto *buttons = new DialogButtons(this, { "OK", "Cancel" });
    main_sizer->Add(buttons, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));
    buttons->GetOK()->Bind(wxEVT_BUTTON, &MultiNozzleSettingsDialog::on_ok, this);
    buttons->GetCANCEL()->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { EndModal(wxID_CANCEL); });

    SetSizer(main_sizer);
    Layout();
}

void MultiNozzleSettingsDialog::build_extruder_rows(wxWindow *parent, int extruder_count,
                                                    const std::vector<double> &diameters,
                                                    const std::vector<int>    &flow_types)
{
    m_extruder_rows.clear();
    const std::vector<double> &sizes = nozzle_size_choices();

    for (int extruder_index = 0; extruder_index < extruder_count; ++extruder_index) {
        auto *row_sizer = new wxBoxSizer(wxHORIZONTAL);

        auto *extruder_label = new Label(parent, wxString::Format(_L("Extruder %d"), extruder_index + 1));
        extruder_label->SetFont(Label::Body_12);
        extruder_label->SetMinSize(wxSize(FromDIP(100), -1));
        row_sizer->Add(extruder_label, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(10));

        auto *size_label = new Label(parent, _L("Size"));
        size_label->SetFont(Label::Body_12);
        row_sizer->Add(size_label, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));

        auto *size_combo = new ComboBox(parent, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, 0, nullptr,
                                        wxCB_READONLY);
        size_combo->SetMinSize(wxSize(FromDIP(90), FromDIP(26)));
        for (double size : sizes)
            size_combo->Append(nozzle_size_label(size), wxNullBitmap);
        const double current_diameter = size_t(extruder_index) < diameters.size() ? diameters[size_t(extruder_index)] : 0.4;
        size_combo->SetSelection(nearest_nozzle_size_index(current_diameter));
        row_sizer->Add(size_combo, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(18));

        auto *flow_label = new Label(parent, _L("Flow"));
        flow_label->SetFont(Label::Body_12);
        row_sizer->Add(flow_label, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));

        auto *flow_combo = new ComboBox(parent, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, 0, nullptr,
                                        wxCB_READONLY);
        flow_combo->SetMinSize(wxSize(FromDIP(110), FromDIP(26)));
        flow_combo->Append(_L("Standard"), wxNullBitmap);
        flow_combo->Append(_L("High Flow"), wxNullBitmap);
        // A single machine-wide default applies to every extruder that has no explicit value.
        const int default_flow = flow_types.empty() ? int(NozzleVolumeType::nvtStandard) : flow_types.front();
        const int current_flow = size_t(extruder_index) < flow_types.size() ? flow_types[size_t(extruder_index)] : default_flow;
        flow_combo->SetSelection(current_flow == int(NozzleVolumeType::nvtHighFlow) ? 1 : 0);
        row_sizer->Add(flow_combo, 0, wxALIGN_CENTER_VERTICAL);

        m_extruder_rows.push_back(ExtruderRow{ size_combo, flow_combo });
        m_content_sizer->Add(row_sizer, 0, wxALIGN_LEFT | wxLEFT | wxRIGHT | wxTOP, FromDIP(10));
    }
}

void MultiNozzleSettingsDialog::build_filament_rows(wxWindow *parent, const std::vector<std::string> &stored_map,
                                                    const std::vector<double> &extruder_diameters,
                                                    const std::vector<int>    &filament_map)
{
    m_filament_size_combos.clear();
    PresetBundle *bundle = wxGetApp().preset_bundle;
    if (bundle == nullptr)
        return;

    const std::vector<double> &sizes = nozzle_size_choices();
    const size_t               filament_count = bundle->filament_presets.size();

    for (size_t filament_index = 0; filament_index < filament_count; ++filament_index) {
        auto *row_sizer = new wxBoxSizer(wxHORIZONTAL);

        auto *filament_label = new Label(parent, wxString::Format(_L("Filament %d"), int(filament_index) + 1));
        filament_label->SetFont(Label::Body_12);
        filament_label->SetMinSize(wxSize(FromDIP(100), -1));
        row_sizer->Add(filament_label, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(10));

        // Which extruder prints this filament (1-based in filament_map).
        int extruder_index = int(filament_index);
        if (filament_index < filament_map.size() && filament_map[filament_index] > 0)
            extruder_index = filament_map[filament_index] - 1;
        const double extruder_diameter = extruder_index >= 0 && size_t(extruder_index) < extruder_diameters.size() ?
                                             extruder_diameters[size_t(extruder_index)] :
                                             0.4;

        auto *combo = new ComboBox(parent, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, 0, nullptr,
                                   wxCB_READONLY);
        combo->SetMinSize(wxSize(FromDIP(200), FromDIP(26)));
        combo->Append(wxString::Format(_L("Auto (Extruder %d: %s)"), extruder_index + 1,
                                       nozzle_size_label(extruder_diameter)),
                      wxNullBitmap);
        for (double size : sizes)
            combo->Append(nozzle_size_label(size), wxNullBitmap);

        int selection = 0; // Auto
        if (filament_index < stored_map.size()) {
            const std::string &stored = stored_map[filament_index];
            // A ';' separated value is a list of available diameters, not a selection.
            if (!stored.empty() && stored.find(';') == std::string::npos) {
                const double stored_diameter = std::strtod(stored.c_str(), nullptr);
                for (size_t i = 0; i < sizes.size(); ++i)
                    if (std::abs(sizes[i] - stored_diameter) < 1e-4) {
                        selection = int(i) + 1;
                        break;
                    }
            }
        }
        combo->SetSelection(selection);
        row_sizer->Add(combo, 0, wxALIGN_CENTER_VERTICAL);

        m_filament_size_combos.push_back(combo);
        m_content_sizer->Add(row_sizer, 0, wxALIGN_LEFT | wxLEFT | wxRIGHT | wxTOP, FromDIP(10));
    }
}

void MultiNozzleSettingsDialog::on_ok(wxCommandEvent &)
{
    apply_to_config();
    EndModal(wxID_OK);
}

void MultiNozzleSettingsDialog::apply_to_config()
{
    PresetBundle *bundle = wxGetApp().preset_bundle;
    if (bundle == nullptr)
        return;

    const std::vector<double> &sizes = nozzle_size_choices();
    std::vector<double>        diameters;
    std::vector<int>           flow_types;
    diameters.reserve(m_extruder_rows.size());
    flow_types.reserve(m_extruder_rows.size());
    for (const ExtruderRow &row : m_extruder_rows) {
        int size_selection = row.diameter != nullptr ? row.diameter->GetSelection() : 0;
        if (size_selection < 0 || size_t(size_selection) >= sizes.size())
            size_selection = 0;
        diameters.push_back(sizes[size_t(size_selection)]);
        flow_types.push_back(row.flow != nullptr && row.flow->GetSelection() == 1 ?
                                 int(NozzleVolumeType::nvtHighFlow) :
                                 int(NozzleVolumeType::nvtStandard));
    }

    const bool mode_on = m_mode_checkbox != nullptr && m_mode_checkbox->GetValue();

    // If the user changes an extruder nozzle, a per-filament override that still carries the extruder's old
    // size has to follow it. Otherwise the explicit override would keep slicing with the old nozzle (e.g. a
    // filament set to 0.6 keeps printing 0.6 after the extruder was changed to 0.4).
    std::vector<int> filament_to_extruder;
    if (const ConfigOptionInts *map = edited_project_config().option<ConfigOptionInts>("filament_map"))
        filament_to_extruder = map->values;
    for (size_t filament_index = 0; filament_index < m_filament_size_combos.size(); ++ filament_index) {
        ComboBox *combo = m_filament_size_combos[filament_index];
        const int selection = combo != nullptr ? combo->GetSelection() : 0;
        if (selection <= 0 || size_t(selection) > sizes.size())
            continue;
        const double current_diameter = sizes[size_t(selection) - 1];

        int extruder_index = -1;
        if (filament_index < filament_to_extruder.size() && filament_to_extruder[filament_index] > 0)
            extruder_index = filament_to_extruder[filament_index] - 1;
        auto sync_from_extruder = [&](int candidate) {
            if (candidate < 0 || size_t(candidate) >= diameters.size() ||
                size_t(candidate) >= m_original_extruder_diameters.size())
                return false;
            const double old_diameter = m_original_extruder_diameters[size_t(candidate)];
            const double new_diameter = diameters[size_t(candidate)];
            if (std::abs(old_diameter - new_diameter) <= 1e-4 ||
                std::abs(current_diameter - old_diameter) > 1e-4)
                return false;
            for (size_t size_index = 0; size_index < sizes.size(); ++ size_index)
                if (std::abs(sizes[size_index] - new_diameter) <= 1e-4) {
                    combo->SetSelection(int(size_index) + 1);
                    return true;
                }
            return false;
        };
        if (sync_from_extruder(extruder_index))
            continue;
        // The filament map may route every filament to one extruder while the rack still carries the
        // nozzle the override names. If exactly one extruder changed away from that size, follow it.
        int matched = -1;
        int matches = 0;
        for (int candidate = 0; candidate < int(diameters.size()); ++ candidate) {
            if (size_t(candidate) >= m_original_extruder_diameters.size())
                continue;
            if (std::abs(m_original_extruder_diameters[size_t(candidate)] - current_diameter) <= 1e-4 &&
                std::abs(diameters[size_t(candidate)] - current_diameter) > 1e-4) {
                matched = candidate;
                ++matches;
            }
        }
        if (matches == 1)
            sync_from_extruder(matched);
    }

    // Printer scope: the nozzle size of every extruder. One nozzle per extruder, so the diameter list has
    // exactly one entry per extruder.
    DynamicPrintConfig printer_config;
    printer_config.set_key_value("nozzle_diameter", new ConfigOptionFloats(diameters));

    bundle->printers.get_edited_preset().config.apply(printer_config);
    if (Tab *tab = wxGetApp().get_tab(Preset::TYPE_PRINTER)) {
        tab->update_dirty();
        tab->reload_config();
    }

    // Project scope: the mode switch, the flow type of every extruder (nozzle_volume_type is a project
    // option in this fork) and the mirrored one-slot-per-extruder rack description used by the ported
    // multi-diameter helpers. The per-filament diameter override is cleared, because the extruder nozzle
    // (through the filament map) is authoritative now.
    DynamicPrintConfig project_config;
    project_config.set_key_value("multi_nozzle_diameter_mode", new ConfigOptionBool(mode_on));
    project_config.set_key_value("extruder_nozzle_count", new ConfigOptionInts(std::vector<int>(diameters.size(), 1)));
    project_config.set_key_value("extruder_nozzle_diameter", new ConfigOptionFloats(diameters));
    // The enum values need their name map: a project config that does not carry nozzle_volume_type yet would
    // otherwise receive an option that cannot be serialized.
    auto *nozzle_volume_types        = new ConfigOptionEnumsGeneric(flow_types);
    nozzle_volume_types->keys_map   = &ConfigOptionEnum<NozzleVolumeType>::get_enum_values();
    project_config.set_key_value("nozzle_volume_type", nozzle_volume_types);

    // Per filament size override. An empty entry means Auto: the nozzle size of the extruder the filament is
    // mapped to (nozzle_diameter_for_filament() falls back to filament_map).
    std::vector<std::string> filament_map(bundle->filament_presets.size(), std::string());
    if (const ConfigOptionStrings *existing = bundle->project_config.option<ConfigOptionStrings>("filament_diameter_map"))
        if (existing->values.size() > filament_map.size())
            filament_map.resize(existing->values.size(), std::string());
    for (size_t filament_index = 0; filament_index < m_filament_size_combos.size() && filament_index < filament_map.size();
         ++filament_index) {
        const int selection = m_filament_size_combos[filament_index]->GetSelection();
        if (selection > 0 && size_t(selection) <= sizes.size())
            filament_map[filament_index] = format_nozzle_diameter_to_str(sizes[size_t(selection) - 1], 1);
        else
            filament_map[filament_index].clear();
    }
    project_config.set_key_value("filament_diameter_map", new ConfigOptionStrings(filament_map));

    bundle->project_config.apply(project_config);

    if (Plater *plater = wxGetApp().plater()) {
        // Push the complete merged config. auto_reslice=false: invalidate the plate and mark it dirty,
        // but leave the actual slicing to the user's Slice button.
        const DynamicPrintConfig full_config = wxGetApp().preset_bundle->full_config();
        plater->on_config_change(full_config, false);
        plater->set_plater_dirty(true);
    }
}

} // namespace GUI
} // namespace Slic3r
