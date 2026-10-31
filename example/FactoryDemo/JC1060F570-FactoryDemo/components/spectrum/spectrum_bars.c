#include "spectrum_bars.h"

#include <math.h>

size_t spectrum_bars_count(int32_t width)
{
    if (width <= 0) {
        return 0U;
    }
    /** Two supported panels, two fixed counts. */
    return width >= SPECTRUM_COUNT_SWITCH_WIDTH ? SPECTRUM_BARS_WIDE
                                                : SPECTRUM_BARS_COMPACT;
}

size_t spectrum_bars_count_for_style(int32_t width, spectrum_style_t style)
{
    if (style == SPECTRUM_STYLE_BLOCKS) {
        /** Use the compact column count for block-style rendering. */
        return SPECTRUM_BARS_BLOCKS;
    }
    return spectrum_bars_count(width);
}

void spectrum_slot_bounds(int32_t x1, int32_t width, size_t index, size_t count,
                          int32_t *slot_x1, int32_t *slot_x2)
{
    if (count == 0U || width <= 0) {
        return;
    }
    if (index >= count) {
        index = count - 1U;
    }
    const int32_t start = x1 + (int32_t)(((int64_t)index * width) / (int64_t)count);
    const int32_t next = x1 + (int32_t)(((int64_t)(index + 1U) * width) / (int64_t)count);
    if (slot_x1 != NULL) {
        *slot_x1 = start;
    }
    if (slot_x2 != NULL) {
        *slot_x2 = next > start ? next - 1 : start;
    }
}

void spectrum_bars_slot(int32_t x1, int32_t width, size_t index, size_t count,
                        int32_t *bar_x1, int32_t *bar_x2)
{
    if (count == 0U || width <= 0) {
        return;
    }
    int32_t start = x1;
    int32_t end = x1;
    spectrum_slot_bounds(x1, width, index, count, &start, &end);
    /** Reserve the final column of each bar slot as a gap. */
    end -= SPECTRUM_BAR_GAP;
    if (bar_x1 != NULL) {
        *bar_x1 = start;
    }
    if (bar_x2 != NULL) {
        *bar_x2 = end > start ? end : start;
    }
}

spectrum_group_t spectrum_bars_group_next(spectrum_group_t group)
{
    if (group >= SPECTRUM_GROUP_COUNT) {
        return SPECTRUM_GROUP_LINEAR;
    }
    return (spectrum_group_t)(((unsigned)group + 1U) % (unsigned)SPECTRUM_GROUP_COUNT);
}

spectrum_style_t spectrum_style_sanitize(uint8_t stored)
{
    /** Fall back to the solid-bar style for unsupported values. */
    return stored < (uint8_t)SPECTRUM_STYLE_COUNT ? (spectrum_style_t)stored
                                                  : SPECTRUM_STYLE_BARS;
}

/**
 * @brief Limit the displayed band to the smaller of 20 kHz and Nyquist.
 * @param sample_rate Sampling rate in Hz.
 * @return Upper display frequency in Hz.
 */
static float spectrum_bars_high_hz(uint32_t sample_rate)
{
    const float nyquist = (float)sample_rate * 0.5f;
    const float cap = (float)SPECTRUM_BAR_HIGH_HZ;
    return nyquist < cap ? nyquist : cap;
}

/**
 * @brief Return the FFT-bin count within the displayed frequency band.
 * @param fft_bins Number of available FFT bins.
 * @param fft_size FFT size.
 * @param sample_rate Sampling rate in Hz.
 * @return Number of bins in the displayed band.
 */
static size_t spectrum_bars_usable_bins(uint32_t fft_bins, uint32_t fft_size,
                                       uint32_t sample_rate)
{
    if (sample_rate == 0U || fft_size == 0U) {
        return fft_bins;
    }
    size_t bins = (size_t)(spectrum_bars_high_hz(sample_rate) * (float)fft_size /
                               (float)sample_rate +
                           0.5f);
    if (bins < 1U) {
        bins = 1U;
    }
    if (bins > fft_bins) {
        bins = fft_bins;
    }
    return bins;
}

static size_t spectrum_bars_bin_for_hz(float hz, uint32_t fft_size, uint32_t sample_rate)
{
    if (sample_rate == 0U || hz <= 0.0f) {
        return 0U;
    }
    const float bin = hz * (float)fft_size / (float)sample_rate;
    if (bin <= 0.0f) {
        return 0U;
    }
    return (size_t)(bin + 0.5f);
}

