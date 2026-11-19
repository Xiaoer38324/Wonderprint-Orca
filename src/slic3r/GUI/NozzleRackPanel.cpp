#include "NozzleRackPanel.hpp"

#include <algorithm>
#include <cmath>
#include <memory>

#include <boost/log/trivial.hpp>

#include <wx/dcbuffer.h>
#include <wx/graphics.h>
#include <wx/sizer.h>

#include "I18N.hpp"
#include "NozzleConfig.hpp"
#include "libslic3r/PrintConfig.hpp"

namespace Slic3r {
namespace GUI {

namespace {

const wxColour SELECTED_BG("#009688");
const wxColour SELECTED_FG("#FFFFFF");
const wxColour NORMAL_BG("#F2F2F2");
const wxColour NORMAL_FG("#6B6B6B");
const wxColour CARD_BORDER("#E6E6E6");

constexpr double kPi = 3.14159265358979323846;

void add_custom_rounded_rect(wxGraphicsPath &path,
                             double          x,
                             double          y,
                             double          w,
                             double          h,
                             double          tl,
                             double          tr,
                             double          br,
                             double          bl)
{
    const double max_r = std::max(0.0, std::min(w, h) * 0.5);
    tl = std::clamp(tl, 0.0, max_r);
    tr = std::clamp(tr, 0.0, max_r);
    br = std::clamp(br, 0.0, max_r);
    bl = std::clamp(bl, 0.0, max_r);

    path.MoveToPoint(x + tl, y);

    if (tr > 0.0) {
        path.AddLineToPoint(x + w - tr, y);
        path.AddArc(x + w - tr, y + tr, tr, -kPi * 0.5, 0.0, true);
    } else {
        path.AddLineToPoint(x + w, y);
    }

    if (br > 0.0) {
        path.AddLineToPoint(x + w, y + h - br);
        path.AddArc(x + w - br, y + h - br, br, 0.0, kPi * 0.5, true);
    } else {
        path.AddLineToPoint(x + w, y + h);
    }

    if (bl > 0.0) {
        path.AddLineToPoint(x + bl, y + h);
        path.AddArc(x + bl, y + h - bl, bl, kPi * 0.5, kPi, true);
    } else {
        path.AddLineToPoint(x, y + h);
    }

    if (tl > 0.0) {
        path.AddLineToPoint(x, y + tl);
        path.AddArc(x + tl, y + tl, tl, kPi, kPi * 1.5, true);
    } else {
        path.AddLineToPoint(x, y);
    }

    path.CloseSubpath();
}

// 外层圆角卡片容器：绘制白底与 1px 浅灰圆角描边
class RoundedCardPanel : public wxPanel
{
public:
    explicit RoundedCardPanel(wxWindow *parent)
        : wxPanel(parent)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetBackgroundColour(*wxWHITE);
        Bind(wxEVT_PAINT, &RoundedCardPanel::on_paint, this);
        Bind(wxEVT_SIZE, [this](wxSizeEvent &evt) {
            Refresh();
            evt.Skip();
        });
    }

private:
    void on_paint(wxPaintEvent &)
    {
        wxAutoBufferedPaintDC dc(this);
        const wxSize          sz = GetClientSize();
        if (sz.x <= 0 || sz.y <= 0)
            return;

        dc.SetBrush(*wxWHITE_BRUSH);
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.DrawRectangle(0, 0, sz.x, sz.y);

        const double r = double(FromDIP(5));
        if (std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc)); gc) {
            wxGraphicsPath path = gc->CreatePath();
            add_custom_rounded_rect(path, 0.5, 0.5, sz.x - 1.0, sz.y - 1.0, r, r, r, r);
            gc->SetBrush(wxBrush(*wxWHITE));
            gc->SetPen(wxPen(CARD_BORDER, 1));
            gc->DrawPath(path);
        } else {
            dc.SetPen(wxPen(CARD_BORDER, 1));
            dc.DrawRoundedRectangle(0, 0, sz.x, sz.y, r);
        }
    }
};

