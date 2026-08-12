/*
 * BLE <-> UART serial bridge based on the Nordic UART Service (NUS) UUIDs.
 *
 * Board: STM32WB5MM-DK
 *   - Bridge UART: LPUART1, PA2 = TX / PA3 = RX (STMod+ connector CN5),
 *     115200 8N1 (see boards/stm32wb5mm_dk.overlay)
 *   - Console/logging: USART1 via the ST-LINK virtual COM port
 *
 * Data received on the bridge UART is sent as NUS TX notifications; data
 * written to the NUS RX characteristic is forwarded to the bridge UART.
 * Both directions are decoupled with ring buffers so that neither the UART
 * ISR nor the Bluetooth stack ever blocks on the other side.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/sys/util.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/services/nus.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(ble_serial_bridge, LOG_LEVEL_INF);

#define UART_TO_BLE_BUF_SIZE 2048
#define BLE_TO_UART_BUF_SIZE 2048
#define UART_FIFO_CHUNK      64
#define NUS_SEND_RETRY_MS    5

/* ATT default MTU per Bluetooth Core Spec; the negotiated value arrives
 * through the att_mtu_updated callback.
 */
#define ATT_DEFAULT_MTU      23

static const struct device *const bridge_uart =
	DEVICE_DT_GET(DT_ALIAS(bridge_uart));

RING_BUF_DECLARE(uart_to_ble_rb, UART_TO_BLE_BUF_SIZE);
RING_BUF_DECLARE(ble_to_uart_rb, BLE_TO_UART_BUF_SIZE);

static atomic_t nus_subscribed;
static atomic_t uart_rx_dropped;
static atomic_t att_mtu = ATOMIC_INIT(ATT_DEFAULT_MTU);

static void ble_tx_work_handler(struct k_work *work);
static void adv_work_handler(struct k_work *work);

static K_WORK_DELAYABLE_DEFINE(ble_tx_work, ble_tx_work_handler);
static K_WORK_DEFINE(adv_work, adv_work_handler);

static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA_BYTES(BT_DATA_UUID128_ALL, BT_UUID_NUS_SRV_VAL),
};

static const struct bt_data sd[] = {
	BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME,
		sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};

/* UART ISR: drain RX FIFO into the UART->BLE ring buffer, feed the TX FIFO
 * from the BLE->UART ring buffer.
 */
static void bridge_uart_isr(const struct device *dev, void *user_data)
{
	ARG_UNUSED(user_data);

	while (uart_irq_update(dev) && uart_irq_is_pending(dev)) {
		if (uart_irq_rx_ready(dev)) {
			uint8_t buf[UART_FIFO_CHUNK];
			int n = uart_fifo_read(dev, buf, sizeof(buf));

			if (n > 0) {
				uint32_t written =
					ring_buf_put(&uart_to_ble_rb, buf, n);

				if (written < (uint32_t)n) {
					atomic_add(&uart_rx_dropped,
						   n - written);
				}
				k_work_schedule(&ble_tx_work, K_NO_WAIT);
			}
		}

		if (uart_irq_tx_ready(dev)) {
			uint8_t *data;
			uint32_t len = ring_buf_get_claim(&ble_to_uart_rb,
							  &data,
							  UART_FIFO_CHUNK);

			if (len == 0) {
				uart_irq_tx_disable(dev);
			} else {
				int sent = uart_fifo_fill(dev, data, len);

				ring_buf_get_finish(&ble_to_uart_rb,
						    MAX(sent, 0));
			}
		}
	}
}

/* System workqueue: push buffered UART data out as NUS notifications,
 * chunked to the negotiated ATT MTU. Retries with a short delay when the
 * host runs out of TX buffers.
 */
