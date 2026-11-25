#include <stdbool.h>
#include <stdint.h>
#include <string.h> // for memcpy

#include <logging/log.h>

#include "error.h"

#include <zephyr.h> // for k_work

#include <nrfx_saadc.h>
#include "nrfx_timer.h"
#include "nrfx_ppi.h"

#include "nordic_adc.h"


LOG_MODULE_REGISTER(nordic_adc_interface);


/******************************************************************************/
/************************** ADC ******************************/
/******************************************************************************/

#define SAADC_BUF_SIZE			2400	// Num Samples, each 2 bytes. Make a multiple of the payload, 244
#define SAADC_BUF_COUNT			2
#define SAADC_VREF_MV 600

#ifndef NRFX_SAADC_CONFIG_IRQ_PRIORITY
#define NRFX_SAADC_CONFIG_IRQ_PRIORITY 7
#endif

NordicAdcCfg_t NordicAdcCfg = {
	.samplingFrequency = 4000,
	.oversamplingSel = NRF_SAADC_OVERSAMPLE_DISABLED,
	//.gainSel = NRF_SAADC_GAIN1,
	//.channelMask = NORDIC_ADC_CHANNEL_1,
	.pChannelCfgArr = NULL,
	.bUseDifferentialMode = false,
	.resolutionSel = NRF_SAADC_RESOLUTION_14BIT,
	.sampleCountThresh = SAADC_BUF_SIZE,
};

/**
This function is provided for upper controllers that want to change 
application parameters specially for user defined parameters.
*/
int32_t NordicAdcGetCfg(void *pCfg)
{
	if(pCfg) {
		*(NordicAdcCfg_t**)pCfg = &NordicAdcCfg;
    	return SUCCESS;
	}
	LOG_ERR("NORDIC ADC CFG MISSING");
	return FAILURE;
}

typedef struct nordic_irq_desc_t {
	struct k_work my_work;
	int16_t* p_buffer;
} nordic_irq_desc_t;

struct nordic_irq_desc_t nordic_irq_desc;


// Buffer size and multi-buffer config
static nrf_saadc_value_t samples[SAADC_BUF_COUNT][SAADC_BUF_SIZE];	// 16-bits
static const nrfx_timer_t m_sample_timer = NRFX_TIMER_INSTANCE(1);
static nrf_ppi_channel_t m_timer_saadc_ppi_channel;

// Simple function to provide an index to the next input buffer
// Will simply alernate between 0 and 1 when SAADC_BUF_COUNT is 2
static uint32_t next_free_buf_index(void)
{
	static uint32_t buffer_index = -1;
	buffer_index = (buffer_index + 1) % SAADC_BUF_COUNT;
	return buffer_index;
}
 
// for nrfx_timer_init()
static void timer_handler(nrf_timer_event_t event_type, void * p_context)
{
}

// for nrfx_saadc_advanced_mode_set()
static void event_handler(nrfx_saadc_evt_t const * p_event)
{
	int ret;
	switch (p_event->type)
	{
		// NRFX_SAADC_EVT_DONE: Event generated when the buffer is filled with samples.
		case NRFX_SAADC_EVT_DONE: {

			//LOG_INF("NRFX_SAADC_EVT_DONE\n");

			nordic_irq_desc.p_buffer = (int16_t *) p_event->data.done.p_buffer;

			// Should not attempt BLE TX within IRQ, so we use a k_work thread
			k_work_submit(&nordic_irq_desc.my_work);

			break;	// End NRFX_SAADC_EVT_DONE
		}

		// NRFX_SAADC_EVT_BUF_REQ: Event generated when the next buffer for continuous conversion is requested.
		case NRFX_SAADC_EVT_BUF_REQ:

			//LOG_INF("NRFX_SAADC_EVT_BUF_REQ\n");

			// Set up the next available buffer
			ret = nrfx_saadc_buffer_set(&samples[next_free_buf_index()][0], NordicAdcCfg.sampleCountThresh);
			if(ret != NRFX_SUCCESS) {
				LOG_ERR("Error setting saadc buffer\n");
			}
			break;
		
		default:
			LOG_INF("SAADC evt %d", p_event->type);
			break;
	}
}


