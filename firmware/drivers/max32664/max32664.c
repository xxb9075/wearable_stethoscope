/*
	MAX32664 biometric sensor hub driver (heart rate and SpO2 from the MAX30101),
	as a Zephyr sensor device.

	The sensor hub communication (family, index and write bytes) and the
	register definitions are ported from the SparkFun Bio Sensor Hub Arduino
	Library (SparkFun Electronics, 2019), which carries the notice:
	"This code is public domain but you buy me a beer if you use this and we
	meet someday (Beerware license)."
*/

/******************************************************************************/
/****************************** Device Tree ***********************************/
/******************************************************************************/
#define DT_DRV_COMPAT max_max32664

/******************************************************************************/
/***************************** Include Files **********************************/
/******************************************************************************/
#include <logging/log.h>
#include "max32664.h"
// Logging support
LOG_MODULE_REGISTER(MAX32664, CONFIG_SENSOR_LOG_LEVEL);

/******************************************************************************/
/**************************** Global Variables ********************************/
/******************************************************************************/
uint8_t _resetPin;
uint8_t _mfioPin;
uint8_t _address;
uint32_t _writeCoefArr[3];
uint8_t _userSelectedMode;
uint8_t _sampleRate = 100;
uint8_t bpmArr[MAXFAST_ARRAY_SIZE];
uint8_t bpmArrTwo[MAXFAST_ARRAY_SIZE + MAXFAST_EXTENDED_DATA];
uint8_t senArr[MAX30101_LED_ARRAY];
uint8_t bpmSenArr[MAXFAST_ARRAY_SIZE + MAX30101_LED_ARRAY];
uint8_t bpmSenArrTwo[MAXFAST_ARRAY_SIZE + MAXFAST_EXTENDED_DATA + MAX30101_LED_ARRAY];
uint8_t fullFrameArr[MAXFAST_ALGOSENSOR_DATA_SIZE];
uint8_t fullFrameArrExtend[MAXFAST_ALGOSENSOR_DATA_SIZE + MAXFAST_EXTENDED_DATA];
static uint16_t _i2c_addr;
static struct device *_i2c_device;
static struct max32664_data max32664_data;
struct bioData _bioData;

/******************************************************************************/
/************************ Function Implementations ****************************/
/******************************************************************************/
// ---------------------writeByte-----------------------
// This function uses the given family, index, write byte, (and a 16 bit integer) as paramters to communicate with the MAX32664 which in turn communicates with downward sensors.
// First a write to the MCU indicating what you want to do, a delay, and then a read to confirm positive transmission.
uint8_t writeByte4(uint8_t _familyByte, uint8_t _indexByte, uint8_t _writeByte, uint16_t _writeVal)
{
    uint8_t tmpBuf[5];
    tmpBuf[0] = _familyByte;
    tmpBuf[1] = _indexByte;
    tmpBuf[2] = _writeByte;
    tmpBuf[3] = _writeVal >> 8;
    tmpBuf[4] = _writeVal;

    i2c_write(_i2c_device, tmpBuf, 5, _i2c_addr);
    k_msleep(CMD_DELAY);
    i2c_read(_i2c_device, tmpBuf, 1, _i2c_addr);
    uint8_t statusByte = tmpBuf[0];

    return statusByte;
}

uint8_t writeByte3(uint8_t _familyByte, uint8_t _indexByte, uint8_t _writeByte)
{
    uint8_t tmpBuf[3];
    tmpBuf[0] = _familyByte;
    tmpBuf[1] = _indexByte;
    tmpBuf[2] = _writeByte;

    i2c_write(_i2c_device, tmpBuf, 3, _i2c_addr);
    k_msleep(CMD_DELAY);
    i2c_read(_i2c_device, tmpBuf, 1, _i2c_addr);
    uint8_t statusByte = tmpBuf[0];

    return statusByte;
}

