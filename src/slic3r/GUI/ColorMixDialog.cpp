#include "ColorMixDialog.hpp"

#include "GUI_App.hpp"
#include "I18N.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/StaticBox.hpp"
#include "wxExtensions.hpp"

#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"

#include <wx/choice.h>
#include <wx/dcbuffer.h>
#include <wx/dcclient.h>
#include <wx/image.h>
#include <wx/panel.h>
#include <wx/sizer.h>
#include <wx/statbmp.h>
#include <wx/stattext.h>
#include <wx/tokenzr.h>

#include <algorithm>
#include <cmath>
#include <numeric>

namespace Slic3r {
namespace GUI {

namespace {

// ---------------------------------------------------------------- helpers

double clamp01(double v) { return std::max(0.0, std::min(1.0, v)); }

void normalize_weights(std::vector<double>& weights)
{
    double sum = 0.0;
    for (double& w : weights) {
        w = std::max(0.0, w);
        sum += w;
    }
    if (sum <= 1e-9) {
        const double eq = weights.empty() ? 0.0 : 1.0 / double(weights.size());
        for (double& w : weights) w = eq;
        return;
    }
    for (double& w : weights) w /= sum;
}

wxColour blend_colours(const std::vector<wxColour>& colours, const std::vector<double>& weights)
{
    const size_t n = std::min(colours.size(), weights.size());
    double r = 0.0, g = 0.0, b = 0.0, sum = 0.0;
    for (size_t i = 0; i < n; ++i) {
        const double w = std::max(0.0, weights[i]);
        r += colours[i].Red() * w;
        g += colours[i].Green() * w;
        b += colours[i].Blue() * w;
        sum += w;
    }
    if (sum <= 1e-9) return *wxWHITE;
    auto to_byte = [](double v) { return (unsigned char) std::max(0.0, std::min(255.0, std::round(v))); };
    return wxColour(to_byte(r / sum), to_byte(g / sum), to_byte(b / sum));
}

struct FilamentChoice
{
    wxString name;
    wxColour colour;
};

std::vector<FilamentChoice> collect_filaments()
{
    std::vector<FilamentChoice> out;
    PresetBundle* pb = wxGetApp().preset_bundle;
    if (pb == nullptr) return out;

    // filament_colour lives in the project config; filament presets do not store it.
    const ConfigOptionStrings* project_colours = nullptr;
    if (pb->project_config.has("filament_colour"))
        project_colours = pb->project_config.option<ConfigOptionStrings>("filament_colour");

    for (size_t i = 0; i < pb->filament_presets.size(); ++i) {
        const std::string& preset_name = pb->filament_presets[i];
        wxColour colour(*wxWHITE);
        bool found = false;
        if (project_colours != nullptr && i < project_colours->values.size()) {
            colour = wxColour(wxString::FromUTF8(project_colours->values[i].c_str()));
            found = colour.IsOk();
        }
        if (!found) {
            if (Preset* preset = pb->filaments.find_preset(preset_name, true);
                preset != nullptr && preset->config.has("filament_colour")) {
                colour = wxColour(wxString::FromUTF8(preset->config.opt_string("filament_colour", (unsigned int) 0).c_str()));
                found = colour.IsOk();
            }
        }
        if (!found)
            colour = wxColour(0x80, 0x80, 0x80);
        out.push_back({ wxString::FromUTF8(preset_name.c_str()), colour });
    }
    return out;
}

wxBitmap* filament_chip_bitmap(const wxColour& colour, int number, int px)
{
    return get_extruder_color_icon(colour.GetAsString(wxC2S_HTML_SYNTAX).ToStdString(),
                                   std::to_string(number), px, px);
}

} // namespace

// ---------------------------------------------------------------- public helpers

std::vector<int> color_mix_integer_counts(const std::vector<double>& ratios, int total)
{
    const int n = int(ratios.size());
    std::vector<int> counts(n, 0);
    if (n == 0 || total <= 0) return counts;

    double sum = 0.0;
    for (double r : ratios) sum += std::max(0.0, r);
    if (sum <= 1e-9) {
        for (int i = 0; i < total; ++i) counts[i % n]++;
        return counts;
    }

    std::vector<double> frac(n, 0.0);
    int assigned = 0;
    for (int i = 0; i < n; ++i) {
        const double quota = std::max(0.0, ratios[i]) / sum * total;
        counts[i] = int(std::floor(quota));
        frac[i]   = quota - counts[i];
        assigned += counts[i];
    }

    std::vector<int> order(n);
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&frac](int a, int b) { return frac[a] > frac[b]; });