static float spectrum_bars_edge_hz(size_t index, size_t count, spectrum_group_t group,
                                   float low_hz, float high_hz)
{
    if (count < 2U || index == 0U) {
        return low_hz;
    }
    if (index >= count) {
        return high_hz;
    }
    const float position = (float)index;

    const float mid_knee = (float)SPECTRUM_GROUP_MID_KNEE_HZ;
    const float high_knee = (float)SPECTRUM_GROUP_HIGH_KNEE_HZ;
    if (group == SPECTRUM_GROUP_VISUAL && low_hz < mid_knee && mid_knee < high_knee &&
        high_knee < high_hz) {
        /** Quarters: the low band owns the left quarter, the common band the middle half and the rarely used top end the right quarter. */
        const size_t low_end = count / SPECTRUM_GROUP_QUARTER_DIVISOR;
        const size_t high_start = count - count / SPECTRUM_GROUP_QUARTER_DIVISOR;
        if (low_end > 0U && high_start > low_end && high_start < count) {
            if (index <= low_end) {
                /** @brief edge(0) = low, edge(low_end) = mid knee. */
                return low_hz * powf(mid_knee / low_hz, position / (float)low_end);
            }
            if (index <= high_start) {
                /** @brief edge(low_end) = mid knee, edge(high_start) = high knee. */
                return mid_knee * powf(high_knee / mid_knee,
                                       (position - (float)low_end) /
                                           (float)(high_start - low_end));
            }
            /** @brief edge(high_start) = high knee, edge(count) = high. */
            return high_knee * powf(high_hz / high_knee,
                                    (position - (float)high_start) /
                                        (float)(count - high_start));
        }
    }

    /** One geometric progression: edge(0) = low, edge(count) = high. */
    if (group == SPECTRUM_GROUP_LOG) {
        return low_hz * powf(high_hz / low_hz, position / (float)count);
    }
    /** Fallback for an unknown grouping; the linear grouping itself is built straight in bin space and never reaches this branch. */
    return low_hz + (high_hz - low_hz) * position / (float)count;
}

void spectrum_dot_span(int32_t x1, int32_t x2, size_t index, size_t count,
                       int32_t *dot_x1, int32_t *dot_x2)
{
    if (count == 0U || x2 < x1) {
        return;
    }
    if (index >= count) {
        index = count - 1U;
    }
    const int32_t width = x2 - x1 + 1;
    const int32_t start = x1 + (int32_t)(((int64_t)index * width) / (int64_t)count);
    const int32_t next = x1 + (int32_t)(((int64_t)(index + 1U) * width) / (int64_t)count);
    /** Preserve square dots and distribute remaining slot pixels among gaps. */
    int32_t end = start + SPECTRUM_DOT_SIZE - 1;
    const int32_t gap_end = next - 1 - SPECTRUM_DOT_GAP;
    if (end > gap_end) {
        end = gap_end;
    }
    if (end < start) {
        end = start;
    }
    if (dot_x1 != NULL) {
        *dot_x1 = start;
    }
    if (dot_x2 != NULL) {
        *dot_x2 = end;
    }
}

size_t spectrum_dot_rows(int32_t plot_height)
{
    if (plot_height <= 0) {
        return 0U;
    }
    const size_t rows = (size_t)(plot_height / SPECTRUM_DOT_PITCH);
    return rows > 0U ? rows : 1U;
}

size_t spectrum_cell_rows(int32_t plot_height)
{
    if (plot_height <= 0) {
        return 0U;
    }
    /** Every row needs at least one lit pixel plus the gap to stay visible. */
    const size_t fit = (size_t)(plot_height / (SPECTRUM_CELL_GAP + 1));
    return fit < SPECTRUM_CELL_ROWS ? fit : SPECTRUM_CELL_ROWS;
}

void spectrum_cell_row(int32_t y1, int32_t y2, size_t index, size_t rows,
                       int32_t *cell_y1, int32_t *cell_y2)
{
    if (rows == 0U || y2 < y1) {
        return;
    }
    if (index >= rows) {
        index = rows - 1U;
    }
    const int32_t height = y2 - y1 + 1;
    /** Start at the bottom edge and distribute height remainders across rows. */
    const int32_t band_top =
        y2 - (int32_t)(((int64_t)(index + 1U) * height) / (int64_t)rows) + 1;
    const int32_t band_bottom = y2 - (int32_t)(((int64_t)index * height) / (int64_t)rows);
    int32_t top = band_top + SPECTRUM_CELL_GAP;
    if (top > band_bottom) {
        top = band_bottom;
    }
    if (cell_y1 != NULL) {
        *cell_y1 = top;
    }
    if (cell_y2 != NULL) {
        *cell_y2 = band_bottom;
    }
}