//------------------readByte--------------
// It starts a request by writing the family byte, an index byte, (and a write byte),
// then delays 6 microseconds, during which the MAX32664 retrieves the requested information.
// An I2C request is then issued, and the information is read.
uint8_t readByte2(uint8_t _familyByte, uint8_t _indexByte )
{
    uint8_t tmpBuf[2];
    tmpBuf[0] = _familyByte;
    tmpBuf[1] = _indexByte;

    i2c_write(_i2c_device, tmpBuf, 2, _i2c_addr);
    k_msleep(CMD_DELAY);
    i2c_read(_i2c_device, tmpBuf, 2,  _i2c_addr);
    uint8_t statusByte = tmpBuf[0];
    uint8_t returnByte = tmpBuf[1];
    // Status byte 0x00 means success
    if(statusByte) return statusByte; // Return the error, see: READ_STATUS_BYTE_VALUE 
    return returnByte; // If good then return the actual byte. 
}

uint8_t readByte3(uint8_t _familyByte, uint8_t _indexByte, uint8_t _writeByte)
{
    uint8_t tmpBuf[3];
    tmpBuf[0] = _familyByte;
    tmpBuf[1] = _indexByte;
    tmpBuf[2] = _writeByte;

    i2c_write(_i2c_device, tmpBuf, 3, _i2c_addr);
    k_msleep(CMD_DELAY);
    i2c_read(_i2c_device, tmpBuf, 2,  _i2c_addr);
    uint8_t statusByte = tmpBuf[0];
    uint8_t returnByte = tmpBuf[1];
    if( statusByte ) return statusByte;

    return returnByte; 
}

uint8_t readByte4(uint8_t _familyByte, uint8_t _indexByte, uint8_t _writeByte)
{
    uint8_t tmpBuf[4];
    tmpBuf[0] = _familyByte;
    tmpBuf[1] = _indexByte;
    tmpBuf[2] = _writeByte;
    tmpBuf[3] = 0x00;

    i2c_write(_i2c_device, tmpBuf, 3, _i2c_addr);
    k_msleep(CMD_DELAY);
    i2c_read(_i2c_device, tmpBuf, 4,  _i2c_addr);
    uint8_t statusByte = tmpBuf[0];
    uint8_t Major = tmpBuf[1];
    uint8_t Minor = tmpBuf[2];
    uint8_t Revision = tmpBuf[3];
    if( statusByte ) return statusByte;

    LOG_INF("sensor_hub_major=0x%02x",Major);
    LOG_INF("sensor_hub_minor=0x%02x",Minor);
    LOG_INF("sensor_hub_revision=0x%02x",Revision);

    return Major; 
}

//-------------readArray-------------
uint8_t readFillArray(uint8_t _familyByte, uint8_t _indexByte, uint8_t _numOfReads, uint8_t *array )
{
    uint8_t tmpBuf[2];
    tmpBuf[0] = _familyByte;
    tmpBuf[1] = _indexByte;

    i2c_write(_i2c_device, tmpBuf, 2, _i2c_addr);
    k_msleep(CMD_DELAY);
    i2c_read(_i2c_device, array, _numOfReads,  _i2c_addr);      // Length passed in has considered the status byte
    uint8_t statusByte = array[0];

    return statusByte;
}

//-------enableByte--------
uint8_t enableWrite(uint8_t _familyByte, uint8_t _indexByte, uint8_t _enableByte)
{
    uint8_t tmpBuf[3];
    tmpBuf[0] = _familyByte;
    tmpBuf[1] = _indexByte;
    tmpBuf[2] = _enableByte;

    i2c_write(_i2c_device, tmpBuf, 3, _i2c_addr);
    k_msleep(ENABLE_CMD_DELAY);
    i2c_read(_i2c_device, tmpBuf, 1,  _i2c_addr);
    uint8_t statusByte = tmpBuf[0];

    return statusByte;
}
#ifdef ACC_ENABLE
uint8_t enableWriteAcc(uint8_t _familyByte, uint8_t _indexByte, uint8_t _enableByte, uint8_t _enableByte2)
{
    uint8_t tmpBuf[4];
    tmpBuf[0] = _familyByte;
    tmpBuf[1] = _indexByte;
    tmpBuf[2] = _enableByte;
    tmpBuf[3] = _enableByte2;

    i2c_write(_i2c_device, tmpBuf, 4, _i2c_addr);
    k_msleep(ENABLE_CMD_DELAY);
    i2c_read(_i2c_device, tmpBuf, 1,  _i2c_addr);
    uint8_t statusByte = tmpBuf[0];

    return statusByte;
}
#endif // ACC_ENABLE

