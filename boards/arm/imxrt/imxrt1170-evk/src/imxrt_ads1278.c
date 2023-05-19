/****************************************************************************
 * boards/arm/imxrt/imxrt1170-evk/src/imxrt_ads1278.c
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

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <sys/types.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <assert.h>
#include <errno.h>
#include <debug.h>
#include <math.h>

#include <nuttx/arch.h>
#include <nuttx/mutex.h>
#include <nuttx/fs/fs.h>
#include <nuttx/irq.h>
#include <nuttx/wqueue.h>
#include <nuttx/analog/adc.h>
#include <nuttx/analog/ioctl.h>

#include "imxrt_config.h"
#include "imxrt_gpio.h"

#include "imxrt1170-evk.h"

#include "hardware/imxrt_pinmux.h"
#include "hardware/imxrt_flexio.h"
#include "hardware/imxrt_ccm.h"
#include "hardware/imxrt_dmamux.h"
#include "imxrt_flexio.h"
#include "imxrt_edma.h"
#include "imxrt_periphclks.h"

#include <arch/board/board.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define FLEXIO_BCLK_PIN                 (13u)
#define FLEXIO_FRAME_SYNC_PIN           (12u)
#define FLEXIO_RX_DATA_PIN              (10u)
#define FLEXIO_BCLK_TIMER_INDEX         0
#define FLEXIO_FS_TIMER_INDEX           1
#define FLEXIO_RX_SHIFTER_INDEX         0

#define FLEXIO_SRC_CLK_HZ               30000000u
#define FLEXIO_ADS1278_WORD_WIDTH       32u
#define FLEXIO_ADS1278_SAMPLE_RATE_HZ   9766u

#define SIGN_EXTEND_24BIT(x) (x) = (uint32_t)((((int32_t)x) << 8) >> 8)

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* This describes a FIFO of ADS1278 samples */

struct ads1278_fifo_s
{
  sem_t    sem;                          /* Counting semaphore */
  uint8_t  head;                         /* Index to the head [IN] index in the circular buffer */
  uint8_t  tail;                         /* Index to the tail [OUT] index in the circular buffer */
  uint32_t buffer[2];
};

struct ads1278_dev_s
{
  struct flexio_dev_s   *flexio;
  const unsigned int    dma_rxreqsrc;   /* DMAMUX source of RX DMA request */
  DMACH_HANDLE          rxdma;          /* DMA channel handle for RX transfers */
  uint32_t              rxbuf[512];      /* RX buffer (2 x 16 samples) */
  uint8_t               ocount;         /* The number of times the device has been opened */
  uint8_t               nrxwaiters;     /* Number of threads waiting to enqueue a message */
  mutex_t               closelock;      /* Locks out new opens while close is in progress */
  struct ads1278_fifo_s recv;           /* Describes receive FIFO */
  bool                  isovr;          /* Flag to indicate an overrun */

  /* The following is a list of poll structures of threads waiting for
   * driver events.  The 'struct pollfd' reference for each open is also
   * retained in the f_priv field of the 'struct file'.
   */

  struct pollfd *fds[CONFIG_ADS1278_NPOLLWAITERS];

};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static void         ads1278_dmarxcallback(DMACH_HANDLE handle, void *arg,
                                          bool done, int result);
static inline void  ads1278_dmarxstart(struct ads1278_dev_s *dev);
static int          ads1278_open(FAR struct file *filep);
static int          ads1278_close(FAR struct file *filep);
static ssize_t      ads1278_read(FAR struct file *fielp, FAR char *buffer,
                                 size_t buflen);
static int          ads1278_ioctl(FAR struct file *filep, int cmd,
                                  unsigned long arg);
static void         ads1278_receive(FAR struct ads1278_dev_s *dev,
                                    uint32_t *rxbuf);
static void         ads1278_notify(FAR struct ads1278_dev_s *dev);
static int          ads1278_poll(FAR struct file *filep, struct pollfd *fds,
                                 bool setup);
