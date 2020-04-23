/*
 * Nuvoton NPCM7xx Timer Controller
 *
 * Copyright 2020 Google LLC
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 */

#include "qemu/osdep.h"

#include "hw/irq.h"
#include "hw/misc/npcm7xx_clk.h"
#include "hw/timer/npcm7xx_timer.h"
#include "migration/vmstate.h"
#include "qemu/bitops.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qemu/units.h"
#include "trace.h"

/* 32-bit register indices. */
enum NPCM7xxTimerRegisters {
    NPCM7XX_TIMER_TCSR0,
    NPCM7XX_TIMER_TCSR1,
    NPCM7XX_TIMER_TICR0,
    NPCM7XX_TIMER_TICR1,
    NPCM7XX_TIMER_TDR0,
    NPCM7XX_TIMER_TDR1,
    NPCM7XX_TIMER_TISR,
    NPCM7XX_TIMER_WTCR,
    NPCM7XX_TIMER_TCSR2,
    NPCM7XX_TIMER_TCSR3,
    NPCM7XX_TIMER_TICR2,
    NPCM7XX_TIMER_TICR3,
    NPCM7XX_TIMER_TDR2,
    NPCM7XX_TIMER_TDR3,
    NPCM7XX_TIMER_TCSR4         = 0x0040 / sizeof(uint32_t),
    NPCM7XX_TIMER_TICR4         = 0x0048 / sizeof(uint32_t),
    NPCM7XX_TIMER_TDR4          = 0x0050 / sizeof(uint32_t),
    NPCM7XX_TIMER_REGS_END,
};

/* Register field definitions. */
#define NPCM7XX_TCSR_CEN                BIT(30)
#define NPCM7XX_TCSR_IE                 BIT(29)
#define NPCM7XX_TCSR_PERIODIC           BIT(27)
#define NPCM7XX_TCSR_CRST               BIT(26)
#define NPCM7XX_TCSR_CACT               BIT(25)
#define NPCM7XX_TCSR_RSVD               0x21ffff00
#define NPCM7XX_TCSR_PRESCALE_START     0
#define NPCM7XX_TCSR_PRESCALE_LEN       8

/*
 * Returns the index of timer in the tc->timer array. This can be used to
 * locate the registers that belong to this timer.
 */
static int npcm7xx_timer_index(NPCM7xxTimerCtrlState *tc, NPCM7xxTimer *timer)
{
    int index = timer - tc->timer;

    g_assert(index >= 0 && index < NPCM7XX_TIMERS_PER_CTRL);

    return index;
}

/* Return the value by which to divide the reference clock rate. */
static uint32_t npcm7xx_timer_prescaler(const NPCM7xxTimer *t)
{
    return extract32(t->tcsr, NPCM7XX_TCSR_PRESCALE_START,
                     NPCM7XX_TCSR_PRESCALE_LEN) + 1;
}

/* Convert a timer cycle count to a time interval in nanoseconds. */
static int64_t npcm7xx_timer_count_to_ns(NPCM7xxTimer *t, uint32_t count)
{
    int64_t ns = count;

    ns *= NANOSECONDS_PER_SECOND / NPCM7XX_TIMER_REF_HZ;
    ns *= npcm7xx_timer_prescaler(t);

    return ns;
}

/* Convert a time interval in nanoseconds to a timer cycle count. */
static uint32_t npcm7xx_timer_ns_to_count(NPCM7xxTimer *t, int64_t ns)
{
    int64_t count;

    count = ns / (NANOSECONDS_PER_SECOND / NPCM7XX_TIMER_REF_HZ);
    count /= npcm7xx_timer_prescaler(t);

    return count;
}

/*
 * Raise the interrupt line if there's a pending interrupt and interrupts are
 * enabled for this timer. If not, lower it.
 */
static void npcm7xx_timer_check_interrupt(NPCM7xxTimer *t)
{
    NPCM7xxTimerCtrlState *tc = t->ctrl;
    int index = npcm7xx_timer_index(tc, t);
    bool pending = (t->tcsr & NPCM7XX_TCSR_IE) && (tc->tisr & BIT(index));

    qemu_set_irq(t->irq, pending);
    trace_npcm7xx_timer_irq(DEVICE(tc)->canonical_path, index, pending);
}

/* Start or resume the timer. */
static void npcm7xx_timer_start(NPCM7xxTimer *t)
{
    int64_t now;

    now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    t->expires_ns = now + t->remaining_ns;
    timer_mod(&t->qtimer, t->expires_ns);
}

/*
 * Called when the counter reaches zero. Sets the interrupt flag, and either
 * restarts or disables the timer.
 */
