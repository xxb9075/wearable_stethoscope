/******************************************************************************/
/***************************** Include Files **********************************/
/******************************************************************************/
#include <stdint.h>
#include <string.h>
#include <logging/log.h>

// BLE related
#include "data_ble.h"
#include <bluetooth/conn.h>

// Application libraries
#include "error.h"
#include "app_data.h"
#include "app_mic.h"
#include "app_ppg.h"
//#include "app_acc.h"

LOG_MODULE_REGISTER(AppData);

/******************************************************************************/
/**************************** Global Variables ********************************/
/******************************************************************************/
uint8_t MicBuff[MICDATA_LEN_BYTES + HEADER_LEN_BYTES];
uint8_t AccBuff[ACCDATA_LEN_BYTES + HEADER_LEN_BYTES];
uint8_t PpgBuff[PPGDATA_LEN_BYTES + HEADER_LEN_BYTES];
uint8_t EcgBuff[ECGDATA_LEN_BYTES + HEADER_LEN_BYTES];

typedef struct {
	bool bEnabled;
	uint8_t *pBuff;
	uint32_t dataLen;
	uint32_t dataSize;
	uint32_t headerLen;
	uint32_t headerVal;
} ModalityDesc_t;

enum Modalities {
	MODALITY_MIC,
	MODALITY_ECG,
	MODALITY_PPG,
	// MODALITY_ACC,
};

enum Headers {
	HEADER_MIC = 1,
	HEADER_ECG = 2,
	HEADER_ACC = 3,
	HEADER_PPG = 4,
};

#define MIC_SAMPLE_1 0
#define MIC_SAMPLE_2 1
#define MIC_SAMPLE_3 2
#define ECG_SAMPLE_1 3

typedef struct {
	struct bt_conn *conn;
	uint16_t payloadLen;
	bool bDataSubscriptionEnabled;
	bool bConnected;
	bool bDataLenUpdated;
	bool bParamUpdated;
	bool bDataStreamEnabled;
} BleDesc_t;

BleDesc_t BleDesc;

ModalityDesc_t ModalityDescs[NUM_MODALITIES];

/******************************************************************************/
/***************************** Private Functions ******************************/
/******************************************************************************/


/******************************************************************************/
/************************ Function Implementations ****************************/
/******************************************************************************/
static int32_t ShowResult(void *pBuff, uint32_t buffLen) {

	uint32_t *pHeader = (uint32_t *) pBuff;
	uint8_t *pData = ((uint8_t *) pBuff) + HEADER_LEN_BYTES;
	uint32_t dataLen = buffLen - HEADER_LEN_BYTES;
	uint32_t dataCount;

	dataCount = 0;

	switch(*pHeader) {
		case(HEADER_MIC): {
			dataCount = dataLen / ModalityDescs[MODALITY_MIC].dataSize;
			ShowResultMic((uint16_t *) pData, dataCount);
			break;
		}

		case(HEADER_ECG): {
			dataCount = dataLen / ModalityDescs[MODALITY_ECG].dataSize;
			ShowResultEcg((uint16_t *) pData, dataCount);
			break;
		}
		case(HEADER_PPG): {
			dataCount = dataLen / ModalityDescs[MODALITY_PPG].dataSize;
			ShowResultPpg((ppg_data_t *) pData);
			break;
		}

		default:
			break;
	}

	return SUCCESS;
}

static bool StreamReady(void) {

	if (BleDesc.bDataSubscriptionEnabled &
		BleDesc.bConnected &
		BleDesc.bDataLenUpdated &
		BleDesc.bParamUpdated &
		BleDesc.bDataStreamEnabled
		) {
		return true;
	}
	else {
		return false;
	}
}