    int k = 0;
    while (assigned < total) {
        counts[order[k % n]]++;
        ++assigned;
        ++k;
    }
    return counts;
}

std::vector<int> color_mix_percentages(const std::vector<double>& ratios)
{
    return color_mix_integer_counts(ratios, 100);
}

wxString color_mix_pattern(const ColorMixConfig& config)
{
    wxString text;
    if (config.filament_indices.size() != config.ratios.size()) return text;
    const auto counts = color_mix_integer_counts(config.ratios);
    for (size_t i = 0; i < counts.size(); ++i)
        for (int k = 0; k < counts[i]; ++k)
            text += wxString::Format("%d", config.filament_indices[i] + 1);
    return text;
}

wxColour color_mix_preview_colour(const ColorMixConfig& config)
{
    if (config.filament_indices.size() != config.ratios.size()) return *wxWHITE;

    const auto filaments = collect_filaments();
    std::vector<wxColour> colours;
    for (int idx : config.filament_indices)
        colours.push_back((idx >= 0 && idx < int(filaments.size())) ? filaments[idx].colour : wxColour(0x80, 0x80, 0x80));
    return blend_colours(colours, config.ratios);
}

// 1-based filament number for a given layer of the pattern; the pattern cycles.
int color_mix_filament_for_layer(const ColorMixConfig& config, size_t layer_index)
{
    if (!config.valid()) return 0;

    const auto counts = color_mix_integer_counts(config.ratios);
    int total = 0;
    for (int count : counts) total += count;
    if (total <= 0) return config.filament_indices.front() + 1;

    int index = int(layer_index % size_t(total));
    for (size_t i = 0; i < counts.size(); ++i) {
        if (index < counts[i]) return config.filament_indices[i] + 1;
        index -= counts[i];
    }
    return config.filament_indices.back() + 1;
}

wxString color_mix_configs_to_string(const std::vector<ColorMixConfig>& configs)
{
    wxString text;
    for (const ColorMixConfig& config : configs) {
        if (!config.valid()) continue;
        if (!text.IsEmpty()) text += "|";
        const auto pct = color_mix_percentages(config.ratios);
        for (size_t i = 0; i < config.filament_indices.size(); ++i) {
            if (i > 0) text += ",";
            text += wxString::Format("%d:%d", config.filament_indices[i], pct[i]);
        }
    }
    return text;
}

std::vector<ColorMixConfig> color_mix_configs_from_string(const wxString& text)
{
    std::vector<ColorMixConfig> configs;
    wxStringTokenizer entries(text, "|");
    while (entries.HasMoreTokens()) {
        ColorMixConfig config;
        wxStringTokenizer items(entries.GetNextToken(), ",");
        while (items.HasMoreTokens()) {
            const wxString item = items.GetNextToken();
            long slot = 0, percent = 0;
            if (item.BeforeFirst(':').ToLong(&slot) && item.AfterFirst(':').ToLong(&percent)) {
                config.filament_indices.push_back(int(slot));
                config.ratios.push_back(double(percent) / 100.0);
            }
        }
        if (config.valid() && std::abs(std::accumulate(config.ratios.begin(), config.ratios.end(), 0.0) - 1.0) < 0.02) {
            normalize_weights(config.ratios);
            configs.push_back(config);
        }
    }
    return configs;
}

// ---------------------------------------------------------------- ColorMixRatioCanvas

ColorMixRatioCanvas::ColorMixRatioCanvas(wxWindow* parent)
    : wxWindow(parent, wxID_ANY)
{
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetBackgroundColour(*wxWHITE);
    SetMinSize(FromDIP(wxSize(360, 250)));

    Bind(wxEVT_PAINT, &ColorMixRatioCanvas::on_paint, this);
    Bind(wxEVT_SIZE, &ColorMixRatioCanvas::on_size, this);
    Bind(wxEVT_LEFT_DOWN, &ColorMixRatioCanvas::on_mouse, this);
    Bind(wxEVT_LEFT_UP, &ColorMixRatioCanvas::on_mouse, this);
    Bind(wxEVT_MOTION, &ColorMixRatioCanvas::on_mouse, this);
}

