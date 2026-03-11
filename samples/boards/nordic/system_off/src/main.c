/*
 * Copyright (c) 2019 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "retained.h"
#include "zephyr/sys/printk.h"

#include <inttypes.h>
#include <stdio.h>

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/drivers/comparator.h>
#include <zephyr/kernel.h>
#include <zephyr/pm/device.h>
#include <zephyr/sys/poweroff.h>
#include <zephyr/sys/util.h>
#include <zephyr/drivers/timer/system_timer.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/hci.h>

#define NON_WAKEUP_RESET_REASON (RESET_PIN | RESET_SOFTWARE | RESET_POR | RESET_DEBUG)

#if defined(CONFIG_GRTC_WAKEUP_ENABLE)
#include <zephyr/drivers/timer/nrf_grtc_timer.h>
#define DEEP_SLEEP_TIME_S 2
#endif
#if defined(CONFIG_GPIO_WAKEUP_ENABLE)
static const struct gpio_dt_spec sw0 = GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);
#endif
#if defined(CONFIG_LPCOMP_WAKEUP_ENABLE)
static const struct device *comp_dev = DEVICE_DT_GET(DT_NODELABEL(comp));
#endif

static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME, sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};

static void connected(struct bt_conn *conn, uint8_t conn_err)
{
	char addr[BT_ADDR_LE_STR_LEN];

	if (conn_err) {
		printk("Connection failed, err 0x%02x %s\n", conn_err, bt_hci_err_to_str(conn_err));
		return;
	}
	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	printk("Connected %s\n", addr);
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	printk("Disconnected, reason 0x%02x %s\n", reason, bt_hci_err_to_str(reason));
}

BT_CONN_CB_DEFINE(connection_cb) = {
	.connected = connected,
	.disconnected = disconnected,
};

int print_reset_cause(uint32_t reset_cause)
{
	int32_t ret;
	uint32_t supported;

	ret = hwinfo_get_supported_reset_cause((uint32_t *)&supported);

	if (ret || !(reset_cause & supported)) {
		return -ENOTSUP;
	}

	if (reset_cause & RESET_DEBUG) {
		printf("Reset by debugger.\n");
	} else if (reset_cause & RESET_CLOCK) {
		printf("Wakeup from System OFF by GRTC.\n");
	} else if (reset_cause & RESET_LOW_POWER_WAKE) {
		printf("Wakeup from System OFF by GPIO.\n");
	} else {
		printf("Other wake up cause 0x%08X.\n", reset_cause);
	}

	return 0;
}

static int bluetooth_activity(void)
{
	int rc = 0;

	rc = bt_enable(NULL);
	if (rc) {
		printf("Bluetooth init failed (%d)\n", rc);
		return 0;
	}
	rc = bt_le_adv_start(BT_LE_ADV_CONN_FAST_1, ad, ARRAY_SIZE(ad), NULL, 0);
	if (rc) {
		printf("Advertising failed to start (%d)\n", rc);
		return 0;
	}
	printf("Bluetooth advertising started\n");

	k_sleep(K_SECONDS(2));

	rc = bt_le_adv_stop();
	if (rc < 0) {
		printf("Could not stop advertising (%d)\n", rc);
		return 0;
	}
	printk("Bluetooth advertising stopped\n");
	rc = bt_disable();
	if (rc < 0) {
		printf("Could not disable Bluetooth (%d)\n", rc);
		return 0;
	}
	printk("Bluetooth disabled\n");

	k_sleep(K_MSEC(500));
}

int main(void)
{
	int rc;
	uint32_t reset_cause;
	const struct device *const cons = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));

	if (!device_is_ready(cons)) {
		printf("%s: device not ready.\n", cons->name);
		return 0;
	}

	printf("\n%s system off demo\n", CONFIG_BOARD);
	hwinfo_get_reset_cause(&reset_cause);
	rc = print_reset_cause(reset_cause);

	if (rc < 0) {
		printf("Reset cause not supported.\n");
		return 0;
	}

	if (IS_ENABLED(CONFIG_APP_USE_RETAINED_MEM)) {
		bool retained_ok = retained_validate();

		if (reset_cause & NON_WAKEUP_RESET_REASON) {
			retained.boots = 0;
			retained.off_count = 0;
			retained.uptime_sum = 0;
			retained.uptime_latest = 0;
			retained_ok = true;
		}
		/* Increment for this boot attempt and update. */
		retained.boots += 1;
		retained_update();

		printf("Retained data: %s\n", retained_ok ? "valid" : "INVALID");
		printf("Boot count: %u\n", retained.boots);
		printf("Off count: %u\n", retained.off_count);
		printf("Active Ticks: %" PRIu64 "\n", retained.uptime_sum);
	} else {
		printf("Retained data not supported\n");
	}

	rc = bluetooth_activity();
	if (rc < 0) {
		printf("Bluetooth failed (%d)\n", rc);
		return 0;
	}

#if defined(CONFIG_SYS_CLOCK_DISABLE)
	printf("System clock will be disabled\n");
#endif
#if defined(CONFIG_GRTC_WAKEUP_ENABLE)
	int err = z_nrf_grtc_wakeup_prepare(DEEP_SLEEP_TIME_S * USEC_PER_SEC);

	if (err < 0) {
		printk("Unable to prepare GRTC as a wake up source (err = %d).\n", err);
		return 0;
	} else {
		printk("Entering system off; wait %u seconds to restart\n", DEEP_SLEEP_TIME_S);
	}
#endif
#if defined(CONFIG_GPIO_WAKEUP_ENABLE)
	/* configure sw0 as input, interrupt as level active to allow wake-up */
	rc = gpio_pin_configure_dt(&sw0, GPIO_INPUT);
	if (rc < 0) {
		printf("Could not configure sw0 GPIO (%d)\n", rc);
		return 0;
	}

	rc = gpio_pin_interrupt_configure_dt(&sw0, GPIO_INT_LEVEL_ACTIVE);
	if (rc < 0) {
		printf("Could not configure sw0 GPIO interrupt (%d)\n", rc);
		return 0;
	}

	printf("Entering system off; press sw0 to restart\n");
#endif
#if defined(CONFIG_LPCOMP_WAKEUP_ENABLE)
	comparator_set_trigger(comp_dev, COMPARATOR_TRIGGER_BOTH_EDGES);
	comparator_trigger_is_pending(comp_dev);
	printf("Entering system off; change signal level at comparator input to restart\n");
#endif

	rc = pm_device_action_run(cons, PM_DEVICE_ACTION_SUSPEND);
	if (rc < 0) {
		printf("Could not suspend console (%d)\n", rc);
		return 0;
	}

	if (IS_ENABLED(CONFIG_APP_USE_RETAINED_MEM)) {
		/* Update the retained state */
		retained.off_count += 1;
		retained_update();
	}

	hwinfo_clear_reset_cause();
#if defined(CONFIG_SYS_CLOCK_DISABLE)
	sys_clock_disable();
#endif
	sys_poweroff();

	return 0;
}