// Read the number of algorithm samples from a specific configuration register of a sensor
uint8_t readAlgoSamples()
{
    uint8_t samples = readByte3(READ_ALGORITHM_CONFIG, READ_AGC_NUM_SAMPLES, READ_AGC_NUM_SAMPLES_ID );
    return samples;
}

// A control function for a fast algorithm in a sensor
uint8_t maximFastAlgoControl(uint8_t mode)
{
    if(mode < 1 || mode > 3)
    return INCORR_PARAM;

    uint8_t statusByte = enableWrite(ENABLE_ALGORITHM, ENABLE_WHRM_ALGO, mode);
    if (statusByte != SUCCESS)
    return statusByte;
    else
    return SUCCESS;
}

// This function enables the MAX30101
uint8_t max30101Control(uint8_t senSwitch) 
{
    if(senSwitch < 0 || senSwitch > 1)
    return INCORR_PARAM;

    uint8_t statusByte = enableWrite(ENABLE_SENSOR, ENABLE_MAX30101, senSwitch);
    if( statusByte != SUCCESS )
    return statusByte;
    else
    return SUCCESS;
}

//
uint8_t agcAlgoControl(uint8_t enable) 
{
    if( enable != 0 && enable != 1)
    return INCORR_PARAM;

    uint8_t statusByte = enableWrite(ENABLE_ALGORITHM, ENABLE_AGC_ALGO, enable);
    if (statusByte != SUCCESS)
    return statusByte;
    else
    return SUCCESS;
}

#ifdef ACC_ENABLE
uint8_t accelControl(uint8_t enable1, uint8_t enable2) 
// This function enables the sensor hub accelerometer, not externally hosted
{
    if(enable1 < 0 || enable1 > 1 || enable2 < 0 || enable2 > 1)
    return INCORR_PARAM;

    uint8_t statusByte = enableWriteAcc(ENABLE_SENSOR, ENABLE_ACCELEROMETER, enable1, enable2);
    if( statusByte != SUCCESS )
    return statusByte;
    else
    return SUCCESS;
}
#endif

// Set the FIFO interrupt threshold on a sensor device
uint8_t setFifoThreshold(uint8_t intThresh) 
{
    uint8_t statusByte = writeByte3(OUTPUT_MODE, WRITE_SET_THRESHOLD, intThresh);
    if( statusByte != SUCCESS)
    return statusByte;
    else
    return SUCCESS;
}

// Configure the output mode of a sensor device
// Control the format and type of data that the sensor sends to the host processor.
uint8_t setOutputMode(uint8_t outputType) 
{
    if ( outputType > SENSOR_ALGO_COUNTER ) 
    return INCORR_PARAM;

    uint8_t statusByte = writeByte3(OUTPUT_MODE, SET_FORMAT, outputType);
    if( statusByte != SUCCESS)
    return statusByte;
    else
    return SUCCESS;
}

// Configure the sensor for BPM
/**
 * @brief Configures the BPM (Beats Per Minute) mode.
 *
 * @param mode The BPM mode to be configured. Valid values are MODE_ONE and MODE_TWO.
 * Note: since the mode is used by maximFastAlgoControl(mode), we assume this mode is for "Algorithm mode" instead of "Output mode".
 * @return uint8_t Returns SUCCESS if the configuration is successful. Otherwise, it returns an error code.
 */