void spectrum_cell_span(int32_t slot_x1, int32_t slot_x2, int32_t *cell_x1,
                        int32_t *cell_x2)
{
    if (slot_x2 < slot_x1) {
        return;
    }
    int32_t end = slot_x2 - SPECTRUM_CELL_GAP;
    if (end < slot_x1) {
        end = slot_x1;
    }
    if (cell_x1 != NULL) {
        *cell_x1 = slot_x1;
    }
    if (cell_x2 != NULL) {
        *cell_x2 = end;
    }
}

size_t spectrum_cells_lit(float level, size_t rows)
{
    if (rows == 0U) {
        return 0U;
    }
    size_t lit = 0U;
    if (level > 0.0f) {
        const float scaled = level * (float)rows;
        lit = scaled >= (float)rows ? rows : (size_t)scaled;
    }
    /** An empty band still lights its bottom cell, the way the solid style still draws one pixel. */
    if (lit == 0U) {
        lit = 1U;
    }
    return lit > rows ? rows : lit;
}

int32_t spectrum_cell_cap_height(int32_t plot_height, size_t rows)
{
    if (plot_height <= 0 || rows == 0U) {
        return 0;
    }
    const int32_t half =
        (plot_height / (int32_t)rows) / (int32_t)SPECTRUM_CELL_CAP_DIVISOR;
    return half > 0 ? half : 1;
}

float spectrum_bar_fall_step(spectrum_style_t style, int32_t plot_height)
{
    if (plot_height <= 0) {
        return 1.0f;
    }
    const float multiplier =
        style == SPECTRUM_STYLE_DOT_MATRIX ? SPECTRUM_DOT_FALL_MULTIPLIER : 1.0f;
    return (SPECTRUM_BAR_FALL_PIXELS * multiplier) / (float)plot_height;
}

float spectrum_bar_center_hz(const spectrum_bar_map_t *map, size_t index,
                             uint32_t fft_size, uint32_t sample_rate)
{
    if (map == NULL || index >= map->count || fft_size == 0U || sample_rate == 0U) {
        return 0.0f;
    }
    const float bin =
        0.5f * (float)(map->spans[index].start_bin + map->spans[index].end_bin);
    return bin * (float)sample_rate / (float)fft_size;
}

float spectrum_bar_tilt_weight(spectrum_group_t group, float hz, float tilt)
{
    /** Only the visual grouping widens the low end, so only it needs the correction; every other grouping - and a disabled tilt - multiplies the display by exactly one. */
    if (group != SPECTRUM_GROUP_VISUAL || !(tilt > 0.0f) || !(hz > 0.0f)) {
        return 1.0f;
    }
    const float knee = (float)SPECTRUM_GROUP_MID_KNEE_HZ;
    if (hz >= knee) {
        return 1.0f;
    }
    float ratio = hz / knee;
    if (ratio < 1.0e-4f) {
        ratio = 1.0e-4f;
    }
    return powf(ratio, tilt);
}

int32_t spectrum_bar_dot_height(float level, int32_t plot_height, int32_t pitch)
{
    if (plot_height <= 0 || pitch <= 0) {
        return 0;
    }
    const int32_t limit = (plot_height / pitch) * pitch;
    int32_t lit = 0;
    if (level > 0.0f) {
        lit = (int32_t)(level * (float)plot_height);
    }
    /** Quantise down onto the dot grid, but never past the bottom row: an empty band still lights one row, the way the solid style still draws 1px. */
    lit = (lit / pitch) * pitch;
    if (lit < pitch) {
        lit = pitch;
    }
    if (lit > limit) {
        lit = limit;
    }
    return lit;
}

