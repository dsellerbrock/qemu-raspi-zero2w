/*
 * Raspberry Pi BCM2835 PixelValve.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_DISPLAY_BCM2835_PIXELVALVE_H
#define HW_DISPLAY_BCM2835_PIXELVALVE_H

#include "hw/core/sysbus.h"
#include "qemu/timer.h"
#include "qom/object.h"

#define TYPE_BCM2835_PIXELVALVE "bcm2835-pixelvalve"
OBJECT_DECLARE_SIMPLE_TYPE(BCM2835PixelValveState, BCM2835_PIXELVALVE)

struct BCM2835PixelValveState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    QEMUTimer *frame_timer;
    qemu_irq irq;
    uint32_t regs[0x100 / sizeof(uint32_t)];
};

#endif
