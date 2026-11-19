#include "MultiNozzleSettingsDialog.hpp"

#include <algorithm>
#include <cmath>

#include <wx/sizer.h>

#include "GUI_App.hpp"
#include "GUI.hpp"
#include "I18N.hpp"
#include "MainFrame.hpp"
#include "NozzleConfig.hpp"
#include "Widgets/StaticLine.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"

namespace Slic3r {
namespace GUI {

namespace {

int nearest_nozzle_size_index(double diameter)
{
    const std::vector<double> &choices = nozzle_size_choices();
    int                        best    = 0;
    for (size_t i = 1; i < choices.size(); ++i)
        if (std::abs(choices[i] - diameter) < std::abs(choices[best] - diameter))
            best = int(i);
    return best;
}

} // namespace

MultiNozzleSettingsDialog::MultiNozzleSettingsDialog(wxWindow *parent)
    : DPIDialog(parent ? parent : static_cast<wxWindow *>(wxGetApp().mainframe), wxID_ANY,
                _L("Multi-head mixed printing Debug panel"), wxDefaultPosition, wxDefaultSize, wxCAPTION | wxCLOSE_BOX)
{
    SetBackgroundColour(*wxWHITE);
    SetFont(Label::Body_14);
    build_ui();
    SetMinSize(wxSize(FromDIP(560), FromDIP(320)));
    SetSize(wxSize(FromDIP(660), FromDIP(560)));
    Layout();
    CentreOnScreen();
}

void MultiNozzleSettingsDialog::build_ui()
{
    const NozzleConfiguration configuration = current_nozzle_configuration();

    auto *main_sizer = new wxBoxSizer(wxVERTICAL);

    // Status of the mixed nozzle print. The mode is no longer a switch: it follows the diameters.
    m_mode_status = new Label(this, multi_nozzle_mode_status_text());
    m_mode_status->SetFont(Label::Head_14);
    main_sizer->Add(m_mode_status, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(20));

    m_mode_detail = new Label(this, multi_nozzle_mode_detail_text());
    m_mode_detail->SetFont(Label::Body_12);
    m_mode_detail->SetForegroundColour(wxColour(107, 107, 107));
    m_mode_detail->Wrap(FromDIP(600));
    main_sizer->Add(m_mode_detail, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(6));

    auto *filament_hint = new Label(
        this, _L("Filaments do not choose a nozzle size: filament 1 prints with nozzle 1, filament 2 with "
                 "nozzle 2, and so on."));
    filament_hint->SetFont(Label::Body_12);
    filament_hint->SetForegroundColour(wxColour(107, 107, 107));
    filament_hint->Wrap(FromDIP(600));
    main_sizer->Add(filament_hint, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(16));

    auto *line = new StaticLine(this);
    main_sizer->Add(line, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(16));

    m_content = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(620), FromDIP(240)), wxVSCROLL);
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

    build_extruder_rows(m_content, configuration.diameters, configuration.flow_types);

    m_content->SetSizer(m_content_sizer);
    m_content->Layout();
    m_content->FitInside();
    main_sizer->Add(m_content, 1, wxEXPAND | wxALL, FromDIP(10));

    // Debug mock server: JSON the printer server would answer with. The "update from printer server"
    // action reads this text; it is persisted in the application config so it survives a restart.
    auto *mock_label = new Label(this, _L("Mock Server (fake server)"));
    mock_label->SetFont(Label::Body_14);
    main_sizer->Add(mock_label, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(15));

    m_mock_server_text = new wxTextCtrl(this, wxID_ANY, wxString::FromUTF8(mock_server_json().c_str()),
                                        wxDefaultPosition, wxSize(-1, FromDIP(70)), wxTE_MULTILINE);
    m_mock_server_text->SetToolTip(
        _L("Enter the JSON the printer server would return, for example: {\"nozzle_diameter\":[0.4,0.4,0.8,0.4]}"));
    m_mock_server_text->Bind(wxEVT_TEXT, [this](wxCommandEvent &) {
        set_mock_server_json(into_u8(m_mock_server_text->GetValue()));
    });
    main_sizer->Add(m_mock_server_text, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(15));

    auto *buttons = new DialogButtons(this, { "OK", "Cancel" });
    main_sizer->Add(buttons, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));
    buttons->GetOK()->Bind(wxEVT_BUTTON, &MultiNozzleSettingsDialog::on_ok, this);
    buttons->GetCANCEL()->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { EndModal(wxID_CANCEL); });

    SetSizer(main_sizer);
    Layout();
}

