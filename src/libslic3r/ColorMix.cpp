#include "ColorMix.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <utility>

namespace Slic3r {

namespace {

void normalize_ratios(std::vector<double>& ratios)
{
    double sum = 0.0;
    for (double& ratio : ratios) {
        ratio = std::max(0.0, ratio);
        sum += ratio;
    }
    if (sum <= 1e-9) {
        const double equal = ratios.empty() ? 0.0 : 1.0 / double(ratios.size());
        for (double& ratio : ratios) ratio = equal;
        return;
    }
    for (double& ratio : ratios) ratio /= sum;
}

} // namespace

std::vector<int> color_mix_integer_counts(const std::vector<double>& ratios, int total)
{
    const int        count_size = int(ratios.size());
    std::vector<int> counts(count_size, 0);
    if (count_size == 0 || total <= 0)
        return counts;

    double sum = 0.0;
    for (double ratio : ratios) sum += std::max(0.0, ratio);
    if (sum <= 1e-9) {
        for (int i = 0; i < total; ++i) counts[i % count_size]++;
        return counts;
    }

    std::vector<double> fractions(count_size, 0.0);
    int                 assigned = 0;
    for (int i = 0; i < count_size; ++i) {
        const double quota = std::max(0.0, ratios[i]) / sum * total;
        counts[i]          = int(std::floor(quota));
        fractions[i]       = quota - counts[i];
        assigned += counts[i];
    }

    std::vector<int> order(count_size);
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&fractions](int a, int b) { return fractions[a] > fractions[b]; });

    int k = 0;
    while (assigned < total) {
        counts[order[k % count_size]]++;
        ++assigned;
        ++k;
    }
    return counts;
}

int color_mix_filament_for_layer(const ColorMixData& config, size_t layer_index)
{
    if (!config.valid())
        return -1;

    const std::vector<int> counts = color_mix_integer_counts(config.ratios);
    int                    total  = 0;
    for (int count : counts) total += count;
    if (total <= 0)
        return config.filament_indices.front();

    int index = int(layer_index % size_t(total));
    for (size_t i = 0; i < counts.size(); ++i) {
        if (index < counts[i])
            return config.filament_indices[i];
        index -= counts[i];
    }
    return config.filament_indices.back();
}

ColorMixStore& ColorMixStore::instance()
{
    static ColorMixStore store;
    return store;
}

void ColorMixStore::set_configs(std::vector<ColorMixData> configs)
{
    for (ColorMixData& config : configs) {
        if (config.filament_indices.size() == config.ratios.size())
            normalize_ratios(config.ratios);
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    m_configs = std::move(configs);
}

std::vector<ColorMixData> ColorMixStore::configs() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_configs;
}

size_t ColorMixStore::size() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_configs.size();
}

int ColorMixStore::first_filament(size_t mixed_index) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (mixed_index >= m_configs.size() || !m_configs[mixed_index].valid())
        return -1;
    return m_configs[mixed_index].filament_indices.front();
}

int ColorMixStore::resolve(size_t mixed_index, size_t layer_index) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (mixed_index >= m_configs.size())
        return -1;
    return color_mix_filament_for_layer(m_configs[mixed_index], layer_index);
}

} // namespace Slic3r