static int timer_init(void)
{
	int ret;

	nrfx_timer_config_t timer_config = NRFX_TIMER_DEFAULT_CONFIG;
	/*
		NRFX_TIMER_DEFAULT_CONFIG has the value:
		{ \
		 .frequency = (nrf_timer_frequency_t)NRFX_TIMER_DEFAULT_CONFIG_FREQUENCY,\
		 .mode = (nrf_timer_mode_t)NRFX_TIMER_DEFAULT_CONFIG_MODE, \
		 .bit_width = (nrf_timer_bit_width_t)NRFX_TIMER_DEFAULT_CONFIG_BIT_WIDTH,\
		 .interrupt_priority = NRFX_TIMER_DEFAULT_CONFIG_IRQ_PRIORITY, \
		 .p_context = NULL \
		}
	*/

	// For accurate sampling rate, ensure that the clock frequency is evenly divisible by the target sample rate

	// Two clock sources are available for the timer: 16MHz and 1MHz
	// A divider is used to produce lower frequencies

	// Note: 1MHz is preferred over 16MHz for ower power consumption
	// For our application, the sampling rate is assumed to be 4kHz, making 1MHz a good match.

	timer_config.frequency = NRF_TIMER_FREQ_1MHz;
	/*
		Possible options:
		NRF_TIMER_FREQ_16MHz
		NRF_TIMER_FREQ_8MHz
		NRF_TIMER_FREQ_4MHz
		NRF_TIMER_FREQ_2MHz
		NRF_TIMER_FREQ_1MHz
		NRF_TIMER_FREQ_500kHz
		NRF_TIMER_FREQ_250kHz
		NRF_TIMER_FREQ_125kHz
		NRF_TIMER_FREQ_62500Hz
		NRF_TIMER_FREQ_31250Hz
	*/

	// Function for initializing the timer.
	ret = nrfx_timer_init(&m_sample_timer, &timer_config, timer_handler);
	if(ret != NRFX_SUCCESS) return ret;

	// Function for setting the timer channel in extended compare mode.
	// NRF_TIMER_CC_CHANNEL0 indicates that we're using the timer capture/compare channel 0
	// nrfx_timer_ms_to_ticks() is used as the compare value stored in CC[0]
	// NRF_TIMER_SHORT_COMPARE0_CLEAR_MASK shortcuts a COMPARE[0] event and CLEAR task
		// COMPARE[0] event occurs when the timer matches the CC[0] compare value

	// Set the timer to go off once every saadc_sampling_period
	uint32_t samplingPeriodUs = 1000000/NordicAdcCfg.samplingFrequency;
	nrfx_timer_extended_compare(&m_sample_timer,
								NRF_TIMER_CC_CHANNEL0,
								nrfx_timer_us_to_ticks(&m_sample_timer, samplingPeriodUs),
								NRF_TIMER_SHORT_COMPARE0_CLEAR_MASK,
								false);

	// Function for resuming the timer.
	nrfx_timer_resume(&m_sample_timer);

	return SUCCESS;
}

static int ppi_init(void)
{
	// Function for allocating a PPI channel.
		// This function allocates the first unused PPI channel.
	// Trigger task sample from timer
	int ret = nrfx_ppi_channel_alloc(&m_timer_saadc_ppi_channel);
	if(ret != NRFX_SUCCESS) return ret;

	// Function for assigning task and event endpoints to the PPI channel.
	/*
		nrfx_ppi_channel_assign() parameters are:

			channel: PPI channel to be assigned endpoints.
			eep: Event endpoint address.
			tep: Task endpoint address.

	*/
	// Event is set to NRF_TIMER_EVENT_COMPARE0, which here will occur once every saadc_sampling_period via nrfx_timer_extended_compare()
	// Task is set to NRF_SAADC_TASK_SAMPLE, which is used for sampling
	ret = nrfx_ppi_channel_assign(m_timer_saadc_ppi_channel, 
									   nrfx_timer_event_address_get(&m_sample_timer, NRF_TIMER_EVENT_COMPARE0),
									   nrf_saadc_task_address_get(NRF_SAADC, NRF_SAADC_TASK_SAMPLE));
	if(ret != NRFX_SUCCESS) return ret;

	// Function for enabling a PPI channel.
	ret = nrfx_ppi_channel_enable(m_timer_saadc_ppi_channel);
	if(ret != NRFX_SUCCESS) return ret;

	return SUCCESS;
}