void MultiNozzleSettingsDialog::build_extruder_rows(wxWindow *parent, const std::vector<double> &diameters,
                                                    const std::vector<int>    &flow_types)
{
    m_extruder_rows.clear();
    const std::vector<double> &sizes = nozzle_size_choices();

    for (size_t extruder_index = 0; extruder_index < diameters.size(); ++extruder_index) {
        auto *row_sizer = new wxBoxSizer(wxHORIZONTAL);

        auto *nozzle_label = new Label(parent, wxString::Format(_L("Nozzle %d"), int(extruder_index) + 1));
        nozzle_label->SetFont(Label::Body_12);
        nozzle_label->SetMinSize(wxSize(FromDIP(110), -1));
        row_sizer->Add(nozzle_label, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(10));

        auto *size_label = new Label(parent, _L("Diameter"));
        size_label->SetFont(Label::Body_12);
        row_sizer->Add(size_label, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));

        auto *size_combo = new ComboBox(parent, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, 0, nullptr,
                                        wxCB_READONLY);
        size_combo->SetMinSize(wxSize(FromDIP(90), FromDIP(26)));
        for (double size : sizes)
            size_combo->Append(wxString::FromUTF8(format_nozzle_diameter_to_str(size, 1).c_str()), wxNullBitmap);
        size_combo->SetSelection(nearest_nozzle_size_index(diameters[extruder_index]));
        row_sizer->Add(size_combo, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(18));

        auto *flow_label = new Label(parent, _L("Flow"));
        flow_label->SetFont(Label::Body_12);
        row_sizer->Add(flow_label, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));

        auto *flow_combo = new ComboBox(parent, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, 0, nullptr,
                                        wxCB_READONLY);
        flow_combo->SetMinSize(wxSize(FromDIP(110), FromDIP(26)));
        flow_combo->Append(_L("Standard"), wxNullBitmap);
        flow_combo->Append(_L("High Flow"), wxNullBitmap);
        const int current_flow = extruder_index < flow_types.size() ? flow_types[extruder_index] :
                                                                      int(NozzleVolumeType::nvtStandard);
        flow_combo->SetSelection(current_flow == int(NozzleVolumeType::nvtHighFlow) ? 1 : 0);
        row_sizer->Add(flow_combo, 0, wxALIGN_CENTER_VERTICAL);

        m_extruder_rows.push_back(ExtruderRow{ size_combo, flow_combo });
        m_content_sizer->Add(row_sizer, 0, wxALIGN_LEFT | wxLEFT | wxRIGHT | wxTOP, FromDIP(10));

        size_combo->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent &) { update_mode_status(); });
        flow_combo->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent &) { update_mode_status(); });
    }
}

void MultiNozzleSettingsDialog::update_mode_status()
{
    const std::vector<double> &sizes = nozzle_size_choices();
    std::vector<double>        diameters;
    diameters.reserve(m_extruder_rows.size());
    for (const ExtruderRow &row : m_extruder_rows) {
        const int selection = row.diameter != nullptr ? row.diameter->GetSelection() : 0;
        diameters.push_back(selection >= 0 && size_t(selection) < sizes.size() ? sizes[size_t(selection)] : 0.4);
    }
    const bool active = !nozzles_all_equal(diameters);
    m_mode_status->SetLabel(active ? _L("Multi nozzle diameter mode: ON") : _L("Multi nozzle diameter mode: OFF"));
    m_mode_detail->SetLabel(active ?
        _L("The nozzles have different diameters. The plate will be sliced for mixed nozzle diameters.") :
        _L("All nozzles have the same diameter. The plate will be sliced with a single nozzle diameter."));
    m_mode_detail->Wrap(FromDIP(600));
    Layout();
}

void MultiNozzleSettingsDialog::on_ok(wxCommandEvent &)
{
    apply_to_config();
    EndModal(wxID_OK);
}

void MultiNozzleSettingsDialog::apply_to_config()
{
    const std::vector<double> &sizes = nozzle_size_choices();

    NozzleConfiguration configuration;
    configuration.diameters.reserve(m_extruder_rows.size());
    configuration.flow_types.reserve(m_extruder_rows.size());
    for (const ExtruderRow &row : m_extruder_rows) {
        int size_selection = row.diameter != nullptr ? row.diameter->GetSelection() : 0;
        if (size_selection < 0 || size_t(size_selection) >= sizes.size())
            size_selection = 0;
        configuration.diameters.push_back(sizes[size_t(size_selection)]);
        configuration.flow_types.push_back(row.flow != nullptr && row.flow->GetSelection() == 1 ?
                                               int(NozzleVolumeType::nvtHighFlow) :
                                               int(NozzleVolumeType::nvtStandard));
    }

    apply_nozzle_configuration(configuration, this);
}

} // namespace GUI
} // namespace Slic3r
