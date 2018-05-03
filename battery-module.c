/**
 * Battery driver for the Acer Switch 11 laptop.
 *
 * Neither the battery nor the mains plug of that laptop were correctly detected
 * by Linux, since the BIOS provides a (very) broken DSDT. Since I was not able
 * to fix the table, I wrote this kernel module in order to provide the battery
 * information.
 *
 * Author: Julian Frimmel <julian.frimmel@gmail.com>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/power_supply.h>
#include <linux/i2c.h>
#include <linux/kthread.h>
#include <linux/delay.h>

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Julian Frimmel <julian.frimmel@gmail.com>");
MODULE_DESCRIPTION("Module for fixing the battery on an Acer Switch 11 Laptop");
MODULE_VERSION("1.0.0");

/** The name that the battery should get in the sysfs */
#define BATTERY_NAME "BAT0"

/** The name that the AC adapter should get in the sysfs */
#define AC_ADAPTER_NAME "ADP0"


/** Bus number of the battery and the AC adapter (depends on used hardware) */
#define I2C_BUS 1

/** Bus address of the battery and the AC adapter (depends on used hardware) */
#define BATTERY_I2C_ADDRESS 0x70
#define AC_ADAPTER_I2C_ADDRESS 0x30

#define BATTERY_REGISTER_STATUS 0xC1
#define BATTERY_REGISTER_RATE 0xD0
#define BATTERY_REGISTER_ENERGY 0xC2
#define BATTERY_REGISTER_VOLTAGE 0xC6
#define AC_ADAPTER_REGISTER 0x6F


/** The time between two samples of the AC adapter state in milli seconds */
#define AC_ADAPTER_CHECK_RATE_MS 500


/**
 * The I2C bus of the battery.
 *
 * This has to be global, since it is used inside the initialization and exit
 * functions. Since they are callbacks, no parameter can be used. The only other
 * way to share this information is this (module-global) variable.
 */
static struct i2c_adapter *i2c_bus;

/**
 * The I2C device of the battery/AC adapter.
 *
 * This has to be global, since it is used inside the initialization and exit
 * functions. Since they are callbacks, no parameter can be used. The only other
 * way to share this information is this (module-global) variable.
 */
static struct i2c_client *battery_device;
static struct i2c_client *ac_adapter_device;

/** The I2C slave information for the device tree */
static struct i2c_board_info __initdata battery_info[] = {
    {I2C_BOARD_INFO("acer-switch-battery", BATTERY_I2C_ADDRESS)}
};
static struct i2c_board_info __initdata ac_adapter_info[] = {
    {I2C_BOARD_INFO("acer-switch-AC", AC_ADAPTER_I2C_ADDRESS)}
};


/**
 * The power supply "battery".
 *
 * This variable holds resource information about the registered power supply.
 */
static struct power_supply *battery;

/** Available properties of the battery */
static enum power_supply_property battery_properties[] = {
    POWER_SUPPLY_PROP_STATUS,
    POWER_SUPPLY_PROP_CAPACITY,
    POWER_SUPPLY_PROP_TIME_TO_EMPTY_NOW,
    POWER_SUPPLY_PROP_TIME_TO_FULL_NOW,
    POWER_SUPPLY_PROP_VOLTAGE_NOW,
    POWER_SUPPLY_PROP_CURRENT_NOW,
    POWER_SUPPLY_PROP_PRESENT,
    POWER_SUPPLY_PROP_ENERGY_FULL,
    POWER_SUPPLY_PROP_ENERGY_NOW,

    POWER_SUPPLY_PROP_TECHNOLOGY,
    POWER_SUPPLY_PROP_MODEL_NAME,
    POWER_SUPPLY_PROP_MANUFACTURER
};

static int battery_get_property(
    struct power_supply*,
    enum power_supply_property,
    union power_supply_propval*
);

/** The descriptor of the battery device */
static const struct power_supply_desc battery_description = {
        .name = BATTERY_NAME,
        .type = POWER_SUPPLY_TYPE_BATTERY,
        .properties = battery_properties,
        .num_properties = ARRAY_SIZE(battery_properties),
        .get_property = battery_get_property
};

