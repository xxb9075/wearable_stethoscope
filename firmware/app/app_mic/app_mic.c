
/******************************************************************************/
/***************************** Include Files **********************************/
/******************************************************************************/
#include <stdint.h>
#include <stdbool.h>
#include <logging/log.h>

#include "error.h"
#include "app_mic.h"
#include "nordic_adc.h"

LOG_MODULE_REGISTER(AppAdc);

/******************************************************************************/
/**************************** Global Variables ********************************/
/******************************************************************************/
uint8_t AdcBuff[ADCDATA_LEN];

AppAdcCfg_t AppAdcCfg = {
	.channelSel = ADCCH_MIC1|ADCCH_MIC2|ADCCH_MIC3|ADCCH_MIC4|ADCCH_MIC5|ADCCH_ECG,
	.dataCount = 0,
	.pBuff = AdcBuff,
	.DataIsr = NULL,
};

ChannelCfg_t ChannelCfgArr[NORDIC_ADC_MAX_NUM_CHANNELS];

/******************************************************************************/
/************************ Function Implementations ****************************/
/******************************************************************************/
void ShowResultMic(uint16_t *pData, uint32_t dataCount) {

	uint32_t showCount = dataCount;

	if(showCount > SHOW_RESULT_LIMIT)
		showCount = SHOW_RESULT_LIMIT;

	LOG_INF("Displaying %d of %d MIC Samples", showCount, dataCount);
	for(int i=0; i<showCount; i++) {
		LOG_INF("Mic Data[%d] = %d", i, pData[i]);
	}
}

void ShowResultEcg(uint16_t *pData, uint32_t dataCount) {

	uint32_t showCount = dataCount;

	if(showCount > SHOW_RESULT_LIMIT)
		showCount = SHOW_RESULT_LIMIT;

	LOG_INF("Displaying %d of %d ECG Samples", showCount, dataCount);

	for(int i=0; i<showCount; i++) {
		LOG_INF("Ecg Data[%d] = %d", i, pData[i]);
	}
}

/**
   This function is provided for upper controllers that want to change 
   application parameters specially for user defined parameters.
*/
int32_t AppAdcGetCfg(void *pCfg) {
	if(pCfg) {
		*(AppAdcCfg_t**)pCfg = &AppAdcCfg;
		return SUCCESS;
	}
	LOG_ERR("APP ADC CFG MISSING");
	return FAILURE;
}

static int32_t GetNumAvailChannels(void) {
	//NordicAdcCfg.channelMask

	int32_t count, ret;
	NordicAdcCfg_t* pNordicAdcCfg;

	ret = NordicAdcGetCfg(&pNordicAdcCfg);
	if(ret) return FAILURE;

	ChannelCfg_t *pCfgArr = pNordicAdcCfg->pChannelCfgArr;

	for(int i=0; i<NORDIC_ADC_MAX_NUM_CHANNELS; i++) {
		if(pCfgArr[i].bEnabled)
			count++;
	}

	return count;
} 

/**
 * @brief Retrieves ADC data based on the selected channels.
 * 
 * @param pChannelSel Pointer to the channel selection mask. This mask indicates 
 *                    which channels the user is interested in. The function 
 *                    updates this mask to reflect the intersection of the 
 *                    selected and available channels.
 * @param pBuff Pointer to the destination buffer where the ADC data will be copied.
 * @param pCount Pointer to a variable that will be updated with the number of 
 *               data sets copied to the destination buffer.
 * 
 * @return int32_t Returns SUCCESS (0) if the operation is successful, otherwise 
 *                 returns FAILURE (non-zero).
 */
