#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/power_supply.h>
#include <linux/i2c.h>

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Julian Frimmel <julian.frimmel@gmail.com>");
MODULE_DESCRIPTION("Module for fixing the battery on an Acer Switch 11 Laptop");
MODULE_VERSION("0.1");

/**
 * Enable debug messages.
 *
 * Uncomment this macro, if you want to enable KERN_DEBUG messages in the kernel
 * log. Those include the battery state and time to full/empty.
 */
// #define DEBUG

/** Bus number of the battery */
#define BATTERY_I2C_BUS 1

/** Bus address of the battery */
#define BATTERY_I2C_ADDRESS 0x70


#define BATTERY_REGISTER_STATUS 0xC1
#define BATTERY_REGISTER_RATE 0xD0
#define BATTERY_REGISTER_ENERGY 0xC2
#define BATTERY_REGISTER_VOLTAGE 0xC6


/**
 * The I2C bus of the battery.
 *
 * This has to be global, since it is used inside the initialization and exit
 * functions. Since they are callbacks, no parameter can be used. The only other
 * way to share this information is this (module-global) variable.
 */
static struct i2c_adapter *battery_bus;

/**
 * The I2C device of the battery.
 *
 * This has to be global, since it is used inside the initialization and exit
 * functions. Since they are callbacks, no parameter can be used. The only other
 * way to share this information is this (module-global) variable.
 */
static struct i2c_client *battery_device;

/** The capacity of the full battery */
static unsigned int last_full_capacity;

static struct power_supply *supply;
static enum power_supply_property supply_properties[] = {
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

static const struct power_supply_desc supply_description[] = {
    {
        .name = "BAT0",
        .type = POWER_SUPPLY_TYPE_BATTERY,
        .properties = supply_properties,
        .num_properties = ARRAY_SIZE(supply_properties),
        .get_property = battery_get_property
    }
};

static const struct power_supply_config supply_config[] = {
    {}
};

static struct i2c_board_info __initdata board_info[] = {
    {I2C_BOARD_INFO("acer-switch-battery", BATTERY_I2C_ADDRESS)}
};


/**
 * Read a single byte from a battery register.
 *
 * The "special" register access operation is used, i.e. 0x80 is written to the
 * slave frist, then the required sub-register and the the read of the byte.
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

/** Read the current (dis-)charging rate in mW */
static unsigned int battery_rate(void) {
    unsigned int rate;
    rate = read_word_register(BATTERY_REGISTER_RATE);
    if (rate > 0x7FFF) rate = 0x10000 - rate;

    return rate * battery_voltage();
}

/** Read the current battery status (chargin, discharging, full or unknown) */
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
    if (!last_full)
        return 0;
    else
        return 100 * battery_energy() / battery_energy_full();
}

/** Read the level of capacity. Calulation based on fixed tresholds */
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
    if (!rate)
        return 0;
    return battery_energy() * 60ULL * 60ULL * 1000ULL / rate;
}

/** Read the estimated time until the battery is fully charged */
static unsigned int battery_time_to_full(void) {
    int energy_missing;
    unsigned int rate = battery_rate();
    if (!rate)
        return 0;

    energy_missing = battery_energy_full() - battery_energy();
    if (energy_missing < 0)
        energy_missing = 0;

    return energy_missing * 60ULL * 60ULL * 1000ULL / rate;
}

/** Read the current in mA */
static unsigned int battery_current(void) {
    unsigned int voltage = battery_voltage();
    if (!voltage) return 0;
    return battery_rate() / voltage;
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
        printk(KERN_ERR "Battery module: unknwon report querried!\n");
        return -EINVAL;
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
 * TODO: since the last full capacity is not read from the battery, a hard-coded
 * value is used. This value is set also in that function.
 *
 * If there were no errors, the function writes a information message to the
 * kernel log and returns 0 (success).
 *
 * If an error has occurred, the function releases all up to that point acquired
 * resources and returns -1 (error).
 */
static __init int battery_module_init(void) {
    last_full_capacity = 3750;

    battery_bus = i2c_get_adapter(BATTERY_I2C_BUS);
    battery_device = i2c_new_device(battery_bus, board_info);
    if (!battery_device) return -ENODEV;

    supply = power_supply_register(NULL, &supply_description[0], &supply_config[0]);
    if (!supply) {
        i2c_unregister_device(battery_device);
        i2c_put_adapter(battery_bus);
        return -EINVAL;
    }

    return 0;
}

/**
 * Exit the kernel module.
 *
 * This function is called, if the module is unloaded. It releases all acquired
 * resources and writes an information message to the kernel log.
 */
static __exit void battery_module_exit(void) {
    power_supply_unregister(supply);
    i2c_unregister_device(battery_device);
    i2c_put_adapter(battery_bus);
}


/**
 * Register initialization function.
 */
module_init(battery_module_init);

/**
 * Register exit function.
 */
module_exit(battery_module_exit);