/** The configuration of the battery device */
static const struct power_supply_config battery_config = {};


/**
 * The power supply "AC adapter".
 *
 * This variable holds resource information about the registered power supply.
 */
static struct power_supply *ac_adapter;

/** Available properties of the mains plug */
static enum power_supply_property ac_adapter_properties[] = {
    POWER_SUPPLY_PROP_ONLINE
};

static int ac_adapter_get_property(
    struct power_supply*,
    enum power_supply_property,
    union power_supply_propval*
);

/** The descriptor of the AC adapter device */
static const struct power_supply_desc ac_adapter_description = {
        .name = AC_ADAPTER_NAME,
        .type = POWER_SUPPLY_TYPE_MAINS,
        .properties = ac_adapter_properties,
        .num_properties = ARRAY_SIZE(ac_adapter_properties),
        .get_property = ac_adapter_get_property
};

/** A list of all power supplies, that are supplied from the AC plug */
static char *ac_adapter_to[] = {
    BATTERY_NAME
};

/**
 * The configuration of the AC adapter device.
 *
 * It contains the battery as a "supplicant". If the AC adapter changes, all
 * supplicants are also updated.
 */
static const struct power_supply_config ac_adapter_config = {
    .supplied_to = ac_adapter_to,
    .num_supplicants = ARRAY_SIZE(ac_adapter_to)
};

/** Holds the current state of the AD adapter. */
static unsigned int ac_adapter_connected;

/** The thread that periodically checks the AC adapter connection status */
static struct task_struct *ac_adapter_thread;

/**
 * Read a single byte from a battery register.
 *
 * The "special" register access operation is used, i.e. 0x80 is written to the
 * slave first, then the required sub-register and the the read of the byte.
 */
static u8 read_byte_register(const u8 reg) {
    struct i2c_msg msg;
    u8 bufo[8] = {0};
    u8 value;
    int ret;
    int tries;
    const int max_tries = 5;

    bufo[0] = 0x02;
    bufo[1] = 0x80;
    bufo[2] = reg;
    msg.addr = battery_device->addr;
    msg.len = 5;
    msg.flags = 0;
    msg.buf = bufo;
    for (tries = 0; tries < max_tries; tries++) {
        ret = i2c_transfer(battery_device->adapter, &msg, 1);
        if (ret == 1)
            break;
        printk(KERN_ERR "Battery module: Write to register 0x%02X failed "
                "(Result: 0x%02X, try %d/%d)\n",
                reg, ret, tries + 1, max_tries
        );
    }
    if (ret != 1) return 0x00;

    msg.addr = battery_device->addr;
    msg.len = 1;
    msg.flags = I2C_M_RD;
    msg.buf = &value;
    for (tries = 0; tries < max_tries; tries++) {
        ret = i2c_transfer(battery_device->adapter, &msg, 1);
        if (ret == 1)
            break;
        printk(KERN_ERR "Battery module: Read of register 0x%02X failed "
                "(Result: 0x%02X, try %d/%d)\n",
                reg, ret, tries + 1, max_tries
        );
    }
    if (ret != 1) return 0x00;

    return value;
}

/**
 * Read a single word from a battery register.
 *
 * The LSB is the register address, the MSB is register address + 1.
 */
static u16 read_word_register(const u8 reg) {
    return (read_byte_register(reg + 1) << 8) | read_byte_register(reg);
}


/** Read the current energy in mWh */
static unsigned int battery_energy(void) {
    return read_word_register(BATTERY_REGISTER_ENERGY) * 10;
}

/** Read the last full energy in mWh. TODO: read from battery */
static unsigned int battery_energy_full(void) {
    return 37500;
}

/** Read the current voltage in mV */
static unsigned int battery_voltage(void) {
    return read_word_register(BATTERY_REGISTER_VOLTAGE);
}

/** Read the current in mA */
static unsigned int battery_current(void) {
    unsigned int rate;
    rate = read_word_register(BATTERY_REGISTER_RATE);
    if (rate > 0x7FFF) rate = 0x10000 - rate;

    return rate;
}

