#ifndef MAIN_H
#define MAIN_H

/******************************************************************************/
/***************************** Include Files **********************************/
/******************************************************************************/
// Zephyr Libraries
#include <zephyr.h>
#include <zephyr/types.h>
#include <device.h>
#include <devicetree.h>
#include <drivers/gpio.h>
#include <drivers/sensor.h>

// Standard Libraries
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h> // for memcpy
#include <errno.h> // We defined a user error.h file

#include <logging/log.h>
#include <sys/printk.h>
#include <sys/byteorder.h>

// Bluetooth Libraries
#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/conn.h>
#include <bluetooth/uuid.h>
#include <bluetooth/gatt.h>

#include <bluetooth/services/bas.h>
#include <bluetooth/services/hrs.h>

// User Libraries
#include "error.h"
#include "app_data.h"

// /******************************************************************************/
// /********************** Macros and Constants Definitions **********************/
// /******************************************************************************/
// /* Zephyr related */
// Parameters for thread
#define STACKSIZE CONFIG_BT_NUS_THREAD_STACK_SIZE
#define PRIORITY 7

// BLE Device Name
#define DEVICE_NAME 		CONFIG_BT_DEVICE_NAME
#define DEVICE_NAME_LEN 	(sizeof(DEVICE_NAME) - 1)

// BLE Connection Parameters
#define INTERVAL_MIN 6			/* Conn interval min in 1.25 ms units: 6*1.25 = 7.5 ms */
#define INTERVAL_MAX 12			/* Conn interval max in 1.25 ms units: 12*1.25 = 15 ms */
#define CONN_LATENCY 0			/* Conn latency, i.e. number of allowable missed BLE events */
#define SUPERVISION_TIMEOUT 600 /* Conn supervision timeout in 10 ms units: 600*10 = 6 s */

#define DATA_LEN BT_GAP_DATA_LEN_MAX   /* Maximum Link Layer transmission payload size in bytes */
#define DATA_TIME BT_GAP_DATA_TIME_MAX /* Maximum Link Layer transmission payload time in us */

#define BT_LE_ADV_CONN_IDENTITY BT_LE_ADV_PARAM(BT_LE_ADV_OPT_CONNECTABLE | BT_LE_ADV_OPT_USE_IDENTITY, \
												BT_GAP_ADV_FAST_INT_MIN_2,                              \
												BT_GAP_ADV_FAST_INT_MAX_2, NULL)

/* Application specific */
// Testing macros
#define APP_GPIO_DEBUG
// #define APP_PPG_DEBUG
// #define APP_ACC_DEBUG
// #define APP_ECG_DEBUG
#define APP_DATA_DEBUG

#ifdef APP_GPIO_DEBUG
/* The devicetree node identifier for the "led0" alias. */
#define LED0_NODE DT_ALIAS(led0)
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED0_NODE, gpios);
#endif // APP_GPIO_DEBUG

#ifdef APP_PPG_DEBUG
// #define APP_PPG_DEBUG_POLLING   // This is for preliminary testing, if not defined, then use the Interrupt driven app_data framework
#include "app_ppg.h"
// #include "SEGGER_RTT.h"
#endif // APP_PPG_DEBUG

// /******************************************************************************/
// /***************************** Exported Types *********************************/
// /******************************************************************************/


// /******************************************************************************/
// /*************************** Exported Functions *******************************/
// /******************************************************************************/

#endif // MAIN_H