uint8_t configBpm(uint8_t mode)
{
    uint8_t statusChauf = 0;
    if (mode != MODE_ONE && mode != MODE_TWO)
        return INCORR_PARAM;

    // Set the output mode to output sensor data (including ACC) + algorithm data
    #if defined(ACC_ENABLE)
    // Changed output mode to output sensor data (including ACC) + algorithm data: AA 10 00 03
    statusChauf = setOutputMode(SENSOR_AND_ALGORITHM); 
    if (statusChauf != SUCCESS)
        return statusChauf;
    #else
    statusChauf = setOutputMode(ALGO_DATA); 
    if (statusChauf != SUCCESS)
        return statusChauf;
    #endif 

    // FIFO threshold of 15 samples (command AA 10 01 0F). MFIO is an output here,
    // so the threshold interrupt is not used; the FIFO is polled.
    // statusChauf = setFifoThreshold(0x01); 
    statusChauf = setFifoThreshold(0x0F); 
    if (statusChauf != SUCCESS)
        return statusChauf;

    // Enable the AGC algorithm: AA 52 00 01
    statusChauf = agcAlgoControl(ENABLE); 
    if (statusChauf != SUCCESS)
        return statusChauf;

    #ifdef ACC_ENABLE
    // Enable the acc with sensor hub side (not the host side): AA 44 04* 01 00
    // 0x00, 0x00: Disable sensor hub accelerometer
    // 0x00, 0x01: Disable external host accelerometer
    // 0x01, 0x00: Enable sensor hub accelerometer
    // 0x01, 0x01: Enable external host accelerometer
    LOG_INF("Config KX132");
    statusChauf = accelControl(0x01, 0x00);
    // LOG_INF("statusChauf=0x%02x", statusChauf);
    // statusChauf = accelControl(0x00, 0x01);
    // LOG_INF("statusChauf=0x%02x", statusChauf);
    // statusChauf = accelControl(0x01, 0x00);
    // LOG_INF("statusChauf=0x%02x", statusChauf);
    if (statusChauf != SUCCESS)
        return statusChauf;
    #endif

    // Enable the MAX30101: AA 44 03 01
    LOG_INF("Config MAX30101");
    statusChauf = max30101Control(ENABLE);
    // LOG_INF("statusChauf=0x%02x", statusChauf);
    if (statusChauf != SUCCESS)
        return statusChauf;

    // Enable the HR/SpO2 algorithm. The format of the samples is shown in Application note Table 9.
    // 0x01: Enable Mode 1 Report
    // 0x02: Enable Mode 2, Extended Report
    statusChauf = maximFastAlgoControl(mode);
    if (statusChauf != SUCCESS)
        return statusChauf;

    _userSelectedMode = mode;
    _sampleRate = readAlgoSamples();
    k_msleep(1000);

    return SUCCESS;
}

// Read the status of the sensor hub
uint8_t readSensorHubStatus(){
    uint8_t status = readByte2(0x00, 0x00); 
    return status; 
}

// Read the number of samples currently available in the FIFO buffer of the sensor hub
uint8_t numSamplesOutFifo() 
{
    uint8_t sampAvail = readByte2(READ_DATA_OUTPUT, NUM_SAMPLES);
    return sampAvail;
}

// Read biometric data from a sensor hub and return it in a struct bioData format
struct bioData readBpm()
{
#ifdef ACC_ENABLE
    struct bioData libBpm = {0};
    uint8_t statusChauf = readSensorHubStatus();
    LOG_INF("statusChauf=0x%02x", statusChauf); 
    statusChauf &= 0x01; 

    // if (statusChauf == 1){ 
    //     libBpm.heartRate = 0;
    //     libBpm.confidence = 0;
    //     libBpm.oxygen = 0;
    //     LOG_INF("communication error");
    //     return libBpm;
    // }
    if (statusChauf != 0){ 
        libBpm.heartRate = 0;
        libBpm.confidence = 0;
        libBpm.oxygen = 0;
        LOG_ERR("communication error");
        return libBpm;
    }

