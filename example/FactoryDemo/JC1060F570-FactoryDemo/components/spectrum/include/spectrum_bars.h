#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Geometry, FFT-bin grouping and level smoothing for spectrum rendering. */

/** @brief One slot is one bar plus one gap; the gap stays 1px on every panel. */
#define SPECTRUM_BAR_GAP 1
/** @brief A band with no content still draws this line, so the plot never looks empty. */
#define SPECTRUM_BAR_MIN_HEIGHT 1
/** @brief The peak-hold marker drawn outside both ends of a bar. */
#define SPECTRUM_PEAK_CAP_HEIGHT 2
/** @brief The cap falls at half the bar's release rate. */
#define SPECTRUM_PEAK_CAP_FALL_DIVISOR 2U

/**
 * @brief Fixed bar counts: the panel decides the count, and the slot width decides the bar width,
 * so the strip always spans the plot exactly.
 */
#define SPECTRUM_BARS_WIDE 64U
#define SPECTRUM_BARS_COMPACT 48U
#define SPECTRUM_MAX_BARS SPECTRUM_BARS_WIDE

/** @brief Reference panel widths and the threshold selecting a bar count. */
#define SPECTRUM_COMPACT_WIDTH 800
#define SPECTRUM_WIDE_WIDTH 1024
#define SPECTRUM_COUNT_SWITCH_WIDTH 912

/** @brief Lower frequency limit shared by the spectrum grouping modes, in Hz. */
#define SPECTRUM_GROUP_LOW_HZ 100U
/** @brief Visual grouping allocates one quarter to lows, one half to mids and one quarter to highs. */
#define SPECTRUM_GROUP_QUARTER_DIVISOR 4U
/** @brief Knee frequencies: the low band ends (and the common band starts) at 1 kHz, and the top end starts at 4 kHz. */
#define SPECTRUM_GROUP_MID_KNEE_HZ 1000U
#define SPECTRUM_GROUP_HIGH_KNEE_HZ 4000U
/** @brief Upper displayed frequency limit, in Hz. */
#define SPECTRUM_BAR_HIGH_HZ 20000U

/** @brief Dot grid dimensions in pixels: a 3-pixel dot and 1-pixel gap. */
#define SPECTRUM_DOT_PITCH 4
#define SPECTRUM_DOT_SIZE 3
#define SPECTRUM_DOT_GAP (SPECTRUM_DOT_PITCH - SPECTRUM_DOT_SIZE)
#define SPECTRUM_DOTS_PER_SLOT 4U

/** @brief Block style uses 48 columns with 2-pixel gaps. */
#define SPECTRUM_BARS_BLOCKS 48U
#define SPECTRUM_CELL_GAP 2
/** @brief How many cell rows the panel is cut into, counted up from the bottom edge. */
#define SPECTRUM_CELL_ROWS 32U
/** @brief The peak cap is half a cell tall. */
#define SPECTRUM_CELL_CAP_DIVISOR 2U

/** @brief Bar decay per frame in pixels; dot-matrix style doubles the rate. */
#define SPECTRUM_BAR_FALL_PIXELS 10.0f
#define SPECTRUM_DOT_FALL_MULTIPLIER 2.0f

/** @brief Frequency grouping mode for spectrum bars. */
typedef enum {
    SPECTRUM_GROUP_LINEAR = 0, /**< Uniform FFT-bin grouping. */
    SPECTRUM_GROUP_VISUAL, /**< Three logarithmic bands with quarter/half/quarter boundaries. */
    SPECTRUM_GROUP_LOG, /**< Logarithmic grouping from the low cutoff to the upper display limit. */
    SPECTRUM_GROUP_COUNT,
} spectrum_group_t;

/** @brief Spectrum rendering style. */
typedef enum {
    SPECTRUM_STYLE_BARS = 0, /**< Solid bars mirrored about the midline. */
    SPECTRUM_STYLE_DOT_MATRIX, /**< A dot grid grown up from the bottom edge, with no mirrored half. */
    SPECTRUM_STYLE_BLOCKS, /**< A grid of solid cells with 2px gaps, filled from the bottom edge. */
    SPECTRUM_STYLE_COUNT,
} spectrum_style_t;

