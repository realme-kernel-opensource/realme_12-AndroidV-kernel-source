/*
 * Awinic aw37004 Interface
 *
 * Copyright (C) 2022 SHANGHAI AWINIC TECHNOLOGY CO., Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */


#ifndef __AW37004_H
#define __AW37004_H

#include <linux/platform_device.h>
#include <linux/interrupt.h>
#include <linux/regmap.h>
#include <linux/atomic.h>
#include <linux/regulator/consumer.h>

#define AW37004_DEBUG 1

#define VIN1_1P35_VOL_MIN   1350000
#define VIN1_1P35_VOL_MAX   1350000
#define VIN2_3P3_VOL_MIN    3296000
#define VIN2_3P3_VOL_MAX    3296000

int aw37004_camera_power_up(int out_iotype, unsigned int out_val);
int aw37004_camera_power_down(int out_iotype);
int aw37004_camera_power_up_eeprom(void);
int aw37004_camera_power_down_eeprom(void);
int aw37004_camera_power_down_all(void);
int aw37004_camera_power_up_all(void);

struct aw37004_chip {
    struct device *dev;
    struct i2c_client *client;
    int    en_gpio;
    struct regulator *vin1;
    struct regulator *vin2;
};

struct aw37004_map {
    u8 reg;
    u8 value;
};

enum {
    OUT_DVDD1,
    OUT_DVDD2,
    OUT_AVDD1,
    OUT_AVDD2,
    VOL_ENABLE,
    VOL_DISABLE,
    DISCHARGE_ENABLE,
    DISCHARGE_DISABLE,
};

#endif