int32_t AppAdcDataGet(uint32_t *pChannelSel, void *pBuff, uint32_t *pCount) {

	int32_t ret;
	uint32_t srcCount, destCount, numDataSets, channelMask;
	uint8_t numAvailChannels;
	uint8_t AvailChannelMap[NORDIC_ADC_MAX_NUM_CHANNELS];
	uint8_t *pSrc = (uint8_t *) AdcBuff;
	uint8_t *pDest = (uint8_t *) pBuff;
	NordicAdcCfg_t* pNordicAdcCfg;

	ret = NordicAdcGetCfg(&pNordicAdcCfg);
	if(ret) return FAILURE;

	ChannelCfg_t *pCfgArr = pNordicAdcCfg->pChannelCfgArr;
	srcCount = AppAdcCfg.dataCount;

	numAvailChannels = 0;
	channelMask = 0;

	// NORDIC_ADC_MAX_NUM_CHANNELS is max number of channels we define in nordic_adc.h
		// It can be less than the total number of channels available in hardware. E.g., we set it to 6 when in hardware there are 8
		// NORDIC_ADC_MAX_NUM_CHANNELS corresponds to the total number of channels we would ever want to sample from for our application
		// For example, NORDIC_ADC_MAX_NUM_CHANNELS = 6 for 5 Mic channels + 1 ecg channel.
	for(int i=0; i<NORDIC_ADC_MAX_NUM_CHANNELS; i++) {

		// Check if the channel is enabled (out of the NORDIC_ADC_MAX_NUM_CHANNELS)
		// bEnabled set in MicAdcInit
		// Here, we go through all the channels that we're enabling for sampling with the ADC
		if(pCfgArr[i].bEnabled) {

			// Channel map stores channelSel values: ADCCH_MIC1, ADCCH_MIC2, ...
			// If an channel is not enabled, then we don't record it in AvailChannelMap
			// the total number of enabled channels sets numAvailChannels
			AvailChannelMap[numAvailChannels] = (0x1 << i);
			numAvailChannels++;

			// Check if the user selected this channel.
			// *pChannelSel is a bitmask, e.g., ADCCH_MIC1|ADCCH_MIC2|ADCCH_ECG
			if((0x1 << i) & *pChannelSel) {
				// Channel mask is the intersection of selected (*pChannelSel) and available channels (the enabled channels)
				channelMask |= (0x1 << i);
			}
		}
	}

	// A data set includes all data for the selected channels
	// srcCount is number of samples from ADC
	numDataSets = srcCount / numAvailChannels;

	destCount = 0;
	// Iterate through all data sets from the source data
	for(int i=0; i<numDataSets; i++) {

		// Iterate through all active channels of a single data set
		for(int j=0; j<numAvailChannels; j++) {

			if(AvailChannelMap[j] & channelMask) {
				memcpy(pDest, pSrc + (j*NORDIC_ADC_SAMPLE_SIZE), NORDIC_ADC_SAMPLE_SIZE);
				pDest += NORDIC_ADC_SAMPLE_SIZE;
				destCount++;
			}
		}

		pSrc += (NORDIC_ADC_SAMPLE_SIZE * numAvailChannels); 
	}

	*pChannelSel = channelMask;
	*pCount = destCount;

	return SUCCESS;
}