void ColorMixRatioCanvas::set_colours(const std::vector<wxColour>& colours)
{
    m_colours = colours;
    if (m_weights.size() != colours.size())
        m_weights.assign(colours.size(), colours.empty() ? 0.0 : 1.0 / double(colours.size()));
    normalize_weights(m_weights);
    m_dirty = true;
    Refresh();
}

void ColorMixRatioCanvas::set_weights(const std::vector<double>& weights)
{
    m_weights = weights;
    normalize_weights(m_weights);
    Refresh();
}

wxRect ColorMixRatioCanvas::content_rect() const
{
    const int pad = FromDIP(18);
    const wxSize size = GetClientSize();
    return wxRect(pad, pad, std::max(10, size.x - 2 * pad), std::max(10, size.y - 2 * pad));
}

void ColorMixRatioCanvas::on_size(wxSizeEvent& evt)
{
    m_dirty = true;
    Refresh();
    evt.Skip();
}

void ColorMixRatioCanvas::rebuild_gradient()
{
    m_dirty = false;
    m_gradient = wxBitmap();

    const wxSize size = GetClientSize();
    const int    n    = int(m_colours.size());
    if (size.x <= 1 || size.y <= 1 || n < 2) return;

    wxImage image(size.x, size.y);
    unsigned char* data = image.GetData();
    std::fill(data, data + size_t(size.x) * size.y * 3, 255);

    const wxRect r = content_rect();
    auto put = [&](int x, int y, const wxColour& colour) {
        if (x < 0 || y < 0 || x >= size.x || y >= size.y) return;
        unsigned char* p = data + (size_t(y) * size.x + x) * 3;
        p[0] = colour.Red();
        p[1] = colour.Green();
        p[2] = colour.Blue();
    };

    if (n == 2) {
        const int cy   = r.y + r.height / 2;
        const int half = FromDIP(7);
        for (int y = std::max(0, cy - half); y <= std::min(size.y - 1, cy + half); ++y) {
            for (int x = r.x; x < r.x + r.width; ++x) {
                const double t = double(x - r.x) / std::max(1, r.width - 1);
                put(x, y, blend_colours(m_colours, { 1.0 - t, t }));
            }
        }
    } else if (n == 3) {
        const wxPoint a(r.x + r.width / 2, r.y);
        const wxPoint b(r.x, r.y + r.height);
        const wxPoint c(r.x + r.width, r.y + r.height);
        const double  denom = double((b.y - c.y) * (a.x - c.x) + (c.x - b.x) * (a.y - c.y));
        if (std::abs(denom) < 1e-6) return;
        for (int y = r.y; y < r.y + r.height; ++y) {
            for (int x = r.x; x < r.x + r.width; ++x) {
                const double w0 = ((b.y - c.y) * (x - c.x) + (c.x - b.x) * (y - c.y)) / denom;
                const double w1 = ((c.y - a.y) * (x - c.x) + (a.x - c.x) * (y - c.y)) / denom;
                const double w2 = 1.0 - w0 - w1;
                if (w0 < -1e-6 || w1 < -1e-6 || w2 < -1e-6) continue;
                put(x, y, blend_colours(m_colours, { w0, w1, w2 }));
            }
        }
    } else {
        for (int y = r.y; y < r.y + r.height; ++y) {
            for (int x = r.x; x < r.x + r.width; ++x) {
                const double u = double(x - r.x) / std::max(1, r.width - 1);
                const double v = double(y - r.y) / std::max(1, r.height - 1);
                put(x, y, blend_colours(m_colours, { (1 - u) * (1 - v), u * (1 - v), u * v, (1 - u) * v }));
            }
        }
    }

    m_gradient = wxBitmap(image);
}