static void npcm7xx_timer_reached_zero(NPCM7xxTimer *t)
{
    NPCM7xxTimerCtrlState *tc = t->ctrl;
    int index = npcm7xx_timer_index(tc, t);

    tc->tisr |= BIT(index);

    if (t->tcsr & NPCM7XX_TCSR_PERIODIC) {
        t->remaining_ns = npcm7xx_timer_count_to_ns(t, t->ticr);
        if (t->tcsr & NPCM7XX_TCSR_CEN) {
            npcm7xx_timer_start(t);
        }
    } else {
        t->tcsr &= ~(NPCM7XX_TCSR_CEN | NPCM7XX_TCSR_CACT);
    }

    npcm7xx_timer_check_interrupt(t);
}

/* Stop counting. Record the time remaining so we can continue later. */
static void npcm7xx_timer_pause(NPCM7xxTimer *t)
{
    int64_t now;

    timer_del(&t->qtimer);
    now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    t->remaining_ns = t->expires_ns - now;
    g_assert(t->remaining_ns > 0);
}

/*
 * Restart the timer from its initial value. If the timer was enabled and stays
 * enabled, adjust the QEMU timer according to the new count. If the timer is
 * transitioning from disabled to enabled, the caller is expected to start the
 * timer later.
 */
static void npcm7xx_timer_restart(NPCM7xxTimer *t, uint32_t old_tcsr)
{
    t->remaining_ns = npcm7xx_timer_count_to_ns(t, t->ticr);

    if (old_tcsr & t->tcsr & NPCM7XX_TCSR_CEN) {
        npcm7xx_timer_start(t);
    }
}

/* Register read and write handlers */

static void npcm7xx_timer_write_tcsr(NPCM7xxTimer *t, uint32_t new_tcsr)
{
    uint32_t old_tcsr = t->tcsr;

    if (new_tcsr & NPCM7XX_TCSR_RSVD) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: reserved bits in 0x%08x ignored\n",
                      __func__, new_tcsr);
        new_tcsr &= ~NPCM7XX_TCSR_RSVD;
    }
    if (new_tcsr & NPCM7XX_TCSR_CACT) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: read-only bits in 0x%08x ignored\n",
                      __func__, new_tcsr);
        new_tcsr &= ~NPCM7XX_TCSR_CACT;
    }

    t->tcsr = (t->tcsr & NPCM7XX_TCSR_CACT) | new_tcsr;

    if ((old_tcsr ^ new_tcsr) & NPCM7XX_TCSR_IE) {
        npcm7xx_timer_check_interrupt(t);
    }
    if (new_tcsr & NPCM7XX_TCSR_CRST) {
        npcm7xx_timer_restart(t, old_tcsr);
        t->tcsr &= ~NPCM7XX_TCSR_CRST;
    }
    if ((old_tcsr ^ new_tcsr) & NPCM7XX_TCSR_CEN) {
        if (new_tcsr & NPCM7XX_TCSR_CEN) {
            npcm7xx_timer_start(t);
        } else {
            npcm7xx_timer_pause(t);
        }
    }
}

static void npcm7xx_timer_write_ticr(NPCM7xxTimer *t, uint32_t new_ticr)
{
    t->ticr = new_ticr;

    npcm7xx_timer_restart(t, t->tcsr);
}

static uint32_t npcm7xx_timer_read_tdr(NPCM7xxTimer *t)
{
    if (t->tcsr & NPCM7XX_TCSR_CEN) {
        int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

        return npcm7xx_timer_ns_to_count(t, t->expires_ns - now);
    }

    return npcm7xx_timer_ns_to_count(t, t->remaining_ns);
}

static hwaddr npcm7xx_tcsr_index(hwaddr reg)
{
    switch (reg) {
    case NPCM7XX_TIMER_TCSR0:
        return 0;
    case NPCM7XX_TIMER_TCSR1:
        return 1;
    case NPCM7XX_TIMER_TCSR2:
        return 2;
    case NPCM7XX_TIMER_TCSR3:
        return 3;
    case NPCM7XX_TIMER_TCSR4:
        return 4;
    default:
        g_assert_not_reached();
    }
}

static hwaddr npcm7xx_ticr_index(hwaddr reg)
{
    switch (reg) {
    case NPCM7XX_TIMER_TICR0:
        return 0;
    case NPCM7XX_TIMER_TICR1:
        return 1;
    case NPCM7XX_TIMER_TICR2:
        return 2;
    case NPCM7XX_TIMER_TICR3:
        return 3;
    case NPCM7XX_TIMER_TICR4:
        return 4;
    default:
        g_assert_not_reached();
    }
}

