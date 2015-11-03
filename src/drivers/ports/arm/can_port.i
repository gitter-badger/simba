/**
 * @file can_port.i
 * @version 1.0
 *
 * @section License
 * Copyright (C) 2014-2015, Erik Moqvist
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * This file is part of the Simba project.
 */

#include "simba.h"

#define MAILBOX_MAX 8

COUNTER_DEFINE("/drivers/can/rx_channel_overflow", can_rx_channel_overflow);

static void isr(struct can_device_t *dev_p)
{
    struct can_frame_t frame;
    struct can_driver_t *drv_p;
    int i;
    uint32_t status;
    uint32_t mid, msr;

    if (dev_p->drv_p == NULL) {
        return;
    }

    drv_p = dev_p->drv_p;

    /* Read the status. */
    status = dev_p->regs_p->SR;

    /* Handle TX complete interrupt. */
    if ((status & 0x1) != 0) {
        //sem_put_irq(&drv_p->tx_sem, 1);
    }

    /* Look for frames in all RX mailboxes. */
    for (i = 1; i < MAILBOX_MAX; i++) {
        if ((status & (1 << i)) == 0) {
            continue;
        }

        /* Read the frame from the hardware. */
        mid = dev_p->regs_p->MAILBOX[i].MID;
        frame.id = ((mid & CAN_MID_MIDVA_MASK) >> CAN_MID_MIDVA_POS);
        msr = dev_p->regs_p->MAILBOX[i].MSR;
        frame.size = ((msr & CAN_MSR_MDLC_MASK) >> CAN_MSR_MDLC_POS);
        frame.rtr = ((msr & CAN_MSR_MRTR) != 0);
        frame.timestamp = 0;
        frame.data.u32[0] = dev_p->regs_p->MAILBOX[i].MDL;
        frame.data.u32[1] = dev_p->regs_p->MAILBOX[i].MDH;

        /* Allow reception of the next message. */
        dev_p->regs_p->MAILBOX[i].MCR = CAN_MCR_MTCR;

        /* Write the received frame to the application input
           channel. */
        if (queue_write_irq(&drv_p->chin,
                            &frame,
                            sizeof(frame)) != sizeof(frame)) {
            COUNTER_INC(can_rx_channel_overflow, 1);
        }
    }
}

ISR(can0)
{
    isr(&can_device[0]);
}

ISR(can1)
{
    isr(&can_device[1]);
}

static ssize_t write_cb(struct can_driver_t *drv_p,
                        const void *buf_p,
                        size_t size)
{
    struct can_device_t *dev_p = drv_p->dev_p;
    struct can_frame_t *frame_p = (struct can_frame_t *)buf_p;

    /* Read the frame from the hardware. */
    dev_p->regs_p->MAILBOX[0].MID = CAN_MID_MIDVA(frame_p->id);
    dev_p->regs_p->MAILBOX[0].MDL = frame_p->data.u32[0];
    dev_p->regs_p->MAILBOX[0].MDH = frame_p->data.u32[1];

    /* Set DLC and trigger the transmission. */
    dev_p->regs_p->MAILBOX[0].MCR = (CAN_MSR_MDLC(frame_p->size)
                                     | CAN_MCR_MTCR);

    /* Wait for transmission to complete. */
    //sem_get(drv_p->tx_sem, NULL);

    return (size);
}

int can_port_init(struct can_driver_t *drv_p,
                  struct can_device_t *dev_p,
                  int speed)
{
    sem_init(&drv_p->tx_sem, 0);

    return (0);
}

int can_port_start(struct can_driver_t *drv_p)
{
    struct can_device_t *dev_p = drv_p->dev_p;
    int i;

    pmc_peripheral_clock_enable(dev_p->id);
    nvic_enable_interrupt(dev_p->id);

    /* Baud rate. */
    dev_p->regs_p->BR = (CAN_BR_PHASE2(2)
                         | CAN_BR_PHASE1(2)
                         | CAN_BR_PROPAG(0)
                         | CAN_BR_SJW(1)
                         | CAN_BR_BRP(0));

    /* One TX mailbox. */
    dev_p->regs_p->MAILBOX[0].MMR = CAN_MMR_MOT_MB_TX;

    /* Use all but one mailbox for RX. */
    for (i = 1; i < MAILBOX_MAX; i++) {
        dev_p->regs_p->MAILBOX[i].MMR = CAN_MMR_MOT_MB_RX;
    }

    dev_p->regs_p->IER = 0xffffffff;

    dev_p->regs_p->MR = (CAN_MR_CANEN);

    return (0);
}

int can_port_stop(struct can_driver_t *drv_p)
{
    struct can_device_t *dev_p = drv_p->dev_p;

    dev_p->regs_p->MR = 0;

    return (0);
}