    uint8_t sample_in_fifo = numSamplesOutFifo();
    LOG_INF("Number of samples in the queue: %u", sample_in_fifo);

    // Normal report mode
    if (_userSelectedMode == MODE_ONE) {
        // The MCU actively send I2C request to the sensor hub to get the data from the FIFO buffer, no interrupt involved, so MFIO pin is not configured as input.
        readFillArray(READ_DATA_OUTPUT, READ_DATA, MAXFAST_ALGOSENSOR_DATA_SIZE, fullFrameArr);
        if(fullFrameArr[0] != 0x00) {
            LOG_INF("I2C communication error");
            return libBpm;
        }
        // Print out each byte of fullFrameArr
        for (int i = 0; i < MAXFAST_ALGOSENSOR_DATA_SIZE; i++) {
            LOG_INF("fullFrameArr[%d]=0x%02x", i, fullFrameArr[i]);
        }

        libBpm.irLed = (uint32_t)fullFrameArr[1] << 16;
        libBpm.irLed = (uint32_t)fullFrameArr[2] << 8;
        libBpm.irLed |= (fullFrameArr[3]);
        libBpm.redLed = (uint32_t)fullFrameArr[4] << 16;
        libBpm.redLed = (uint32_t)fullFrameArr[5] << 8;
        libBpm.redLed |= (fullFrameArr[6]);

        libBpm.xVal = (int16_t)fullFrameArr[13] << 8;
        libBpm.xVal |= fullFrameArr[14];
        libBpm.yVal = (int16_t)fullFrameArr[15] << 8;
        libBpm.yVal |= fullFrameArr[16];
        libBpm.zVal = (int16_t)fullFrameArr[17] << 8;
        libBpm.zVal |= fullFrameArr[18];

        libBpm.heartRate = (uint16_t)fullFrameArr[19] << 8;
        libBpm.heartRate |= (fullFrameArr[20]);

        libBpm.confidence = fullFrameArr[21];

        libBpm.oxygen = (uint16_t)fullFrameArr[22] << 8;
        libBpm.oxygen |= fullFrameArr[23];

        libBpm.status = fullFrameArr[24];
        return libBpm;
    }
    else if (_userSelectedMode == MODE_TWO) {
        readFillArray(READ_DATA_OUTPUT, READ_DATA, MAXFAST_ALGOSENSOR_DATA_SIZE + MAXFAST_EXTENDED_DATA, fullFrameArrExtend);

        libBpm.irLed = (uint32_t)fullFrameArrExtend[1] << 16;
        libBpm.irLed = (uint32_t)fullFrameArrExtend[2] << 8;
        libBpm.irLed |= (fullFrameArrExtend[3]);
        libBpm.redLed = (uint32_t)fullFrameArrExtend[4] << 16;
        libBpm.redLed = (uint32_t)fullFrameArrExtend[5] << 8;
        libBpm.redLed |= (fullFrameArrExtend[6]);

        libBpm.xVal = (int16_t)fullFrameArrExtend[13] << 8;
        libBpm.xVal |= fullFrameArrExtend[14];
        libBpm.yVal = (int16_t)fullFrameArrExtend[15] << 8;
        libBpm.yVal |= fullFrameArrExtend[16];
        libBpm.zVal = (int16_t)fullFrameArrExtend[17] << 8;
        libBpm.zVal |= fullFrameArrExtend[18];

        libBpm.heartRate = (uint16_t)fullFrameArrExtend[19] << 8;
        libBpm.heartRate |= (fullFrameArrExtend[20]);

        libBpm.confidence = fullFrameArrExtend[21];

        libBpm.oxygen = (uint16_t)fullFrameArrExtend[22] << 8;
        libBpm.oxygen |= fullFrameArrExtend[23];

        libBpm.status = fullFrameArrExtend[24];
        return libBpm;
    }
    else {
        libBpm.heartRate  = 0;
        libBpm.confidence = 0;
        libBpm.oxygen = 0;
        return libBpm;
    }
#else
    struct bioData libBpm = {0};
    uint8_t statusChauf = readSensorHubStatus();
    LOG_INF("statusChauf=0x%02x", statusChauf); 
    statusChauf &= 0x01; 