static hwaddr npcm7xx_tdr_index(hwaddr reg)
{
    switch (reg) {
    case NPCM7XX_TIMER_TDR0:
        return 0;
    case NPCM7XX_TIMER_TDR1:
        return 1;
    case NPCM7XX_TIMER_TDR2:
        return 2;
    case NPCM7XX_TIMER_TDR3:
        return 3;
    case NPCM7XX_TIMER_TDR4:
        return 4;
    default:
        g_assert_not_reached();
    }
}

static uint64_t npcm7xx_timer_read(void *opaque, hwaddr offset, unsigned size)
{
    NPCM7xxTimerCtrlState *s = opaque;
    uint64_t value = 0;
    hwaddr reg;

    reg = offset / sizeof(uint32_t);
    switch (reg) {
    case NPCM7XX_TIMER_TCSR0:
    case NPCM7XX_TIMER_TCSR1:
    case NPCM7XX_TIMER_TCSR2:
    case NPCM7XX_TIMER_TCSR3:
    case NPCM7XX_TIMER_TCSR4:
        value = s->timer[npcm7xx_tcsr_index(reg)].tcsr;
        break;

    case NPCM7XX_TIMER_TICR0:
    case NPCM7XX_TIMER_TICR1:
    case NPCM7XX_TIMER_TICR2:
    case NPCM7XX_TIMER_TICR3:
    case NPCM7XX_TIMER_TICR4:
        value = s->timer[npcm7xx_ticr_index(reg)].ticr;
        break;

    case NPCM7XX_TIMER_TDR0:
    case NPCM7XX_TIMER_TDR1:
    case NPCM7XX_TIMER_TDR2:
    case NPCM7XX_TIMER_TDR3:
    case NPCM7XX_TIMER_TDR4:
        value = npcm7xx_timer_read_tdr(&s->timer[npcm7xx_tdr_index(reg)]);
        break;

    case NPCM7XX_TIMER_TISR:
        value = s->tisr;
        break;

    case NPCM7XX_TIMER_WTCR:
        value = s->wtcr;
        break;

    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: invalid offset 0x%04" HWADDR_PRIx "\n",
                      __func__, offset);
        break;
    }

    trace_npcm7xx_timer_read(DEVICE(s)->canonical_path, offset, value);

    return value;
}

static void npcm7xx_timer_write(void *opaque, hwaddr offset,
                                uint64_t v, unsigned size)
{
    uint32_t reg = offset / sizeof(uint32_t);
    NPCM7xxTimerCtrlState *s = opaque;
    uint32_t value = v;

    trace_npcm7xx_timer_write(DEVICE(s)->canonical_path, offset, value);

    switch (reg) {
    case NPCM7XX_TIMER_TCSR0:
    case NPCM7XX_TIMER_TCSR1:
    case NPCM7XX_TIMER_TCSR2:
    case NPCM7XX_TIMER_TCSR3:
    case NPCM7XX_TIMER_TCSR4:
        npcm7xx_timer_write_tcsr(&s->timer[npcm7xx_tcsr_index(reg)], value);
        return;

    case NPCM7XX_TIMER_TICR0:
    case NPCM7XX_TIMER_TICR1:
    case NPCM7XX_TIMER_TICR2:
    case NPCM7XX_TIMER_TICR3:
    case NPCM7XX_TIMER_TICR4:
        npcm7xx_timer_write_ticr(&s->timer[npcm7xx_ticr_index(reg)], value);
        return;

    case NPCM7XX_TIMER_TDR0:
    case NPCM7XX_TIMER_TDR1:
    case NPCM7XX_TIMER_TDR2:
    case NPCM7XX_TIMER_TDR3:
    case NPCM7XX_TIMER_TDR4:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: register @ 0x%04" HWADDR_PRIx " is read-only\n",
                      __func__, offset);
        return;

    case NPCM7XX_TIMER_TISR:
        s->tisr &= ~value;
        return;

    case NPCM7XX_TIMER_WTCR:
        qemu_log_mask(LOG_UNIMP, "%s: WTCR write not implemented: 0x%08x\n",
                      __func__, value);
        return;
    }

    qemu_log_mask(LOG_GUEST_ERROR,
                  "%s: invalid offset 0x%04" HWADDR_PRIx "\n",
                  __func__, offset);
}

static const struct MemoryRegionOps npcm7xx_timer_ops = {
    .read       = npcm7xx_timer_read,
    .write      = npcm7xx_timer_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid      = {
        .min_access_size        = 4,
        .max_access_size        = 4,
        .unaligned              = false,
    },
};