//The following function is called whenever the Nordic ADC buffer is full / ready to be offloaded
static void AppDataISR(void) {

	// // Get data for Mic
	// // pData points to where data is stored in the buffer, after the header
	// // dataCount is the number of samples

	uint8_t *pData;
	uint32_t dataCount;
	uint32_t maxPayloadLen, payloadDataLen, numPackets, payloadLen, payloadRemainder;
	int32_t ret;
	uint32_t channelSel;

	//pData = ModalityDescs[MODALITY_MIC].pBuff + ModalityDescs[MODALITY_MIC].headerLen;
	//pData = MultiModalBuff;

	// After this is called, the buffer will contain the samples
	dataCount = 0;
	// AppAdcISR((void *) pData, &dataCount);	// To-do: rename to something reflecting multi-modal data
	AppAdcDataFetch();	// Fetch data from channels enabled in AppDataInit()

	if(ModalityDescs[MODALITY_MIC].bEnabled) {
		pData = ModalityDescs[MODALITY_MIC].pBuff + ModalityDescs[MODALITY_MIC].headerLen;
		dataCount = 0;
		channelSel = ADCCH_MIC1|ADCCH_MIC2|ADCCH_MIC3|ADCCH_MIC4|ADCCH_MIC5;
		AppAdcDataGet(&channelSel, (void *) pData, &dataCount);
		ModalityDescs[MODALITY_MIC].dataLen = dataCount * ModalityDescs[MODALITY_MIC].dataSize;
		LOG_INF("Got %d MIC Samples\n", dataCount);
	}

	if(ModalityDescs[MODALITY_ECG].bEnabled) {
		pData = ModalityDescs[MODALITY_ECG].pBuff + ModalityDescs[MODALITY_ECG].headerLen;
		dataCount = 0;
		channelSel = ADCCH_ECG;
		AppAdcDataGet(&channelSel, (void *) pData, &dataCount);
		ModalityDescs[MODALITY_ECG].dataLen = dataCount * ModalityDescs[MODALITY_ECG].dataSize;
		LOG_INF("Got %d ECG Samples\n", dataCount);
	}

	// // Convert number of samples to number of bytes
	// // dataLen will be used during BLE transmission

	// PPG: offload to app_ppg layer to get data
	if(ModalityDescs[MODALITY_PPG].bEnabled) {
		pData = ModalityDescs[MODALITY_PPG].pBuff + ModalityDescs[MODALITY_PPG].headerLen;
		dataCount = 0;
		AppPpgTimed((void *) pData, &dataCount);
		ModalityDescs[MODALITY_PPG].dataLen = dataCount * ModalityDescs[MODALITY_PPG].dataSize;
		LOG_INF("Got %d PPG Samples\n", dataCount);
	}

	// Acc: offload to app_acc layer to get data, this is when we directly use the separate Acc I2C driver, not from the Sensorhub itself
	// pData = ModalityDescs[MODALITY_ACC].pBuff + ModalityDescs[MODALITY_ACC].headerLen;
	// dataCount = 0;
	// AppAccTimed((void *) pData, &dataCount);
	// ModalityDescs[MODALITY_ACC].dataLen = dataCount * ModalityDescs[MODALITY_ACC].dataSize;

	// Transmit Data
	// Iterate through number of modalities
	for(int i=0; i<NUM_MODALITIES; i++) {

		// Skip any modalities that are not enabled
		if(!ModalityDescs[i].bEnabled)
			continue;

		// The number of bytes we transmit transmit (not including the header) should be evenly divisible by the sample size
		// For example, transmitLen starts as the max size, 247 bytes
		uint8_t *pBuffIter;	// pBuffIter will move through pBuff for data transmission
		maxPayloadLen = 0;
		payloadDataLen = 0;
		payloadRemainder = 0;

		#ifdef TEST_ACQ_ONLY
			maxPayloadLen = MAX_PAYLOAD_LEN_BYTES;
		#else
			maxPayloadLen = BleDesc.payloadLen;
		#endif


	 	pBuffIter = ModalityDescs[i].pBuff;	// Begin at the start of the buffer

	 	// Each packet payload will have a header
	 	payloadDataLen = maxPayloadLen - ModalityDescs[i].headerLen;	// payloadLen is now the max bytes we can allocate toward data
	 	payloadDataLen = payloadDataLen / ModalityDescs[i].dataSize * ModalityDescs[i].dataSize;	// Ensure that the payload is divisible into sample size

	 	numPackets = ModalityDescs[i].dataLen / payloadDataLen;
	 	if(ModalityDescs[i].dataLen % payloadDataLen) {
	 		payloadRemainder = ModalityDescs[i].dataLen - (numPackets*payloadDataLen);
	 		numPackets++;
	 	}

	 	payloadLen = payloadDataLen + ModalityDescs[i].headerLen;	// The number of bytes (data and header) to send per packet
	 	LOG_INF("BLE Payload Len (bytes) - Total: %d, Header: %d, Data: %d", payloadLen, ModalityDescs[i].headerLen, payloadDataLen);
	 	LOG_INF("Payload Remainder Val: %d\n", payloadRemainder);

 		for(int j=0; j<numPackets; j++) {

			// Copy header info into buffer
 			// Note: this will overwite a part of the buffer which has already which has already been transmitted
 			memcpy(pBuffIter, (void*) &ModalityDescs[i].headerVal, ModalityDescs[i].headerLen);

 			if((j == (numPackets-1)) && (payloadRemainder > 0)) {
 				payloadLen = payloadRemainder + HEADER_LEN_BYTES;
 			}

 			#ifdef TEST_ACQ_ONLY

				if (StreamReady()) {
					LOG_INF("Ready to stream\n");
				}
				else {
					LOG_INF("Not ready to stream\n");
				}
 				ShowResult((void*) pBuffIter, payloadLen);

 			#else

				if (!(StreamReady())) {
					break;
				}

				ret = DataBleSetTxSource((uint8_t *)pBuffIter, &payloadLen); // This will update num_bytes accordingly
				if(ret) {
					LOG_ERR("Could not set BLE TX Source\n");
					continue;
				}

				LOG_INF("Attempting to TX: %d bytes", payloadLen);
				ret = DataBleTxData(BleDesc.conn);
				if(ret) {
					LOG_ERR("Could not transmit data\n");
					break;
					// k_sleep(K_MSEC(50));
				}

 			#endif

 			pBuffIter += payloadDataLen;	// Iterate through the buffer for the next data transmission
	 	}
	}
}

