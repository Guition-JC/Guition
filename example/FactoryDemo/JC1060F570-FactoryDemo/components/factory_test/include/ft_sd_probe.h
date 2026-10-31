/** SPDX-License-Identifier: GPL-2.0-only */
/** SD card mount test. Existing mounts are left intact; mounts created by the probe are released by ft_sd_probe_release(). */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief SD mount and communication probe result. */
typedef enum {
    FT_SD_STATUS_OK = 0,
    FT_SD_STATUS_UNAVAILABLE,   /**< Board has no fs_sdcard device at all. */
    FT_SD_STATUS_MOUNT_FAILED,  /**< No card inserted, or the mount was rejected. */
    FT_SD_STATUS_COMM_FAILED,   /**< Mounted, but the card did not answer. */
} ft_sd_probe_status_t;

/** @brief SD probe outcome and mount ownership. */
typedef struct {
    ft_sd_probe_status_t status;
    bool mounted_before;    /**< Already mounted when the test started. */
    bool mounted_by_probe;  /**< This probe performed the mount (release it). */
    uint64_t capacity_bytes;
    char card_name[16];
    int last_error;         /**< Raw driver/board error of the failing step. */
} ft_sd_probe_report_t;

/** @brief Board operations and context used throughout an SD probe. */
typedef struct {
    void *context;
    /** true when esp_board_manager already holds a live fs_sdcard handle. */
    int (*query_mounted)(void *context, bool *mounted);
    /** esp_board_device_init("fs_sdcard"). */
    int (*mount)(void *context);
    /** esp_board_device_deinit("fs_sdcard"). */
    int (*unmount)(void *context);
} ft_sd_probe_ops_t;

/**
 * @brief Run the chain and fill `report`. The ops table must stay valid until ft_sd_probe_release() has been called.
 * @param ops Operations to use.
 * @param report Report data.
 */
void ft_sd_probe_run(const ft_sd_probe_ops_t *ops, ft_sd_probe_report_t *report);

/**
 * @brief Release only a mount created by this probe; repeated calls leave existing mounts intact.
 * @param ops Operations to use.
 * @param report Report data.
 */
void ft_sd_probe_release(const ft_sd_probe_ops_t *ops, ft_sd_probe_report_t *report);

/**
 * @brief Return the board-manager-backed SD operations table.
 * @return Board-manager-backed SD operations table.
 */
const ft_sd_probe_ops_t *ft_sd_probe_default_ops(void);

#ifdef __cplusplus
}
#endif