static int          ads1278_reset_fifo(FAR struct ads1278_dev_s *dev);
static void         ads1278_initialize(struct ads1278_dev_s *dev);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct file_operations g_ads1278_fops =
{
  ads1278_open,   /* open */
  ads1278_close,  /* close */
  ads1278_read,   /* read */
  NULL,           /* write */
  NULL,           /* seek */
  ads1278_ioctl,  /* ioctl */
  NULL,           /* mmap */
  NULL,           /* truncate */
  ads1278_poll    /* poll */
};

static struct ads1278_dev_s g_ads1278 =
{
  .dma_rxreqsrc = IMXRT_DMACHAN_FLEXIO2_01,
};


/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: ads1278_open
 *
 * Description:
 *   This function is called whenever the ADS1278 device is opened.
 *
 ****************************************************************************/

static int ads1278_open(FAR struct file *filep)
{
  FAR struct inode     *inode = filep->f_inode;
  FAR struct ads1278_dev_s *dev = inode->i_private;
  uint8_t               tmp;
  int                   ret;

  /* If the port is the middle of closing, wait until the close is
   * finished.
   */

  ret = nxmutex_lock(&dev->closelock);
  if (ret >= 0)
    {
      /* Increment the count of references to the device. If this is the
       * first time that the driver has been opened for this device, then
       * initialize the device.
       */

      tmp = dev->ocount + 1;
      if (tmp == 0)
        {
          /* More than 255 opens; uint8_t overflows to zero */

          ret = -EMFILE;
        }
      else
        {
          /* Check if this is the first time that the driver has been
           * opened.
           */

          if (tmp == 1)
            {
              /* Yes.. perform one time hardware initialization. */

              irqstate_t flags = enter_critical_section();

              /* Mark the FIFOs empty */

              dev->recv.head = 0;
              dev->recv.tail = 0;

              /* Clear overrun indicator */

              dev->isovr = false;

              ads1278_initialize(dev);

              leave_critical_section(flags);
            }

          /* Save the new open count on success */

          dev->ocount = tmp;
        }

      nxmutex_unlock(&dev->closelock);
    }

  return ret;
}

/****************************************************************************
 * Name: ads1278_close
 *
 * Description:
 *   This routine is called when the ADS1278 device is closed.
 *
 ****************************************************************************/

static int ads1278_close(FAR struct file *filep)
{
  FAR struct inode     *inode = filep->f_inode;
  FAR struct ads1278_dev_s *dev = inode->i_private;
  irqstate_t            flags;
  int                   ret;

  ret = nxmutex_lock(&dev->closelock);
  if (ret >= 0)
    {
      /* Decrement the references to the driver. If the reference count will
       * decrement to 0, then uninitialize the driver.
       */

      if (dev->ocount > 1)
        {
          dev->ocount--;
          nxmutex_unlock(&dev->closelock);
        }
      else
        {
          /* There are no more references to the port */

          dev->ocount = 0;

          flags = enter_critical_section();    /* Disable interrupts */

          /* TODO: Disable the ADS1278 */

          leave_critical_section(flags);

          nxmutex_unlock(&dev->closelock);
        }
    }

  return ret;
}

/****************************************************************************
 * Name: ads1278_read
 ****************************************************************************/