    // if (statusChauf == 1){ 
    //     libBpm.heartRate = 0;
    //     libBpm.confidence = 0;
    //     libBpm.oxygen = 0;
    //     LOG_INF("communication error");
    //     return libBpm;
    // }
    if (statusChauf != 0){ 
        libBpm.heartRate = 0;
        libBpm.confidence = 0;
        libBpm.oxygen = 0;
        LOG_ERR("communication error");
        return libBpm;
    }

    uint8_t sample_in_fifo = numSamplesOutFifo();
    LOG_INF("Number of samples in the queue: %u", sample_in_fifo);

    if (_userSelectedMode == MODE_ONE) {
        // The MCU actively send I2C request to the sensor hub to get the data from the FIFO buffer, no interrupt involved, so MFIO pin is not configured as input.
        readFillArray(READ_DATA_OUTPUT, READ_DATA, MAXFAST_ARRAY_SIZE, bpmArr);
        if(bpmArr[0] != 0x00) {
            LOG_INF("I2C communication error");
            return libBpm;
        }
        // Print out each byte of bpmArr
        for (int i = 0; i < MAXFAST_ARRAY_SIZE; i++) {
            LOG_INF("bpmArr[%d]=0x%02x", i, bpmArr[i]);
        }

        libBpm.heartRate = (uint16_t)bpmArr[1] << 8;
        libBpm.heartRate |= (bpmArr[2]);
        // libBpm.heartRate /= 10;

        libBpm.confidence = bpmArr[3];

        libBpm.oxygen = (uint16_t)bpmArr[4] << 8;
        libBpm.oxygen |= bpmArr[5];
        // libBpm.oxygen /= 10;

        libBpm.status = bpmArr[6];

        #ifdef ACC_ENABLE
        LOG_INF("xVal=%d",  libBpm.xVal);
        LOG_INF("yVal=%d",  libBpm.yVal);
        LOG_INF("zVal=%d",  libBpm.zVal);
        #endif // ACC_ENABLE
        LOG_INF("10xbpm=%d",  libBpm.heartRate);
        LOG_INF("confidence=%d",  libBpm.confidence);
        LOG_INF("10xoxygen=%d",  libBpm.oxygen);
        LOG_INF("status=%d",  libBpm.status);

        return libBpm;
    }
    else if (_userSelectedMode == MODE_TWO) {
        readFillArray(READ_DATA_OUTPUT, READ_DATA, MAXFAST_ARRAY_SIZE + MAXFAST_EXTENDED_DATA, bpmArrTwo);

        libBpm.heartRate = (uint16_t)bpmArrTwo[1] << 8;
        libBpm.heartRate |= (bpmArrTwo[2]);
        // libBpm.heartRate /= 10;

        libBpm.confidence = bpmArrTwo[3];
    
        libBpm.oxygen = (uint16_t)bpmArrTwo[4] << 8;
        libBpm.oxygen |= bpmArrTwo[5];
        // libBpm.oxygen /= 10.0;

        libBpm.status = bpmArrTwo[6];

        uint16_t tempVal = (uint16_t)bpmArrTwo[7] << 8;
        tempVal |= bpmArrTwo[8];
        libBpm.rValue = tempVal;
        libBpm.rValue /= 10.0;

        libBpm.extStatus = bpmArrTwo[9];

        return libBpm;
    }
    else {
        libBpm.heartRate  = 0;
        libBpm.confidence = 0;
        libBpm.oxygen = 0;
        return libBpm;
    }
#endif // ACC_ENABLE
}

