/****************************************************************************
 * arch/arm/src/imxrt/imxrt_clockconfig.h
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

#ifndef __ARCH_ARM_SRC_IMXRT_IMXRT_CLOCKCONFIG_H
#define __ARCH_ARM_SRC_IMXRT_IMXRT_CLOCKCONFIG_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

/* IMXRT11XX Clock pheripheral is different from IMXRT10XX
 * hence we use a VER2 driver for clockconfig
 */

#ifdef CONFIG_IMXRT_CLOCKCONFIG_VER2
#include "imxrt_clockconfig_ver2.h"
#else
#include "imxrt_clockconfig_ver1.h"
#endif

#endif /* __ARCH_ARM_SRC_IMXRT_IMXRT_CLOCKCONFIG_H */
