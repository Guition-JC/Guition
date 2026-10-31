#include "spectrum_fft.h"

#include <math.h>
#include <string.h>

#define SPECTRUM_PI 3.14159265358979323846f
#define SPECTRUM_NOISE_GATE_MIN 0.00035f
/** @brief Low-frequency cutoff, in Hz. */
#define SPECTRUM_LOW_CUT_HZ 100U
/** @brief Exclude the DC bin and the adjacent offset bin. */
#define SPECTRUM_DC_BINS 2U

static void spectrum_fft_transform(float *real, float *imag)
{
    memset(imag, 0, sizeof(float) * SPECTRUM_FFT_SIZE);
    for (size_t i = 0, j = 0; i < SPECTRUM_FFT_SIZE; ++i) {
        if (i < j) {
            const float temp = real[i];
            real[i] = real[j];
            real[j] = temp;
        }
        size_t bit = SPECTRUM_FFT_SIZE >> 1U;
        while (bit != 0U && (j & bit) != 0U) {
            j ^= bit;
            bit >>= 1U;
        }
        j ^= bit;
    }

    for (size_t length = 2U; length <= SPECTRUM_FFT_SIZE; length <<= 1U) {
        const float angle = -2.0f * SPECTRUM_PI / (float)length;
        const float sine = sinf(angle);
        const float cosine = cosf(angle);
        for (size_t base = 0; base < SPECTRUM_FFT_SIZE; base += length) {
            float wr = 1.0f;
            float wi = 0.0f;
            const size_t half = length / 2U;
            for (size_t i = 0; i < half; ++i) {
                const size_t even = base + i;
                const size_t odd = even + half;
                const float tr = wr * real[odd] - wi * imag[odd];
                const float ti = wr * imag[odd] + wi * real[odd];
                const float er = real[even];
                const float ei = imag[even];
                real[even] = er + tr;
                imag[even] = ei + ti;
                real[odd] = er - tr;
                imag[odd] = ei - ti;
                const float next_wr = wr * cosine - wi * sine;
                wi = wr * sine + wi * cosine;
                wr = next_wr;
            }
        }
    }
}

static void spectrum_fft_compute_radix2(spectrum_fft_t *fft)
{
    const float scale = 4.0f / (float)SPECTRUM_FFT_SIZE;

    for (size_t microphone = 0; microphone < SPECTRUM_MICROPHONE_COUNT;
         ++microphone) {
        for (size_t i = 0; i < SPECTRUM_FFT_SIZE; ++i) {
            fft->real[i] = ((float)fft->microphone_samples[microphone][i] / 32768.0f) *
                           fft->window[i];
        }
        spectrum_fft_transform(fft->real, fft->imag);
        for (size_t i = 0; i < SPECTRUM_FFT_BINS; ++i) {
            const float magnitude = sqrtf(fft->real[i] * fft->real[i] +
                                          fft->imag[i] * fft->imag[i]) * scale;
            if (microphone == 0U) {
                fft->magnitudes[i] = magnitude;
            } else {
                /** Sum independent microphone powers: phase differences between ch0 and ch2 cannot cancel the audible signal. */
                const float combined = sqrtf(fft->magnitudes[i] * fft->magnitudes[i] +
                                             magnitude * magnitude);
                fft->magnitudes[i] = combined > 1.0f ? 1.0f : combined;
            }
        }
    }
    spectrum_fft_apply_noise_gate(fft);
}

bool spectrum_fft_init(spectrum_fft_t *fft, uint32_t sample_rate, uint8_t channels,
                       uint8_t bits_per_sample)
{
    if (fft == NULL || sample_rate == 0U || channels < 3U ||
        channels > SPECTRUM_MAX_CHANNELS || bits_per_sample != 16U) {
        return false;
    }
    memset(fft, 0, sizeof(*fft));
    fft->sample_rate = sample_rate;
    fft->channels = channels;
    fft->bits_per_sample = bits_per_sample;
    for (size_t i = 0; i < SPECTRUM_FFT_SIZE; ++i) {
        fft->window[i] = 0.5f - 0.5f * cosf(2.0f * SPECTRUM_PI * (float)i /
                                             (float)(SPECTRUM_FFT_SIZE - 1U));
    }
    return true;
}

void spectrum_fft_reset(spectrum_fft_t *fft)
{
    if (fft == NULL) {
        return;
    }
    const uint32_t sample_rate = fft->sample_rate;
    const uint8_t channels = fft->channels;
    const uint8_t bits = fft->bits_per_sample;
    (void)spectrum_fft_init(fft, sample_rate, channels, bits);
}

void spectrum_fft_deinit(spectrum_fft_t *fft)
{
    if (fft != NULL) {
        memset(fft, 0, sizeof(*fft));
    }
}

size_t spectrum_fft_feed(spectrum_fft_t *fft, const uint8_t *data, size_t len)
{
    if (fft == NULL || data == NULL || len == 0U || fft->channels == 0U) {
        return 0U;
    }

    const size_t frame_bytes = (size_t)fft->channels * sizeof(int16_t);
    size_t consumed = 0U;
    while (consumed < len) {
        const size_t needed = frame_bytes - fft->pending_bytes;
        const size_t copy = (len - consumed) < needed ? (len - consumed) : needed;
        memcpy(fft->pending_frame + fft->pending_bytes, data + consumed, copy);
        fft->pending_bytes += copy;
        consumed += copy;
        if (fft->pending_bytes != frame_bytes) {
            continue;
        }

        int16_t samples[SPECTRUM_MAX_CHANNELS] = {0};
        memcpy(samples, fft->pending_frame, frame_bytes);
        const size_t sample_index = fft->next_sample_index++;
        fft->microphone_samples[0][sample_index] = samples[0];
        fft->microphone_samples[1][sample_index] = samples[2];
        fft->pending_bytes = 0U;
        if (fft->next_sample_index == SPECTRUM_FFT_SIZE) {
            spectrum_fft_compute_radix2(fft);
            fft->next_sample_index = 0U;
            ++fft->windows_processed;
        }
    }
    return consumed;
}

const float *spectrum_fft_magnitudes(const spectrum_fft_t *fft)
{
    return fft == NULL ? NULL : fft->magnitudes;
}

uint32_t spectrum_fft_windows_processed(const spectrum_fft_t *fft)
{
    return fft == NULL ? 0U : fft->windows_processed;
}

void spectrum_fft_apply_noise_gate(spectrum_fft_t *fft)
{
    if (fft == NULL || fft->sample_rate == 0U) {
        return;
    }
    /** Round the cut up, so a bin whose centre sits below it cannot survive: the first bar's axis edge and the gate then agree on where the band starts. */
    size_t low_cut_bin =
        ((size_t)SPECTRUM_LOW_CUT_HZ * SPECTRUM_FFT_SIZE + fft->sample_rate - 1U) /
        fft->sample_rate;
    if (low_cut_bin < SPECTRUM_DC_BINS) {
        low_cut_bin = SPECTRUM_DC_BINS;
    }
    for (size_t i = 0; i < SPECTRUM_FFT_BINS; ++i) {
        if (i < low_cut_bin) {
            fft->magnitudes[i] = 0.0f;
            continue;
        }
        const float magnitude = fft->magnitudes[i];
        if (magnitude <= SPECTRUM_NOISE_GATE_MIN) {
            fft->magnitudes[i] = 0.0f;
        } else {
            /** Preserve the actual spectrum shape while removing the fixed display floor; unlike an adaptive floor, this cannot learn a tone that happens to be playing when the app opens. */
            fft->magnitudes[i] = magnitude - SPECTRUM_NOISE_GATE_MIN;
        }
    }
}
