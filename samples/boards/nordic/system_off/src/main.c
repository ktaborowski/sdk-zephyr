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
#include <zephyr/devicetree.h>
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

void debug_print(void)
{
	// Show the contents of the RESETREAS register (reset reason flags, cumulative unless cleared by firmware)
	printk("RESETREAS: 0x%08x\n", NRF_POWER->RESETREAS);

	// Show GPIO P0 and P1 LATCH registers (flags for detected pin events not yet acknowledged)
	printk("GPIO P0 LATCH: 0x%08x\n", NRF_P0->LATCH);
	printk("GPIO P1 LATCH: 0x%08x\n", NRF_P1->LATCH);

	// Show DETECTMODE registers (edge vs level sense configuration for GPIO group P0 and P1)
	printk("P0 DETECTMODE: 0x%08x\n", NRF_P0->DETECTMODE);
	printk("P1 DETECTMODE: 0x%08x\n", NRF_P1->DETECTMODE);

	// Show PIN_CNF register for button 1 (pin configuration, including SENSE field for wakeup)
	uint8_t btn1_pin = 11; // nrf52840dk button 1 is P0.11
	printk("Button1 PIN_CNF: 0x%08x\n", NRF_P0->PIN_CNF[btn1_pin]);
	// SENSE field is bits [17:16]: 0=Disabled, 2=High, 3=Low

	// Show current state of inputs on port P0 (used to see status of detected pins)
	printk("P0 IN: 0x%08x\n", NRF_P0->IN);

	// Clear LATCH registers on both ports before entering System OFF (removes stale detect events)
	NRF_P0->LATCH = 0xFFFFFFFF;
	NRF_P1->LATCH = 0xFFFFFFFF;
	printk("LATCH cleared. P0: 0x%08x P1: 0x%08x\n", NRF_P0->LATCH, NRF_P1->LATCH);

	// Show radio power control register (0x40001FFC) directly to check if radio is powered
	printk("RADIO.POWER: 0x%08x\n", *(volatile uint32_t *)0x40001FFC);

	// Show radio state register (current state of radio peripheral)
	printk("RADIO.STATE: 0x%08x\n", NRF_RADIO->STATE);

	// Show enabled PPI channels (Programmable Peripheral Interconnect, used by BLE and peripherals)
	printk("PPI.CHEN: 0x%08x\n", NRF_PPI->CHEN);
	printk("PPI.CHENSET: 0x%08x\n", NRF_PPI->CHENSET);
	printk("PPI.CH[16].EEP: 0x%08x\n", NRF_PPI->CH[16].EEP);
	printk("PPI.CH[16].TEP: 0x%08x\n", NRF_PPI->CH[16].TEP);
	printk("PPI.FORK[16].TEP: 0x%08x\n", NRF_PPI->FORK[16].TEP);

	// Show EasyDMA peripheral modes (to check for activity on system timers)
	printk("TIMER0.MODE: 0x%08x\n", NRF_TIMER0->MODE);
	printk("TIMER1.MODE: 0x%08x\n", NRF_TIMER1->MODE);
	printk("TIMER2.MODE: 0x%08x\n", NRF_TIMER2->MODE);

	// Show high frequency (HFCLK) and low frequency (LFCLK) clock status registers
	printk("CLOCK.HFCLKSTAT: 0x%08x\n", NRF_CLOCK->HFCLKSTAT);
	printk("CLOCK.LFCLKSTAT: 0x%08x\n", NRF_CLOCK->LFCLKSTAT);

	// Show enabled RTC0 events (real-time counter)
	printk("RTC0.EVTEN:  0x%08x\n", NRF_RTC0->EVTEN);
}

#if defined(CONFIG_GRTC_WAKEUP_ENABLE)
#include <zephyr/drivers/timer/nrf_grtc_timer.h>
#define DEEP_SLEEP_TIME_S 2
#endif
#if defined(CONFIG_GPIO_WAKEUP_ENABLE)
static const struct gpio_dt_spec sw0 = GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);
#endif
#if DT_NODE_EXISTS(DT_ALIAS(sw2))
static const struct gpio_dt_spec sw2 = GPIO_DT_SPEC_GET(DT_ALIAS(sw2), gpios);
#endif
#if DT_NODE_EXISTS(DT_ALIAS(sw3))
static const struct gpio_dt_spec sw3 = GPIO_DT_SPEC_GET(DT_ALIAS(sw3), gpios);
#endif

