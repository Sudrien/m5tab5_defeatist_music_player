/* host stand-in: storage.h names the I2C bus handle in one prototype,
 * and nothing on the host calls it. */
#pragma once
typedef struct i2c_master_bus_t *i2c_master_bus_handle_t;