/* Called when the QEMU timer expires. */
static void npcm7xx_timer_expired(void *opaque)
{
    NPCM7xxTimer *t = opaque;

    if (t->tcsr & NPCM7XX_TCSR_CEN) {
        npcm7xx_timer_reached_zero(t);
    }
}

static void npcm7xx_timer_enter_reset(Object *obj, ResetType type)
{
    NPCM7xxTimerCtrlState *s = NPCM7XX_TIMER(obj);
    int i;

    for (i = 0; i < NPCM7XX_TIMERS_PER_CTRL; i++) {
        NPCM7xxTimer *t = &s->timer[i];

        timer_del(&t->qtimer);
        t->expires_ns = 0;
        t->remaining_ns = 0;
        t->tcsr = 0x00000005;
        t->ticr = 0x00000000;
    }

    s->tisr = 0x00000000;
    s->wtcr = 0x00000400;
}

static void npcm7xx_timer_hold_reset(Object *obj)
{
    NPCM7xxTimerCtrlState *s = NPCM7XX_TIMER(obj);
    int i;

    for (i = 0; i < NPCM7XX_TIMERS_PER_CTRL; i++) {
        qemu_irq_lower(s->timer[i].irq);
    }
}

static void npcm7xx_timer_realize(DeviceState *dev, Error **errp)
{
    NPCM7xxTimerCtrlState *s = NPCM7XX_TIMER(dev);
    SysBusDevice *sbd = &s->parent;
    int i;

    for (i = 0; i < NPCM7XX_TIMERS_PER_CTRL; i++) {
        NPCM7xxTimer *t = &s->timer[i];
        t->ctrl = s;
        timer_init_ns(&t->qtimer, QEMU_CLOCK_VIRTUAL, npcm7xx_timer_expired, t);
        sysbus_init_irq(sbd, &t->irq);
    }

    memory_region_init_io(&s->iomem, OBJECT(s), &npcm7xx_timer_ops, s,
                          TYPE_NPCM7XX_TIMER, 4 * KiB);
    sysbus_init_mmio(sbd, &s->iomem);
}

static const VMStateDescription vmstate_npcm7xx_timer = {
    .name = "npcm7xx-timer",
    .version_id = 0,
    .minimum_version_id = 0,
    .fields = (VMStateField[]) {
        VMSTATE_TIMER(qtimer, NPCM7xxTimer),
        VMSTATE_INT64(expires_ns, NPCM7xxTimer),
        VMSTATE_INT64(remaining_ns, NPCM7xxTimer),
        VMSTATE_UINT32(tcsr, NPCM7xxTimer),
        VMSTATE_UINT32(ticr, NPCM7xxTimer),
        VMSTATE_END_OF_LIST(),
    },
};

static const VMStateDescription vmstate_npcm7xx_timer_ctrl = {
    .name = "npcm7xx-timer-ctrl",
    .version_id = 0,
    .minimum_version_id = 0,
    .fields = (VMStateField[]) {
        VMSTATE_UINT32(tisr, NPCM7xxTimerCtrlState),
        VMSTATE_UINT32(wtcr, NPCM7xxTimerCtrlState),
        VMSTATE_STRUCT_ARRAY(timer, NPCM7xxTimerCtrlState,
                             NPCM7XX_TIMERS_PER_CTRL, 0, vmstate_npcm7xx_timer,
                             NPCM7xxTimer),
        VMSTATE_END_OF_LIST(),
    },
};

static void npcm7xx_timer_class_init(ObjectClass *klass, void *data)
{
    ResettableClass *rc = RESETTABLE_CLASS(klass);
    DeviceClass *dc = DEVICE_CLASS(klass);

    QEMU_BUILD_BUG_ON(NPCM7XX_TIMER_REGS_END > NPCM7XX_TIMER_NR_REGS);

    dc->desc = "NPCM7xx Timer Controller";
    dc->realize = npcm7xx_timer_realize;
    dc->vmsd = &vmstate_npcm7xx_timer_ctrl;
    rc->phases.enter = npcm7xx_timer_enter_reset;
    rc->phases.hold = npcm7xx_timer_hold_reset;
}

static const TypeInfo npcm7xx_timer_info = {
    .name               = TYPE_NPCM7XX_TIMER,
    .parent             = TYPE_SYS_BUS_DEVICE,
    .instance_size      = sizeof(NPCM7xxTimerCtrlState),
    .class_init         = npcm7xx_timer_class_init,
};

static void npcm7xx_timer_register_type(void)
{
    type_register_static(&npcm7xx_timer_info);
}
type_init(npcm7xx_timer_register_type);
