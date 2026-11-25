/*
	KX132 accelerometer driver (I2C through nrfx TWI).
	Register definitions in kx132.h. Not enabled in the default build
	(CONFIG_KX132=n).
*/

#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>

#include <zephyr.h>		// Required for k_sleep (NCS SDK function)

#include <hal/nrf_gpio.h>
#include "kx132.h"
#include "nrfx_gpiote.h"	// Changed to nrfx 2022.06.02
#include "nrfx_twi.h"	// Changed to nrfx 2022.06.02
#include "error.h"	// Added 2022.06.16
//#include "boards.h"		// Commented out 2022.06.02
//#include "app_util_platform.h"	// Commented out 2022.06.16
//#include "app_error.h"	// Commented out 2022.06.02
//#include "nrf_gpio.h"		// Commented out 2022.06.16
// #include "nrf_delay.h" Removed for compatibility with NCS SDK
// #include "nrf_twi.h" // Commented out 2022.06.16



/* TWI instance ID. */
/*
#if NRFX_TWI0	// Found in prj.conf
#define TWI_INSTANCE_ID     0
#elif NRFX_TWI1	// Found in prj.conf
#define TWI_INSTANCE_ID     1
#endif
*/

#define TWI_INSTANCE_ID	0	// Manual Define for now

static bool m_acc_xfer_done;

// The following aren't defined in the nRF Connect SDK libraries
nrfx_err_t nrfx_twi_tx(nrfx_twi_t const * p_instance,
						 uint8_t            address,
						 uint8_t    const * p_data,
						 size_t             length,
						 bool               no_stop)
{
	nrfx_twi_xfer_desc_t xfer = NRFX_TWI_XFER_DESC_TX(address, (uint8_t*)p_data, length);
	return nrfx_twi_xfer(p_instance, &xfer, no_stop ? NRFX_TWI_FLAG_TX_NO_STOP : 0);
}

nrfx_err_t nrfx_twi_rx(nrfx_twi_t const * p_instance,
						 uint8_t            address,
						 uint8_t *          p_data,
						 size_t             length)
{
	nrfx_twi_xfer_desc_t xfer = NRFX_TWI_XFER_DESC_RX(address, p_data, length);
	return nrfx_twi_xfer(p_instance, &xfer, 0);
}


/* TWI instance. */
static const nrfx_twi_t m_twi_acc = NRFX_TWI_INSTANCE(TWI_INSTANCE_ID);  	// Changed to nrfx 2022.06.02

static void data_handler(uint8_t * response){
}

static void twi_acc_handler(nrfx_twi_evt_t const *p_event, void *p_context)  // Changed to nrfx 2022.06.02
{
	//Check the event to see what type of event occurred
	switch (p_event->type) {

		//If data transmission or receiving is finished
		case NRFX_TWI_EVT_DONE: {												// Changed to nrfx 2022.06.02
			if (p_event->xfer_desc.type == NRFX_TWI_XFER_RX) {					// Changed to nrfx 2022.06.02
				// data_handler(response);
			}
			m_acc_xfer_done = true;//Set the flag
			break;
		}
		
		default: {
		// do nothing
			break;
		}
	}
}

/**
 * @brief TWI initialization.
 */
static int32_t twi_acc_init(void)
{
	nrfx_err_t ret;

	// Associate the NRFX IRQ handler with the i2c0's hardware interrupt vector using the IRQCONNECT macro.
	
	IRQ_CONNECT(DT_IRQN(DT_NODELABEL(i2c0)),
							DT_IRQ(DT_NODELABEL(i2c0), priority),
							nrfx_isr, nrfx_twi_0_irq_handler, 0);


	const nrfx_twi_config_t twi_config = {									// Changed to nrfx 2022.06.02
		 .scl 					= NRF_GPIO_PIN_MAP(0,15),					// when calling nrfx_twi_init, .scl just needs to refer to the correct pin (need to be correct numbers)
		 .sda 					= NRF_GPIO_PIN_MAP(0,14),						// This call seems to work fine with NCS
		 .frequency 			= NRF_TWI_FREQ_100K,								// May need to change this
		 .interrupt_priority	= NRFX_TWI_DEFAULT_CONFIG_IRQ_PRIORITY,			// May need to change this
		 //.clear_bus_init     = false	// Not found in NCS nrfx
	};

	ret = nrfx_twi_init(&m_twi_acc, &twi_config, twi_acc_handler, NULL);	// Changed to nrfx 2022.06.02
	//APP_ERROR_CHECK(err_code);
	if(ret != NRFX_SUCCESS) return ret;

	nrfx_twi_enable(&m_twi_acc);												// Changed to nrfx 2022.06.02

	nrf_gpio_cfg_input(NRF_GPIO_PIN_MAP(0,14), NRF_GPIO_PIN_PULLUP);
	nrf_gpio_cfg_input(NRF_GPIO_PIN_MAP(0,15), NRF_GPIO_PIN_PULLUP);

	return SUCCESS;
}