static void ble_tx_work_handler(struct k_work *work)
{
	struct k_work_delayable *dwork = k_work_delayable_from_work(work);
	uint8_t *data;
	uint32_t len;

	atomic_val_t dropped = atomic_clear(&uart_rx_dropped);

	if (dropped > 0) {
		LOG_WRN("UART RX overrun, %ld bytes dropped", (long)dropped);
	}

	if (!atomic_get(&nus_subscribed)) {
		/* No subscriber: discard buffered data so stale bytes do not
		 * burst out once a client connects.
		 */
		while ((len = ring_buf_get_claim(&uart_to_ble_rb, &data,
						 UART_TO_BLE_BUF_SIZE)) > 0) {
			ring_buf_get_finish(&uart_to_ble_rb, len);
		}
		return;
	}

	uint32_t max_chunk = (uint32_t)atomic_get(&att_mtu) - 3U;

	while ((len = ring_buf_get_claim(&uart_to_ble_rb, &data,
					 max_chunk)) > 0) {
		int err = bt_nus_send(NULL, data, (uint16_t)len);

		if (err == -EAGAIN || err == -ENOMEM) {
			/* Out of buffers: keep the data, retry shortly. */
			ring_buf_get_finish(&uart_to_ble_rb, 0);
			k_work_reschedule(dwork, K_MSEC(NUS_SEND_RETRY_MS));
			return;
		}

		ring_buf_get_finish(&uart_to_ble_rb, len);

		if (err != 0 && err != -ENOTCONN) {
			LOG_WRN("bt_nus_send failed (err %d), %u bytes dropped",
				err, len);
		}
	}
}

static void adv_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	int err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_2, ad, ARRAY_SIZE(ad),
				  sd, ARRAY_SIZE(sd));

	if (err == -EALREADY) {
		return;
	} else if (err != 0) {
		LOG_ERR("Failed to start advertising (err %d)", err);
	} else {
		LOG_INF("Advertising as \"%s\"", CONFIG_BT_DEVICE_NAME);
	}
}

static void nus_notif_enabled(bool enabled, void *ctx)
{
	ARG_UNUSED(ctx);

	atomic_set(&nus_subscribed, enabled ? 1 : 0);
	LOG_INF("NUS notifications %s", enabled ? "enabled" : "disabled");

	if (enabled) {
		k_work_schedule(&ble_tx_work, K_NO_WAIT);
	}
}

static void nus_received(struct bt_conn *conn, const void *data, uint16_t len,
			 void *ctx)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(ctx);

	uint32_t written = ring_buf_put(&ble_to_uart_rb, data, len);

	if (written < len) {
		LOG_WRN("UART TX overflow, %u bytes dropped", len - written);
	}
	if (written > 0) {
		uart_irq_tx_enable(bridge_uart);
	}
}

static struct bt_nus_cb nus_listener = {
	.notif_enabled = nus_notif_enabled,
	.received = nus_received,
};

static void connected(struct bt_conn *conn, uint8_t err)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	if (err != 0) {
		LOG_WRN("Connection to %s failed (err %u)", addr, err);
		return;
	}

	/* ATT MTU starts at the default until the central negotiates more */
	atomic_set(&att_mtu, ATT_DEFAULT_MTU);
	LOG_INF("Connected: %s", addr);
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	atomic_set(&nus_subscribed, 0);
	LOG_INF("Disconnected: %s (reason 0x%02x)", addr, reason);
}

static void recycled(void)
{
	/* Connection object is free again: resume advertising */
	k_work_submit(&adv_work);
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = connected,
	.disconnected = disconnected,
	.recycled = recycled,
};

static void att_mtu_updated(struct bt_conn *conn, uint16_t tx, uint16_t rx)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(rx);

	atomic_set(&att_mtu, tx);
	LOG_INF("ATT MTU: %u (%u byte payload per notification)", tx, tx - 3);
}

static struct bt_gatt_cb gatt_callbacks = {
	.att_mtu_updated = att_mtu_updated,
};

int main(void)
{
	int err;

	if (!device_is_ready(bridge_uart)) {
		LOG_ERR("Bridge UART not ready");
		return 0;
	}

	err = bt_nus_cb_register(&nus_listener, NULL);
	if (err != 0) {
		LOG_ERR("Failed to register NUS callbacks (err %d)", err);
		return 0;
	}

	bt_gatt_cb_register(&gatt_callbacks);

	err = bt_enable(NULL);
	if (err != 0) {
		LOG_ERR("Failed to enable Bluetooth (err %d)", err);
		return 0;
	}

	err = uart_irq_callback_user_data_set(bridge_uart, bridge_uart_isr,
					      NULL);
	if (err != 0) {
		LOG_ERR("Failed to set UART callback (err %d)", err);
		return 0;
	}
	uart_irq_rx_enable(bridge_uart);

	k_work_submit(&adv_work);

	LOG_INF("BLE serial bridge ready, UART %s @ %u baud",
		bridge_uart->name,
		DT_PROP(DT_ALIAS(bridge_uart), current_speed));

	return 0;
}
