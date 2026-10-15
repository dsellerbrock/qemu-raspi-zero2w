/*
 * QTests for the Raspberry Pi DWC2 USB controller.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest-single.h"

#define DWC2_HPRT0         0x3f980440
#define HPRT0_RST          (1U << 8)
#define HPRT0_ENA           (1U << 2)
#define HPRT0_CONNSTS       (1U << 0)

static char *trace_file;

static void test_root_port_reset_keeps_device_attached(void)
{
    uint32_t port = readl(DWC2_HPRT0);

    g_assert_cmphex(port & HPRT0_CONNSTS, !=, 0);
    writel(DWC2_HPRT0, port | HPRT0_RST);
    writel(DWC2_HPRT0, port & ~HPRT0_RST);

    port = readl(DWC2_HPRT0);
    g_assert_cmphex(port & HPRT0_CONNSTS, !=, 0);
    g_assert_cmphex(port & HPRT0_ENA, !=, 0);
}

int main(int argc, char **argv)
{
    int ret;
    char *qtest_args;
    char *trace;
    gsize trace_len;

    g_test_init(&argc, &argv, NULL);
    trace_file = g_strdup_printf("%s/qtest-usb-dwc2-%u.log", g_get_tmp_dir(),
                                 (unsigned)getpid());
    qtest_args = g_strdup_printf("-machine raspi3ap -smp 4 -usb -device usb-kbd "
                                 "-d trace:usb_dwc2_detach,"
                                 "trace:usb_dwc2_hreg0_action -D %s", trace_file);
    qtest_add_func("/bcm2835/usb/root_port_reset_keeps_device_attached",
                   test_root_port_reset_keeps_device_attached);
    qtest_start(qtest_args);
    ret = g_test_run();
    qtest_end();

    if (ret == 0) {
        GError *error = NULL;
        const char *detach;
        gsize detach_offset;

        g_assert_true(g_file_get_contents(trace_file, &trace, &trace_len,
                                          &error));
        g_assert_no_error(error);
        g_assert_nonnull(g_strstr_len(trace, trace_len,
                                      "call usb_device_reset"));
        detach = g_strstr_len(trace, trace_len, "usb_dwc2_detach");
        g_assert_nonnull(detach);
        /* The fixture emits one detach; a port reset must not add another. */
        detach_offset = detach - trace + strlen("usb_dwc2_detach");
        g_assert_null(g_strstr_len(trace + detach_offset,
                                   trace_len - detach_offset,
                                   "usb_dwc2_detach"));
        g_free(trace);
    }

    g_unlink(trace_file);
    g_free(trace_file);
    g_free(qtest_args);
    return ret;
}