/** Read the current (dis-)charging rate in mW */
static unsigned int battery_rate(void) {
    return battery_current() * battery_voltage();
}

/** Read the current battery status (charging, discharging, full or unknown) */
static unsigned int battery_status(void) {
    const u8 status = read_byte_register(BATTERY_REGISTER_STATUS);

    if (status & 0x01)
        return POWER_SUPPLY_STATUS_DISCHARGING;
    else if (status & 0x02)
        return POWER_SUPPLY_STATUS_CHARGING;
    else if ((status & 0x03) == 0x00)
        return POWER_SUPPLY_STATUS_FULL;
    else
        return POWER_SUPPLY_STATUS_UNKNOWN;
}

/** Read the capacity in % (energy compared to energy if full) */
static unsigned int battery_capacity(void) {
    unsigned int last_full = battery_energy_full();
    if (unlikely(!last_full))
        return 0;
    else
        return 100 * battery_energy() / battery_energy_full();
}

/** Read the level of capacity. Calculation based on fixed thresholds */
static unsigned int battery_capaity_level(void) {
    const unsigned int capacity = battery_capacity();
    if (capacity == 100)
        return POWER_SUPPLY_CAPACITY_LEVEL_FULL;
    else if (capacity <= 5)
        return POWER_SUPPLY_CAPACITY_LEVEL_CRITICAL;
    else if (capacity <= 15)
        return POWER_SUPPLY_CAPACITY_LEVEL_LOW;
    else
        return POWER_SUPPLY_CAPACITY_LEVEL_NORMAL;
}

/** Read the estimated time until the battery is empty */
static unsigned int battery_time_to_empty(void) {
    unsigned int rate = battery_rate();
    if (unlikely(!rate))
        return 0;
    return battery_energy() * 60ULL * 60ULL * 1000ULL / rate;
}

/** Read the state of the AC plug */
static unsigned int ac_adapter_online(void) {
    u8 data = i2c_smbus_read_byte_data(ac_adapter_device, AC_ADAPTER_REGISTER);

    if (data & 0x10)
        return 1;
    else
        return 0;
}

/** Read the estimated time until the battery is fully charged */
static unsigned int battery_time_to_full(void) {
    int energy_missing;
    unsigned int rate;
    if (!ac_adapter_online())
        return 0;

    rate = battery_rate();
    if (unlikely(!rate))
        return 0;

    energy_missing = battery_energy_full() - battery_energy();
    if (unlikely(energy_missing) < 0)
        energy_missing = 0;

    return energy_missing * 60ULL * 60ULL * 1000ULL / rate;
}


/**
 * Query a property from the battery.
 *
 * The function is called by the kernel, if any information from the driver is
 * required.
 *
 * The function is currently designed in a way, that the "battery information"
 * (see ACPI documentation) is read at every call to this function. The relevant
 * data is the used inside the function.
 *
 * The function returns 0 (success) on every known property, otherwise the
 * negative value of the "invalid value" error is returned (negative, since the
 * function is a callback, that should return a negative number on failure).
 */
static int battery_get_property(
    struct power_supply *supply,
    enum power_supply_property property,
    union power_supply_propval *val
) {
    switch (property) {
    case POWER_SUPPLY_PROP_CAPACITY:
        val->intval = battery_capacity();
        break;
    case POWER_SUPPLY_PROP_STATUS:
        val->intval = battery_status();
        break;
    case POWER_SUPPLY_PROP_TIME_TO_EMPTY_NOW:
        val->intval = battery_time_to_empty();
        break;
    case POWER_SUPPLY_PROP_TIME_TO_FULL_NOW:
        val->intval = battery_time_to_full();
        break;
    case POWER_SUPPLY_PROP_VOLTAGE_NOW:
        val->intval = battery_voltage();
        break;
    case POWER_SUPPLY_PROP_CURRENT_NOW:
        val->intval = battery_current();
        break;
    case POWER_SUPPLY_PROP_ENERGY_FULL:
        /* we calculate in mW, but the value is assumed to be in uW */
        val->intval = battery_energy_full() * 1000;
        break;
    case POWER_SUPPLY_PROP_ENERGY_NOW:
        /* we calculate in mW, but the value is assumed to be in uW */
        val->intval = battery_energy() * 1000;
        break;
    case POWER_SUPPLY_PROP_CAPACITY_LEVEL:
        val->intval = battery_capaity_level();
        break;

    case POWER_SUPPLY_PROP_PRESENT:
        val->intval = 1;
        break;
    case POWER_SUPPLY_PROP_TECHNOLOGY:
        val->intval = POWER_SUPPLY_TECHNOLOGY_LION;
        break;
    case POWER_SUPPLY_PROP_MANUFACTURER:
        val->strval = "Acer";
        break;
    case POWER_SUPPLY_PROP_MODEL_NAME:
        val->strval = "Acer Switch 11 Battery by jfrimmel";
        break;

    default:
        return -EINVAL;
    }
    return 0;
}