// 顶部 Tab 栏底条：绘制浅灰背景及左上/右上圆角
class RoundedTabBarPanel : public wxPanel
{
public:
    explicit RoundedTabBarPanel(wxWindow *parent)
        : wxPanel(parent)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetBackgroundColour(NORMAL_BG);
        Bind(wxEVT_PAINT, &RoundedTabBarPanel::on_paint, this);
        Bind(wxEVT_SIZE, [this](wxSizeEvent &evt) {
            Refresh();
            evt.Skip();
        });
    }

private:
    void on_paint(wxPaintEvent &)
    {
        wxAutoBufferedPaintDC dc(this);
        const wxSize          sz = GetClientSize();
        if (sz.x <= 0 || sz.y <= 0)
            return;

        dc.SetBrush(*wxWHITE_BRUSH);
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.DrawRectangle(0, 0, sz.x, sz.y);

        const double r = double(FromDIP(5));
        if (std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc)); gc) {
            wxGraphicsPath path = gc->CreatePath();
            add_custom_rounded_rect(path, 0.0, 0.0, double(sz.x), double(sz.y), r, r, 0.0, 0.0);
            gc->SetBrush(wxBrush(NORMAL_BG));
            gc->SetPen(*wxTRANSPARENT_PEN);
            gc->FillPath(path);
        } else {
            dc.SetBrush(wxBrush(NORMAL_BG));
            dc.DrawRectangle(0, 0, sz.x, sz.y);
        }
    }
};

// 单个 Tab 项：首项绘制左上圆角，若右边缘贴合底栏则自动绘制右上圆角
class RoundedTabPanel : public wxPanel
{
public:
    RoundedTabPanel(wxWindow *parent, bool round_top_left)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize)
        , m_round_top_left(round_top_left)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetBackgroundColour(NORMAL_BG);
        SetCursor(wxCursor(wxCURSOR_HAND));
        Bind(wxEVT_PAINT, &RoundedTabPanel::on_paint, this);
        Bind(wxEVT_SIZE, [this](wxSizeEvent &evt) {
            Refresh();
            evt.Skip();
        });
    }

private:
    bool m_round_top_left = false;

    void on_paint(wxPaintEvent &)
    {
        wxAutoBufferedPaintDC dc(this);
        const wxSize          sz = GetClientSize();
        if (sz.x <= 0 || sz.y <= 0)
            return;

        const wxColour bg       = GetBackgroundColour();
        const bool     round_tr = GetParent() != nullptr &&
                                  (GetPosition().x + sz.x >= GetParent()->GetClientSize().x - 1);

        if (!m_round_top_left && !round_tr) {
            dc.SetBrush(wxBrush(bg));
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.DrawRectangle(0, 0, sz.x, sz.y);
            return;
        }

        dc.SetBrush(*wxWHITE_BRUSH);
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.DrawRectangle(0, 0, sz.x, sz.y);

        const double r = double(FromDIP(5));
        if (std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc)); gc) {
            wxGraphicsPath path = gc->CreatePath();
            add_custom_rounded_rect(path, 0.0, 0.0, double(sz.x), double(sz.y),
                                    m_round_top_left ? r : 0.0,
                                    round_tr ? r : 0.0,
                                    0.0, 0.0);
            gc->SetBrush(wxBrush(bg));
            gc->SetPen(*wxTRANSPARENT_PEN);
            gc->FillPath(path);
        } else {
            dc.SetBrush(wxBrush(bg));
            dc.DrawRectangle(0, 0, sz.x, sz.y);
        }
    }
};

