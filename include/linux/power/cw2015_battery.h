/*
 * Fuel gauge driver for CellWise 2013 / 2015
 *
 * Copyright (C) 2012, RockChip
 * Copyright (C) 2019, Tobias Schramm
 *
 * Authors: xuhuicong <xhc@rock-chips.com>
 * Authors: Tobias Schramm <tobias@t-sys.eu>
 *
 * Based on rk30_adc_battery.c

 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */

#ifndef CW2015_BATTERY_H
#define CW2015_BATTERY_H

#define CW2015_SIZE_BATINFO    64

#define CW2015_GPIO_HIGH  1
#define CW2015_GPIO_LOW   0

#define CW2015_READ_TRIES 30

#define CW2015_REG_VERSION             0x0
#define CW2015_REG_VCELL               0x2
#define CW2015_REG_SOC                 0x4
#define CW2015_REG_RRT_ALERT           0x6
#define CW2015_REG_CONFIG              0x8
#define CW2015_REG_MODE                0xA
#define CW2015_REG_BATINFO             0x10

#define CW2015_MODE_SLEEP_MASK         (0x3<<6)
#define CW2015_MODE_SLEEP              (0x3<<6)
#define CW2015_MODE_NORMAL             (0x0<<6)
#define CW2015_MODE_QUICK_START        (0x3<<4)
#define CW2015_MODE_RESTART            (0xf<<0)

#define CW2015_CONFIG_UPDATE_FLG       (0x01<<1)
#define CW2015_ATHD                    (0x00<<3)
#define CW2015_MASK_ATHD               (0x1f<<3)
#define CW2015_MASK_SOC                (0x1fff)

#define CW2015_I2C_SPEED			100000
#define CW2015_BATTERY_UP_MAX_CHANGE		(420 * 1000)
#define CW2015_BATTERY_DOWN_MAX_CHANGE		(120 * 1000)
#define CW2015_BATTERY_DOWN_CHANGE		60
#define CW2015_BATTERY_DOWN_MIN_CHANGE_RUN	30
#define CW2015_BATTERY_DOWN_MIN_CHANGE_SLEEP	1800
#define CW2015_BATTERY_JUMP_TO_ZERO		(30 * 1000)
#define CW2015_BATTERY_CAPACITY_ERROR		(40 * 1000)
#define CW2015_BATTERY_CHARGING_ZERO		(1800 * 1000)

#define CW2015_DOUBLE_SERIES_BATTERY	0

#define CW2015_CHARGING_ON		1
#define CW2015_NO_CHARGING		0

#define CW2015_BATTERY_DOWN_MAX_CHANGE_RUN_AC_ONLINE 3600

#define CW2015_NO_STANDARD_AC_BIG_CHARGE_MODE 1
/* #define CW2015_SYSTEM_SHUTDOWN_VOLTAGE  3400000 */
#define CW2015_BAT_LOW_INTERRUPT    1

#define CW2015_USB_CHARGER_MODE        1
#define CW2015_AC_CHARGER_MODE         2
#define   CW2015_QUICKSTART         0

#define CW2015_TIMER_MS_COUNTS			1000
#define CW2015_DEFAULT_MONITOR_SEC		8

/* virtual params */
#define CW2015_VIRTUAL_CURRENT			1000
#define CW2015_VIRTUAL_VOLTAGE			3888
#define CW2015_VIRTUAL_SOC			66
#define CW2015_VIRTUAL_PRESET			1
#define CW2015_VIRTUAL_TEMPERATURE		188
#define CW2015_VIRTUAL_TIME2EMPTY		60
#define CW2015_VIRTUAL_STATUS			POWER_SUPPLY_STATUS_CHARGING

enum bat_mode {
	MODE_BATTERY = 0,
	MODE_VIRTUAL,
};

struct cw_bat_platform_data {
	int divider_res1;
	int divider_res2;
	u32 *cw_bat_config_info;
	int design_capacity;
};

struct cw_battery {
	struct i2c_client *client;
	struct workqueue_struct *battery_workqueue;
	struct delayed_work battery_delay_work;
	struct cw_bat_platform_data plat_data;

	struct power_supply *rk_bat;

#ifdef CONFIG_PM
	struct timespec64 suspend_time_before;
	struct timespec64 after;
	int suspend_resume_mark;
#endif
	int charger_mode;
	int capacity;
	int voltage;
	int status;
	int time_to_empty;
	int alt;
	u32 monitor_sec;
	u32 bat_mode;
	int bat_change;
	bool dual_battery;
	int charge_count;
};

#endif
