/** SPDX-License-Identifier: GPL-2.0-only */
#include "ft_sd_probe.h"

#include <string.h>

void ft_sd_probe_run(const ft_sd_probe_ops_t *ops, ft_sd_probe_report_t *report)
{
    if (ops == NULL || report == NULL) {
        return;
    }

    memset(report, 0, sizeof(*report));
    report->status = FT_SD_STATUS_UNAVAILABLE;

    if (ops->query_mounted == NULL || ops->mount == NULL) {
        return;
    }

    bool mounted = false;
    int ret = ops->query_mounted(ops->context, &mounted);
    if (ret != 0) {
        report->last_error = ret;
        return;
    }
    report->mounted_before = mounted;

    if (!mounted) {
        ret = ops->mount(ops->context);
        if (ret != 0) {
            report->last_error = ret;
            report->status = FT_SD_STATUS_MOUNT_FAILED;
            return;
        }
        report->mounted_by_probe = true;
    }

    report->last_error = 0;
    report->status = FT_SD_STATUS_OK;
}

void ft_sd_probe_release(const ft_sd_probe_ops_t *ops, ft_sd_probe_report_t *report)
{
    if (ops == NULL || report == NULL || !report->mounted_by_probe) {
        return;
    }
    if (ops->unmount != NULL) {
        (void)ops->unmount(ops->context);
    }
    /** Clear mount ownership after one unmount attempt to avoid duplicate device-reference release. */
    report->mounted_by_probe = false;
}

#ifndef FT_SD_PROBE_HOST_TEST

#include "esp_board_device.h"
#include "esp_board_manager.h"

/** @brief Device name declared by components/gen_bmgr_codes and board_devices.yaml. */
#define FT_SD_DEVICE_NAME "fs_sdcard"

static int sd_probe_query_mounted(void *context, bool *mounted)
{
    (void)context;
    if (mounted == NULL) {
        return (int)ESP_ERR_INVALID_ARG;
    }

    /** Check the board-manager handle to determine whether the SD card is mounted. */
    void *handle = NULL;
    *mounted = esp_board_manager_get_device_handle(FT_SD_DEVICE_NAME, &handle) == ESP_OK &&
               handle != NULL;
    return 0;
}

static int sd_probe_mount(void *context)
{
    (void)context;
    return (int)esp_board_device_init(FT_SD_DEVICE_NAME);
}

static int sd_probe_unmount(void *context)
{
    (void)context;
    return (int)esp_board_device_deinit(FT_SD_DEVICE_NAME);
}

const ft_sd_probe_ops_t *ft_sd_probe_default_ops(void)
{
    static const ft_sd_probe_ops_t s_ops = {
        .context = NULL,
        .query_mounted = sd_probe_query_mounted,
        .mount = sd_probe_mount,
        .unmount = sd_probe_unmount,
    };
    return &s_ops;
}

#endif