std::vector<double> ColorMixRatioCanvas::weights_from_point(const wxPoint& pt) const
{
    const int      n = int(m_colours.size());
    const wxRect   r = content_rect();
    std::vector<double> weights;

    if (n == 2) {
        const double t = clamp01(double(pt.x - r.x) / std::max(1, r.width - 1));
        weights = { 1.0 - t, t };
    } else if (n == 3) {
        const wxPoint a(r.x + r.width / 2, r.y);
        const wxPoint b(r.x, r.y + r.height);
        const wxPoint c(r.x + r.width, r.y + r.height);
        const double  denom = double((b.y - c.y) * (a.x - c.x) + (c.x - b.x) * (a.y - c.y));
        if (std::abs(denom) < 1e-6) return m_weights;
        double w0 = ((b.y - c.y) * (pt.x - c.x) + (c.x - b.x) * (pt.y - c.y)) / denom;
        double w1 = ((c.y - a.y) * (pt.x - c.x) + (a.x - c.x) * (pt.y - c.y)) / denom;
        double w2 = 1.0 - w0 - w1;
        w0 = std::max(0.0, w0);
        w1 = std::max(0.0, w1);
        w2 = std::max(0.0, w2);
        weights = { w0, w1, w2 };
    } else {
        const double u = clamp01(double(pt.x - r.x) / std::max(1, r.width - 1));
        const double v = clamp01(double(pt.y - r.y) / std::max(1, r.height - 1));
        weights = { (1 - u) * (1 - v), u * (1 - v), u * v, (1 - u) * v };
    }

    normalize_weights(weights);
    return weights;
}

void ColorMixRatioCanvas::set_weights_from_point(const wxPoint& pt, bool notify)
{
    m_weights = weights_from_point(pt);
    Refresh();
    if (notify && m_on_change) m_on_change();
}

wxPoint ColorMixRatioCanvas::point_from_weights() const
{
    const int    n = int(m_weights.size());
    const wxRect r = content_rect();

    if (n == 2) {
        const double t = clamp01(m_weights.size() > 1 ? m_weights[1] : 0.0);
        return wxPoint(r.x + int(t * std::max(1, r.width - 1)), r.y + r.height / 2);
    }
    if (n == 3) {
        const wxPoint a(r.x + r.width / 2, r.y);
        const wxPoint b(r.x, r.y + r.height);
        const wxPoint c(r.x + r.width, r.y + r.height);
        return wxPoint(int(m_weights[0] * a.x + m_weights[1] * b.x + m_weights[2] * c.x),
                       int(m_weights[0] * a.y + m_weights[1] * b.y + m_weights[2] * c.y));
    }
    if (n == 4) {
        const double u = m_weights[1] + m_weights[2];
        const double v = m_weights[2] + m_weights[3];
        return wxPoint(r.x + int(clamp01(u) * std::max(1, r.width - 1)),
                       r.y + int(clamp01(v) * std::max(1, r.height - 1)));
    }
    return wxPoint(r.x + r.width / 2, r.y + r.height / 2);
}

void ColorMixRatioCanvas::draw_vertices(wxDC& dc) const
{
    const int    n = int(m_colours.size());
    const wxRect r = content_rect();
    std::vector<wxPoint> pts;
    if (n == 2) {
        const int cy = r.y + r.height / 2;
        pts = { wxPoint(r.x, cy), wxPoint(r.x + r.width - 1, cy) };
    } else if (n == 3) {
        pts = { wxPoint(r.x + r.width / 2, r.y), wxPoint(r.x, r.y + r.height), wxPoint(r.x + r.width, r.y + r.height) };
    } else if (n == 4) {
        pts = { wxPoint(r.x, r.y), wxPoint(r.x + r.width - 1, r.y), wxPoint(r.x + r.width - 1, r.y + r.height - 1),
                wxPoint(r.x, r.y + r.height - 1) };
    }
    dc.SetPen(*wxWHITE_PEN);
    for (size_t i = 0; i < pts.size() && i < m_colours.size(); ++i) {
        dc.SetBrush(wxBrush(m_colours[i]));
        dc.DrawCircle(pts[i], FromDIP(4));
    }
}

void ColorMixRatioCanvas::draw_knob(wxDC& dc) const
{
    const wxPoint pt = point_from_weights();
    dc.SetBrush(*wxWHITE_BRUSH);
    dc.SetPen(wxPen(wxColour("#4A4A4A"), 2));
    dc.DrawCircle(pt, FromDIP(7));
    dc.SetBrush(wxBrush(wxColour("#4A4A4A")));
    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.DrawCircle(pt, FromDIP(2));
}

void ColorMixRatioCanvas::on_paint(wxPaintEvent&)
{
    wxPaintDC dc(this);
    dc.SetBackground(*wxWHITE_BRUSH);
    dc.Clear();

    if (m_dirty || !m_gradient.IsOk() || m_gradient.GetSize() != GetClientSize())
        rebuild_gradient();

    if (m_gradient.IsOk())
        dc.DrawBitmap(m_gradient, 0, 0, false);

    if (m_colours.size() >= 2) {
        draw_vertices(dc);
        draw_knob(dc);
    }
}

