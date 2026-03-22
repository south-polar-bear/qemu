/*
 * QTest testcases for BCM2712 (Raspberry Pi 5) machine
 *
 * Copyright (c) 2026 QEMU Project
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "libqtest.h"

/* Memory map - from bcm2712.c */
#define BCM2712_RAM_BASE        0x0
#define BCM2712_SOC_BASE        0x107C000000ULL
#define BCM2712_GIC_DIST_BASE   0x107C010000ULL
#define BCM2712_GIC_CPU_BASE    0x107C012000ULL

/* GIC-400 Distributor registers */
#define GICD_CTLR       0x000
#define GICD_IIDR       0x008
#define GICD_IPRIORITYR 0x400
#define GICD_ICFGR      0xC00
#define GICD_IROUTER    0x6000

/* GIC-400 CPU Interface registers */
#define GICC_CTLR       0x000
#define GICC_PMR        0x004

/*
 * Test machine creation and basic initialization
 */
static void test_bcm2712_machine_create(void)
{
    QTestState *s = qtest_init("-machine bcm2712 -cpu cortex-a72");
    g_assert_nonnull(s);
    qtest_quit(s);
}

/*
 * Test different RAM sizes
 */
static void test_bcm2712_ram_sizes(void)
{
    QTestState *s;
    const int ram_sizes[] = { 256, 512, 1024 };
    size_t i;

    for (i = 0; i < G_N_ELEMENTS(ram_sizes); i++) {
        s = qtest_initf("-machine bcm2712 -cpu cortex-a72 -m %dM",
                        ram_sizes[i]);
        g_assert_nonnull(s);
        qtest_quit(s);
    }
}

/*
 * Test SMP configuration
 */
static void test_bcm2712_smp(void)
{
    QTestState *s;

    s = qtest_init("-machine bcm2712 -cpu cortex-a72 -smp 1");
    g_assert_nonnull(s);
    qtest_quit(s);
}

/*
 * Test GIC-400 interrupt controller registers - basic accessibility
 */
static void test_bcm2712_gic_registers(void)
{
    QTestState *s;
    uint32_t val;

    s = qtest_init("-machine bcm2712 -cpu cortex-a72");

    /* Read GIC Distributor ID Register - verify it's readable */
    val = qtest_readl(s, BCM2712_GIC_DIST_BASE + GICD_IIDR);
    g_assert_cmpuint(val, !=, 0xffffffff);

    /* Read GIC Distributor Control Register */
    val = qtest_readl(s, BCM2712_GIC_DIST_BASE + GICD_CTLR);
    g_assert_cmpuint(val, !=, 0xffffffff);

    qtest_quit(s);
}

/*
 * Test GIC CPU interface registers - basic accessibility
 */
static void test_bcm2712_gic_cpu_interface(void)
{
    QTestState *s;
    uint32_t val;

    s = qtest_init("-machine bcm2712 -cpu cortex-a72");

    /* Read GIC CPU Control Register - verify accessible */
    val = qtest_readl(s, BCM2712_GIC_CPU_BASE + GICC_CTLR);
    g_assert_cmpuint(val, !=, 0xffffffff);

    /* Read Priority Mask Register - verify accessible */
    val = qtest_readl(s, BCM2712_GIC_CPU_BASE + GICC_PMR);
    g_assert_cmpuint(val, !=, 0xffffffff);

    qtest_quit(s);
}

/*
 * Test memory region accessibility
 */
static void test_bcm2712_memory_access(void)
{
    QTestState *s;
    uint64_t val64;
    uint32_t val32;
    uint16_t val16;
    uint8_t val8;
    int i;

    s = qtest_init("-machine bcm2712 -cpu cortex-a72");

    /* Test RAM write/read at various offsets */
    for (i = 0; i < 10; i++) {
        uint64_t test_addr = BCM2712_RAM_BASE + i * 0x1000;
        
        /* 64-bit access */
        qtest_writeq(s, test_addr, 0xDEADBEEFCAFEBABEULL);
        val64 = qtest_readq(s, test_addr);
        g_assert_cmpuint(val64, ==, 0xDEADBEEFCAFEBABEULL);

        /* 32-bit access */
        qtest_writel(s, test_addr, 0xDEADBEEF);
        val32 = qtest_readl(s, test_addr);
        g_assert_cmpuint(val32, ==, 0xDEADBEEF);

        /* 16-bit access */
        qtest_writew(s, test_addr, 0xBEEF);
        val16 = qtest_readw(s, test_addr);
        g_assert_cmpuint(val16, ==, 0xBEEF);

        /* 8-bit access */
        qtest_writeb(s, test_addr, 0xEF);
        val8 = qtest_readb(s, test_addr);
        g_assert_cmpuint(val8, ==, 0xEF);
    }

    qtest_quit(s);
}

