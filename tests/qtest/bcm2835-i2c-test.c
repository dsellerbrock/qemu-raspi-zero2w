/*
 * QTest testcase for Broadcom Serial Controller (BSC)
 *
 * Copyright (c) 2024 Rayhan Faizel <rayhan.faizel@gmail.com>
 *
 * SPDX-License-Identifier: MIT
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/osdep.h"
#include "libqtest-single.h"

#include "hw/i2c/bcm2835_i2c.h"
#include "hw/sensor/tmp105_regs.h"

static const uint32_t bsc_base_addrs[] = {
    0x3f205000,                         /* I2C0 */
    0x3f804000,                         /* I2C1 */
    0x3f805000,                         /* I2C2 */
};

static void bcm2835_i2c_init_transfer(uint32_t base_addr, bool read)
{
    /* read flag is bit 0 so we can write it directly */
    int interrupt = read ? BCM2835_I2C_C_INTR : BCM2835_I2C_C_INTT;

    writel(base_addr + BCM2835_I2C_C,
           BCM2835_I2C_C_I2CEN | BCM2835_I2C_C_INTD |
           BCM2835_I2C_C_ST | BCM2835_I2C_C_CLEAR | interrupt | read);
}

static void test_i2c_read_write(gconstpointer data)
{
    uint32_t i2cdata;
    intptr_t index = (intptr_t) data;
    uint32_t base_addr = bsc_base_addrs[index];

    /* Write to TMP105 register */
    writel(base_addr + BCM2835_I2C_A, 0x50);
    writel(base_addr + BCM2835_I2C_DLEN, 3);

    bcm2835_i2c_init_transfer(base_addr, 0);

    writel(base_addr + BCM2835_I2C_FIFO, TMP105_REG_T_HIGH);
    writel(base_addr + BCM2835_I2C_FIFO, 0xde);
    writel(base_addr + BCM2835_I2C_FIFO, 0xad);

    /* Clear flags */
    writel(base_addr + BCM2835_I2C_S, BCM2835_I2C_S_DONE | BCM2835_I2C_S_ERR |
                                      BCM2835_I2C_S_CLKT);

    /* Read from TMP105 register */
    writel(base_addr + BCM2835_I2C_A, 0x50);
    writel(base_addr + BCM2835_I2C_DLEN, 1);

    bcm2835_i2c_init_transfer(base_addr, 0);

    writel(base_addr + BCM2835_I2C_FIFO, TMP105_REG_T_HIGH);

    writel(base_addr + BCM2835_I2C_DLEN, 2);
    bcm2835_i2c_init_transfer(base_addr, 1);

    i2cdata = readl(base_addr + BCM2835_I2C_FIFO);
    g_assert_cmpint(i2cdata, ==, 0xde);

    i2cdata = readl(base_addr + BCM2835_I2C_FIFO);
    g_assert_cmpint(i2cdata, ==, 0xa0);

    /* Clear flags */
    writel(base_addr + BCM2835_I2C_S, BCM2835_I2C_S_DONE | BCM2835_I2C_S_ERR |
                                      BCM2835_I2C_S_CLKT);

}

static void test_i2c_done_w1c_deasserts_irq(void)
{
    uint32_t base_addr = bsc_base_addrs[2];

    /* Select a TMP105 register before checking the completed read interrupt. */
    writel(base_addr + BCM2835_I2C_A, 0x50);
    writel(base_addr + BCM2835_I2C_DLEN, 1);
    bcm2835_i2c_init_transfer(base_addr, 0);
    writel(base_addr + BCM2835_I2C_FIFO, TMP105_REG_T_HIGH);
    writel(base_addr + BCM2835_I2C_S, BCM2835_I2C_S_DONE |
           BCM2835_I2C_S_ERR | BCM2835_I2C_S_CLKT);

    writel(base_addr + BCM2835_I2C_A, 0x50);
    writel(base_addr + BCM2835_I2C_DLEN, 2);
    bcm2835_i2c_init_transfer(base_addr, 1);
    readl(base_addr + BCM2835_I2C_FIFO);
    readl(base_addr + BCM2835_I2C_FIFO);

    g_assert_true(readl(base_addr + BCM2835_I2C_S) & BCM2835_I2C_S_DONE);
    g_assert_true(qtest_get_irq(global_qtest, 0));

    writel(base_addr + BCM2835_I2C_S, BCM2835_I2C_S_DONE);

    g_assert_false(qtest_get_irq(global_qtest, 0));
}