void ColorMixRatioCanvas::on_mouse(wxMouseEvent& evt)
{
    if (evt.LeftDown()) {
        m_dragging = true;
        if (!HasCapture()) CaptureMouse();
        set_weights_from_point(evt.GetPosition(), true);
    } else if (evt.LeftUp()) {
        if (m_dragging) {
            m_dragging = false;
            if (HasCapture()) ReleaseMouse();
            set_weights_from_point(evt.GetPosition(), true);
        }
    } else if (evt.Dragging() && m_dragging) {
        set_weights_from_point(evt.GetPosition(), true);
    }
    evt.Skip();
}

// ---------------------------------------------------------------- ColorMixPreviewStrip

ColorMixPreviewStrip::ColorMixPreviewStrip(wxWindow* parent)
    : wxWindow(parent, wxID_ANY)
{
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetBackgroundColour(*wxWHITE);
    SetMinSize(FromDIP(wxSize(220, 120)));
    Bind(wxEVT_PAINT, &ColorMixPreviewStrip::on_paint, this);
}

void ColorMixPreviewStrip::set_data(const std::vector<wxColour>& colours, const std::vector<int>& counts)
{
    m_stripes.clear();
    for (size_t i = 0; i < colours.size() && i < counts.size(); ++i)
        for (int k = 0; k < counts[i]; ++k)
            m_stripes.push_back(colours[i]);
    Refresh();
}

void ColorMixPreviewStrip::on_paint(wxPaintEvent&)
{
    wxPaintDC dc(this);
    dc.SetBackground(*wxWHITE_BRUSH);
    dc.Clear();
    if (m_stripes.empty()) return;

    const wxSize size = GetClientSize();
    const int    n    = int(m_stripes.size());
    const double h    = double(size.y) / n;
    for (int i = 0; i < n; ++i) {
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.SetBrush(wxBrush(m_stripes[i]));
        dc.DrawRectangle(0, int(std::round(i * h)), size.x, int(std::ceil(h)) + 1);
    }
}

// ---------------------------------------------------------------- ColorMixDialog

ColorMixDialog::ColorMixDialog(wxWindow* parent, const ColorMixConfig& initial)
    : wxDialog(parent, wxID_ANY, _L("Color Mixing"), wxDefaultPosition, wxDefaultSize,
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
{
    SetBackgroundColour(*wxWHITE);

    const auto filaments = collect_filaments();
    for (const FilamentChoice& filament : filaments) {
        m_names.push_back(filament.name);
        m_colours.push_back(filament.colour);
    }

    const int count = int(m_names.size());
    if (initial.valid()) {
        std::vector<int> sel;
        for (int idx : initial.filament_indices)
            if (idx >= 0 && idx < count && std::find(sel.begin(), sel.end(), idx) == sel.end())
                sel.push_back(idx);
        if (sel.size() >= 2) {
            m_sel     = sel;
            m_weights = initial.ratios;
        }
    }
    if (m_sel.size() < 2) {
        m_sel.clear();
        for (int i = 0; i < std::min(count, 2); ++i) m_sel.push_back(i);
        m_weights.assign(m_sel.size(), 0.5);
    }
    if (m_sel.size() > 4) m_sel.resize(4);
    if (m_weights.size() != m_sel.size()) m_weights.assign(m_sel.size(), 1.0 / std::max<size_t>(1, m_sel.size()));
    normalize_weights(m_weights);

    build_ui();
    update_ui_from_weights();

    SetMinSize(FromDIP(wxSize(620, 800)));
    SetSize(FromDIP(wxSize(620, 800)));
    CentreOnParent();
}

void ColorMixDialog::build_ui()
{
    m_ratio_page = build_ratio_page(this);
    m_cycle_page = build_placeholder_page(this, _L("Cycle mixing is coming soon."));
    m_match_page = build_placeholder_page(this, _L("Match mixing is coming soon."));

    auto* main_sizer = new wxBoxSizer(wxVERTICAL);
    build_tab_bar(main_sizer);

    main_sizer->Add(m_ratio_page, 1, wxEXPAND);
    main_sizer->Add(m_cycle_page, 1, wxEXPAND);
    main_sizer->Add(m_match_page, 1, wxEXPAND);
    m_cycle_page->Hide();
    m_match_page->Hide();

    auto* buttons = new wxBoxSizer(wxHORIZONTAL);
    buttons->AddStretchSpacer(1);

    auto* cancel_btn = new Button(this, _L("Cancel"));
    cancel_btn->SetStyle(ButtonStyle::Regular, ButtonType::Window);
    cancel_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_CANCEL); });
    buttons->Add(cancel_btn, 0, wxRIGHT, FromDIP(10));

    m_btn_ok = new Button(this, _L("OK"));
    m_btn_ok->SetStyle(ButtonStyle::Confirm, ButtonType::Window);
    m_btn_ok->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { accept(); });
    buttons->Add(m_btn_ok, 0);

    main_sizer->Add(buttons, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));
    SetSizer(main_sizer);
}