void update_tab_min_size(wxWindow *owner, wxPanel *tab_panel, Label *tab_label)
{
    tab_label->SetFont(Label::Body_14);
    const wxSize text_sz = tab_label->GetTextExtent(tab_label->GetLabel());
    // 预留 4 DIP 宽度余量，避免 Windows 下 wxStaticText 在空格处自动换行
    tab_label->SetMinSize(wxSize(text_sz.x + owner->FromDIP(4), text_sz.y));
    tab_panel->SetMinSize(wxSize(std::max(owner->FromDIP(72), text_sz.x + owner->FromDIP(24)),
                                 std::max(owner->FromDIP(30), text_sz.y + owner->FromDIP(10))));
}

int nearest_size_index(double diameter)
{
    const std::vector<double> &choices = nozzle_size_choices();
    int                        best    = 0;
    for (size_t i = 1; i < choices.size(); ++i)
        if (std::abs(choices[i] - diameter) < std::abs(choices[best] - diameter))
            best = int(i);
    return best;
}

} // namespace

NozzleRackPanel::NozzleRackPanel(wxWindow *parent)
    : wxPanel(parent)
{
    SetBackgroundColour(*wxWHITE);

    auto *main_sizer = new wxBoxSizer(wxVERTICAL);

    auto *card_panel = new RoundedCardPanel(this);
    auto *card_sizer = new wxBoxSizer(wxVERTICAL);

    auto *tab_bar = new RoundedTabBarPanel(card_panel);
    m_tab_sizer   = new wxBoxSizer(wxHORIZONTAL);
    tab_bar->SetSizer(m_tab_sizer);
    card_sizer->Add(tab_bar, 0, wxEXPAND);

    build_content();
    // 留出与圆角半径一致的 5 DIP 白色边距，确保 m_content_panel 不会遮挡卡片底部的圆角与 1px 边框
    card_sizer->Add(m_content_panel, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(5));

    card_panel->SetSizer(card_sizer);
    // 不额外添加外边距，使卡片左右边缘与上方打印机卡片保持完全对齐
    main_sizer->Add(card_panel, 0, wxEXPAND);

    SetSizer(main_sizer);
    reload();
}

void NozzleRackPanel::build_content()
{
    wxWindow *card_panel = (m_tab_sizer != nullptr && m_tab_sizer->GetContainingWindow() != nullptr)
                               ? m_tab_sizer->GetContainingWindow()->GetParent()
                               : this;
    m_content_panel = new wxPanel(card_panel);
    m_content_panel->SetBackgroundColour(*wxWHITE);

    auto *content_sizer = new wxBoxSizer(wxVERTICAL);
    auto *row_sizer     = new wxBoxSizer(wxHORIZONTAL);

    m_diameter_label = new Label(m_content_panel, _L("Diameter"));
    m_diameter_label->SetFont(Label::Body_14);
    row_sizer->Add(m_diameter_label, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(10));

    m_diameter_combo = new ComboBox(m_content_panel, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, 0,
                                    nullptr, wxCB_READONLY);
    m_diameter_combo->SetMinSize(wxSize(FromDIP(95), FromDIP(30)));
    for (double size : nozzle_size_choices())
        m_diameter_combo->Append(wxString::FromUTF8(format_nozzle_diameter_to_str(size, 1).c_str()) + "mm", wxNullBitmap);
    row_sizer->Add(m_diameter_combo, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(20));

    m_flow_label = new Label(m_content_panel, _L("Flow"));
    m_flow_label->SetFont(Label::Body_14);
    row_sizer->Add(m_flow_label, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(10));

    m_flow_combo = new ComboBox(m_content_panel, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, 0, nullptr,
                                wxCB_READONLY);
    m_flow_combo->SetMinSize(wxSize(FromDIP(95), FromDIP(30)));
    m_flow_combo->Append(_L("Standard"), wxNullBitmap);
    m_flow_combo->Append(_L("High Flow"), wxNullBitmap);
    row_sizer->Add(m_flow_combo, 1, wxALIGN_CENTER_VERTICAL);

    // 结合外部 5 DIP 边距后，最终视觉留白为：上 14 DIP、下 14 DIP、左右各 12 DIP
    content_sizer->AddSpacer(FromDIP(14));
    content_sizer->Add(row_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(7));
    content_sizer->AddSpacer(FromDIP(9));

    m_content_panel->SetSizer(content_sizer);
    m_diameter_combo->Bind(wxEVT_COMBOBOX, &NozzleRackPanel::on_diameter_changed, this);
    m_flow_combo->Bind(wxEVT_COMBOBOX, &NozzleRackPanel::on_flow_changed, this);
}

