/*
 * mediadir.c -- see mediadir.h. The FatFs side.
 *
 * SPDX-License-Identifier: MIT
 */
#include "mediadir.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "ff.h"
#include "storage.h"
#include "storage_io.h"

static const char *TAG = "mediadir";

#define CLS     STORAGE_IO_BACKGROUND

/* Static: FILINFO holds a whole long name, and one folder is open at a
 * time. */
static FF_DIR  s_dir;
static FILINFO s_fi;
static bool    s_open;
static char    s_ffpath[8 + MIDX_PATH_MAX + 2];

bool mdir_open(const char *path)
{
    if (s_open) mdir_close();

    const storage_id_t id = storage_of_path(path);
    const int drv = (id == STORAGE_COUNT) ? -1 : storage_ff_drive(id);
    if (drv < 0) {
        ESP_LOGW(TAG, "no FatFs drive for %s", path);
        return false;
    }
    const char *rel = path + strlen(storage_mount_path(id));
    if (*rel == '\0') rel = "/";
    const int n = snprintf(s_ffpath, sizeof(s_ffpath), "%d:%s", drv, rel);
    if (n < 0 || (size_t)n >= sizeof(s_ffpath)) return false;

    storage_io_acquire(CLS);
    const FRESULT r = f_opendir(&s_dir, s_ffpath);
    storage_io_release();
    if (r != FR_OK) {
        ESP_LOGW(TAG, "f_opendir(%s): %d", s_ffpath, (int)r);
        return false;
    }
    s_open = true;
    return true;
}

int mdir_next(mdir_ent_t *out)
{
    if (!s_open || !out) return -1;
    storage_io_acquire(CLS);
    const FRESULT r = f_readdir(&s_dir, &s_fi);
    storage_io_release();
    if (r != FR_OK) {
        ESP_LOGW(TAG, "f_readdir(%s): %d", s_ffpath, (int)r);
        return -1;
    }
    if (s_fi.fname[0] == '\0') return 0;
    out->name = s_fi.fname;
    out->is_dir = (s_fi.fattrib & AM_DIR) != 0;
    out->stamp.mtime = midx_fat_time(s_fi.fdate, s_fi.ftime);
    out->stamp.size = (uint64_t)s_fi.fsize;
    return 1;
}

void mdir_close(void)
{
    if (!s_open) return;
    storage_io_acquire(CLS);
    f_closedir(&s_dir);
    storage_io_release();
    s_open = false;
}
