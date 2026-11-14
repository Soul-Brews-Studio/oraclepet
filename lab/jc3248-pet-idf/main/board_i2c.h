/*
 * board_i2c.h — lcd28 and sc01: the one I2C bus the FT6336 touch (0x38) sits on.
 * lcd28: SDA16/SCL15, shared with the ES8311 codec (0x18). sc01: SDA6/SCL5, touch only.
 * Created on first use.
 */
#pragma once
#include "driver/i2c_master.h"

#define BOARD_I2C_PORT I2C_NUM_0
#if defined(PET_BOARD_SC01)
#define BOARD_I2C_SDA  6
#define BOARD_I2C_SCL  5
#else
#define BOARD_I2C_SDA  16
#define BOARD_I2C_SCL  15
#endif

i2c_master_bus_handle_t board_i2c_bus(void);