static void test_i2c_read_fifo_status(void)
{
    uint32_t base_addr = bsc_base_addrs[2];
    uint32_t status;
    int i;

    writel(base_addr + BCM2835_I2C_A, 0x50);
    writel(base_addr + BCM2835_I2C_DLEN, 1);
    bcm2835_i2c_init_transfer(base_addr, 0);
    writel(base_addr + BCM2835_I2C_FIFO, TMP105_REG_T_HIGH);
    writel(base_addr + BCM2835_I2C_S, BCM2835_I2C_S_DONE |
           BCM2835_I2C_S_ERR | BCM2835_I2C_S_CLKT);

    writel(base_addr + BCM2835_I2C_A, 0x50);
    writel(base_addr + BCM2835_I2C_DLEN, 16);
    bcm2835_i2c_init_transfer(base_addr, 1);

    status = readl(base_addr + BCM2835_I2C_S);
    g_assert_true(status & BCM2835_I2C_S_DONE);
    g_assert_true(status & BCM2835_I2C_S_RXD);
    g_assert_true(status & BCM2835_I2C_S_RXF);
    g_assert_true(status & BCM2835_I2C_S_RXR);
    g_assert_false(status & BCM2835_I2C_S_TA);
    g_assert_cmpuint(readl(base_addr + BCM2835_I2C_DLEN), ==, 0);

    g_assert_cmpuint(readl(base_addr + BCM2835_I2C_FIFO), ==, 0xde);
    status = readl(base_addr + BCM2835_I2C_S);
    g_assert_true(status & BCM2835_I2C_S_RXD);
    g_assert_false(status & BCM2835_I2C_S_RXR);
    g_assert_cmpuint(readl(base_addr + BCM2835_I2C_FIFO), ==, 0xa0);
    for (i = 2; i < 16; i++) {
        readl(base_addr + BCM2835_I2C_FIFO);
    }
    status = readl(base_addr + BCM2835_I2C_S);
    g_assert_false(status & BCM2835_I2C_S_RXD);
    g_assert_true(status & BCM2835_I2C_S_DONE);
    g_assert_true(qtest_get_irq(global_qtest, 0));

    writel(base_addr + BCM2835_I2C_S, BCM2835_I2C_S_DONE);
    g_assert_false(qtest_get_irq(global_qtest, 0));
}

static void test_i2c_clear_aborts_active_transfer(void)
{
    uint32_t base_addr = bsc_base_addrs[2];
    uint32_t status;

    writel(base_addr + BCM2835_I2C_A, 0x50);
    writel(base_addr + BCM2835_I2C_DLEN, 2);
    writel(base_addr + BCM2835_I2C_C,
           BCM2835_I2C_C_I2CEN | BCM2835_I2C_C_READ);
    g_assert_false(readl(base_addr + BCM2835_I2C_S) & BCM2835_I2C_S_TA);

    writel(base_addr + BCM2835_I2C_C, BCM2835_I2C_C_I2CEN |
           BCM2835_I2C_C_INTR | BCM2835_I2C_C_ST | BCM2835_I2C_C_READ);
    g_assert_true(readl(base_addr + BCM2835_I2C_S) & BCM2835_I2C_S_TA);

    writel(base_addr + BCM2835_I2C_C, BCM2835_I2C_C_CLEAR);

    status = readl(base_addr + BCM2835_I2C_S);
    g_assert_false(readl(base_addr + BCM2835_I2C_C) & BCM2835_I2C_C_CLEAR);
    g_assert_false(status & (BCM2835_I2C_S_TA | BCM2835_I2C_S_RXD |
                             BCM2835_I2C_S_RXR | BCM2835_I2C_S_DONE));
    g_assert_cmpuint(readl(base_addr + BCM2835_I2C_DLEN), ==, 2);
}

static void test_i2c_missing_address_completes_with_error(void)
{
    uint32_t base_addr = bsc_base_addrs[2];
    uint32_t status;

    writel(base_addr + BCM2835_I2C_A, 0x51);
    writel(base_addr + BCM2835_I2C_DLEN, 2);
    writel(base_addr + BCM2835_I2C_C, BCM2835_I2C_C_I2CEN |
           BCM2835_I2C_C_INTR | BCM2835_I2C_C_INTD | BCM2835_I2C_C_ST |
           BCM2835_I2C_C_READ | BCM2835_I2C_C_CLEAR);

    status = readl(base_addr + BCM2835_I2C_S);
    g_assert_true(status & BCM2835_I2C_S_ERR);
    g_assert_true(status & BCM2835_I2C_S_DONE);
    g_assert_false(status & (BCM2835_I2C_S_TA | BCM2835_I2C_S_RXD |
                             BCM2835_I2C_S_RXR));
    g_assert_true(qtest_get_irq(global_qtest, 0));

    writel(base_addr + BCM2835_I2C_S, BCM2835_I2C_S_DONE |
           BCM2835_I2C_S_ERR | BCM2835_I2C_S_CLKT);
    g_assert_false(qtest_get_irq(global_qtest, 0));
}

int main(int argc, char **argv)
{
    int ret;
    int i;

    g_test_init(&argc, &argv, NULL);

    for (i = 0; i < 3; i++) {
        g_autofree char *test_name =
        g_strdup_printf("/bcm2835/bcm2835-i2c%d/read_write", i);
        qtest_add_data_func(test_name, (void *)(intptr_t) i,
                            test_i2c_read_write);
    }
    qtest_add_func("/bcm2835/bcm2835-i2c/done_w1c_deasserts_irq",
                   test_i2c_done_w1c_deasserts_irq);
    qtest_add_func("/bcm2835/bcm2835-i2c/read_fifo_status",
                   test_i2c_read_fifo_status);
    qtest_add_func("/bcm2835/bcm2835-i2c/clear_aborts_active_transfer",
                   test_i2c_clear_aborts_active_transfer);
    qtest_add_func("/bcm2835/bcm2835-i2c/missing_address_completes_with_error",
                   test_i2c_missing_address_completes_with_error);

    /* Run I2C tests with TMP105 slaves on all three buses */
    qtest_start("-M raspi3b "
                "-device tmp105,address=0x50,bus=i2c-bus.0 "
                "-device tmp105,address=0x50,bus=i2c-bus.1 "
                "-device tmp105,address=0x50,bus=i2c-bus.2");
    qtest_irq_intercept_out_named(global_qtest,
                                  "/machine/soc/peripherals/bcm2835-i2c2",
                                  "sysbus-irq");
    ret = g_test_run();
    qtest_end();

    return ret;
}
