#pragma once

/** @brief AI chat audio sample rate in Hz. */
#define AICHAT_AUDIO_SAMPLE_RATE 16000U
/** @brief Audio front-end sample rate in Hz. */
#define AICHAT_AUDIO_AFE_SAMPLE_RATE 16000U
/** @brief Codec sample rate in Hz. */
#define AICHAT_AUDIO_CODEC_SAMPLE_RATE 16000U
/** @brief Default playback sample rate in Hz. */
#define AICHAT_AUDIO_DEFAULT_OUTPUT_SAMPLE_RATE 48000U
/** @brief Number of playback channels. */
#define AICHAT_AUDIO_OUTPUT_CHANNELS 2U
/** @brief Stack size used by the GMF audio task. */
#define AICHAT_AUDIO_GMF_TASK_STACK_SIZE 32768U
/** @brief Number of captured audio channels. */
#define AICHAT_AUDIO_CHANNELS 4U
/** @brief Number of bits per captured sample. */
#define AICHAT_AUDIO_BITS 16U
/** @brief Input gain in decibels. */
#define AICHAT_AUDIO_INPUT_GAIN_DB 18.0f
/** @brief Encoded audio frame duration in milliseconds. */
#define AICHAT_AUDIO_FRAME_DURATION_MS 60U
/** @brief Partition containing the speech models. */
#define AICHAT_AUDIO_MODEL_PARTITION "model"
/** @brief GMF codec device I/O name. */
#define AICHAT_GMF_CODEC_IO_NAME "io_codec_dev"