static ssize_t ads1278_read(FAR struct file *filep, FAR char *buffer,
                            size_t buflen)
{
  FAR struct inode     *inode = filep->f_inode;
  FAR struct ads1278_dev_s *dev = inode->i_private;
  size_t                nread;
  irqstate_t            flags;
  int                   ret = 0;
  int                   datalen = sizeof(uint32_t);

  if (buflen >= datalen)
    {
      /* Interrupts must be disabled while accessing the receive FIFO */

      flags = enter_critical_section();

      while (dev->recv.head == dev->recv.tail)
        {
          /* Check if there was an overrun, if set we need to return EIO */

          if (dev->isovr)
            {
              dev->isovr = false;
              ret = -EIO;
              goto return_with_irqdisabled;
            }

          /* The receive FIFO is empty -- was non-blocking mode selected? */

          if (filep->f_oflags & O_NONBLOCK)
            {
              ret = -EAGAIN;
              goto return_with_irqdisabled;
            }

          /* Wait for a data to be received */

          dev->nrxwaiters++;
          ret = nxsem_wait(&dev->recv.sem);
          dev->nrxwaiters--;
          if (ret < 0)
            {
              goto return_with_irqdisabled;
            }
        }

      /* The receive FIFO is not empty. Copy all buffered data that will fit
       * in the user buffer.
       */

      nread = 0;

      do
        {
          uint32_t *data = &dev->recv.buffer[dev->recv.head];

          /* Will the next data in the FIFO fit into the user buffer? */

          if (nread + datalen > buflen)
            {
              /* No, break out of the loop now with nread equal to the
               * actual number of bytes transferred.
               */

              break;
            }

          /* Copy the data to the user buffer */

          memcpy(&buffer[nread], data, sizeof(uint32_t));

          nread += datalen;

          /* Increment the head of the circular data buffer */

          if (++dev->recv.head >= 2)
            {
              dev->recv.head = 0;
            }
        }
      while (dev->recv.head != dev->recv.tail);

      /* All of the data have been transferred. Return the number of
       * bytes that were read.
       */

      ret = nread;

return_with_irqdisabled:
      leave_critical_section(flags);
    }

  return ret;
}

/****************************************************************************
 * Name: ads1278_ioctl
 ****************************************************************************/

static int ads1278_ioctl(FAR struct file *filep, int cmd, unsigned long arg)
{
  FAR struct inode *inode = filep->f_inode;
  FAR struct ads1278_dev_s *dev = inode->i_private;
  int ret;

  switch (cmd)
    {
    case ANIOC_RESET_FIFO:
    {
      ret = ads1278_reset_fifo(dev);
    }
    break;

    default:
      break;
    }

  return ret;
}

/****************************************************************************
 * Name: ads1278_receive
 ****************************************************************************/