/** @brief Half-open FFT-bin range feeding one bar. */
typedef struct {
    uint16_t start_bin; /**< Bin range feeding one bar: [start_bin, end_bin). */
    uint16_t end_bin;
} spectrum_bar_span_t;

/** @brief FFT-bin spans for the active bars. */
typedef struct {
    size_t count;
    spectrum_bar_span_t spans[SPECTRUM_MAX_BARS];
} spectrum_bar_map_t;

/**
 * @brief Bar count for a plot width: 64 on the 1024x600 panel, 48 on the 800x480 one.
 * @param width Width in pixels.
 * @return Bar count for a plot width: 64 on the 1024x600 panel, 48 on the 800x480 one.
 */
size_t spectrum_bars_count(int32_t width);

/**
 * @brief Bar count for one style: the block grid draws 2px gaps on both axes, so it uses
 * SPECTRUM_BARS_BLOCKS on every panel; the other styles follow the panel.
 */
size_t spectrum_bars_count_for_style(int32_t width, spectrum_style_t style);

/**
 * @brief Slot bounds of one bar, as inclusive pixel columns, before the gap is taken out. The
 * dot-matrix style tiles its dots across the whole slot.
 */
void spectrum_slot_bounds(int32_t x1, int32_t width, size_t index, size_t count,
                          int32_t *slot_x1, int32_t *slot_x2);

/**
 * @brief Return inclusive bar bounds with a one-pixel trailing gap; distribute width remainders across slots.
 * @param x1 Starting horizontal coordinate.
 * @param width Width in pixels.
 * @param index Index of the item.
 * @param count Number of items.
 * @param[out] bar_x1 Receives the first bar column.
 * @param[out] bar_x2 Receives the last bar column.
 */
void spectrum_bars_slot(int32_t x1, int32_t width, size_t index, size_t count,
                        int32_t *bar_x1, int32_t *bar_x2);

/**
 * @brief The next grouping in the button's cycle: linear -> visual -> log -> linear.
 * @param group Frequency grouping mode.
 * @return The next grouping in the button's cycle: linear -> visual -> log -> linear.
 */
spectrum_group_t spectrum_bars_group_next(spectrum_group_t group);

/**
 * @brief Sanitise a stored style: anything the current firmware does not know (an older or
 * corrupted value) falls back to the solid-bar style.
 */
spectrum_style_t spectrum_style_sanitize(uint8_t stored);

/**
 * @brief Return inclusive dot bounds within a slot while preserving square dot dimensions.
 * @param x1 Starting horizontal coordinate.
 * @param x2 Ending horizontal coordinate.
 * @param index Index of the item.
 * @param count Number of items.
 * @param[out] dot_x1 Receives the first dot column.
 * @param[out] dot_x2 Receives the last dot column.
 */
void spectrum_dot_span(int32_t x1, int32_t x2, size_t index, size_t count,
                       int32_t *dot_x1, int32_t *dot_x2);

/**
 * @brief Dot rows a plot of this height holds; row 0 is the bottom edge.
 * @param plot_height Plot height in pixels.
 * @return Dot rows a plot of this height holds; row 0 is the bottom edge.
 */
size_t spectrum_dot_rows(int32_t plot_height);

/**
 * @brief Cell rows a plot of this height holds; row 0 is the bottom edge.
 * @param plot_height Plot height in pixels.
 * @return Cell rows a plot of this height holds; row 0 is the bottom edge.
 */
size_t spectrum_cell_rows(int32_t plot_height);

/**
 * @brief Return inclusive cell-row bounds with a two-pixel gap; row zero is at the bottom.
 * @param y1 Starting vertical coordinate.
 * @param y2 Ending vertical coordinate.
 * @param index Index of the item.
 * @param rows Number of rows.
 * @param[out] cell_y1 Receives the first cell row coordinate.
 * @param[out] cell_y2 Receives the last cell row coordinate.
 */
void spectrum_cell_row(int32_t y1, int32_t y2, size_t index, size_t rows,
                       int32_t *cell_y1, int32_t *cell_y2);

/**
 * @brief Bounds of one cell inside a bar slot, after the 2px gap is taken out.
 * @param slot_x1 First slot column.
 * @param slot_x2 Last slot column.
 * @param cell_x1 Receives the first cell column.
 * @param cell_x2 Receives the last cell column.
 */
