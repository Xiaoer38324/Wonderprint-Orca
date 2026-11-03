#ifndef slic3r_GUI_ColorMixDialog_hpp_
#define slic3r_GUI_ColorMixDialog_hpp_

#include <wx/bitmap.h>
#include <wx/colour.h>
#include <wx/dialog.h>
#include <wx/string.h>
#include <wx/window.h>

#include <functional>
#include <utility>
#include <vector>

class Button;
class wxBoxSizer;
class wxChoice;
class wxDC;
class wxMouseEvent;
class wxPaintEvent;
class wxPanel;
class wxPoint;
class wxRect;
class wxSizeEvent;
class wxSizer;
class wxStaticText;

namespace Slic3r {
namespace GUI {

// One "Color Mixing" entry: 2..4 real filaments and the ratio of each of them.
struct ColorMixConfig
{
    std::vector<int>    filament_indices; // 0-based indices into the current filament list
    std::vector<double> ratios;           // parallel to filament_indices, sum ~= 1.0

    bool valid() const
    {
        return filament_indices.size() >= 2 && filament_indices.size() <= 4 &&
               filament_indices.size() == ratios.size();
    }
};

// Number of layers / stripes used to convert the ratios into an integer pattern.
constexpr int COLOR_MIX_CYCLE_LEN = 10;

// Largest-remainder rounding of the ratios so that the counts sum up to "total".
std::vector<int> color_mix_integer_counts(const std::vector<double>& ratios, int total = COLOR_MIX_CYCLE_LEN);
// Same rounding with total == 100 (for the UI).
std::vector<int> color_mix_percentages(const std::vector<double>& ratios);

// e.g. "1222333444": the filament numbers (1-based), each repeated by its integer count.
wxString color_mix_pattern(const ColorMixConfig& config);

// Filament number (1-based) used on the given layer of the pattern; the pattern cycles.
// This is the core of the per-layer alternation used by the slicer.
int color_mix_filament_for_layer(const ColorMixConfig& config, size_t layer_index);

// Weighted RGB blend of the selected filaments.
wxColour color_mix_preview_colour(const ColorMixConfig& config);

// "0:10,1:33,2:27,3:30|..." (0-based slots, percentages summing to 100 per entry)
wxString                    color_mix_configs_to_string(const std::vector<ColorMixConfig>& configs);
std::vector<ColorMixConfig> color_mix_configs_from_string(const wxString& text);

// Gradient area used to pick the mixing ratio:
// 2 filaments -> a line, 3 -> a triangle, 4 -> a square.
class ColorMixRatioCanvas : public wxWindow
{
public:
    explicit ColorMixRatioCanvas(wxWindow* parent);

    void set_colours(const std::vector<wxColour>& colours);
    void set_weights(const std::vector<double>& weights);
    const std::vector<double>& weights() const { return m_weights; }

    void set_on_change(std::function<void()> on_change) { m_on_change = std::move(on_change); }

private:
    void on_paint(wxPaintEvent& evt);
    void on_size(wxSizeEvent& evt);
    void on_mouse(wxMouseEvent& evt);
    void rebuild_gradient();

    wxRect              content_rect() const;
    wxPoint             point_from_weights() const;
    std::vector<double> weights_from_point(const wxPoint& pt) const;
    void                set_weights_from_point(const wxPoint& pt, bool notify);

    void draw_vertices(wxDC& dc) const;
    void draw_knob(wxDC& dc) const;

    std::vector<wxColour> m_colours;
    std::vector<double>   m_weights;
    wxBitmap              m_gradient;
    bool                  m_dirty { true };
    bool                  m_dragging { false };
    std::function<void()> m_on_change;
};

// The "how the mix is composed" strip (10 horizontal bands, e.g. 1,2,2,2,3,3,3,4,4,4).
class ColorMixPreviewStrip : public wxWindow
{
public:
    explicit ColorMixPreviewStrip(wxWindow* parent);

    void set_data(const std::vector<wxColour>& colours, const std::vector<int>& counts);

private:
    void on_paint(wxPaintEvent& evt);

    std::vector<wxColour> m_stripes;
};

// The Color Mixing editor dialog (Ratio / Cycle / Match tabs, only Ratio is implemented for now).
class ColorMixDialog : public wxDialog
{
public:
    ColorMixDialog(wxWindow* parent, const ColorMixConfig& initial);
    ~ColorMixDialog() override = default;

    const ColorMixConfig& config() const { return m_config; }

private:
    struct Row
    {
        wxWindow* panel { nullptr };
        wxWindow* chip { nullptr };
        wxChoice* choice { nullptr };
    };

    void build_ui();
    wxPanel* build_ratio_page(wxWindow* parent);
    wxPanel* build_placeholder_page(wxWindow* parent, const wxString& text);
    void build_tab_bar(wxBoxSizer* parent_sizer);

    void rebuild_rows();
    void on_row_changed(size_t row);
    void set_filament_count(size_t count, bool keep_ratios);
    void update_ui_from_weights();
    void update_ratio_labels();
    void update_preview();
    void refresh_row_chips();
    void accept();

    std::vector<wxString> m_names;
    std::vector<wxColour> m_colours;

    std::vector<int>    m_sel;     // filament index per row
    std::vector<double> m_weights; // ratio per row

    std::vector<Row> m_rows;
    wxBoxSizer*      m_rows_sizer { nullptr };

    ColorMixRatioCanvas*  m_canvas { nullptr };
    ColorMixPreviewStrip* m_preview { nullptr };
    wxStaticText*         m_pattern_label { nullptr };
    wxPanel*              m_mix_effect { nullptr };
    wxBoxSizer*           m_ratio_values_sizer { nullptr };

    wxPanel* m_ratio_page { nullptr };
    wxPanel* m_cycle_page { nullptr };
    wxPanel* m_match_page { nullptr };

    Button* m_tab_ratio { nullptr };
    Button* m_tab_cycle { nullptr };
    Button* m_tab_match { nullptr };
    Button* m_btn_add { nullptr };
    Button* m_btn_remove { nullptr };
    Button* m_btn_ok { nullptr };

    ColorMixConfig m_config;
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_ColorMixDialog_hpp_