static void ads1278_receive(FAR struct ads1278_dev_s *dev, uint32_t *rxbuf)
{
  FAR struct ads1278_fifo_s *fifo = &dev->recv;
  uint32_t *rxdata = &fifo->buffer[fifo->tail];
  int i, j;
  int nexttail;

  /* Check if adding this new message would over-run the drivers ability to
   * enqueue read data.
   */

  nexttail = fifo->tail + 1;
  if (nexttail >= 2)
    {
      nexttail = 0;
    }

  imxrt_gpio_write(GPIO_DEBUG_PIN, 1);

  rxbuf[8]  = (rxbuf[0] & 0x7fffff80) >> 7;
  rxbuf[9]  = (rxbuf[0] & 0x0000007f) << 17;
  SIGN_EXTEND_24BIT(rxbuf[8]);
 
  rxbuf[9] |= (rxbuf[1] & 0xffff8000) >> 15;
  rxbuf[10]  = (rxbuf[1] & 0x00007fff) << 9;
  SIGN_EXTEND_24BIT(rxbuf[9]);
  
  rxbuf[10] |= (rxbuf[2] & 0xff800000) >> 23;
  rxbuf[11]  = (rxbuf[2] & 0x007fffff) << 1;
  SIGN_EXTEND_24BIT(rxbuf[10]);
  
  rxbuf[11] |= (rxbuf[3] & 0x80000000) >> 31;
  rxbuf[12]  = (rxbuf[3] & 0x7fffff80) >> 7;
  rxbuf[13]  = (rxbuf[3] & 0x0000007f) << 17;
  SIGN_EXTEND_24BIT(rxbuf[11]);
  SIGN_EXTEND_24BIT(rxbuf[12]);
  
  rxbuf[13] |= (rxbuf[4] & 0xffff8000) >> 15;
  rxbuf[14]  = (rxbuf[4] & 0x00007fff) << 9;
  SIGN_EXTEND_24BIT(rxbuf[13]);
 
  rxbuf[14] |= (rxbuf[5] & 0xff800000) >> 23;
  rxbuf[15]  = (rxbuf[5] & 0x007fffff) << 1;
  SIGN_EXTEND_24BIT(rxbuf[14]);
  
  rxbuf[15] |= (rxbuf[6] & 0x80000000) >> 31;
  SIGN_EXTEND_24BIT(rxbuf[15]);

  memcpy(&rxbuf[0], &rxbuf[8], 32);

  for (i = 16, j = 8; i < 256; i += 16, j += 8)
    {
      rxbuf[j]  = (rxbuf[i] & 0x7fffff80) >> 7;
      rxbuf[j + 1]  = (rxbuf[i] & 0x0000007f) << 17;
      SIGN_EXTEND_24BIT(rxbuf[j]);
     
      rxbuf[j + 1] |= (rxbuf[i + 1] & 0xffff8000) >> 15;
      rxbuf[j + 2]  = (rxbuf[i + 1] & 0x00007fff) << 9;
      SIGN_EXTEND_24BIT(rxbuf[j + 1]);
      
      rxbuf[j + 2] |= (rxbuf[i + 2] & 0xff800000) >> 23;
      rxbuf[j + 3]  = (rxbuf[i + 2] & 0x007fffff) << 1;
      SIGN_EXTEND_24BIT(rxbuf[j + 2]);
      
      rxbuf[j + 3] |= (rxbuf[i + 3] & 0x80000000) >> 31;
      rxbuf[j + 4]  = (rxbuf[i + 3] & 0x7fffff80) >> 7;
      rxbuf[j + 5]  = (rxbuf[i + 3] & 0x0000007f) << 17;
      SIGN_EXTEND_24BIT(rxbuf[j + 3]);
      SIGN_EXTEND_24BIT(rxbuf[j + 4]);
      
      rxbuf[j + 5] |= (rxbuf[i + 4] & 0xffff8000) >> 15;
      rxbuf[j + 6]  = (rxbuf[i + 4] & 0x00007fff) << 9;
      SIGN_EXTEND_24BIT(rxbuf[j + 5]);
     
      rxbuf[j + 6] |= (rxbuf[i + 5] & 0xff800000) >> 23;
      rxbuf[j + 7]  = (rxbuf[i + 5] & 0x007fffff) << 1;
      SIGN_EXTEND_24BIT(rxbuf[j + 6]);
      
      rxbuf[j + 7] |= (rxbuf[i + 6] & 0x80000000) >> 31;
      SIGN_EXTEND_24BIT(rxbuf[j + 7]);
    }
  
  /* Refuse the new data if the FIFO is full */

  if (nexttail != fifo->head)
    {

      /* Add the new, decoded ADS1278 buffer address at the tail of the FIFO */

      *rxdata = (uint32_t)rxbuf;

      /* Increment the tail of the circular buffer */

      fifo->tail = nexttail;

      ads1278_notify(dev);
    }

  imxrt_gpio_write(GPIO_DEBUG_PIN, 0);
}

/****************************************************************************
 * Name: ads1278_notify
 ****************************************************************************/

static void ads1278_notify(FAR struct ads1278_dev_s *dev)
{
  FAR struct ads1278_fifo_s *fifo = &dev->recv;

  /* If there are threads waiting on poll() for data to become available,
   * then wake them up now.
   */

  poll_notify(dev->fds, CONFIG_ADS1278_NPOLLWAITERS, POLLIN);

  /* If there are threads waiting for read data, then signal one of them
   * that the read data is available.
   */

  if (dev->nrxwaiters > 0)
    {
      nxsem_post(&fifo->sem);
    }
}

/****************************************************************************
 * Name: ads1278_poll
 ****************************************************************************/

