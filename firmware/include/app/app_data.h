/*
Packet assembly for BLE streaming.

Each modality has a buffer laid out as [header | data] and a ModalityDesc_t
entry in ModalityDescs[], indexed by the modality enum (MODALITY_MIC,
MODALITY_ECG, MODALITY_PPG; ECG is disabled). An entry holds the enable flag,
the buffer pointer, the header value and length, the data length and the size
of one data point. NUM_MODALITIES sets the number of entries; the accelerometer
entry is not used.
*/

#ifndef APP_DATA_H
#define APP_DATA_H

/******************************************************************************/
/***************************** Include Files **********************************/
/******************************************************************************/
#include <stdint.h>
#include <string.h>
#include <logging/log.h>
#include <bluetooth/conn.h>

/* Application libraries */
#include "error.h"
#include "app_data.h"
#include "app_mic.h"
#include "app_ppg.h"

/******************************************************************************/
/********************** Macros and Constants Definitions **********************/
/******************************************************************************/
// BLE related
#define DEFAULT_PAYLOAD_LEN_BYTES 24
#define BLE_L2CAP_HEADER_LEN_BYTES 4
#define BLE_ATT_HEADER_LEN_BYTES 3


#define HEADER_LEN_BYTES 4
#define MICDATA_LEN_BYTES 4800
#define ACCDATA_LEN_BYTES 4
#define PPGDATA_LEN_BYTES (sizeof(ppg_data_t)) // 1 byte each: heart rate integer part and tenths, SpO2 integer part and tenths, status, confidence
#define ECGDATA_LEN_BYTES 4800
#define NUM_MODALITIES 3 // Set to 4 for ACC

//#define TEST_ACQ_ONLY

#ifdef TEST_ACQ_ONLY
	#define MAX_PAYLOAD_LEN_BYTES 247
#endif

/******************************************************************************/
/***************************** Exported Types *********************************/
/******************************************************************************/

/******************************************************************************/
/*************************** Exported Functions *******************************/
/******************************************************************************/
/**
 * @brief Initializes the application data.
 *
 * @return int32_t Returns SUCCESS if initialization is successful, otherwise returns an error code.
 */
int32_t AppDataInit(void);

/* BLE related */
/**
 * @brief Callback function that is called when a BLE connection is established.
 * 
 * @param conn Pointer to the bt_conn structure representing the connection.
 * @param err Error code indicating the result of the connection attempt.
 *            A value of 0 indicates success, while non-zero values indicate
 *            an error.
 */
void AppDataBleObserverConnected(struct bt_conn *conn, uint8_t err);

/**
 * @brief Handles BLE disconnection events.
 * 
 * @param conn Pointer to the BLE connection object that was disconnected.
 * @param reason Reason code for the disconnection.
 */
void AppDataBleObserverDisconnected(struct bt_conn *conn, uint8_t reason);

/**
 * @brief Callback function that gets called when BLE connection parameters are updated.
 * 
 * @param conn Pointer to the BLE connection object.
 * @param interval New connection interval value.
 * @param latency New connection latency value.
 * @param timeout New connection timeout value.
 */
void AppDataBleObserverParamUpdated(struct bt_conn *conn, uint16_t interval, uint16_t latency, uint16_t timeout);

/**
 * @brief Updates the BLE observer data length information.
 * 
 * @param conn Pointer to the BLE connection structure.
 * @param info Pointer to the structure containing the updated data length information.
 */
void AppDataBleObserverDataLenUpdated(struct bt_conn *conn, struct bt_conn_le_data_len_info *info);

/**
 * @brief Handles changes in BLE observer data subscription status.
 * 
 * @param enabled A boolean indicating the new subscription status:
 *                - true: Central device has started listening.
 *                - false: Central device has stopped listening.
 */
void AppDataBleObserverDataSubscriptionChanged(bool enabled);

/**
 * @brief Callback function that gets called when the PHY (Physical Layer) of a BLE connection is updated.
 * 
 * @param conn Pointer to the Bluetooth connection object.
 * @param param Pointer to the structure containing the updated PHY information.
 */
void AppDataBleObserverPhyUpdated(struct bt_conn *conn, struct bt_conn_le_phy_info *param);

/**
 * @brief Handles the change in BLE observer stream signal.
 * 
 * @param enabled A boolean indicating whether the stream signal is enabled (true) or disabled (false).
 */
void AppDataBleObserverStreamSignalChanged(bool enabled);
#endif // APP_DATA_H