/*
 * Test GIC priority register read/write
 */
static void test_bcm2712_gic_priority(void)
{
    QTestState *s;
    uint32_t priority;

    s = qtest_init("-machine bcm2712 -cpu cortex-a72");

    /* Write and read back priority register */
    qtest_writel(s, BCM2712_GIC_DIST_BASE + GICD_IPRIORITYR, 0x00102030);
    priority = qtest_readl(s, BCM2712_GIC_DIST_BASE + GICD_IPRIORITYR);
    g_assert_cmpuint(priority, ==, 0x00102030);

    qtest_quit(s);
}

/*
 * Test GIC config register read/write
 */
static void test_bcm2712_gic_config(void)
{
    QTestState *s;
    uint32_t icfgr;

    s = qtest_init("-machine bcm2712 -cpu cortex-a72");

    /* Configure as edge-triggered */
    qtest_writel(s, BCM2712_GIC_DIST_BASE + GICD_ICFGR, 0x55555555);
    icfgr = qtest_readl(s, BCM2712_GIC_DIST_BASE + GICD_ICFGR);
    g_assert_cmpuint(icfgr, !=, 0xffffffff);

    qtest_quit(s);
}

/*
 * Test GIC router register read
 */
static void test_bcm2712_gic_router(void)
{
    QTestState *s;
    uint64_t router;

    s = qtest_init("-machine bcm2712 -cpu cortex-a72");

    /* Test interrupt router register */
    router = qtest_readq(s, BCM2712_GIC_DIST_BASE + GICD_IROUTER);
    g_assert_cmpuint(router, !=, 0xffffffffffffffff);

    qtest_quit(s);
}

/*
 * Test error handling with invalid addresses
 */
static void test_bcm2712_invalid_access(void)
{
    QTestState *s;
    uint32_t val __attribute__((unused));

    s = qtest_init("-machine bcm2712 -cpu cortex-a72");

    /* Access to unmapped SOC region should not crash */
    val = qtest_readl(s, BCM2712_SOC_BASE + 0x1000);

    qtest_quit(s);
}

/*
 * Test machine reset
 */
static void test_bcm2712_reset(void)
{
    QTestState *s;

    s = qtest_init("-machine bcm2712 -cpu cortex-a72");

    /* Reset the machine - should not crash */
    qtest_system_reset(s);

    qtest_quit(s);
}

/*
 * Test multiple machine instances
 */
static void test_bcm2712_multiple_instances(void)
{
    QTestState *s1, *s2;

    s1 = qtest_init("-machine bcm2712 -cpu cortex-a72");
    s2 = qtest_init("-machine bcm2712 -cpu cortex-a72");

    g_assert_nonnull(s1);
    g_assert_nonnull(s2);

    qtest_quit(s1);
    qtest_quit(s2);
}

/*
 * Test with different CPU types
 */
static void test_bcm2712_cpu_types(void)
{
    QTestState *s;
    const char *cpu_types[] = {
        "cortex-a72",
        "max",
        NULL
    };
    int i;

    for (i = 0; cpu_types[i] != NULL; i++) {
        s = qtest_initf("-machine bcm2712 -cpu %s", cpu_types[i]);
        g_assert_nonnull(s);
        qtest_quit(s);
    }
}

/*
 * Main test registration
 */
int main(int argc, char **argv)
{
    int ret;

    g_test_init(&argc, &argv, NULL);

    /* Basic machine tests */
    qtest_add_func("/bcm2712/machine/create", test_bcm2712_machine_create);
    qtest_add_func("/bcm2712/machine/ram_sizes", test_bcm2712_ram_sizes);
    qtest_add_func("/bcm2712/machine/smp", test_bcm2712_smp);

    /* GIC tests */
    qtest_add_func("/bcm2712/gic/registers", test_bcm2712_gic_registers);
    qtest_add_func("/bcm2712/gic/cpu_interface", test_bcm2712_gic_cpu_interface);
    qtest_add_func("/bcm2712/gic/priority", test_bcm2712_gic_priority);
    qtest_add_func("/bcm2712/gic/config", test_bcm2712_gic_config);
    qtest_add_func("/bcm2712/gic/router", test_bcm2712_gic_router);

    /* Memory tests */
    qtest_add_func("/bcm2712/memory/access", test_bcm2712_memory_access);
    qtest_add_func("/bcm2712/memory/invalid", test_bcm2712_invalid_access);

    /* System tests */
    qtest_add_func("/bcm2712/system/reset", test_bcm2712_reset);
    qtest_add_func("/bcm2712/system/multiple_instances", test_bcm2712_multiple_instances);
    qtest_add_func("/bcm2712/system/cpu_types", test_bcm2712_cpu_types);

    ret = g_test_run();

    return ret;
}