static int ads1278_poll(
  FAR struct file *filep,
  struct pollfd *fds,
  bool setup)
{
  FAR struct inode *inode = filep->f_inode;
  FAR struct ads1278_dev_s *dev = inode->i_private;
  irqstate_t flags;
  int ret = 0;
  int i;

  /* Interrupts must be disabled while accessing the list of poll structures
   * and ad_recv FIFO.
   */

  flags = enter_critical_section();

  if (setup)
    {
      /* Ignore waits that do not include POLLIN */

      if ((fds->events & POLLIN) == 0)
        {
          ret = -EDEADLK;
          goto return_with_irqdisabled;
        }

      /* This is a request to set up the poll.  Find an available
       * slot for the poll structure reference
       */

      for (i = 0; i < CONFIG_ADS1278_NPOLLWAITERS; i++)
        {
          /* Find an available slot */

          if (!dev->fds[i])
            {
              /* Bind the poll structure and this slot */

              dev->fds[i] = fds;
              fds->priv   = &dev->fds[i];
              break;
            }
        }

      if (i >= CONFIG_ADS1278_NPOLLWAITERS)
        {
          fds->priv    = NULL;
          ret          = -EBUSY;
          goto return_with_irqdisabled;
        }

      /* Should we immediately notify on any of the requested events? */

      if (dev->recv.head != dev->recv.tail)
        {
          poll_notify(dev->fds, CONFIG_ADS1278_NPOLLWAITERS, POLLIN);
        }
    }
  else if (fds->priv)
    {
      /* This is a request to tear down the poll. */

      struct pollfd **slot = (struct pollfd **)fds->priv;

      /* Remove all memory of the poll setup */

      *slot                = NULL;
      fds->priv            = NULL;
    }

return_with_irqdisabled:
  leave_critical_section(flags);
  return ret;
}

/****************************************************************************
 * Name: ads1278_reset_fifo
 ****************************************************************************/

static int ads1278_reset_fifo(FAR struct ads1278_dev_s *dev)
{
  irqstate_t flags;
  FAR struct ads1278_fifo_s *fifo = &dev->recv;

  /* Interrupts must be disabled while accessing the receive FIFO */

  flags = enter_critical_section();

  fifo->head = fifo->tail;

  leave_critical_section(flags);

  return OK;
}

/****************************************************************************
 * Name: ads1278_register
 ****************************************************************************/

static int ads1278_register(
  FAR const char *path,
  FAR struct ads1278_dev_s *dev)
{
  int ret;

  DEBUGASSERT(path != NULL && dev != NULL);

  /* Initialize the ADS1278 device structure */

  dev->ocount = 0;

  /* Initialize semaphores & mutex */

  nxsem_init(&dev->recv.sem, 0, 0);
  nxmutex_init(&dev->closelock);

  /* Register the ADS1278 character driver */

  ret = register_driver(path, &g_ads1278_fops, 0444, dev);
  if (ret < 0)
    {
      nxsem_destroy(&dev->recv.sem);
      nxmutex_destroy(&dev->closelock);
    }

  return ret;
}

/****************************************************************************
 * Name: ads1278_initialize
 *
 * Description:
 *   Initialize the ADS1278 I2S in its default state
 *   (Master, 32-bit, etc.)
 *
 * Input Parameters:
 *   dev - private ADS1278 device structure
 *
 * Returned Value:
 *   None
 *
 ****************************************************************************/

