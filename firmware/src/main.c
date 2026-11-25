/* main.c - Application main entry point */
/*
 * Copyright (c) 2016 Intel Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/******************************************************************************/
/***************************** Include Files **********************************/
/******************************************************************************/
#include "../include/main.h"

/******************************************************************************/
/********************** Macros and Constants Definitions **********************/
/******************************************************************************/
LOG_MODULE_REGISTER(main);

/******************************************************************************/
/**************************** Global Variables ********************************/
/******************************************************************************/
/* BLE related */
/**
 * @brief Advertising data (flags, device name) and the current and
 *        authenticating connections.
 */
static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA(BT_DATA_NAME_COMPLETE, DEVICE_NAME, DEVICE_NAME_LEN),
};
static struct bt_conn *current_conn;
static struct bt_conn *auth_conn;
static struct bt_le_conn_param *conn_param = BT_LE_CONN_PARAM(INTERVAL_MIN, INTERVAL_MAX, CONN_LATENCY, SUPERVISION_TIMEOUT);

/******************************************************************************/
/**************************** Private Functions *******************************/
/******************************************************************************/
/* BLE protocol stack related */
// The following functions are used to manage the Bluetooth connection and its parameters, project specific handling is defined in app_data.c.
/**
 * @brief Converts a PHY value to its corresponding string representation.
 * 
 * @param phy The PHY value to be converted.
 * @return const char* A string representing the PHY type.
 */
static const char *phy2str(uint8_t phy)
{
	switch (phy)
	{
	case 0:
		return "No packets";
	case BT_GAP_LE_PHY_1M:
		return "LE 1M";
	case BT_GAP_LE_PHY_2M:
		return "LE 2M";
	case BT_GAP_LE_PHY_CODED:
		return "LE Coded";
	default:
		return "Unknown";
	}
}

/**
 * @brief Updates the connection parameters for the current Bluetooth connection.
 * 
 * @return int Returns 0 on success, or a negative error code on failure.
 */
static int update_connection_parameters(void)
{
	int err;
	err = bt_conn_le_param_update(current_conn, conn_param);
	if (err)
	{
		LOG_ERR("Cannot update connection parameter (err: %d)", err);
		return err;
	}
	LOG_INF("Connection parameters update requested");
	return 0;
}

/**
 * @brief Callback function for MTU exchange.
 * 
 * @param conn Pointer to the Bluetooth connection structure.
 * @param err Error code indicating the result of the MTU exchange. A value of 0 indicates success.
 * @param params Pointer to the GATT exchange parameters structure.
 */
static void MTU_exchange_cb(struct bt_conn *conn, uint8_t err, struct bt_gatt_exchange_params *params)
{
	uint8_t payload_length = 24;
	if (!err)
	{
		LOG_INF("MTU exchange done. ");
		payload_length = bt_gatt_get_mtu(current_conn) - 3; // 3 bytes ATT header

		// Ensure payload length is divisible by 24 for our applicaiton
		payload_length = (payload_length / 24) * 24;

		LOG_INF("Payload: %d bytes", payload_length);
	}
	else
	{
		LOG_WRN("MTU exchange failed (err %" PRIu8 ")", err);
	}
}

/**
 * @brief Request an MTU exchange with the current connection.
 * 
 * @note This function logs a warning if the MTU exchange fails.
 */
static void request_mtu_exchange(void)
{
	int err;
	static struct bt_gatt_exchange_params exchange_params;
	exchange_params.func = MTU_exchange_cb;

	err = bt_gatt_exchange_mtu(current_conn, &exchange_params);
	if (err)
	{
		LOG_WRN("MTU exchange failed (err %d)", err);
	}
	else
	{
		LOG_INF("MTU exchange pending");
	}
}

/**
 * @brief Requests an update to the LE data length parameters for the current connection.
 * 
 * @note Ensure that `current_conn` is a valid and active Bluetooth connection before calling this function.
 */
static void request_data_len_update(void)
{
	int err;
	err = bt_conn_le_data_len_update(current_conn, BT_LE_DATA_LEN_PARAM_MAX);
	if (err)
	{
		LOG_ERR("LE data length update request failed: %d", err);
	}
}

/**
 * @brief Requests an update to the PHY (Physical Layer) of the current Bluetooth connection.
 * 
 * @note Ensure that `current_conn` is a valid and active Bluetooth connection before calling this function.
 */
static void request_phy_update(void)
{
	int err;

	err = bt_conn_le_phy_update(current_conn, BT_CONN_LE_PHY_PARAM_2M);
	if (err)
	{
		LOG_ERR("Phy update request failed: %d", err);
	}
}

/**
 * @brief Callback function that is called when a Bluetooth connection is established.
 * 
 * @param conn Pointer to the Bluetooth connection structure.
 * @param err Error code indicating the status of the connection. A value of 0 indicates success,
 *            while non-zero values indicate failure.
 */
static void connected(struct bt_conn *conn, uint8_t err)
{
	char addr[BT_ADDR_LE_STR_LEN];

	if (err)
	{
		LOG_ERR("Connection failed (err %u)", err);
		return;
	}

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	LOG_INF("Connected %s", log_strdup(addr));
	printk("CONNECTED \n");
	current_conn = bt_conn_ref(conn);

	/*
	int32_t ret;
	for(int i=0; i<3; i++) {
		ret = update_connection_parameters();
		if(ret) {
			k_msleep(10);
			continue;	// update request failed
		}
		else {
			break;		// update request succesful
		}
	}
	*/

	AppDataBleObserverConnected(conn, err);

	update_connection_parameters();
	request_mtu_exchange();
	request_data_len_update();
	request_phy_update();
}