static int adc_configure(void)
{
	int ret;

	uint8_t numChannels;

	// For Zephyr:
	IRQ_CONNECT(DT_IRQN(DT_NODELABEL(adc)),
			DT_IRQ(DT_NODELABEL(adc), priority),
			nrfx_saadc_irq_handler, NULL, 0);

	nrfx_saadc_adv_config_t saadc_adv_config = NRFX_SAADC_DEFAULT_ADV_CONFIG;

	/*
		NRFX_SAADC_DEFAULT_ADV_CONFIG has the value:
		{ \
		 .oversampling = NRF_SAADC_OVERSAMPLE_DISABLED, \
		 .burst = NRF_SAADC_BURST_DISABLED, \
		 .internal_timer_cc = 0, \
		 .start_on_end = false, \
		}
	*/

	saadc_adv_config.oversampling = NordicAdcCfg.oversamplingSel;;
	saadc_adv_config.burst = NRF_SAADC_BURST_ENABLED;

	saadc_adv_config.internal_timer_cc = 0;	// Internal timer capture and compare value.
	saadc_adv_config.start_on_end = true;	// Flag indicating if the START task is to be triggered on the END event.

	ret = nrfx_saadc_init(NRFX_SAADC_CONFIG_IRQ_PRIORITY);
	if(ret != NRFX_SUCCESS) return ret;

	// SAADC channel configuration structure
	static nrfx_saadc_channel_t channelCfgs[NORDIC_ADC_MAX_NUM_CHANNELS];	// Must be const

	uint8_t channelIdxMask, activeChannelCount;//, tempChannelMask, currNordicChannel;
	channelIdxMask = 0;
	activeChannelCount = 0;
	//tempChannelMask = NordicAdcCfg.channelMask;
	//currNordicChannel = 0;
	ChannelCfg_t *pCurrChannelCfg;

	for(int i=0; i<NORDIC_ADC_MAX_NUM_CHANNELS; i++) {

		pCurrChannelCfg = &NordicAdcCfg.pChannelCfgArr[i];

		if(!pCurrChannelCfg->bEnabled)
			continue;

		// 
		nrfx_saadc_channel_t currCfg;

		//uint8_t currNordicChannel = 0x1 << i;

		// Apply default config to each channel

			/*
			NRFX_SAADC_DEFAULT_CHANNEL_SE has the value:
			{ \
			 .channel_config = \
			 { \
			 .resistor_p = NRF_SAADC_RESISTOR_DISABLED, \
			 .resistor_n = NRF_SAADC_RESISTOR_DISABLED, \
			 .gain = NRF_SAADC_GAIN1_6, \
			 .reference = NRF_SAADC_REFERENCE_INTERNAL, \
			 .acq_time = NRF_SAADC_ACQTIME_10US, \
			 .mode = NRF_SAADC_MODE_SINGLE_ENDED, \
			 .burst = NRF_SAADC_BURST_DISABLED, \
			 }, \
			 .pin_p = (nrf_saadc_input_t)_pin_p, \
			 .pin_n = NRF_SAADC_INPUT_DISABLED, \
			 .channel_index = _index, \
			}
		*/

		//if(tempChannelMask & 0x1) {

		if(NordicAdcCfg.bUseDifferentialMode) {
			nrfx_saadc_channel_t tempConfig = NRFX_SAADC_DEFAULT_CHANNEL_DIFFERENTIAL(
				pCurrChannelCfg->channelSel,
				NORDIC_ADC_CHANNEL_REF,
				activeChannelCount);

			memcpy(&currCfg, &tempConfig, sizeof(currCfg));
		}

		else {
			nrfx_saadc_channel_t tempConfig = NRFX_SAADC_DEFAULT_CHANNEL_SE(
				pCurrChannelCfg->channelSel,
				activeChannelCount);
			memcpy(&currCfg, &tempConfig, sizeof(currCfg));
		}
		//}

		/*
		// Channel won't be used
		else {
			LOG_INF("CHANNEL %d DISABLED", i+1);
			continue;
		}
		*/

		// Replace some parameters in default config
		//currCfg.channel_config.reference = NRF_SAADC_REFERENCE_VDD4;          
		currCfg.channel_config.gain = pCurrChannelCfg->gainSel;
		currCfg.channel_config.acq_time = NRF_SAADC_ACQTIME_3US;		// 3us is fastest

		currCfg.channel_config.resistor_p = NRF_SAADC_RESISTOR_VDD1_2;	// For testing - bias an AC signal at VDD/2
		currCfg.channel_config.resistor_n = NRF_SAADC_RESISTOR_VDD1_2;	// For testing - bias the reference pin at VDD/2

		// Copy the current configuration to each configuration array
		memcpy(&channelCfgs[activeChannelCount], &currCfg, sizeof(currCfg));

		// Update channel mask
		channelIdxMask |= 0x1 << activeChannelCount;
		activeChannelCount++;

		switch(pCurrChannelCfg->channelSel) {
			case NORDIC_ADC_CHANNEL_1:
				LOG_INF("CHANNEL: 1");
				break;
			case NORDIC_ADC_CHANNEL_2:
				LOG_INF("CHANNEL: 2");
				break;
			case NORDIC_ADC_CHANNEL_3:
				LOG_INF("CHANNEL: 3");
				break;
			case NORDIC_ADC_CHANNEL_4:
				LOG_INF("CHANNEL: 4");
				break;
			case NORDIC_ADC_CHANNEL_5:
				LOG_INF("CHANNEL: 5");
				break;
			case NORDIC_ADC_CHANNEL_6:
				LOG_INF("CHANNEL: 6");
				break;
			default:
				LOG_INF("UNKNOWN CHANNEL");
		}

		switch(pCurrChannelCfg->gainSel) {
			case NRF_SAADC_GAIN1_6:
				LOG_INF("GAIN: 1/6");
				break;
			case NRF_SAADC_GAIN1_5:
				LOG_INF("GAIN: 1/5");
				break;
			case NRF_SAADC_GAIN1_4:
				LOG_INF("GAIN: 1/4");
				break;
			case NRF_SAADC_GAIN1_3:
				LOG_INF("GAIN: 1/3");
				break;
			case NRF_SAADC_GAIN1_2:
				LOG_INF("GAIN: 1/2");
				break;
			case NRF_SAADC_GAIN1:
				LOG_INF("GAIN: 1");
				break;
			case NRF_SAADC_GAIN2:
				LOG_INF("GAIN: 2");
				break;
			case NRF_SAADC_GAIN4:
				LOG_INF("GAIN: 4");
				break;
			default:
				LOG_INF("UNKNOWN GAIN");
		}


		/*
		// Shift at the end of each iteration
		tempChannelMask = tempChannelMask >> 1;		
		*/
	}

	/*
		nrfx_saadc_channels_config() has the parameters:
			p_channels: Pointer to the array of channel configuration structures.
			channel_count: Number of channels to be configured.

		Note: The values of the nrf_saadc_channel_config_t::burst fields in channel configurations are ignored.
				They will be overridden by the value suitable for the selected driver operation mode.

		Note: The desired mode (simple or advanced) must be set after the channels are configured.
	*/

	ret = nrfx_saadc_channels_config(channelCfgs, activeChannelCount);
	if(ret != NRFX_SUCCESS) {
		LOG_ERR("SAADC CHANNELS CONFIG ERROR");
		return FAILURE;
	}
	/*
		nrfx_saadc_advanced_mode_set() parameters are:
			channel_mask: Bitmask of channels to be used in the advanced mode.
			resolution: Resolution configuration.
			p_config: Pointer to the structure with the advanced mode configuration.
			event_handler: Event handler provided by the user. In case of providing NULL, the conversion will be performed in the blocking manner.
	*/

	ret = nrfx_saadc_advanced_mode_set(channelIdxMask,
											NordicAdcCfg.resolutionSel,
											&saadc_adv_config,
											event_handler);
	if(ret != NRFX_SUCCESS) {
		LOG_ERR("SAADC ADVANCED MODE SET ERROR");
		return FAILURE;
	}
											
	// Configure two buffers to ensure double buffering of samples, to avoid data loss when the sampling frequency is high
	// After calling next_free_buf_index for each buffer, we'll end up at the 0th buffer
	// First call
	ret = nrfx_saadc_buffer_set(&samples[next_free_buf_index()][0], NordicAdcCfg.sampleCountThresh);
	if(ret != NRFX_SUCCESS) {
		LOG_ERR("SAADC BUFFER SET ERROR");
		return FAILURE;
	}
	// Second Call
	ret = nrfx_saadc_buffer_set(&samples[next_free_buf_index()][0], NordicAdcCfg.sampleCountThresh);
	if(ret != NRFX_SUCCESS) {
		LOG_ERR("SAADC BUFFER SET ERROR");
		return FAILURE;
	}
	// Must call to "initiate sampling" before starting the external timer
	ret = nrfx_saadc_mode_trigger();
	if(ret != NRFX_SUCCESS) {
		LOG_ERR("SAADC MODE TRIGGER ERROR");
		return FAILURE;
	}

	return SUCCESS;
}

