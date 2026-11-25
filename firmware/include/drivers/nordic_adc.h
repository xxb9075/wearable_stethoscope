#ifndef NORDIC_ADC_H_
#define NORDIC_ADC_H_

#include <nrfx_saadc.h>

// SAADC channel mapping (apnea_v2 board):
// Mic 1 -> AIN5, Mic 2 -> AIN4, Mic 3 -> AIN6, Mic 4 -> AIN7
// Channels 5 and 6 (AIN3, AIN2) are spare.
// AIN0 is the negative input in differential mode (NORDIC_ADC_CHANNEL_REF).

#define NORDIC_ADC_CHANNEL_1	NRF_SAADC_INPUT_AIN5
#define NORDIC_ADC_CHANNEL_2	NRF_SAADC_INPUT_AIN4
#define NORDIC_ADC_CHANNEL_3	NRF_SAADC_INPUT_AIN6
#define NORDIC_ADC_CHANNEL_4	NRF_SAADC_INPUT_AIN7
#define NORDIC_ADC_CHANNEL_5	NRF_SAADC_INPUT_AIN3
#define NORDIC_ADC_CHANNEL_6	NRF_SAADC_INPUT_AIN2
#define NORDIC_ADC_CHANNEL_REF	NRF_SAADC_INPUT_AIN0

#define NORDIC_ADC_MAX_NUM_CHANNELS	6

#define NORDIC_ADC_SAMPLE_SIZE 2	// Bytes

int32_t NordicAdcGetCfg(void *pCfg);
int32_t NordicAdcInit(void);
int32_t NordicAdcDataProcess(int16_t* const pData, uint32_t *pDataCount, uint8_t gainSel);
int32_t NordicAdcDataRead(void *pBuffer, uint32_t *pCount);

typedef struct {
	uint8_t gainSel;
	uint8_t channelSel;
	bool bEnabled;
} ChannelCfg_t;

typedef struct {
	uint32_t samplingFrequency;
	uint32_t oversamplingSel;
	ChannelCfg_t *pChannelCfgArr;
	bool bUseDifferentialMode;
	uint8_t resolutionSel;
	uint32_t sampleCountThresh;
	void (*SampleIsr)(void);
} NordicAdcCfg_t;

#endif // NORDIC_ADC_H_