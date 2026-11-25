/***************************************************************************//**
 *   @file   error.h
 *   @brief  Error codes definition
*******************************************************************************/

#ifndef ERROR_H_
#define ERROR_H_
/******************************************************************************/
/***************************** Include Files **********************************/
/******************************************************************************/
#include <errno.h>

/******************************************************************************/
/********************** Macros and Constants Definitions **********************/
/******************************************************************************/
#ifdef SUCCESS
#undef SUCCESS
#endif
#define SUCCESS		0
#ifdef FAILURE
#undef FAILURE
#endif
#define FAILURE		-1

#ifndef __ELASTERROR
#define __ELASTERROR 2000
#endif

#define EOVERRUN	(__ELASTERROR + 1) /* Circular buffer overrun */

#define IS_ERR_VALUE(x)	((x) < 0)

#endif // ERROR_H_