static void gpio_init(void)
{
	int32_t ret;
	ret = nrfx_gpiote_init(NRFX_GPIOTE_DEFAULT_CONFIG_IRQ_PRIORITY);		// Changed to nrfx 2022.06.02
	//APP_ERROR_CHECK(err_code);
	
}


/* Accelerometer Methods*/
int32_t writeAccel(uint8_t *p_data, uint8_t cmdLength)
{
	nrfx_err_t ret;

	m_acc_xfer_done = false;

	// ret = nrfx_twi_tx(&m_twi_acc, kx132_ADDRESS, p_data, cmdLength, false);	// Changed to nrfx 2022.06.02
	// if(ret!= NRFX_SUCCESS) return FAILURE;

	// kx132_ADDRESS or kx132_ADDRESS_ALT
	nrfx_twi_xfer_desc_t xfer = NRFX_TWI_XFER_DESC_TX(kx132_ADDRESS, p_data, cmdLength);

	nrfx_twi_xfer(&m_twi_acc, &xfer, 0);

	//while (m_acc_xfer_done == false);

	k_msleep(CMD_DELAY);														// Changed to k_msleep 2022.06.02

	return SUCCESS;
}




int32_t readFillAccelArray(uint8_t reg, uint8_t cmdLength, uint8_t readBytes, uint8_t *p_data) {
	int32_t ret;

	m_acc_xfer_done = false;

	//ret = nrfx_twi_tx(&m_twi_acc, kx132_ADDRESS, &reg, cmdLength, false);	// Changed to nrfx 2022.06.02
	nrfx_twi_xfer_desc_t xfer_tx = NRFX_TWI_XFER_DESC_TX(kx132_ADDRESS, &reg, cmdLength);

	nrfx_twi_xfer(&m_twi_acc, &xfer_tx, 0);


	//if(ret != NRFX_SUCCESS) return FAILURE;

	//while (m_acc_xfer_done == false);

	//k_msleep(CMD_DELAY);														// Changed to k_msleep 2022.06.02
	k_usleep(250);	// Found emperically for nrf52840dk with breakout board

	m_acc_xfer_done = false;

	nrfx_twi_xfer_desc_t xfer_rx = NRFX_TWI_XFER_DESC_RX(kx132_ADDRESS, p_data, readBytes);

	nrfx_twi_xfer(&m_twi_acc, &xfer_rx, 0);

	//ret = nrfx_twi_rx(&m_twi_acc, kx132_ADDRESS, data, readBytes);		// Changed to nrfx 2022.06.02
	//if(ret != NRFX_SUCCESS) return FAILURE;

	//while (m_acc_xfer_done == false);
	k_msleep(CMD_DELAY);														// Changed to k_msleep 2022.06.02

	return SUCCESS;
}


/*
	Function to initialize the kx132
*/ 
int32_t accelInit(void) {
	int32_t ret;

	ret = twi_acc_init();
	if(ret != SUCCESS) return FAILURE;
	
	uint8_t buf[2] = {kx132_CNTL1, 0xE0};
	ret = writeAccel(buf, sizeof(buf));
	if(ret != SUCCESS) return FAILURE;

	uint8_t buf2[2] = {kx132_INS2, 0x10};
	ret = writeAccel(buf2, sizeof(buf2));
	if(ret != SUCCESS) return FAILURE;

	return SUCCESS;
}



/*
	A Function to read accelerometer's values from the internal registers of kx132
*/ 
int32_t readAccelSamples(int16_t * pACC_X, int16_t * pACC_Y, int16_t * pACC_Z)
{
	uint8_t accSampleLength = 6;			// 2 bytes for X, Y, and Z data each
	uint8_t accSamples[accSampleLength];

	uint8_t accelReg = kx132_XOUT_L;		// 0x08
	readFillAccelArray(accelReg, 1, accSampleLength, accSamples);

	*pACC_X = ((int16_t)(accSamples[1] << 8)) | (int16_t)accSamples[0];
	if(*pACC_X & 0x8000){ *pACC_X-=65536;}

	*pACC_Y = ((int16_t)(accSamples[3] << 8)) | (int16_t)accSamples[2];
	if(*pACC_Y & 0x8000){ *pACC_Y-=65536;}

	*pACC_Z = ((int16_t)(accSamples[5] << 8)) | (int16_t)accSamples[4];
	if(*pACC_Z & 0x8000){ *pACC_Z-=65536;}

	return SUCCESS;
}