// To-do: Move workqueue to higher level, e.g., app_mic.

static void nordic_adc_work_cb(struct k_work *work) {
	
	if(NordicAdcCfg.SampleIsr) {
		NordicAdcCfg.SampleIsr();
	}

	else {
		LOG_ERR("NO ISR CALLBACK EXISTS");
	}
}

static int32_t work_init(void) {
	k_work_init(&nordic_irq_desc.my_work, nordic_adc_work_cb);

	return SUCCESS;
}

/**
 * @brief ADC driver APIs
 * @defgroup adc_interface ADC driver APIs
 * @ingroup io_interfaces
 * @{
 */

static int adc_gain_invert(uint8_t gain,
			int32_t *value)
{
	struct gain_desc {
		uint8_t mul;
		uint8_t div;
	};
	static const struct gain_desc gains[] = {
		[NRF_SAADC_GAIN1_6] = {.mul = 6, .div = 1},
		[NRF_SAADC_GAIN1_5] = {.mul = 5, .div = 1},
		[NRF_SAADC_GAIN1_4] = {.mul = 4, .div = 1},
		[NRF_SAADC_GAIN1_3] = {.mul = 3, .div = 1},
		[NRF_SAADC_GAIN1_2] = {.mul = 2, .div = 1},
		[NRF_SAADC_GAIN1] = {.mul = 1, .div = 1},
		[NRF_SAADC_GAIN2] = {.mul = 1, .div = 2},
		[NRF_SAADC_GAIN4] = {.mul = 1, .div = 4},
	};
	int rv = -EINVAL;

	if ((uint8_t)gain < ARRAY_SIZE(gains)) {
		const struct gain_desc *gdp = &gains[gain];

		if ((gdp->mul != 0) && (gdp->div != 0)) {
			*value = (gdp->mul * *value) / gdp->div;
			rv = 0;
		}
	}

	return rv;
}