void ColorMixDialog::build_tab_bar(wxBoxSizer* parent_sizer)
{
    auto* bar = new wxBoxSizer(wxHORIZONTAL);

    auto show_page = [this](wxPanel* page) {
        m_ratio_page->Show(page == m_ratio_page);
        m_cycle_page->Show(page == m_cycle_page);
        m_match_page->Show(page == m_match_page);
        auto style = [](Button* btn, bool active) {
            if (btn == nullptr) return;
            btn->SetStyle(active ? ButtonStyle::Confirm : ButtonStyle::Regular, ButtonType::Choice);
            btn->Refresh();
        };
        style(m_tab_ratio, page == m_ratio_page);
        style(m_tab_cycle, page == m_cycle_page);
        style(m_tab_match, page == m_match_page);
        Layout();
    };

    auto make_tab = [this, bar, show_page](const wxString& text, wxPanel* page) {
        auto* btn = new Button(this, text);
        btn->SetStyle(ButtonStyle::Regular, ButtonType::Choice);
        btn->Bind(wxEVT_BUTTON, [show_page, page](wxCommandEvent&) { show_page(page); });
        bar->Add(btn, 0, wxRIGHT, FromDIP(6));
        return btn;
    };

    m_tab_ratio = make_tab(_L("Ratio"), m_ratio_page);
    m_tab_cycle = make_tab(_L("Cycle"), m_cycle_page);
    m_tab_match = make_tab(_L("Match"), m_match_page);

    bar->AddStretchSpacer(1);
    parent_sizer->Add(bar, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(12));

    // initial active state
    m_tab_ratio->SetStyle(ButtonStyle::Confirm, ButtonType::Choice);
}

wxPanel* ColorMixDialog::build_placeholder_page(wxWindow* parent, const wxString& text)
{
    auto* page = new wxPanel(parent);
    page->SetBackgroundColour(*wxWHITE);
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    auto* label = new wxStaticText(page, wxID_ANY, text);
    sizer->AddStretchSpacer(1);
    sizer->Add(label, 0, wxALIGN_CENTER | wxALL, FromDIP(24));
    sizer->AddStretchSpacer(1);
    page->SetSizer(sizer);
    return page;
}