static void ads1278_initialize(struct ads1278_dev_s *dev)
{
  struct flexio_dev_s *flexio = dev->flexio;
  struct flexio_shifter_config_s shifter_config = {0};
  struct flexio_timer_config_s timer_config     = {0};

  flexio->ops->reset(flexio);

  shifter_config.timer_select   = FLEXIO_BCLK_TIMER_INDEX;
  shifter_config.pin_select     = FLEXIO_RX_DATA_PIN;
  shifter_config.timer_polarity = FLEXIO_SHIFTER_TIMER_POLARITY_ON_NEGATIVE;
  shifter_config.pin_config     = FLEXIO_PIN_CONFIG_OUTPUT_DISABLED;
  shifter_config.pin_polarity   = FLEXIO_PIN_ACTIVE_HIGH;
  shifter_config.shifter_mode   = FLEXIO_SHIFTER_MODE_RECEIVE;
  shifter_config.input_source   = FLEXIO_SHIFTER_INPUT_FROM_PIN;
  shifter_config.shifter_stop   = FLEXIO_TIMER_STOP_BIT_DISABLED;
  shifter_config.shifter_start  =
    FLEXIO_SHIFTER_START_BIT_DISABLED_LOAD_DATA_ON_ENABLE;

  flexio->ops->set_shifter_config(
    flexio,
    FLEXIO_RX_SHIFTER_INDEX,
    &shifter_config);

  /* Set Timer for bit clock */

  timer_config.trigger_select   = 0;
  timer_config.trigger_polarity = FLEXIO_TIMER_TRIGGER_POLARITY_ACTIVE_HIGH;
  timer_config.trigger_source   = FLEXIO_TIMER_TRIGGER_SOURCE_INTERNAL;
  timer_config.pin_select       = FLEXIO_BCLK_PIN;
  timer_config.pin_config       = FLEXIO_PIN_CONFIG_OUTPUT;
  timer_config.pin_polarity     = FLEXIO_PIN_ACTIVE_HIGH;
  timer_config.timer_mode       = FLEXIO_TIMER_MODE_DUAL8_BIT_BAUD_BIT;
  timer_config.timer_output     =
    FLEXIO_TIMER_OUTPUT_ONE_NOT_AFFECTED_BY_RESET;
  timer_config.timer_decrement  =
    FLEXIO_TIMER_DEC_SRC_ON_FLEX_IO_CLOCK_SHIFT_TIMER_OUTPUT;
  timer_config.timer_reset      = FLEXIO_TIMER_RESET_NEVER;
  timer_config.timer_disable    = FLEXIO_TIMER_DISABLE_NEVER;
  timer_config.timer_enable     = FLEXIO_TIMER_ENABLED_ALWAYS;
  timer_config.timer_start      = FLEXIO_TIMER_START_BIT_DISABLED;
  timer_config.timer_stop       = FLEXIO_TIMER_STOP_BIT_DISABLED;
  timer_config.timer_compare    = 63 << 8 | 2; /* 5MHz BCLK frequency */

  flexio->ops->set_timer_config(
    flexio,
    FLEXIO_BCLK_TIMER_INDEX,
    &timer_config);

  /* Set Timer for frame sync */

  timer_config.trigger_select   =
    FLEXIO_TIMER_TRIGGER_SEL_TIMn(FLEXIO_BCLK_TIMER_INDEX);
  timer_config.trigger_polarity = FLEXIO_TIMER_TRIGGER_POLARITY_ACTIVE_HIGH;
  timer_config.trigger_source   = FLEXIO_TIMER_TRIGGER_SOURCE_INTERNAL;
  timer_config.pin_select       = FLEXIO_FRAME_SYNC_PIN;
  timer_config.pin_config       = FLEXIO_PIN_CONFIG_OUTPUT;
  timer_config.pin_polarity     = FLEXIO_PIN_ACTIVE_HIGH;
  timer_config.timer_mode       = FLEXIO_TIMER_MODE_SINGLE16_BIT;
  timer_config.timer_output     =
    FLEXIO_TIMER_OUTPUT_ONE_NOT_AFFECTED_BY_RESET;
  timer_config.timer_decrement  =
    FLEXIO_TIMER_DEC_SRC_ON_TRIGGER_INPUT_SHIFT_TIMER_OUTPUT;
  timer_config.timer_reset      = FLEXIO_TIMER_RESET_NEVER;
  timer_config.timer_disable    = FLEXIO_TIMER_DISABLE_NEVER;
  timer_config.timer_enable     = FLEXIO_TIMER_ENABLE_ON_PREV_TIMER_ENABLE;
  timer_config.timer_start      = FLEXIO_TIMER_START_BIT_DISABLED;
  timer_config.timer_stop       = FLEXIO_TIMER_STOP_BIT_DISABLED;
  timer_config.timer_compare    = 512U - 1; /* 512 BCLK periods for one FSYNC (pos and neg edge) */

  flexio->ops->set_timer_config(
    flexio,
    FLEXIO_FS_TIMER_INDEX,
    &timer_config);

  if (dev->dma_rxreqsrc != 0)
    {
      if (dev->rxdma == NULL)
        {
          dev->rxdma = imxrt_dmach_alloc(dev->dma_rxreqsrc |
                                         DMAMUX_CHCFG_ENBL, 0);
          DEBUGASSERT(dev->rxdma);
        }
    }
  else
    {
      dev->rxdma = NULL;
    }

  up_invalidate_dcache((uintptr_t)dev->rxbuf,
                       (uintptr_t)dev->rxbuf + 2048U);

  /* Set up the DMA */

  struct imxrt_edma_xfrconfig_s config;

  config.saddr  = flexio->ops->get_shifter_buffer_address(
                    flexio,
                    FLEXIO_SHIFTER_BUFFER_BIT_SWAPPED,
                    FLEXIO_RX_SHIFTER_INDEX);
  config.daddr  = (uint32_t)&dev->rxbuf[0];
  config.soff   = 0;
  config.doff   = 4;
  config.iter   = 512;
  config.flags  = EDMA_CONFIG_LINKTYPE_LINKNONE |
                  EDMA_CONFIG_LOOPDEST | 
                  EDMA_CONFIG_INTHALF |
                  EDMA_CONFIG_INTMAJOR;
  config.ssize  = EDMA_32BIT;
  config.dsize  = EDMA_32BIT;
  config.nbytes = 4;

  imxrt_dmach_xfrsetup(dev->rxdma, &config);

  /* Start the DMA */

  ads1278_dmarxstart(dev);

  flexio->ops->enable(flexio, true);
}

