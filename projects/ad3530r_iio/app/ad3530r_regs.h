/***************************************************************************//**
* @file   ad3530r_regs.h
* @brief  Header file for the ad3530r registers map
********************************************************************************
* Copyright (c) 2023 Analog Devices, Inc.
* All rights reserved.
*
* This software is proprietary to Analog Devices, Inc. and its licensors.
* By using this software you agree to the terms of the associated
* Analog Devices Software License Agreement.
*******************************************************************************/
#ifndef _AD3530R_REGS_H_
#define _AD3530R_REGS_H_

/******************************************************************************/
/***************************** Include Files **********************************/
/******************************************************************************/

#include <stdint.h>
#include "ad3530r.h"

/******************************************************************************/
/********************* Macros and Constants Definition ************************/
/******************************************************************************/

#define AD3530R_REG_MAP_END		0xffff

/******************************************************************************/
/********************** Public/Extern Declarations ****************************/
/******************************************************************************/

extern const uint32_t ad3530r_regs [];
extern const uint32_t ad3531r_regs [];

#endif /* _AD3530R_REGS_H_ */
