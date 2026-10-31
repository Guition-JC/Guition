#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SPECTRUM_FFT_SIZE 2048U
#define SPECTRUM_FFT_BINS (SPECTRUM_FFT_SIZE / 2U)
#define SPECTRUM_MAX_CHANNELS 8U
#define SPECTRUM_MICROPHONE_COUNT 2U

/** @brief PCM assembly, FFT workspace and current magnitudes. */
typedef struct {
    uint32_t sample_rate;
    uint8_t channels;
    uint8_t bits_per_sample;
    uint32_t next_sample_index;
    uint32_t windows_processed;
    size_t pending_bytes;
    int16_t microphone_samples[SPECTRUM_MICROPHONE_COUNT][SPECTRUM_FFT_SIZE]; /**< Only ch0 and ch2 are microphone inputs; retain their windows only. */
    uint8_t pending_frame[2U * SPECTRUM_MAX_CHANNELS];
    float magnitudes[SPECTRUM_FFT_BINS];
    float window[SPECTRUM_FFT_SIZE];
    float real[SPECTRUM_FFT_SIZE];
    float imag[SPECTRUM_FFT_SIZE];
} spectrum_fft_t;

/**
 * @brief Initialize FFT state for the supplied PCM format.
 * @param fft FFT state.
 * @param sample_rate Sample rate in Hz.
 * @param channels Number of audio channels.
 * @param bits_per_sample Bits per PCM sample.
 * @return true if FFT state was initialized successfully.
 */
bool spectrum_fft_init(spectrum_fft_t *fft, uint32_t sample_rate, uint8_t channels,
                       uint8_t bits_per_sample);
/**
 * @brief Clear pending input and accumulated spectrum results.
 * @param fft FFT state.
 */
void spectrum_fft_reset(spectrum_fft_t *fft);
/**
 * @brief Release FFT processing state.
 * @param fft FFT state.
 */
void spectrum_fft_deinit(spectrum_fft_t *fft);

/**
 * @brief Consume interleaved PCM, retaining an incomplete trailing frame for the next call. Combine microphone channels 0 and 2 before FFT.
 * @param fft FFT state.
 * @param data Interleaved PCM bytes.
 * @param len Number of PCM bytes supplied.
 * @return Number of PCM bytes consumed.
 */
size_t spectrum_fft_feed(spectrum_fft_t *fft, const uint8_t *data, size_t len);

/**
 * @brief Borrow the current half-spectrum magnitude array.
 * @param fft FFT state.
 * @return Borrowed half-spectrum magnitude array.
 */
const float *spectrum_fft_magnitudes(const spectrum_fft_t *fft);
/**
 * @brief Return the number of completed FFT windows.
 * @param fft FFT state.
 * @return The number of completed FFT windows.
 */
uint32_t spectrum_fft_windows_processed(const spectrum_fft_t *fft);
/**
 * @brief Apply the fixed low-frequency cutoff and magnitude noise gate.
 * @param fft FFT state.
 */
void spectrum_fft_apply_noise_gate(spectrum_fft_t *fft);

#ifdef __cplusplus
}
#endif