wxPanel* ColorMixDialog::build_ratio_page(wxWindow* parent)
{
    auto* page = new wxPanel(parent);
    page->SetBackgroundColour(*wxWHITE);

    auto* sizer = new wxBoxSizer(wxVERTICAL);

    // --- filament selection -------------------------------------------------
    auto* title_row = new wxBoxSizer(wxHORIZONTAL);
    auto* title     = new wxStaticText(page, wxID_ANY, _L("Filament Selection"));
    title->SetFont(wxFont(wxFontInfo(12).Bold()));
    title_row->Add(title, 0, wxALIGN_CENTER_VERTICAL);
    title_row->AddStretchSpacer(1);

    m_btn_add = new Button(page, _L("Add"));
    m_btn_add->SetStyle(ButtonStyle::Regular, ButtonType::Compact);
    m_btn_add->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { set_filament_count(m_sel.size() + 1, true); });
    title_row->Add(m_btn_add, 0, wxRIGHT, FromDIP(6));

    m_btn_remove = new Button(page, _L("Remove"));
    m_btn_remove->SetStyle(ButtonStyle::Regular, ButtonType::Compact);
    m_btn_remove->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { set_filament_count(m_sel.size() - 1, true); });
    title_row->Add(m_btn_remove, 0);

    sizer->Add(title_row, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(12));

    m_rows_sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(m_rows_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(12));

    // --- mixing ratio -------------------------------------------------------
    auto* ratio_title = new wxStaticText(page, wxID_ANY, _L("Mixing Ratio"));
    ratio_title->SetFont(wxFont(wxFontInfo(12).Bold()));
    sizer->Add(ratio_title, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(12));

    m_canvas = new ColorMixRatioCanvas(page);
    m_canvas->set_on_change([this]() {
        m_weights = m_canvas->weights();
        update_ratio_labels();
        update_preview();
    });
    sizer->Add(m_canvas, 1, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(12));

    m_ratio_values_sizer = new wxBoxSizer(wxHORIZONTAL);
    sizer->Add(m_ratio_values_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(12));

    m_pattern_label = new wxStaticText(page, wxID_ANY, wxEmptyString);
    sizer->Add(m_pattern_label, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(12));

    // --- preview + mix effect ----------------------------------------------
    auto* preview_row = new wxBoxSizer(wxHORIZONTAL);

    auto* preview_col = new wxBoxSizer(wxVERTICAL);
    auto* preview_lbl = new wxStaticText(page, wxID_ANY, _L("Preview"));
    preview_col->Add(preview_lbl, 0, wxBOTTOM, FromDIP(4));
    m_preview = new ColorMixPreviewStrip(page);
    preview_col->Add(m_preview, 1, wxEXPAND);

    auto* effect_col = new wxBoxSizer(wxVERTICAL);
    auto* effect_lbl = new wxStaticText(page, wxID_ANY, _L("Mix Effect"));
    effect_col->Add(effect_lbl, 0, wxBOTTOM, FromDIP(4));
    m_mix_effect = new wxPanel(page);
    m_mix_effect->SetMinSize(FromDIP(wxSize(150, 120)));
    m_mix_effect->SetBackgroundColour(*wxWHITE);
    effect_col->Add(m_mix_effect, 1, wxEXPAND);

    preview_row->Add(preview_col, 1, wxEXPAND | wxRIGHT, FromDIP(12));
    preview_row->Add(effect_col, 0, wxEXPAND);

    sizer->Add(preview_row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP | wxBOTTOM, FromDIP(12));

    page->SetSizer(sizer);
    return page;
}

void ColorMixDialog::rebuild_rows()
{
    if (m_rows_sizer == nullptr || m_ratio_page == nullptr) return;

    m_rows_sizer->Clear(false);
    for (Row& row : m_rows)
        if (row.panel != nullptr) row.panel->Destroy();
    m_rows.clear();

    wxArrayString names;
    for (size_t i = 0; i < m_names.size(); ++i)
        names.Add(wxString::Format("%d  %s", int(i) + 1, m_names[i].c_str()));

    for (size_t i = 0; i < m_sel.size(); ++i) {
        Row row;
        row.panel = new wxPanel(m_ratio_page);
        row.panel->SetBackgroundColour(*wxWHITE);
        auto* row_sizer = new wxBoxSizer(wxHORIZONTAL);

        auto* label = new wxStaticText(row.panel, wxID_ANY, _L("Filament"));
        row_sizer->Add(label, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));

        auto* chip = new wxStaticBitmap(row.panel, wxID_ANY,
                                        *filament_chip_bitmap(m_colours[m_sel[i]], m_sel[i] + 1, FromDIP(20)));
        row.chip = chip;
        row_sizer->Add(chip, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));

        row.choice = new wxChoice(row.panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, names);
        row.choice->SetSelection(m_sel[i]);
        const int row_index = int(i);
        row.choice->Bind(wxEVT_CHOICE, [this, row_index](wxCommandEvent&) { on_row_changed(size_t(row_index)); });
        row_sizer->Add(row.choice, 1, wxALIGN_CENTER_VERTICAL);

        row.panel->SetSizer(row_sizer);
        m_rows_sizer->Add(row.panel, 0, wxEXPAND | wxTOP | wxBOTTOM, FromDIP(3));
        m_rows.push_back(row);
    }

    m_ratio_page->Layout();
}

void ColorMixDialog::refresh_row_chips()
{
    for (size_t i = 0; i < m_rows.size() && i < m_sel.size(); ++i) {
        if (auto* chip = dynamic_cast<wxStaticBitmap*>(m_rows[i].chip))
            chip->SetBitmap(*filament_chip_bitmap(m_colours[m_sel[i]], m_sel[i] + 1, FromDIP(20)));
    }
}

