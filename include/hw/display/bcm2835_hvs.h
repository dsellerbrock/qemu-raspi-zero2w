/*
 * Raspberry Pi VC4 Hardware Video Scaler.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef BCM2835_HVS_H
#define BCM2835_HVS_H

#include "hw/core/sysbus.h"
#include "hw/display/bcm2835_fb.h"
#include "qom/object.h"

#define TYPE_BCM2835_HVS "bcm2835-hvs"
OBJECT_DECLARE_SIMPLE_TYPE(BCM2835HVSState, BCM2835_HVS)

#define BCM2835_HVS_DLIST_WORDS (0x4000 / sizeof(uint32_t))

struct BCM2835HVSState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    BCM2835FBState *fb;
    uint32_t regs[0x2000 / sizeof(uint32_t)];
    uint32_t dlist[BCM2835_HVS_DLIST_WORDS];
    uint32_t active_dlist[3];
    uint32_t channel_status[3];
};

#endif
