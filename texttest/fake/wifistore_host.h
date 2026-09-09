/* Host stand-in for NVS. See wifistoretest.c. */
#pragma once
#include <stdio.h>
#include <string.h>
#include "esp_err.h"
esp_err_t host_blob_write(const void *data, size_t len, int count);
int       host_blob_read(void *data, size_t len);
