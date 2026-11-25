/***************************************************************************//**
 *   @file   data_ble.c
 *   @brief  BLE data service: notifications and start/stop control
********************************************************************************
*******************************************************************************/

/******************************************************************************/
/***************************** Include Files **********************************/
/******************************************************************************/

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/conn.h>
#include <bluetooth/uuid.h>
#include <bluetooth/gatt.h>

#include <stdbool.h>
#include <logging/log.h>

#include "data_ble.h"
#include "error.h"

#include "app_data.h"

#define DATABLE_MAX_PAYLOAD_LEN 244

LOG_MODULE_REGISTER(DataBle);

/******************************************************************************/
/********************** Macros and Constants Definitions **********************/
/******************************************************************************/

// UUID Declarations
//***************************************//

// UUID DATABLE Service
// 86dd0000-b956-4fdc-9b9e-fa64a9b436f2
struct bt_uuid_128 UUID_DATABLE_SVC = BT_UUID_INIT_128(
	BT_UUID_128_ENCODE(0x86dd0000,0xb956,0x4fdc,0x9b9e,0xfa64a9b436f2));

// UUID DATABLE Characteristic Data
// 86dd0001-b956-4fdc-9b9e-fa64a9b436f2
struct bt_uuid_128 UUID_DATABLE_CHRC_DATA = BT_UUID_INIT_128(
	BT_UUID_128_ENCODE(0x86dd0001,0xb956,0x4fdc,0x9b9e,0xfa64a9b436f2));

// UUID DATABLE Characteristic Start and Stop
// 86dd0002-b956-4fdc-9b9e-fa64a9b436f2
struct bt_uuid_128 UUID_DATABLE_CHRC_STARTSTOPCMD = BT_UUID_INIT_128(
	BT_UUID_128_ENCODE(0x86dd0002,0xb956,0x4fdc,0x9b9e,0xfa64a9b436f2));

/******************************************************************************/
/*************************** Types Declarations *******************************/
/******************************************************************************/

// BLE Attribute Array Index
enum BleAttrArrIdx {
	DATABLE_SVC = 0,
	DATABLE_CHRC_DATA = 1,
};

typedef struct {
	uint8_t* pData;
	uint32_t dataLen;
	uint8_t centralSubscription;
	uint8_t stopStartCmd;
} InterfaceDesc_t;

InterfaceDesc_t InterfaceDesc;

/******************************************************************************/
/************************ Functions Definitions *******************************/
/******************************************************************************/

// Note: bt_gatt_attr_read() is a GATT API call
// To-do: create single read handler and a single write handler and use a case/switch statement based on *attr, or use pointers

static ssize_t ReadChrcStartStopCmd(struct bt_conn *conn, const struct bt_gatt_attr *attr,
		       void *buf, uint16_t len, uint16_t offset)
{
	const char *value = attr->user_data;

	return bt_gatt_attr_read(conn, attr, buf, len, offset, value,
				 sizeof(InterfaceDesc.stopStartCmd));
}

static ssize_t WriteChrc(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			const void *buf, uint16_t len, uint16_t offset,
			uint8_t flags)
{
	uint8_t *value = attr->user_data;

	memcpy(value + offset, buf, len);

	return len;
}

static ssize_t WriteChrcStartStopCmd(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			const void *buf, uint16_t len, uint16_t offset,
			uint8_t flags)
{
	uint8_t *pData = (uint8_t*) buf;

	uint8_t expectedDataLen = sizeof(InterfaceDesc.stopStartCmd);

	if(len > expectedDataLen) {
		printk("BAD WRITE");
		//return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
	}

	// Using "magic numbers" since these refer to raw bytes
	if((*pData == 0x0) || (*pData == 0x1))
	{
		AppDataBleObserverStreamSignalChanged(*pData == 0x1 ? true : false);
	}
	else {
		printk("start stop error\n");
	}

	return WriteChrc(conn, attr, buf, len, offset, flags);
}


static void CentralSubscriptionUpdated(const struct bt_gatt_attr *attr, uint16_t value)
{

	LOG_DBG("Notifications turned %s",
		value == BT_GATT_CCC_NOTIFY ? "on" : "off");

	InterfaceDesc.centralSubscription = value;

	AppDataBleObserverDataSubscriptionChanged(value == BT_GATT_CCC_NOTIFY ? true : false);
}

static void DataSent(struct bt_conn *conn, void *user_data)
{
	ARG_UNUSED(user_data);

	LOG_DBG("Data sent, conn %p", (void *)conn);
}

//*************************************************//

/* DataBle Service Declaration */
BT_GATT_SERVICE_DEFINE(GattDataBleSvc,

	BT_GATT_PRIMARY_SERVICE(&UUID_DATABLE_SVC.uuid),

	BT_GATT_CHARACTERISTIC(&UUID_DATABLE_CHRC_DATA.uuid,
					BT_GATT_CHRC_NOTIFY,
					BT_GATT_PERM_NONE,
					NULL, NULL, NULL),
	BT_GATT_CCC(CentralSubscriptionUpdated, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
	BT_GATT_CUD("DATABLE: DATA", BT_GATT_PERM_READ),

	BT_GATT_CHARACTERISTIC(& UUID_DATABLE_CHRC_STARTSTOPCMD.uuid,
					BT_GATT_CHRC_READ|BT_GATT_CHRC_WRITE,
			       	BT_GATT_PERM_READ | BT_GATT_PERM_WRITE,
			       	ReadChrcStartStopCmd, WriteChrcStartStopCmd, &InterfaceDesc.stopStartCmd),
	BT_GATT_CUD("DATABLE: STARTSTOPCMD", BT_GATT_PERM_READ),
);

//*************************************************//

int32_t DataBleInit(void) {

	InterfaceDesc.pData = NULL;
	InterfaceDesc.dataLen = 0;
	InterfaceDesc.centralSubscription = 0;
	InterfaceDesc.stopStartCmd = 0;

	return SUCCESS;
}

// Set data source
int32_t DataBleSetTxSource(uint8_t *pSource, uint32_t *pNumBytes) {

	if(*pNumBytes > DATABLE_MAX_PAYLOAD_LEN)
		return FAILURE;

	InterfaceDesc.pData = (uint8_t*) pSource;
	InterfaceDesc.dataLen = *pNumBytes;

	return SUCCESS;
}

// Transmit data
int32_t DataBleTxData(struct bt_conn *conn) {

	struct bt_gatt_notify_params params = 
	{
		//.uuid	= ,
		.attr   = &GattDataBleSvc.attrs[DATABLE_CHRC_DATA],
		.data   = InterfaceDesc.pData,
		.len    = InterfaceDesc.dataLen,
		.func   = DataSent,
	};

	return bt_gatt_notify_cb(conn, &params);
}