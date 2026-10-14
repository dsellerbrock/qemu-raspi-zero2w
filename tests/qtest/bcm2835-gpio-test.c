/*
 * QTest testcase for BCM2835 GPIO edge interrupts (on Raspberry Pi 3).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest-single.h"

#define RASPI3_GPIO_BASE 0x3f200000
#define RASPI3_IC_BASE   0x3f00b200

#define GPSET0       0x1c
#define GPSET1       0x20
#define GPCLR0       0x28
#define GPCLR1       0x2c
#define GPLEV0       0x34
#define GPLEV1       0x38
#define GPEDS0       0x40
#define GPEDS1       0x44
#define GPREN0       0x4c
#define GPREN1       0x50
#define GPFEN0       0x58
#define GPFEN1       0x5c

#define IRQ_PENDING_2 0x08
#define IRQ_ENABLE_2  0x14

#define GPIO_IRQ_BASE 49
#define GPIO_IRQ_COUNT 4
#define GPIO_BANK1_MASK 0x003fffff

static uint32_t gpio_pin_mask(unsigned pin)
{
    return 1U << (pin % 32);
}

static bool gpio_irq_line_asserted(unsigned pin, unsigned line)
{
    if (line == 3) {
        return true;
    }
    return pin < 32 ? line == 0 : (line == 1 || line == 2);
}

static uint32_t gpio_irq_mask(unsigned line)
{
    return 1U << (GPIO_IRQ_BASE + line - 32);
}

static void gpio_drive(unsigned pin, bool high)
{
    uint32_t offset;

    if (pin < 32) {
        offset = high ? GPSET0 : GPCLR0;
    } else {
        offset = high ? GPSET1 : GPCLR1;
    }
    writel(RASPI3_GPIO_BASE + offset, gpio_pin_mask(pin));
}

static uint32_t gpio_read_bank(unsigned reg0, unsigned reg1, unsigned pin)
{
    return readl(RASPI3_GPIO_BASE + (pin < 32 ? reg0 : reg1));
}

static uint32_t gpio_level(unsigned pin)
{
    return gpio_read_bank(GPLEV0, GPLEV1, pin) & gpio_pin_mask(pin);
}

static uint32_t gpio_pending(void)
{
    return readl(RASPI3_IC_BASE + IRQ_PENDING_2);
}

static void gpio_assert_expected_irqs(unsigned pin, bool asserted)
{
    unsigned line;

    for (line = 0; line < GPIO_IRQ_COUNT; line++) {
        bool expected = asserted && gpio_irq_line_asserted(pin, line);
        g_assert_cmphex(gpio_pending() & gpio_irq_mask(line),
                        ==, expected ? gpio_irq_mask(line) : 0);
    }
}

static void test_gpio_edges_and_irq_routing(void)
{
    unsigned pin;

    writel(RASPI3_GPIO_BASE + GPREN0, 0);
    writel(RASPI3_GPIO_BASE + GPREN1, 0);
    writel(RASPI3_GPIO_BASE + GPFEN0, 0);
    writel(RASPI3_GPIO_BASE + GPFEN1, 0);
    writel(RASPI3_GPIO_BASE + GPEDS0, UINT32_MAX);
    writel(RASPI3_GPIO_BASE + GPEDS1, GPIO_BANK1_MASK);
    writel(RASPI3_IC_BASE + IRQ_ENABLE_2,
           gpio_irq_mask(0) | gpio_irq_mask(1) |
           gpio_irq_mask(2) | gpio_irq_mask(3));

    for (pin = 0; pin < 54; pin++) {
        unsigned bank = pin < 32 ? 0 : 1;
        uint32_t ren = bank ? GPREN1 : GPREN0;
        uint32_t fen = bank ? GPFEN1 : GPFEN0;
        uint32_t eds = bank ? GPEDS1 : GPEDS0;
        uint32_t mask = gpio_pin_mask(pin);

        writel(RASPI3_GPIO_BASE + GPREN0, 0);
        writel(RASPI3_GPIO_BASE + GPREN1, 0);
        writel(RASPI3_GPIO_BASE + GPFEN0, 0);
        writel(RASPI3_GPIO_BASE + GPFEN1, 0);
        writel(RASPI3_GPIO_BASE + GPEDS0, UINT32_MAX);
        writel(RASPI3_GPIO_BASE + GPEDS1, GPIO_BANK1_MASK);
        gpio_drive(pin, false);
        writel(RASPI3_GPIO_BASE + ren, mask);
        writel(RASPI3_GPIO_BASE + fen, mask);

        gpio_drive(pin, true);
        g_assert_cmphex(gpio_level(pin), ==, mask);
        g_assert_cmphex(readl(RASPI3_GPIO_BASE + eds) & mask, ==, mask);
        gpio_assert_expected_irqs(pin, true);

        writel(RASPI3_GPIO_BASE + eds, mask);
        gpio_assert_expected_irqs(pin, false);
        gpio_drive(pin, false);
        g_assert_cmphex(gpio_level(pin), ==, 0);
        g_assert_cmphex(readl(RASPI3_GPIO_BASE + eds) & mask, ==, mask);
        gpio_assert_expected_irqs(pin, true);

        writel(RASPI3_GPIO_BASE + eds, mask);
        gpio_assert_expected_irqs(pin, false);
    }
}

static void test_gpio_irq_remains_asserted_for_other_events(void)
{
    const uint32_t first = 1U << 6;
    const uint32_t second = 1U << 7;
    const uint32_t irq_mask0 = gpio_irq_mask(0);
    const uint32_t irq_mask3 = gpio_irq_mask(3);

    writel(RASPI3_GPIO_BASE + GPREN0, first | second);
    writel(RASPI3_GPIO_BASE + GPFEN0, 0);
    writel(RASPI3_GPIO_BASE + GPEDS0, UINT32_MAX);
    gpio_drive(6, false);
    gpio_drive(7, false);
    gpio_drive(6, true);
    gpio_drive(7, true);

    g_assert_cmphex(readl(RASPI3_GPIO_BASE + GPEDS0) & (first | second),
                    ==, first | second);
    g_assert_cmphex(gpio_pending() & (irq_mask0 | irq_mask3),
                    ==, irq_mask0 | irq_mask3);

    writel(RASPI3_GPIO_BASE + GPEDS0, first);
    g_assert_cmphex(readl(RASPI3_GPIO_BASE + GPEDS0) & (first | second),
                    ==, second);
    g_assert_cmphex(gpio_pending() & (irq_mask0 | irq_mask3),
                    ==, irq_mask0 | irq_mask3);

    writel(RASPI3_GPIO_BASE + GPEDS0, second);
    g_assert_cmphex(gpio_pending() & (irq_mask0 | irq_mask3), ==, 0);
}

int main(int argc, char **argv)
{
    int ret;

    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/bcm2835/gpio/edges_and_irq_routing",
                   test_gpio_edges_and_irq_routing);
    qtest_add_func("/bcm2835/gpio/irq_remains_asserted_for_other_events",
                   test_gpio_irq_remains_asserted_for_other_events);
    qtest_start("-machine raspi3ap -smp 4");
    ret = g_test_run();
    qtest_end();
    return ret;
}