int32_t AppDataInit(void) {

	int32_t ret;

	/* !!Change the application parameters here if you want to change it to none-default value */
	AppAdcCfg_t *pAdcCfg;
  
	AppAdcGetCfg(&pAdcCfg);
	pAdcCfg->channelSel = ADCCH_MIC1|ADCCH_MIC2|ADCCH_MIC3|ADCCH_MIC4/*|ADCCH_MIC5/*|ADCCH_ECG*/;	// Select channels to enable. By default, all channels are enabled

	// To-do: perform init checks on the size of buffers, particularly after parsing data from the Nordic ADC
	// Initialize Mic
	ret = AppAdcInit(MicBuff, MICDATA_LEN_BYTES);	// Buffer arguments are currently unused by AppAdcInit()
	if(ret != SUCCESS) return ret;
	AppAdcRegCb(AppDataISR); // Register for callback

	ModalityDesc_t *pModalityDesc;
	// Set Mic Parameters
	pModalityDesc = &ModalityDescs[MODALITY_MIC];
	pModalityDesc -> bEnabled = true;
	pModalityDesc -> pBuff = MicBuff;
	pModalityDesc -> dataLen = 0;
	pModalityDesc -> dataSize = sizeof(uint16_t);
	pModalityDesc -> headerLen = HEADER_LEN_BYTES;
	pModalityDesc -> headerVal = HEADER_MIC;
	// Set ECG Parameters, the init function is omit because it is implemented by the app_mic library
	pModalityDesc = &ModalityDescs[MODALITY_ECG];
	pModalityDesc -> bEnabled = false;
	pModalityDesc -> pBuff = EcgBuff;
	pModalityDesc -> dataLen = 0;
	pModalityDesc -> dataSize = sizeof(uint16_t);
	pModalityDesc -> headerLen = HEADER_LEN_BYTES;
	pModalityDesc -> headerVal = HEADER_ECG;

	// PPG init
	ret = AppPpgInit(PpgBuff, PPGDATA_LEN_BYTES);
	// Set PPG Parameters
	pModalityDesc = &ModalityDescs[MODALITY_PPG];
	pModalityDesc -> bEnabled = true;
	pModalityDesc -> pBuff = PpgBuff;
	// We will have only one samples per ADC interrupt, the dataLen will be updated in the ISR
	pModalityDesc -> dataLen = 0;
	// We want to send the heartRate (including 0.1 precision fractional), confidence, oxygen (including 0.1 precision fractional), and status, the following dataSize would be == PPGDATA_LEN_BYTES
	// One sample per ADC interrupt, should be dataSize * number of samples
	pModalityDesc -> dataSize = PPGDATA_LEN_BYTES;
	pModalityDesc -> headerLen = HEADER_LEN_BYTES;
	pModalityDesc -> headerVal = HEADER_PPG;

	// We wait for CCCD enabled AND connection interval updated
	// k_sem_take decrements the count of the semaphore, unless the semaphore is unavailable (i.e. at zero)
	BleDesc.bConnected = false;
	BleDesc.conn = NULL;
	BleDesc.bDataSubscriptionEnabled = false;
	BleDesc.bConnected = false;
	BleDesc.bDataLenUpdated = false;
	BleDesc.bParamUpdated = false;
	BleDesc.payloadLen = DEFAULT_PAYLOAD_LEN_BYTES;
	BleDesc.bDataStreamEnabled = false;
	DataBleInit();

	return SUCCESS;
}