/****************************************************************************
 * Name: ads1278_dmarxcallback
 *
 * Description:
 *   Called when the RX DMA completes
 *
 ****************************************************************************/

static void ads1278_dmarxcallback(DMACH_HANDLE handle, void *arg, bool done,
                                  int result)
{
  struct ads1278_dev_s *dev = (struct ads1278_dev_s *)arg;
  
  up_invalidate_dcache((uintptr_t)dev->rxbuf,
                       (uintptr_t)dev->rxbuf + 2048U);

  if (done)
    {
      ads1278_receive(dev, &dev->rxbuf[256]);
    }
  else
    {
      ads1278_receive(dev, &dev->rxbuf[0]);
    }
}

/****************************************************************************
 * Name: ads1278_dmarxstart
 *
 * Description:
 *   Start RX DMA
 *
 ****************************************************************************/

static inline void ads1278_dmarxstart(struct ads1278_dev_s *dev)
{
  struct flexio_dev_s *flexio = dev->flexio;

  flexio->ops->enable_shifter_status_dma(
    flexio,
    1u << FLEXIO_RX_SHIFTER_INDEX,
    true);
  flexio->ops->clear_shifter_status_flags(
    flexio,
    1u << FLEXIO_RX_SHIFTER_INDEX);

  imxrt_dmach_start(dev->rxdma, ads1278_dmarxcallback, dev);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: imxrt_ads1278_initialize
 *
 * Description:
 *   Initialize the ADS1278 device
 *
 * Returned Value:
 *   Valid device structure reference on success; a NULL on failure
 *
 ****************************************************************************/

void imxrt_ads1278_initialize(void)
{
  struct ads1278_dev_s *dev = &g_ads1278;
  struct flexio_dev_s *flexio;
  int ret;

  imxrt_config_gpio(GPIO_FLEXIO2_FSYNC);
  imxrt_config_gpio(GPIO_FLEXIO2_BCLK);
  imxrt_config_gpio(GPIO_FLEXIO2_RX);

  /* REMOVE: Debugging*/

  imxrt_config_gpio(GPIO_DEBUG_PIN);        /* GPIO_AD_14 */

  /* End Debugging */

  flexio = imxrt_flexio_initialize(2);

  DEBUGASSERT(flexio);

  dev->flexio = flexio;

  /* Register the ADS1278 driver at "/dev/ads" */

  ret = ads1278_register("/dev/ads", dev);
  if (ret < 0)
    {
      _err("ERROR: ads1278_register ads failed: %d\n", ret);
    }

  ////////////TEST
  // {
  //   int fd;
  //   uint32_t buffer[8];

  //   fd  = open("/dev/ads", O_RDONLY);
  //   while (1)
  //     {
  //       ret = read(fd, buffer, 32);
  //       if (ret < 0)
  //         {
  //           ret = 0;
  //           _err("ERROR: ads1278 read failed: %d\n", ret);
  //         }
  //     }
  // }
  ////////////END TEST
}
