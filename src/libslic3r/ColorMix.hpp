#ifndef slic3r_ColorMix_hpp_
#define slic3r_ColorMix_hpp_

#include <cstddef>
#include <mutex>
#include <vector>

namespace Slic3r {

// Number of layers / stripes used to convert the mixing ratios into an integer pattern.
constexpr int COLOR_MIX_CYCLE_LEN = 10;

// One "Color Mixing" entry: 2..4 real filaments and their ratios.
struct ColorMixData
{
    std::vector<int>    filament_indices; // 0-based indices into the print's filament list
    std::vector<double> ratios;           // parallel to filament_indices, sum ~= 1.0

    bool valid() const
    {
        return filament_indices.size() >= 2 && filament_indices.size() <= 4 &&
               filament_indices.size() == ratios.size();
    }
};

// Largest-remainder rounding of the ratios so that the counts sum up to "total".
std::vector<int> color_mix_integer_counts(const std::vector<double>& ratios, int total = COLOR_MIX_CYCLE_LEN);

// 0-based real filament index used on the given layer of the pattern (the pattern cycles),
// or -1 when the config is invalid.
int color_mix_filament_for_layer(const ColorMixData& config, size_t layer_index);

// Process wide registry shared by the GUI (writer) and the slicer (reader).
// The mixed filaments are virtual: their indices are real_filament_count + 0 .. real_filament_count + size() - 1.
class ColorMixStore
{
public:
    static ColorMixStore& instance();

    void                      set_configs(std::vector<ColorMixData> configs);
    std::vector<ColorMixData> configs() const;
    size_t                    size() const;

    // 0-based first constituent of the given mixed filament, or -1.
    int first_filament(size_t mixed_index) const;

    // Resolve the 0-based mixed filament index to the 0-based real filament index used on layer_index.
    // Returns -1 if mixed_index is out of range.
    int resolve(size_t mixed_index, size_t layer_index) const;

private:
    ColorMixStore() = default;
    ColorMixStore(const ColorMixStore&) = delete;
    ColorMixStore& operator=(const ColorMixStore&) = delete;

    mutable std::mutex        m_mutex;
    std::vector<ColorMixData> m_configs;
};

} // namespace Slic3r

#endif // slic3r_ColorMix_hpp_