/**
 * @brief Callback function that handles Bluetooth disconnection events.
 * 
 * @param conn Pointer to the Bluetooth connection object that was disconnected.
 * @param reason Reason code for the disconnection.
 */
static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	char addr[BT_ADDR_LE_STR_LEN];

	// To-do: make an array of observers?

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	LOG_INF("Disconnected: %s (reason %u)", log_strdup(addr), reason);
	printk("DISCONNECTED \n");
	if (auth_conn)
	{
		bt_conn_unref(auth_conn);
		auth_conn = NULL;
	}

	if (current_conn)
	{
		bt_conn_unref(current_conn);
		current_conn = NULL;
	}

	AppDataBleObserverDisconnected(conn, reason);

}

/* Callbacks for updated connection parameters, data length, and phy option */
/**
 * @brief Callback function that is called when the connection parameters are updated.
 * 
 * @param conn Pointer to the Bluetooth connection structure.
 * @param interval Connection interval in units of 1.25 ms.
 * @param latency Connection latency.
 * @param timeout Supervision timeout in units of 10 ms.
 */
static void le_param_updated(struct bt_conn *conn, uint16_t interval, uint16_t latency, uint16_t timeout)
{
	uint32_t interval_int = interval * 1.25;
	LOG_INF("Conn params updated: interval %d ms, latency %d, timeout: %d0 ms", interval_int, latency, timeout);

	AppDataBleObserverParamUpdated(conn, interval, latency, timeout);
}

/**
 * @brief Callback function that is called when the LE data length is updated.
 * 
 * @param conn Pointer to the Bluetooth connection structure.
 * @param info Pointer to the structure containing the updated LE data length information.
 */
static void le_data_len_updated(struct bt_conn *conn,
								struct bt_conn_le_data_len_info *info)
{
	LOG_INF("LE data len updated: TX (len: %d time: %d)"
			" RX (len: %d time: %d)\n",
			info->tx_max_len,
			info->tx_max_time, info->rx_max_len, info->rx_max_time);

	AppDataBleObserverDataLenUpdated(conn, info);
}

/**
 * @brief Callback function that is called when the LE PHY is updated.
 * 
 * @param conn Pointer to the Bluetooth connection object.
 * @param param Pointer to the structure containing the updated PHY information.
 */
static void le_phy_updated(struct bt_conn *conn,
						   struct bt_conn_le_phy_info *param)
{
	LOG_INF("LE PHY updated: TX PHY %s, RX PHY %s\n",
			phy2str(param->tx_phy), phy2str(param->rx_phy));

	AppDataBleObserverPhyUpdated(conn, param);
}

/**
 * @brief Callback function to be called when Bluetooth is ready.
 * 
 * @note This function assumes that the Bluetooth subsystem has been properly
 * initialized before calling this function.
 * 
 * @retval None
 */
static void bt_ready(void)
{
	int err;

	LOG_INF("Bluetooth initialized\n");

	err = bt_le_adv_start(BT_LE_ADV_CONN_IDENTITY, ad, ARRAY_SIZE(ad), NULL, 0);
	if (err)
	{
		LOG_ERR("Advertising failed to start (err %d)\n", err);
		return;
	}

	LOG_INF("Advertising successfully started\n");
}

/**
 * @brief Cancels the Bluetooth pairing process.
 * 
 * @param conn Pointer to the Bluetooth connection structure.
 */
static void auth_cancel(struct bt_conn *conn)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	printk("Pairing cancelled: %s\n", addr);
}

static struct bt_conn_auth_cb conn_auth_callbacks = {
	.cancel = auth_cancel,
};

/* Set all callbacks for BLE */
static struct bt_conn_cb conn_callbacks = {
	.connected = connected,
	.disconnected = disconnected,
	.le_param_updated = le_param_updated,
	.le_phy_updated = le_phy_updated,
	.le_data_len_updated = le_data_len_updated,
	/*.le_param_req = le_param_req,*/ // Not Used
};

int main(void)
{
	int ret;

	ret = gpio_pin_configure_dt(&led, GPIO_OUTPUT_LOW);
	if (ret < 0)
	{
		return 0;
	}

	bt_conn_cb_register(&conn_callbacks);
	bt_conn_auth_cb_register(&conn_auth_callbacks);

#ifdef APP_PPG_DEBUG
	printk("***** PPG + sensor hub testing *****\n");

	uint8_t PpgBuff[22];
	ret = AppPpgInit(PpgBuff, PPGDATA_LEN_BYTES);
	while(1){
		uint8_t dataCount = 0;
		AppPpgTimed((void *) (PpgBuff + 4), &dataCount);
		ShowResultPpg((ppg_data_t *) (PpgBuff + 4));
		k_sleep(K_SECONDS(1));
	}
	return 0;
#endif // APP_PPG_DEBUG

#ifdef APP_DATA_DEBUG

/*
	ret = AppMicInit(&AppMicCfg);
	if (ret)
	{
		LOG_ERR("Application mic init failed ((err %d)\n", ret);
	}
*/

	ret = AppDataInit();

	ret = bt_enable(NULL);
	if (ret)
	{
		LOG_ERR("Bluetooth init failed (err %d)\n", ret);
		return FAILURE;
	}

	bt_ready();
	printk("Advertising started\n");

	while (1)
	{
		#ifdef APP_GPIO_DEBUG
		gpio_pin_toggle_dt(&led);
		#endif // APP_GPIO_DEBUG
		__WFI();
		//		update_connection_parameters();
		// printk("Hello World!\n");
		k_msleep(1000);
	}
	return SUCCESS;
#endif // APP_DATA_DEBUG
}