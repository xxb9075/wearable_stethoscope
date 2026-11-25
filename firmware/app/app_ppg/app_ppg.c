/******************************************************************************/
/***************************** Include Files **********************************/
/******************************************************************************/
#include "app_ppg.h"
LOG_MODULE_REGISTER(app_ppg);

/******************************************************************************/
/**************************** Global Variables ********************************/
/******************************************************************************/
// The MAX32664 device, the max32664.h header has already included the <device.h> header.
const struct device *dev;
// Local buffer used to read data from the max32664 driver
static uint8_t* pAppBuff;
static uint8_t buffLen;
// Local structs used to read data from the max32664 driver
static struct sensor_value hr, spo2, status, confidence;

/******************************************************************************/
/************************ Function Implementations ****************************/
/******************************************************************************/
void ShowResultPpg(ppg_data_t const * const pData) {
    uint8_t *byte_ptr = (uint8_t*)pData;
    LOG_INF("ppg sample size %d", sizeof(ppg_data_t));
    for (size_t i = 0; i < sizeof(ppg_data_t); i++) {
        LOG_INF("Byte %zu: %02x", i, byte_ptr[i]);
    }
    LOG_INF("PPG sample: Heart Rate: %d.%01d, Oxygen: %d.%01d, Status: %d, Confidence: %d",
        pData->heart_rate_int_part, pData->heart_rate_frac_part,
        pData->oxygen_int_part, pData->oxygen_frac_part,
        pData->status, pData->confidence);
    return;
}

int32_t AppPpgInit(void *pBuff, uint8_t buffLength) {
	dev = device_get_binding(DT_LABEL(DT_INST(0, max_max32664)));
	// pAppBuff = (uint8_t*)pBuff;
	buffLen = buffLength;
	return SUCCESS;
}

int32_t AppPpgTimed(void *pBuff, uint32_t* sampleCount){
	if (dev == NULL){
		return FAILURE;
	}
	// Fetch sensor samples: 0 if successful, negative errno code if failure
	int err = sensor_sample_fetch(dev);
	if (err){
		LOG_ERR("Failed to fetch sensor samples (err %d)\n", err);
		return FAILURE;
	}

	// Get sensor data
	err |= sensor_channel_get(dev, SENSOR_CHAN_HEARTRATE, &hr);
	err |= sensor_channel_get(dev, SENSOR_CHAN_SPO2, &spo2);
	err |= sensor_channel_get(dev, SENSOR_CHAN_HR_STATUS, &status);
	err |= sensor_channel_get(dev, SENSOR_CHAN_CONFIDENCE, &confidence);

	if (err){
		LOG_ERR("Failed to get sensor data (err %d)\n", err);
		return FAILURE;
	}

	/* Assemble the data here */
	/* sensor_value: val1 is the integer part, val2 the fraction in millionths.
	   Heart rate and SpO2 are sent as integer part and tenths (val2 / 1e5). */

	// printk("HR: %d bpm, SpO2: %d %%, Status: %d, Confidence: %d%%\n",
	// 		hr.val1, spo2.val1, status.val1, confidence.val1);

	ppg_data_t data = {
		.heart_rate_int_part = (uint8_t)hr.val1,
		.heart_rate_frac_part = (uint8_t)(hr.val2 / 1E5),
		.oxygen_int_part = (uint8_t)spo2.val1,
		.oxygen_frac_part = (uint8_t)(spo2.val2 / 1E5),
		.status = (uint8_t)status.val1,
		.confidence = (uint8_t)confidence.val1
	};

	// Prevent buffer overflow from struct padding
	// size_t dataSize = sizeof(data.heart_rate_int_part) + sizeof(data.heart_rate_frac_part) +
	// 				sizeof(data.oxygen_int_part) + sizeof(data.oxygen_frac_part) +
	// 				sizeof(data.status) + sizeof(data.confidence);
	// Modified to all elements in uint8_t, no need to calculate individual element size since there is no padding
	size_t dataSize = sizeof(ppg_data_t);
	// LOG_INF("Data size: %d", dataSize);
	// LOG_INF("Buffer length: %d", buffLen);
	if (dataSize != buffLen) {
		LOG_ERR("Data size does not match buffer length");
		return FAILURE;
	}
	memcpy(pBuff, &data, dataSize);
	*sampleCount = 1;
	return SUCCESS;
}