/**
 * @brief Convert a raw ADC value to millivolts.
 *
 * @param ref_mv the reference voltage used for the measurement, in
 * millivolts.  This may be from adc_ref_internal() or a known
 * external reference.
 *
 * @param gain the ADC gain configuration used to sample the input
 *
 * @param resolution the number of bits in the absolute value of the
 * sample.  For differential sampling this may be one less than the
 * resolution in struct adc_sequence.
 *
 * @param valp pointer to the raw measurement value on input, and the
 * corresponding millivolt value on successful conversion.  If
 * conversion fails the stored value is left unchanged.
 *
 * @retval 0 on successful conversion
 * @retval -EINVAL if the gain is not reversible
 */
static inline int adc_raw_to_millivolts(int32_t ref_mv,
					uint8_t gain,
					uint8_t resolution,
					int32_t *valp)
{
	int32_t adc_mv = *valp * ref_mv;
	int ret = adc_gain_invert(gain, &adc_mv);

	if (ret == 0) {
		*valp = (adc_mv >> resolution);
	}

	return ret;
}

static uint8_t GetResolutionBits(void) {

	uint8_t resolutionSel, resolutionBits;
	bool bUseDifferentialMode;
	
	resolutionSel = NordicAdcCfg.resolutionSel;
	resolutionBits = 8;

	bUseDifferentialMode = NordicAdcCfg.bUseDifferentialMode;

	switch(resolutionSel) {

		case NRF_SAADC_RESOLUTION_8BIT:
			resolutionBits = 8;
			break;

		case NRF_SAADC_RESOLUTION_10BIT:
			resolutionBits = 10;
			break;

		case NRF_SAADC_RESOLUTION_12BIT:
			resolutionBits = 12;
			break;

		case NRF_SAADC_RESOLUTION_14BIT:
			resolutionBits = 14;
			break;

		default:
			resolutionBits = 8;
	}

	// If sampling in differential mode, the MSB is used as a sign-bit
	if(bUseDifferentialMode) {
		resolutionBits--;
	}

	return resolutionBits;
}