void NozzleRackPanel::rebuild_tabs(size_t nozzle_count)
{
    wxWindow *tab_parent = (m_tab_sizer != nullptr && m_tab_sizer->GetContainingWindow() != nullptr)
                               ? m_tab_sizer->GetContainingWindow()
                               : this;

    // Clear(true) destroys the tab windows, so the vector has to be emptied before the new tabs are built.
    m_tab_sizer->Clear(true);
    m_tabs.clear();
    m_tabs.reserve(nozzle_count);

    for (size_t nozzle_index = 0; nozzle_index < nozzle_count; ++nozzle_index) {
        Tab tab;
        tab.panel = new RoundedTabPanel(tab_parent, nozzle_index == 0);
        tab.panel->SetBackgroundColour(NORMAL_BG);

        tab.label = new Label(tab.panel, wxString::Format(_L("Nozzle %d"), int(nozzle_index) + 1),
                              LB_PROPAGATE_MOUSE_EVENT | wxALIGN_CENTRE_HORIZONTAL);
        tab.label->SetCursor(wxCursor(wxCURSOR_HAND));
        update_tab_min_size(this, tab.panel, tab.label);

        auto *tab_sizer = new wxBoxSizer(wxVERTICAL);
        tab_sizer->AddStretchSpacer(1);
        tab_sizer->Add(tab.label, 0, wxALIGN_CENTER_HORIZONTAL);
        tab_sizer->AddStretchSpacer(1);
        tab.panel->SetSizer(tab_sizer);

        tab.panel->Bind(wxEVT_LEFT_DOWN, [this, nozzle_index](wxMouseEvent &evt) {
            BOOST_LOG_TRIVIAL(warning) << "NozzleRackPanel tab click idx=" << nozzle_index
                                       << " local_x=" << evt.GetPosition().x
                                       << " screen_x=" << wxGetMousePosition().x;
            select_nozzle(nozzle_index);
        });
        m_tab_sizer->Add(tab.panel, 0, wxEXPAND);
        m_tabs.emplace_back(std::move(tab));
    }

    // 右侧留空以展示浅灰圆角底栏
    m_tab_sizer->AddStretchSpacer(1);
    tab_parent->Layout();
    tab_parent->Refresh();
}

void NozzleRackPanel::reload()
{
    const NozzleConfiguration configuration = current_nozzle_configuration();
    m_diameters  = configuration.diameters;
    m_flow_types = configuration.flow_types;
    if (m_flow_types.size() < m_diameters.size())
        m_flow_types.resize(m_diameters.size(), int(NozzleVolumeType::nvtStandard));

    const size_t count = std::max<size_t>(1, m_diameters.size());
    if (m_tabs.size() != count)
        rebuild_tabs(count);
    if (m_selected >= count)
        m_selected = count - 1;

    update_tab_styles();
    refresh_content();
    if (m_content_panel != nullptr && m_content_panel->GetParent() != nullptr)
        m_content_panel->GetParent()->Layout();
    Layout();
    Refresh();
}

void NozzleRackPanel::update_tab_styles()
{
    m_updating = true;
    for (size_t i = 0; i < m_tabs.size(); ++i) {
        const bool     selected = i == m_selected;
        const wxColour bg       = selected ? SELECTED_BG : NORMAL_BG;
        const wxColour fg       = selected ? SELECTED_FG : NORMAL_FG;
        m_tabs[i].panel->SetBackgroundColour(bg);
        m_tabs[i].label->SetBackgroundColour(bg);
        m_tabs[i].label->SetForegroundColour(fg);
        m_tabs[i].panel->Refresh();
        m_tabs[i].label->Refresh();
    }
    m_updating = false;
}

