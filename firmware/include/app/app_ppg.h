#ifndef APP_PPG_H
#define APP_PPG_H
/******************************************************************************/
/***************************** Include Files **********************************/
/******************************************************************************/
#include <stdint.h>
#include <stdbool.h>
#include <logging/log.h>
// Custom header files
#include "error.h"
// MAX32664 driver header file
#include "max32664.h"

/******************************************************************************/
/********************** Macros and Constants Definitions **********************/
/******************************************************************************/

/******************************************************************************/
/***************************** Exported Types *********************************/
/******************************************************************************/
// Local struct used to store the PPG data
typedef struct {
    uint8_t heart_rate_int_part;
    uint8_t heart_rate_frac_part;
    uint8_t oxygen_int_part;
    uint8_t oxygen_frac_part;
    uint8_t status;
    uint8_t confidence;
} ppg_data_t;

/******************************************************************************/
/*************************** Exported Functions *******************************/
/******************************************************************************/
/**
 * @brief Initializes the PPG (Photoplethysmography) application.
 * 
 * @param pBuff Pointer to the buffer used for initialization.
 * @param buffLength Length of the buffer.
 * @return int32_t Returns SUCCESS if the initialization is successful, otherwise returns an error code.
 */
int32_t AppPpgInit(void *pBuff, uint8_t buffLen);

/**
 * @brief Fetches PPG sensor samples and assembles the data into a buffer.
 *
 * @param pBuff Pointer to the buffer where the assembled data will be stored.
 * @param sampleCount Pointer to a variable that will store the number of samples fetched (always 1 in this case).
 * @return int32_t Returns SUCCESS (0) if the sensor samples are fetched and assembled successfully, or FAILURE (-1) if there is an error.
 */
int32_t AppPpgTimed(void *pBuff, uint32_t* sampleCount);

/**
 * @brief Displays the result of PPG data.
 * 
 * @param pData A pointer to a `ppg_data_t` structure containing the PPG data.
 */
void ShowResultPpg(ppg_data_t const * const pData);
#endif // APP_PPG_H