// Transfer Data into the given buffer
int32_t NordicAdcDataRead(void *pBuffer, uint32_t *pCount) {

	uint32_t bytesToRead;
// As of 2024-02-22, we're no longer checking threshold here
// This should be checked at init
/*
	if(*pCount < NordicAdcCfg.sampleCountThresh) {
		LOG_ERR("PROVIDED BUFFER TOO SMALL");
		return FAILURE;
	}
*/
	bytesToRead = NordicAdcCfg.sampleCountThresh * sizeof(samples[0][0]);
	memcpy(pBuffer, nordic_irq_desc.p_buffer, bytesToRead);

	*pCount = NordicAdcCfg.sampleCountThresh;

	return SUCCESS;
}

/* Depending on the data type, do appropriate data pre-process before return back to controller */
int32_t NordicAdcDataProcess(int16_t* const pData, uint32_t *pDataCount, uint8_t gainSel) {

	uint32_t i, srcDataCount, destDataCount;
	int32_t processedData_i32;

	srcDataCount = *pDataCount;

	int16_t* pProcessedData = (int16_t *) pData;

	// Convert all raw measurements in buffer
	for(i=0; i<srcDataCount; i++) {

		processedData_i32 = pData[i];	// Don't typecast or else sign-extension errors can occur
		adc_raw_to_millivolts(SAADC_VREF_MV,
							gainSel,
							GetResolutionBits(),
							&processedData_i32);
		pProcessedData[i] = (int16_t) processedData_i32;
	}

	// No size conversions were made, but we keep this here to maintain pattern
	destDataCount = srcDataCount;

	*pDataCount = destDataCount;

	return SUCCESS;
}

int32_t NordicAdcInit(void) {

	LOG_INF("nrfx_saadc_api2 simple SAADC Continuous Sampling Example using timer and PPI.\n");	

	int32_t ret;

	ret = adc_configure();
	if(ret != SUCCESS) {
		LOG_ERR("ADC Config Failure, ((err %d)\n", ret);
		return FAILURE;
	}

	ret = work_init();
	if(ret != SUCCESS) {
		LOG_ERR("Work Init Failure, ((err %d)\n", ret);
		return FAILURE;
	}

	ret = ppi_init();
	if(ret != SUCCESS) {
		LOG_ERR("PPI Init Failure, ((err %d)\n", ret);
		return FAILURE;
	}

	ret = timer_init();
	if(ret != SUCCESS) {
		LOG_ERR("Timer Init Failure, ((err %d)\n", ret);
		return FAILURE;
	}

	return SUCCESS;
}