/* Console device for power-off (used from work handler) */
static const struct device *cons_dev;

static int bluetooth_activity(void);

#if DT_NODE_EXISTS(DT_ALIAS(sw3))
static void power_off_work_handler(struct k_work *work)
{
	int rc;

	ARG_UNUSED(work);
#if defined(CONFIG_GRTC_WAKEUP_ENABLE)
	int err = z_nrf_grtc_wakeup_prepare(DEEP_SLEEP_TIME_S * USEC_PER_SEC);

	if (err < 0) {
		printk("Unable to prepare GRTC as wake up source (err = %d).\n", err);
		return;
	}
#endif
#if defined(CONFIG_LPCOMP_WAKEUP_ENABLE)
	comparator_set_trigger(comp_dev, COMPARATOR_TRIGGER_BOTH_EDGES);
	comparator_trigger_is_pending(comp_dev);
#endif

	debug_print();

	rc = pm_device_action_run(cons_dev, PM_DEVICE_ACTION_SUSPEND);
	if (rc < 0) {
		printf("Could not suspend console (%d)\n", rc);
		return;
	}
	if (IS_ENABLED(CONFIG_APP_USE_RETAINED_MEM)) {
		retained.off_count += 1;
		retained_update();
	}
	hwinfo_clear_reset_cause();
#if defined(CONFIG_SYS_CLOCK_DISABLE)
	sys_clock_disable();
#endif
	sys_poweroff();
}
K_WORK_DEFINE(power_off_work, power_off_work_handler);
static struct gpio_callback sw3_cb_data;
static void sw3_callback(const struct device *port, struct gpio_callback *cb, uint32_t pins)
{
	ARG_UNUSED(port);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);
	printk("Power off\n");
	k_work_submit(&power_off_work);
}
#endif

#if DT_NODE_EXISTS(DT_ALIAS(sw2))
static void bluetooth_activity_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	printk("Bluetooth activity\n");
	(void)bluetooth_activity();
}
K_WORK_DEFINE(bluetooth_activity_work, bluetooth_activity_work_handler);
static struct gpio_callback sw2_cb_data;
static void sw2_callback(const struct device *port, struct gpio_callback *cb, uint32_t pins)
{
	ARG_UNUSED(port);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);
	k_work_submit(&bluetooth_activity_work);
}
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

	return 0;
}

int main(void)
{
	int rc;
	uint32_t reset_cause;
	const struct device *const cons = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));

	cons_dev = cons;

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

#if defined(CONFIG_GPIO_WAKEUP_ENABLE)
	/* Button 0 (sw0): configure as wake-up source for when we enter system off */
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
	printf("Press sw0 to wake from system off\n");
#endif

#if DT_NODE_EXISTS(DT_ALIAS(sw2))
	rc = gpio_pin_configure_dt(&sw2, GPIO_INPUT);
	if (rc >= 0) {
		gpio_init_callback(&sw2_cb_data, sw2_callback, BIT(sw2.pin));
		rc = gpio_add_callback(sw2.port, &sw2_cb_data);
	}
	if (rc >= 0) {
		rc = gpio_pin_interrupt_configure_dt(&sw2, GPIO_INT_EDGE_FALLING);
	}
	if (rc < 0) {
		printf("Could not configure sw2 (button 2) (%d)\n", rc);
	} else {
		printf("Button 2 (sw2): Bluetooth activity\n");
	}
#endif
#if DT_NODE_EXISTS(DT_ALIAS(sw3))
	rc = gpio_pin_configure_dt(&sw3, GPIO_INPUT);
	if (rc >= 0) {
		gpio_init_callback(&sw3_cb_data, sw3_callback, BIT(sw3.pin));
		rc = gpio_add_callback(sw3.port, &sw3_cb_data);
	}
	if (rc >= 0) {
		rc = gpio_pin_interrupt_configure_dt(&sw3, GPIO_INT_EDGE_FALLING);
	}
	if (rc < 0) {
		printf("Could not configure sw3 (button 3) (%d)\n", rc);
	} else {
		printf("Button 3 (sw3): Power off\n");
	}
#endif

	for (;;) {
		k_sleep(K_FOREVER);
	}
	return 0;
}
