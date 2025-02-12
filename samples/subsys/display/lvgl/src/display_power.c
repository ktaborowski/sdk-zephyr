
#include "display_power.h"
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(display_power, CONFIG_LOG_DEFAULT_LEVEL);

static const struct gpio_dt_spec ada_pwr_sw =
    GPIO_DT_SPEC_GET_OR(DT_NODELABEL(adapter_power_switch), gpios, {0});

static int adapter_power_init(void)
{
    int err;

    if (!device_is_ready(ada_pwr_sw.port)) {
        LOG_ERR("GPIO device %s is not ready", ada_pwr_sw.port->name);
        return -ENODEV;
    }

    err = gpio_pin_configure_dt(&ada_pwr_sw, GPIO_OUTPUT_HIGH);
    if (err) {
        LOG_ERR("Failed to set adapter pin: %d", err);
        return err;
    }

    k_busy_wait(300U*1000U);

    return 0;
}

SYS_INIT(adapter_power_init, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEVICE);