void AppDataBleObserverConnected(struct bt_conn *conn, uint8_t err) {
    LOG_INF("BleObserverConnected\n");

	BleDesc.conn = bt_conn_ref(conn);
	BleDesc.bConnected = true;
}

void AppDataBleObserverDisconnected(struct bt_conn *conn, uint8_t reason) {

	// Reset all BLE items
	BleDesc.bConnected = false;
	BleDesc.bDataSubscriptionEnabled = false;
	BleDesc.bConnected = false;
	BleDesc.bDataLenUpdated = false;
	BleDesc.bParamUpdated = false;
	BleDesc.payloadLen = DEFAULT_PAYLOAD_LEN_BYTES;
	BleDesc.bDataStreamEnabled = false;

	if (BleDesc.conn) {
		bt_conn_unref(BleDesc.conn);
		BleDesc.conn = NULL;
	}

    LOG_INF("BleObserverDisconnected\n");
}

void AppDataBleObserverParamUpdated(struct bt_conn *conn, uint16_t interval, uint16_t latency, uint16_t timeout) {
    LOG_INF("BleObserverParamUpdated\n");
	BleDesc.bParamUpdated = true;
}

void AppDataBleObserverDataLenUpdated(struct bt_conn *conn, struct bt_conn_le_data_len_info *info) {

	// Account for bytes reserved for the L2CAP header and ATT header
    BleDesc.payloadLen = info->tx_max_len - BLE_L2CAP_HEADER_LEN_BYTES - BLE_ATT_HEADER_LEN_BYTES; 
	BleDesc.bDataLenUpdated = true;

    LOG_INF("BleObserverDataLenUpdated: Payload length is %d bytes\n", BleDesc.payloadLen);
}

void AppDataBleObserverDataSubscriptionChanged(bool enabled) {
	if (enabled) {
		LOG_INF("BleObserverDataSubscriptionChanged: Central Started Listening");

		// k_sem_give increments the count of the semaphore, unless the count is already equal to the limit
		BleDesc.bDataSubscriptionEnabled = true;

	}
	else {
		LOG_INF("BleObserverDataSubscriptionChanged: Central Stopped Listening");
		BleDesc.bDataSubscriptionEnabled = false;

	}
}

void AppDataBleObserverPhyUpdated(struct bt_conn *conn, struct bt_conn_le_phy_info *param) {
    LOG_INF("BleObserverDataPhyUpdated\n");
}

void AppDataBleObserverStreamSignalChanged(bool enabled) {
	if (enabled) {
		LOG_INF("BleObserverStreamSignalChanged: Central Started Stream");
		BleDesc.bDataStreamEnabled = true;
	}
	else {
		LOG_INF("BleObserverStreamSignalChanged: Central Stopped Stream ");
		BleDesc.bDataStreamEnabled = false;
	}
}