/** Query a property of the AC adapter. */
static int ac_adapter_get_property(
    struct power_supply *supply,
    enum power_supply_property property,
    union power_supply_propval *val
) {
    switch (property) {
    case POWER_SUPPLY_PROP_ONLINE:
        val->intval = ac_adapter_connected;
        break;

    default:
        return -EINVAL;
    }
    return 0;
}

static int ac_adapter_updater(void *params) {
    unsigned int last_state = -1;
    while (!kthread_should_stop()) {
        ac_adapter_connected = ac_adapter_online();
        if (unlikely(ac_adapter_connected != last_state))
            power_supply_changed(ac_adapter);
        last_state = ac_adapter_connected;

        msleep_interruptible(AC_ADAPTER_CHECK_RATE_MS);
    }
    return 0;
}


/**
 * Initialize the kernel module.
 *
 * This function is called, if the module is loaded/inserted into the kernel.
 * It acquires or registers resources, such as an I2C slave (the battery) or the
 * power supply.
 *
 * The function returns 0 on success, -1 otherwise. Resources acquired during
 * the initialization phase are released in the case of an error.
 */
static __init int battery_module_init(void) {
    i2c_bus = i2c_get_adapter(I2C_BUS);
    if (!i2c_bus) goto i2c_bus_adapter_not_available;

    battery_device = i2c_new_device(i2c_bus, battery_info);
    if (!battery_device) goto battery_device_creation_failed;

    ac_adapter_device = i2c_new_device(i2c_bus, ac_adapter_info);
    if (!ac_adapter_device) goto ac_adapter_device_creation_failed;

    battery = power_supply_register(
        NULL,
        &battery_description,
        &battery_config
    );
    if (!battery) goto battery_registration_failure;

    ac_adapter = power_supply_register(
        NULL,
        &ac_adapter_description,
        &ac_adapter_config
    );
    if (!ac_adapter) goto ac_adapter_registration_failure;

    ac_adapter_thread = kthread_run(ac_adapter_updater, NULL, "AC update");
    if (!ac_adapter_thread) goto thread_creation_failed;

    return 0;

thread_creation_failed:
    power_supply_unregister(ac_adapter);
ac_adapter_registration_failure:
    power_supply_unregister(battery);
battery_registration_failure:
    i2c_unregister_device(ac_adapter_device);
ac_adapter_device_creation_failed:
    i2c_unregister_device(battery_device);
battery_device_creation_failed:
    i2c_put_adapter(i2c_bus);
i2c_bus_adapter_not_available:
    return -1;
}

/**
 * Exit the kernel module.
 *
 * This function is called, if the module is unloaded. It releases all acquired
 * resources.
 */
static __exit void battery_module_exit(void) {
    kthread_stop(ac_adapter_thread);
    power_supply_unregister(ac_adapter);
    power_supply_unregister(battery);
    i2c_unregister_device(ac_adapter_device);
    i2c_unregister_device(battery_device);
    i2c_put_adapter(i2c_bus);
}


/**
 * Register initialization function.
 */
module_init(battery_module_init);

/**
 * Register exit function.
 */
module_exit(battery_module_exit);