void spectrum_bar_map_build(spectrum_bar_map_t *map, size_t count, spectrum_group_t group,
                            uint32_t fft_bins, uint32_t fft_size, uint32_t sample_rate)
{
    if (map == NULL) {
        return;
    }
    if (count > SPECTRUM_MAX_BARS) {
        count = SPECTRUM_MAX_BARS;
    }
    map->count = count;
    if (count == 0U || fft_bins == 0U) {
        return;
    }

    if (group == SPECTRUM_GROUP_LINEAR || sample_rate == 0U) {
        /** Divide the displayed FFT frequency range into equal spans. */
        const size_t usable = spectrum_bars_usable_bins(fft_bins, fft_size, sample_rate);
        const size_t first = spectrum_bars_bin_for_hz((float)SPECTRUM_GROUP_LOW_HZ,
                                                     fft_size, sample_rate);
        const size_t span = usable > first ? usable - first : 1U;
        for (size_t i = 0; i < count; ++i) {
            size_t start = first + (i * span) / count;
            size_t end = first + ((i + 1U) * span) / count;
            if (start >= usable) {
                start = usable - 1U;
            }
            if (end <= start) {
                end = start + 1U;
            }
            if (end > usable) {
                end = usable;
            }
            map->spans[i].start_bin = (uint16_t)start;
            map->spans[i].end_bin = (uint16_t)end;
        }
        return;
    }

    float low_hz = (float)SPECTRUM_GROUP_LOW_HZ;
    const float high_hz = spectrum_bars_high_hz(sample_rate);
    const size_t usable = spectrum_bars_usable_bins(fft_bins, fft_size, sample_rate);
    if (low_hz >= high_hz) {
        low_hz = high_hz * 0.5f;
    }
    for (size_t i = 0; i < count; ++i) {
        size_t start = spectrum_bars_bin_for_hz(
            spectrum_bars_edge_hz(i, count, group, low_hz, high_hz), fft_size, sample_rate);
        size_t end = spectrum_bars_bin_for_hz(
            spectrum_bars_edge_hz(i + 1U, count, group, low_hz, high_hz), fft_size, sample_rate);
        if (start >= usable) {
            start = usable - 1U;
        }
        if (end <= start) {
            end = start + 1U;
        }
        if (end > usable) {
            end = usable;
        }
        map->spans[i].start_bin = (uint16_t)start;
        map->spans[i].end_bin = (uint16_t)end;
    }
}

void spectrum_bar_map_aggregate(const spectrum_bar_map_t *map, const float *magnitudes,
                                float *levels)
{
    if (map == NULL || magnitudes == NULL || levels == NULL) {
        return;
    }
    for (size_t i = 0; i < map->count; ++i) {
        const uint16_t start = map->spans[i].start_bin;
        const uint16_t end = map->spans[i].end_bin;
        float peak = 0.0f;
        for (uint16_t bin = start; bin < end; ++bin) {
            if (magnitudes[bin] > peak) {
                peak = magnitudes[bin];
            }
        }
        levels[i] = peak;
    }
}

float spectrum_bar_step_level(float current, float target, float fall_step)
{
    if (target >= current) {
        /** Attack immediately so real transients are never delayed. */
        return target;
    }
    const float released = current - fall_step;
    return released > target ? released : target;
}

float spectrum_bar_step_cap(float cap, float level, float fall_step)
{
    if (level >= cap) {
        /** The cap rides the bar up, so a rising bar never leaves it behind. */
        return level;
    }
    const float step = fall_step / (float)SPECTRUM_PEAK_CAP_FALL_DIVISOR;
    const float released = cap - step;
    return released > level ? released : level;
}

int32_t spectrum_bar_height(float level, int32_t plot_height)
{
    if (plot_height <= 0) {
        return 0;
    }
    int32_t height = (int32_t)(level * (float)plot_height);
    if (height < SPECTRUM_BAR_MIN_HEIGHT) {
        height = SPECTRUM_BAR_MIN_HEIGHT;
    }
    if (height > plot_height) {
        height = plot_height;
    }
    return height;
}

int32_t spectrum_bar_cap_height(float level, int32_t plot_height)
{
    if (plot_height <= 0) {
        return 0;
    }
    /** Keep room for the cap itself: it is drawn outside the bar's end. */
    int32_t limit = plot_height - SPECTRUM_PEAK_CAP_HEIGHT;
    if (limit < SPECTRUM_BAR_MIN_HEIGHT) {
        limit = SPECTRUM_BAR_MIN_HEIGHT;
    }
    int32_t height = (int32_t)(level * (float)plot_height);
    if (height < SPECTRUM_BAR_MIN_HEIGHT) {
        height = SPECTRUM_BAR_MIN_HEIGHT;
    }
    if (height > limit) {
        height = limit;
    }
    return height;
}
