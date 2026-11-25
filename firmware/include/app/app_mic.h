#ifndef APP_MIC_H_
#define APP_MIC_H_

/******************************************************************************/
/***************************** Include Files **********************************/
/******************************************************************************/
#include <nrfx_saadc.h>

/******************************************************************************/
/********************** Macros and Constants Definitions **********************/
/******************************************************************************/
#define ADCCH_MIC1	0x01
#define ADCCH_MIC2	0x02
#define ADCCH_MIC3	0x04
#define ADCCH_MIC4	0x08
#define ADCCH_MIC5	0x10
#define ADCCH_ECG	0x20

#define ADCDATA_LEN 2400 * NORDIC_ADC_SAMPLE_SIZE // Bytes; the sample count is divisible by 240 for whole BLE payloads with any channel count
// 2400 samples (4800 bytes) / 4 channels = 600 samples (1200 bytes) per channel, every TX
// 2400 samples / 3 channels = 800 samples per channel, every TX
// 2400 samples / 2 channels = 1200 samples per channel, every TX
// 2400 samples / 1 channel  = 2400 samples per channel, every TX

// Show result control
#define SHOW_RESULT_LIMIT 1

/******************************************************************************/
/***************************** Exported Types *********************************/
/******************************************************************************/
typedef struct {
	void (*DataIsr)(void);
	uint32_t channelSel;
	uint32_t dataCount;
	uint8_t *pBuff;
} AppAdcCfg_t;

/******************************************************************************/
/*************************** Exported Functions *******************************/
/******************************************************************************/
/**
 * @brief Displays a limited number of microphone samples.
 * 
 * @param pData Pointer to an array of microphone sample data.
 * @param dataCount The total number of microphone samples available.
 */
void ShowResultMic(uint16_t *pData, uint32_t dataCount);

/**
 * @brief Displays a limited number of ECG samples from the provided data.
 * 
 * @param pData Pointer to the array containing ECG data samples.
 * @param dataCount Total number of ECG data samples in the array.
 */
void ShowResultEcg(uint16_t *pData, uint32_t dataCount);

/**
 * @brief Initializes the ADC for the microphone application.
 * 
 * @param pBuff Pointer to the buffer (currently unused).
 * @param buffLen Length of the buffer (currently unused).
 * 
 * @return int32_t Returns SUCCESS (0) if initialization is successful, 
 * or an error code if initialization fails.
 */
int32_t AppAdcInit(void *pBuff, uint32_t buffLen);


// int32_t AppAdcISR(void *pBuff, uint32_t *pCount);

/**
 * @brief Registers a callback function for ADC data ISR.
 * 
 * @param pfn_cb Pointer to the callback function to be registered.
 * 
 * @return int32_t Returns SUCCESS if the callback is successfully registered, 
 * otherwise returns FAILURE.
 */
int32_t AppAdcRegCb(void (*pfn_cb));

/**
 * @brief Retrieves ADC data based on the selected channels.
 * 
 * @param pChannelSel Pointer to the channel selection mask. This mask indicates 
 *                    which channels the user is interested in. The function 
 *                    updates this mask to reflect the intersection of the 
 *                    selected and available channels.
 * @param pBuff Pointer to the destination buffer where the ADC data will be copied.
 * @param pCount Pointer to a variable that will be updated with the number of
 *               samples (of all selected channels) copied to the destination buffer.
 * 
 * @return int32_t Returns SUCCESS (0) if the operation is successful, otherwise 
 *                 returns FAILURE (non-zero).
 */
int32_t AppAdcDataGet(uint32_t *pChannelSel, void *pBuff, uint32_t *pCount);

/**
 * @brief Fetches ADC data and processes it.
 * 
 * @return int32_t Returns SUCCESS if the operation is successful, otherwise returns an error code.
 */

int32_t AppAdcDataFetch(void);

/**
 * @brief Retrieves the ADC configuration.
 * 
 * @param pCfg Pointer to a location where the ADC configuration address will be stored.
 * @return int32_t Returns SUCCESS if the configuration is successfully retrieved, otherwise returns FAILURE.
 */
int32_t AppAdcGetCfg(void *pCfg);

#endif /* APP_MIC_H_ */