void NozzleRackPanel::refresh_content()
{
    if (m_diameter_combo == nullptr || m_flow_combo == nullptr || m_tabs.empty())
        return;

    const double diameter        = m_selected < m_diameters.size() ? m_diameters[m_selected] : 0.4;
    const int    flow            = m_selected < m_flow_types.size() ? m_flow_types[m_selected] :
                                                                      int(NozzleVolumeType::nvtStandard);
    const bool   allow_high_flow = true;

    m_updating = true;
    m_diameter_combo->SetSelection(nearest_size_index(diameter));
    m_flow_combo->SetSelection((allow_high_flow && flow == int(NozzleVolumeType::nvtHighFlow)) ? 1 : 0);
    m_flow_combo->Enable(allow_high_flow);
    m_updating = false;
}

void NozzleRackPanel::select_nozzle(size_t index)
{
    BOOST_LOG_TRIVIAL(warning) << "NozzleRackPanel::select_nozzle index=" << index << " selected=" << m_selected
                               << " tabs=" << m_tabs.size() << " updating=" << m_updating;
    if (m_updating || index >= m_tabs.size() || index == m_selected)
        return;
    m_selected = index;
    update_tab_styles();
    refresh_content();
    Layout();
}

void NozzleRackPanel::on_diameter_changed(wxCommandEvent &)
{
    if (m_updating)
        return;
    const int selection = m_diameter_combo->GetSelection();
    if (selection < 0 || size_t(selection) >= nozzle_size_choices().size() || m_selected >= m_diameters.size())
        return;
    m_diameters[m_selected] = nozzle_size_choices()[size_t(selection)];
    if (m_diameters[m_selected] <= 0.25 && m_selected < m_flow_types.size())
        m_flow_types[m_selected] = int(NozzleVolumeType::nvtStandard);
    apply_edit();
    reload();
}

void NozzleRackPanel::on_flow_changed(wxCommandEvent &)
{
    if (m_updating)
        return;
    if (m_selected >= m_flow_types.size())
        return;
    m_flow_types[m_selected] = m_flow_combo->GetSelection() == 1 ? int(NozzleVolumeType::nvtHighFlow) :
                                                                   int(NozzleVolumeType::nvtStandard);
    apply_edit();
    reload();
}

bool NozzleRackPanel::apply_edit()
{
    NozzleConfiguration configuration;
    configuration.diameters  = m_diameters;
    configuration.flow_types = m_flow_types;
    apply_nozzle_configuration(configuration, this);
    return true;
}

void NozzleRackPanel::msw_rescale()
{
    for (Tab &tab : m_tabs) {
        update_tab_min_size(this, tab.panel, tab.label);
        tab.panel->Layout();
        tab.panel->Refresh();
    }
    if (m_diameter_label != nullptr)
        m_diameter_label->SetFont(Label::Body_14);
    if (m_flow_label != nullptr)
        m_flow_label->SetFont(Label::Body_14);
    if (m_diameter_combo != nullptr) {
        m_diameter_combo->SetMinSize(wxSize(FromDIP(95), FromDIP(30)));
        m_diameter_combo->Rescale();
    }
    if (m_flow_combo != nullptr) {
        m_flow_combo->SetMinSize(wxSize(FromDIP(95), FromDIP(30)));
        m_flow_combo->Rescale();
    }
    if (m_tab_sizer != nullptr && m_tab_sizer->GetContainingWindow() != nullptr) {
        m_tab_sizer->GetContainingWindow()->Layout();
        m_tab_sizer->GetContainingWindow()->Refresh();
    }
    if (m_content_panel != nullptr && m_content_panel->GetParent() != nullptr) {
        m_content_panel->GetParent()->Layout();
        m_content_panel->GetParent()->Refresh();
    }
    Layout();
    Refresh();
}

} // namespace GUI
} // namespace Slic3r