static int max32664_sample_fetch(const struct device *dev, enum sensor_channel chan)
{
    _bioData = readBpm();
    // #ifdef ACC_ENABLE
    // LOG_INF("xVal=%d",  _bioData.xVal);
    // LOG_INF("yVal=%d",  _bioData.yVal);
    // LOG_INF("zVal=%d",  _bioData.zVal);
    // #endif // ACC_ENABLE
    // LOG_INF("10xbpm=%d",  _bioData.heartRate);
    // LOG_INF("confidence=%d",  _bioData.confidence);
    // LOG_INF("10xoxygen=%d",  _bioData.oxygen);
    // LOG_INF("status=%d",  _bioData.status);
	return 0;
}

// Provide the current value of a specified sensor channel to the caller
static int max32664_channel_get(const struct device *dev, enum sensor_channel chan, struct sensor_value *val)
{
    #if defined (ACC_ENABLE)
    switch (chan) {
        case SENSOR_CHAN_ACCEL_X:
            val->val1 = _bioData.xVal / 1000;
            val->val2 = (_bioData.xVal - val->val1 * 1000) * 1000;
            break;
        case SENSOR_CHAN_ACCEL_Y:
            val->val1 = _bioData.yVal / 1000;
            val->val2 = (_bioData.yVal - val->val1 * 1000) * 1000;
            break;
        case SENSOR_CHAN_ACCEL_Z:
            val->val1 = _bioData.zVal / 1000;
            val->val2 = (_bioData.zVal - val->val1 * 1000) * 1000;
            break;
	    case SENSOR_CHAN_HEARTRATE:
		    val->val1 = _bioData.heartRate / 10;             // Integer part
		    val->val2 = (_bioData.heartRate % 10) * 1E5; // fractional part
		    break;
	    case SENSOR_CHAN_SPO2:
		    val->val1 = _bioData.oxygen / 10;             // Integer part
		    val->val2 = (_bioData.oxygen % 10) * 1E5; // fractional part
		    break;
	    case SENSOR_CHAN_CONFIDENCE:
		    val->val1 = _bioData.confidence;
		    val->val2 = 0;
		    break;
	    case SENSOR_CHAN_HR_STATUS:
		    val->val1 = _bioData.status;
		    val->val2 = 0;
		    break;
	    default:
		    LOG_ERR("Unsupported sensor channel");
		    return -ENOTSUP;
	}
    #else
    switch (chan) {
	    case SENSOR_CHAN_HEARTRATE:
		    val->val1 = _bioData.heartRate / 10;             // Integer part
		    val->val2 = (_bioData.heartRate % 10) * 1E5; // fractional part, 1E6 times, but original value is one digit integer already
		    break;
	    case SENSOR_CHAN_SPO2:
		    val->val1 = _bioData.oxygen / 10;             // Integer part
		    val->val2 = (_bioData.oxygen % 10) * 1E5; // fractional part
		    break;
	    case SENSOR_CHAN_CONFIDENCE:
		    val->val1 = _bioData.confidence;
		    val->val2 = 0;
		    break;
	    case SENSOR_CHAN_HR_STATUS:
		    val->val1 = _bioData.status;
		    val->val2 = 0;
		    break;
	    default:
		    LOG_ERR("Unsupported sensor channel");
		    return -ENOTSUP;
	}
    #endif // ACC_ENABLE
    return 0;
}

// This structure is used by the sensor framework to call into the driver for specific operations
static const struct sensor_driver_api max32664_driver_api = {
    .sample_fetch = max32664_sample_fetch,
    .channel_get = max32664_channel_get
};