void spectrum_cell_span(int32_t slot_x1, int32_t slot_x2, int32_t *cell_x1,
                        int32_t *cell_x2);

/**
 * @brief Cells a level lights: whole cells, but never fewer than the bottom one.
 * @param level Level to use.
 * @param rows Number of rows.
 * @return Cells a level lights: whole cells, but never fewer than the bottom one.
 */
size_t spectrum_cells_lit(float level, size_t rows);

/**
 * @brief Height of the peak cap: half a cell, at least one pixel.
 * @param plot_height Plot height in pixels.
 * @param rows Number of rows.
 * @return Height of the peak cap: half a cell, at least one pixel.
 */
int32_t spectrum_cell_cap_height(int32_t plot_height, size_t rows);

/**
 * @brief Release step of one frame, as a fraction of the plot height, for a style. A height of
 * zero falls back to a full-plot step so a caller can never stall.
 */
float spectrum_bar_fall_step(spectrum_style_t style, int32_t plot_height);

/**
 * @brief Centre frequency of one bar's bin span, in Hz (0 for an invalid index).
 * @param map Bar-to-bin mapping.
 * @param index Index of the item.
 * @param fft_size FFT transform length.
 * @param sample_rate Sample rate in Hz.
 * @return Centre frequency of one bar's bin span, in Hz (0 for an invalid index).
 */
float spectrum_bar_center_hz(const spectrum_bar_map_t *map, size_t index,
                             uint32_t fft_size, uint32_t sample_rate);

/**
 * @brief Apply (hz / knee)^tilt below the visual grouping mid knee; return 1 for other groups or zero tilt.
 * @param group Frequency grouping mode.
 * @param hz Frequency in Hz.
 * @param tilt Tilt exponent.
 * @return Tilt weight, or 1 for other groups or zero tilt.
 */
float spectrum_bar_tilt_weight(spectrum_group_t group, float hz, float tilt);

/**
 * @brief Bar height in the dot-matrix style: quantised down to the dot pitch, but never below one
 * row, so an empty band still lights the bottom row.
 */
int32_t spectrum_bar_dot_height(float level, int32_t plot_height, int32_t pitch);

/**
 * @brief Build bar-to-bin spans; fft_bins is the half-spectrum length and fft_size is the transform length.
 * @param map Bar-to-bin mapping.
 * @param count Number of items.
 * @param group Frequency grouping mode.
 * @param fft_bins Number of FFT bins.
 * @param fft_size FFT transform length.
 * @param sample_rate Sample rate in Hz.
 */
void spectrum_bar_map_build(spectrum_bar_map_t *map, size_t count, spectrum_group_t group,
                            uint32_t fft_bins, uint32_t fft_size, uint32_t sample_rate);

/**
 * @brief Reduce each bar's bin range to its largest magnitude.
 * @param map Bar-to-bin mapping.
 * @param magnitudes FFT magnitudes.
 * @param[out] levels Receives the per-band levels.
 */
void spectrum_bar_map_aggregate(const spectrum_bar_map_t *map, const float *magnitudes,
                                float *levels);

/**
 * @brief Attack immediately, release by at most fall_step.
 * @param current Current value.
 * @param target Target value.
 * @param fall_step Maximum release step per frame.
 * @return Updated level after attack or release.
 */
float spectrum_bar_step_level(float current, float target, float fall_step);

/**
 * @brief Rise with the bar, fall at half its release rate.
 * @param cap Current peak cap level.
 * @param level Level to use.
 * @param fall_step Maximum release step per frame.
 * @return Updated peak-cap level.
 */
float spectrum_bar_step_cap(float cap, float level, float fall_step);

/**
 * @brief Bar height in pixels: at least SPECTRUM_BAR_MIN_HEIGHT, never past the plot.
 * @param level Level to use.
 * @param plot_height Plot height in pixels.
 * @return Bar height in pixels, clamped to the plot.
 */
int32_t spectrum_bar_height(float level, int32_t plot_height);

/**
 * @brief Cap height in pixels, clamped so the 2px cap still fits inside the plot half.
 * @param level Level to use.
 * @param plot_height Plot height in pixels.
 * @return Cap height in pixels, clamped so the 2px cap still fits inside the plot half.
 */
int32_t spectrum_bar_cap_height(float level, int32_t plot_height);

#ifdef __cplusplus
}
#endif
