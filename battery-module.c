#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/timer.h>
#include <linux/power_supply.h>
#include <linux/i2c.h>
#include <linux/kthread.h>
#include <linux/delay.h>

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Julian Frimmel <julian.frimmel@gmail.com>");
MODULE_DESCRIPTION("Module for fixing the battery on an Acer Switch 11 Laptop");
MODULE_VERSION("0.1");

#define DEBUG

#define BATTERY_REFRESH_RATE_MS 1000

#define BATTERY_I2C_BUS 1
#define BATTERY_I2C_ADDRESS 0x70

static __init int battery_module_init(void);
static __exit void battery_module_exit(void);

static struct i2c_adapter *i2c_dev;
static struct i2c_client *i2c_client;
static unsigned int last_full_capacity;
static struct task_struct *battery_update_thread;

static struct i2c_board_info __initdata board_info[] = {
    {I2C_BOARD_INFO("acer-switch-battery", BATTERY_I2C_ADDRESS)}
};

static unsigned char battery_read_register(const unsigned char reg) {
    struct i2c_msg msg;
    u8 bufo[8] = {0};
    u8 value;
    int ret;
    int tries;
    const int max_tries = 5;

    bufo[0] = 0x02;
    bufo[1] = 0x80;
    bufo[2] = reg;
    msg.addr = i2c_client->addr;
    msg.len = 5;
    msg.flags = 0;
    msg.buf = bufo;
    for (tries = 0; tries < max_tries; tries++) {
        ret = i2c_transfer(i2c_client->adapter, &msg, 1);
        if (ret == 1)
            break;
        printk(KERN_ERR "Battery module: Write to register 0x%02X failed "
                "(Result: 0x%02X, try %d/%d)\n",
                reg, ret, tries + 1, max_tries
        );
    }
    if (ret != 1) return 0x00;

    msg.addr = i2c_client->addr;
    msg.len = 1;
    msg.flags = I2C_M_RD;
    msg.buf = &value;
    for (tries = 0; tries < max_tries; tries++) {
        ret = i2c_transfer(i2c_client->adapter, &msg, 1);
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


static unsigned int *battery_read_state(unsigned int *PBST) {
    unsigned int tmp;

    PBST[0] = battery_read_register(0xC1);

    PBST[2] = (battery_read_register(0xC3) << 8) | battery_read_register(0xC2);

    PBST[3] = (battery_read_register(0xC7) << 8) | battery_read_register(0xC6);

    tmp = (battery_read_register(0xD1) << 8) | battery_read_register(0xD0);
    if (tmp > 0x7FFF) tmp = 0x10000 - tmp;
    PBST[1] = tmp * PBST[3];

    return PBST;
}

// TODO: add sliding average
static void handle_discharging_battery(const unsigned int *pbst) {
    const unsigned int battery_state = pbst[0];
    const unsigned int discharging_rate = pbst[1];
    const unsigned int remaining_capacity = pbst[2] * 10;
    unsigned int secs_to_empty;

    if (discharging_rate / 1000 == 0)
        secs_to_empty = 0;
    else
        secs_to_empty = remaining_capacity * 3600 / (discharging_rate / 1000);

#ifdef DEBUG
    printk(KERN_DEBUG "Battery module: discharging%s. Time to empty: %uh %umin\n",
            battery_state & 0x04 ? " (critical!)" : "",
            secs_to_empty / 60 / 60,
            secs_to_empty / 60 % 60
    );
#endif
}

static void handle_charging_battery(const unsigned int *pbst) {
    const unsigned int charging_rate = pbst[1];
    const unsigned int remaining_capacity = pbst[2];
    unsigned int missing_capacity;
    unsigned int secs_to_full;

    missing_capacity = last_full_capacity - remaining_capacity;
    if ((int) missing_capacity < 0) missing_capacity = 0;
    missing_capacity *= 10;

    if (charging_rate / 1000 == 0)
        secs_to_full = 0;
    else
        secs_to_full = missing_capacity * 3600 / (charging_rate / 1000);

#ifdef DEBUG
    printk(KERN_DEBUG "Battery module: charging. Time to full: %uh %umin\n",
            secs_to_full / 60 / 60,
            secs_to_full / 60 % 60
    );
#endif
}

static void handle_ac_online(const unsigned int *pbst) {
    (void) pbst;
#ifdef DEBUG
    printk(KERN_DEBUG "Battery module: AC online, battery fully charged.\n");
#endif
}

static void handle_battery_state(const unsigned int *pbst) {
    const unsigned int battery_state = pbst[0];

    if (!pbst[0] && !pbst[1] && !pbst[2] && !pbst[3])
        printk(KERN_ERR "Battery module: error reading battery state\n");
    else if (battery_state & 0x01)
        handle_discharging_battery(pbst);
    else if (battery_state & 0x02)
        handle_charging_battery(pbst);
    else
        handle_ac_online(pbst);
}

static int battery_thread(void *params) {
    (void) params;
    while (!kthread_should_stop()) {
        unsigned int PBST[4] = {0, ~0, ~0, ~0};
        handle_battery_state(battery_read_state(PBST));
        msleep_interruptible(BATTERY_REFRESH_RATE_MS);
    }
    return 0;
}

int battery_module_init(void) {
    printk(KERN_INFO "Battery module: loading...\n");

    last_full_capacity = 3750;
    i2c_dev = i2c_get_adapter(BATTERY_I2C_BUS);
    i2c_client = i2c_new_device(i2c_dev, board_info);

    battery_update_thread = kthread_run(battery_thread, NULL, "battery update");

    return 0 - (i2c_client == NULL || battery_update_thread == NULL);
}

void battery_module_exit(void) {
    i2c_unregister_device(i2c_client);
    kthread_stop(battery_update_thread);
    printk(KERN_INFO "Battery module: unloaded\n");
}

module_init(battery_module_init);
module_exit(battery_module_exit);