static void DataShowResults(int16_t* const pData, uint32_t dataCount) {

	int16_t* pSrcData = (int16_t*) pData;

	uint8_t numAvailChannels = GetNumAvailChannels();

	LOG_INF("Printing %d points", dataCount);

	for(int i=0; i<dataCount/numAvailChannels; i++) {
		switch(numAvailChannels) {
			case 8:
				LOG_INF("ADC Values: %6d %6d %6d %6d %6d %6d %6d %6d",
					pSrcData[0], pSrcData[1], pSrcData[2], pSrcData[3], pSrcData[4], pSrcData[5], pSrcData[6], pSrcData[7]);
				break;
			case 7:
				LOG_INF("ADC Values: %6d %6d %6d %6d %6d %6d %6d",
					pSrcData[0], pSrcData[1], pSrcData[2], pSrcData[3], pSrcData[4], pSrcData[5], pSrcData[6]);
				break;
			case 6:
				LOG_INF("ADC Values: %6d %6d %6d %6d %6d %6d",
					pSrcData[0], pSrcData[1], pSrcData[2], pSrcData[3], pSrcData[4], pSrcData[5]);
				break;
			case 5:
				LOG_INF("ADC Values: %6d %6d %6d %6d %6d",
					pSrcData[0], pSrcData[1], pSrcData[2], pSrcData[3], pSrcData[4]);
				break;
			case 4:
				LOG_INF("ADC Values: %6d %6d %6d %6d",
					pSrcData[0], pSrcData[1], pSrcData[2], pSrcData[3]);
				break;
			case 3:
				LOG_INF("ADC Values: %6d %6d %6d",
					pSrcData[0], pSrcData[1], pSrcData[2]);
				break;
			case 2:
				LOG_INF("ADC Values: %6d %6d",
					pSrcData[0], pSrcData[1]);
				break;
			default:
				LOG_INF("ADC Values: %6d",
					pSrcData[0]);
				break;
		}

		pSrcData += numAvailChannels;
	}
}

static void AdcIsr(void) {

/*
	int32_t ret;

	//LOG_INF("DATA ISR CALLED");

	uint32_t dataCount = APPBUFF_SIZE;
	ret = NordicAdcDataRead((void *) gAppBuff, &dataCount);
	if(ret) {
		LOG_ERR("ADC Data Read Failed ((err %d)\n", ret);
	}

*/

	// DataProcess with mV output is quantization noise due to truncation
	// DataProcess will be skipped until we change to unit more more granular than mV, e.g., uV
	/*
	ret = NordicAdcDataProcess(gAppBuff, &dataCount);
	if(ret) {
		LOG_ERR("ADC Data Process Failed ((err %d)\n", ret);
	}
	*/

	//DataShowResults(gAppBuff, dataCount);

	//AppAdcCfg.DataIsr(gAppBuff, dataCount);
	AppAdcCfg.DataIsr();
}

/**
 * @brief Fetches ADC data and processes it.
 * 
 * @return int32_t Returns SUCCESS if the operation is successful, otherwise returns an error code.
 */

int32_t AppAdcDataFetch(void) {

	int32_t ret;
	uint32_t *pCount = &AppAdcCfg.dataCount;
	uint8_t *pBuff = AppAdcCfg.pBuff;

	// pCount is the number of data samples. Each data sample is 2 bytes per the Nordic ADC
	ret = NordicAdcDataRead((void *) pBuff, pCount);
	if(ret) {
		LOG_ERR("ADC Data Read Failed ((err %d)\n", ret);
	}

	// Do any data processing here
	// AppAdcDataProcess();

	return SUCCESS;
}

// To-do: change parameters to be more generalized, such as the naming convention that Zephr provides (using ifndef)
/**
 * @brief Initializes the ADC configuration for the microphone and ECG channels.
 * 
 * @return int32_t Returns SUCCESS (0) if initialization is successful, otherwise returns an error code.
 */