void ColorMixDialog::on_row_changed(size_t row)
{
    if (row >= m_rows.size() || m_rows[row].choice == nullptr) return;
    const int new_index = m_rows[row].choice->GetSelection();
    if (new_index < 0 || new_index >= int(m_names.size())) return;

    for (size_t i = 0; i < m_sel.size(); ++i) {
        if (i != row && m_sel[i] == new_index) {
            m_sel[i] = m_sel[row];
            if (m_rows[i].choice != nullptr) m_rows[i].choice->SetSelection(m_sel[i]);
            break;
        }
    }
    m_sel[row] = new_index;

    refresh_row_chips();
    update_ui_from_weights();
}

void ColorMixDialog::set_filament_count(size_t count, bool keep_ratios)
{
    const size_t max_count = std::min<size_t>(4, m_names.size());
    count = std::max<size_t>(2, std::min(count, max_count));
    if (count == m_sel.size()) return;

    while (m_sel.size() > count) m_sel.pop_back();
    while (m_sel.size() < count) {
        int candidate = 0;
        while (candidate < int(m_names.size()) &&
               std::find(m_sel.begin(), m_sel.end(), candidate) != m_sel.end())
            ++candidate;
        if (candidate >= int(m_names.size())) candidate = 0;
        m_sel.push_back(candidate);
    }

    if (!keep_ratios || m_weights.size() != m_sel.size())
        m_weights.assign(m_sel.size(), 1.0 / double(m_sel.size()));
    normalize_weights(m_weights);

    rebuild_rows();
    update_ui_from_weights();
    m_ratio_page->Layout();
}

void ColorMixDialog::update_ui_from_weights()
{
    normalize_weights(m_weights);
    if (m_weights.size() != m_sel.size()) m_weights.assign(m_sel.size(), 1.0 / std::max<size_t>(1, m_sel.size()));

    std::vector<wxColour> colours;
    for (int idx : m_sel) colours.push_back(m_colours[idx]);

    if (m_canvas != nullptr) {
        m_canvas->set_colours(colours);
        m_canvas->set_weights(m_weights);
    }
    if (m_btn_add != nullptr) m_btn_add->Enable(m_sel.size() < 4 && m_sel.size() < m_names.size());
    if (m_btn_remove != nullptr) m_btn_remove->Enable(m_sel.size() > 2);
    if (m_btn_ok != nullptr) m_btn_ok->Enable(m_names.size() >= 2);

    update_ratio_labels();
    update_preview();
    if (m_ratio_page != nullptr) m_ratio_page->Layout();
}

void ColorMixDialog::update_ratio_labels()
{
    if (m_ratio_values_sizer == nullptr || m_ratio_page == nullptr) return;

    m_ratio_values_sizer->Clear(true);
    if (m_weights.size() != m_sel.size()) return;

    const auto pct = color_mix_percentages(m_weights);
    for (size_t i = 0; i < m_sel.size() && i < pct.size(); ++i) {
        auto* chip = new wxStaticBitmap(m_ratio_page, wxID_ANY,
                                        *filament_chip_bitmap(m_colours[m_sel[i]], m_sel[i] + 1, FromDIP(16)));
        m_ratio_values_sizer->Add(chip, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));
        auto* text = new wxStaticText(m_ratio_page, wxID_ANY, wxString::Format("%d%%", pct[i]));
        m_ratio_values_sizer->Add(text, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(14));
    }
    m_ratio_page->Layout();
}

void ColorMixDialog::update_preview()
{
    if (m_weights.size() != m_sel.size()) return;

    std::vector<wxColour> colours;
    for (int idx : m_sel) colours.push_back(m_colours[idx]);

    if (m_preview != nullptr)
        m_preview->set_data(colours, color_mix_integer_counts(m_weights));

    if (m_pattern_label != nullptr) {
        ColorMixConfig preview_config;
        preview_config.filament_indices = m_sel;
        preview_config.ratios           = m_weights;
        m_pattern_label->SetLabel(wxString::Format(_L("Pattern: %s"), color_mix_pattern(preview_config).c_str()));
    }

    if (m_mix_effect != nullptr) {
        m_mix_effect->SetBackgroundColour(blend_colours(colours, m_weights));
        m_mix_effect->Refresh();
    }
}

void ColorMixDialog::accept()
{
    if (m_sel.size() < 2 || m_sel.size() > 4 || m_sel.size() != m_weights.size()) return;

    m_config.filament_indices = m_sel;
    m_config.ratios           = m_weights;
    normalize_weights(m_config.ratios);
    EndModal(wxID_OK);
}

} // namespace GUI
} // namespace Slic3r
