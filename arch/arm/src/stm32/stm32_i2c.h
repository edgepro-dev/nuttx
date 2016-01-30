/************************************************************************************
 * arch/arm/src/stm32/stm32_i2c.h
 *
 *   Copyright (C) 2009, 2011, 2013 Gregory Nutt. All rights reserved.
 *   Author: Gregory Nutt <gnutt@nuttx.org>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name NuttX nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ************************************************************************************/

#ifndef __ARCH_ARM_SRC_STM32_STM32_I2C_H
#define __ARCH_ARM_SRC_STM32_STM32_I2C_H

/************************************************************************************
 * Pre-processor Definitions
 ************************************************************************************/

/* If a dynamic timeout is selected, then a non-negative, non-zero micro-
 * seconds per byte value must be provided as well.
 */

#ifdef CONFIG_STM32_I2C_DYNTIMEO
#  if CONFIG_STM32_I2C_DYNTIMEO_USECPERBYTE < 1
#    warning "Ignoring CONFIG_STM32_I2C_DYNTIMEO because of CONFIG_STM32_I2C_DYNTIMEO_USECPERBYTE"
#    undef CONFIG_STM32_I2C_DYNTIMEO
#  endif
#endif

/************************************************************************************
 * Included Files
 ************************************************************************************/

#include <nuttx/config.h>
#include <nuttx/i2c/i2c_master.h>

#include "chip.h"
#if defined(CONFIG_STM32_STM32F30XX)
#  include "chip/stm32f30xxx_i2c.h"
#else
#  include "chip/stm32_i2c.h"
#endif

#endif /* __ARCH_ARM_SRC_STM32_STM32_I2C_H */