static int32_t MicEcgAdcInit(void) {

	int32_t ret;

	NordicAdcCfg_t* pNordicAdcCfg;
	ret = NordicAdcGetCfg(&pNordicAdcCfg);
	if(ret) return FAILURE;

	/* Configure general parameters */
	// Channel mapping: see nordic_adc.h

	pNordicAdcCfg->samplingFrequency = 4000;
	pNordicAdcCfg->oversamplingSel = NRF_SAADC_OVERSAMPLE_8X;	// NRF_SAADC_OVERSAMPLE_DISABLED; // NRF_SAADC_OVERSAMPLE_16X;
	//pNordicAdcCfg->gainSel = NRF_SAADC_GAIN1_6;
	//pNordicAdcCfg->channelMask = NORDIC_ADC_CHANNEL_1 | NORDIC_ADC_CHANNEL_2 | NORDIC_ADC_CHANNEL_3 | NORDIC_ADC_CHANNEL_4;
	pNordicAdcCfg->bUseDifferentialMode = true;	// Using false will reference the measurements to GND. True will reference to NORDIC_ADC_CHANNEL_REF
	pNordicAdcCfg->resolutionSel = NRF_SAADC_RESOLUTION_14BIT;
	pNordicAdcCfg->sampleCountThresh = ADCDATA_LEN/NORDIC_ADC_SAMPLE_SIZE;		// Samples

	pNordicAdcCfg->SampleIsr = AdcIsr;


	// Data will be stored in the order of the ChannelCfg_t structs defined for ChannelCfgArr;
	ChannelCfg_t *pChannelCfg;

	// MIC Channel 1
	pChannelCfg = &ChannelCfgArr[0];
	pChannelCfg->channelSel = NORDIC_ADC_CHANNEL_1;
	pChannelCfg->gainSel = NRF_SAADC_GAIN1;
	pChannelCfg->bEnabled = (AppAdcCfg.channelSel & ADCCH_MIC1) ? true : false;

	// MIC Channel 2
	pChannelCfg = &ChannelCfgArr[1];
	pChannelCfg->channelSel = NORDIC_ADC_CHANNEL_2;
	pChannelCfg->gainSel = NRF_SAADC_GAIN1;
	pChannelCfg->bEnabled = (AppAdcCfg.channelSel & ADCCH_MIC2) ? true : false;

	// MIC Channel 3
	pChannelCfg = &ChannelCfgArr[2];
	pChannelCfg->channelSel = NORDIC_ADC_CHANNEL_3;
	pChannelCfg->gainSel = NRF_SAADC_GAIN1;
	pChannelCfg->bEnabled = (AppAdcCfg.channelSel & ADCCH_MIC3) ? true : false;

	// MIC Channel 4
	pChannelCfg = &ChannelCfgArr[3];
	pChannelCfg->channelSel = NORDIC_ADC_CHANNEL_4;
	pChannelCfg->gainSel = NRF_SAADC_GAIN1;
	pChannelCfg->bEnabled = (AppAdcCfg.channelSel & ADCCH_MIC4) ? true : false;

	// MIC Channel 5
	pChannelCfg = &ChannelCfgArr[4];
	pChannelCfg->channelSel = NORDIC_ADC_CHANNEL_5;
	pChannelCfg->gainSel = NRF_SAADC_GAIN1;
	pChannelCfg->bEnabled = (AppAdcCfg.channelSel & ADCCH_MIC5) ? true : false;

	// ECG Channel
	pChannelCfg = &ChannelCfgArr[5];
	pChannelCfg->channelSel = NORDIC_ADC_CHANNEL_6;
	pChannelCfg->gainSel = NRF_SAADC_GAIN1_6;
	pChannelCfg->bEnabled = (AppAdcCfg.channelSel & ADCCH_ECG) ? true : false;

	pNordicAdcCfg->pChannelCfgArr = ChannelCfgArr;

	ret = NordicAdcInit();
	if(ret != SUCCESS) return ret;

	return SUCCESS;

}

int32_t AppAdcRegCb(void (*pfn_cb)) {

	if(pfn_cb) {
		AppAdcCfg.DataIsr = pfn_cb;
	}
	else {
		LOG_ERR("NO DATA ISR PROVIDED");
		return FAILURE;
	}

	return SUCCESS;
}

// Module name is AppAdc
// Here, we'll call any initialization functions for:
	// Drivers
	// BLE
int32_t AppAdcInit(void *pBuff, uint32_t buffLen) {
	int32_t ret;

	ret = MicEcgAdcInit();
	if(ret != SUCCESS) return ret;

	/*
	if(pAppAdcCfg) {
		AppAdcCfg.DataIsr = pAppAdcCfg->DataIsr;
	}
	else {
		LOG_ERR("NO DATA ISR PROVIDED");
		return FAILURE;
	}
	*/

	return SUCCESS;
}