// Initialize the sensor and communicate with the rest part of system
static int max32664_init(const struct device *dev)
{
	const struct max32664_config *config = dev->config;
	struct max32664_data *data = dev->data;
    const struct device *rst_gpio, *mfio_gpio;

    // Use RST and MFIO pins to reset the sensor and put it in application mode
    rst_gpio = device_get_binding(DT_INST_GPIO_LABEL(0, rst_gpios));
    mfio_gpio = device_get_binding(DT_INST_GPIO_LABEL(0, mfio_gpios));
    if (rst_gpio == NULL || mfio_gpio == NULL){
    	LOG_ERR("Could not get pointer to %s device.",DT_INST_GPIO_LABEL(0, rst_gpios));
        return -EINVAL;
    }
    gpio_pin_configure(rst_gpio, DT_INST_GPIO_PIN(0, rst_gpios), GPIO_OUTPUT | DT_INST_GPIO_FLAGS(0, rst_gpios));

    // We are actively pulling data from the FIFO not triggering interrupt, so MFIO set as output
    // If Interrupt description: Set the threshold for the FIFO interrupt bit/pin. The MFIO pin is used as the interrupt and the host should configure this pin as an input interrupt pin. The status bit DataRdyInt is set when this threshold is reached. (MAX32664A)
    // MFIO asserts low as an output when the sensor hub needs to communication with the host; MFIO acts as an input and when held low during a reset, the sensor hub enters bootloader mode.
    gpio_pin_configure(mfio_gpio, DT_INST_GPIO_PIN(0, mfio_gpios),  GPIO_OUTPUT);
    // While RSTN is low, set the MFIO pin to high: Application mode
    //     The MAX32664 enters application mode based on the sequencing of the RSTN pin and the MFIO pin. The necessary sequence is as follows:
    // - Set the RSTN pin low for 10ms.
    // - While RSTN is low, set the MFIO pin to high.
    // - After the 10ms has elapsed, set the RSTN pin high. (MFIO pin should be set high at least 1ms before RSTN pin is set high.)
    // - After an additional 50ms has elapsed, the MAX32664 is in application mode and the application performs its initialization of the application software.
    // - Approximately 1.5 second after the RSTN is set to high, the application completes the initialization and the device is ready to accept I2C commands. (For MAX32664A and MAX32664D, the startup time is 1.0 second).
    gpio_pin_set(rst_gpio, DT_INST_GPIO_PIN(0, rst_gpios), false);
    gpio_pin_set(mfio_gpio, DT_INST_GPIO_PIN(0, mfio_gpios), true);
    k_msleep(10);
    gpio_pin_set(rst_gpio, DT_INST_GPIO_PIN(0, rst_gpios), true);
    k_msleep(1000);
    // MFIO still output pin, now enable the pull-up resistor 
    gpio_pin_configure(mfio_gpio, DT_INST_GPIO_PIN(0, mfio_gpios),  DT_INST_GPIO_FLAGS(0, mfio_gpios));

    data->i2c = device_get_binding(config->i2c_label);
	if (!data->i2c) {
	    LOG_ERR("Could not find I2C device");
		return -EINVAL;
	}

    // Fetch the I2C device instance & address from the device tree and assign to the local variable
	_i2c_device = data->i2c;
	_i2c_addr = config->i2c_addr;

    // Check if the sensor is working properly
    uint8_t responseByte = readByte2(READ_DEVICE_MODE, 0x00); 
    if (responseByte == 0 ){
        LOG_INF("Sensor OK!");
    } else {
        LOG_INF("Sensor error=%d", responseByte);
    }

    // Configure the sensor for BPM, fast algorithm mode is mode 1, normal report length, as shown in application note Table 8
    int error = configBpm(MODE_ONE);  
    if(error == 0){
        LOG_INF("Sensor configured.");
    }
    else {
        LOG_INF("Error configuring sensor.%d", error);
    }

    uint8_t returnByte = readByte3(IDENTITY, READ_MCU_TYPE, NO_WRITE);
    LOG_INF("mcu_info=0x%02x",returnByte);
    returnByte = readByte4(IDENTITY, READ_SENSOR_HUB_VERS, NO_WRITE);
    k_msleep(4000);
    return 0;
}

// Define a structure to store the configuration information of MAX32664
static struct max32664_config max32664_config = {
	.i2c_label = DT_INST_BUS_LABEL(0),
	.i2c_addr = DT_INST_REG_ADDR(0)
};

DEVICE_DT_INST_DEFINE(0, max32664_init, device_pm_control_nop,
	&max32664_data, &max32664_config, APPLICATION, 
    CONFIG_SENSOR_INIT_PRIORITY, &max32664_driver_api);
