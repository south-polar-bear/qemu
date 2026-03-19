/*
 * QEMU Raspberry Pi 5 (BCM2712) machine - Single Core Debug Version
 * 
 * Minimal implementation for debugging:
 * - 1x Cortex-A72 core (simpler, easier to debug)
 * - 1GB RAM starting at 0x0
 * - GIC-400 interrupt controller
 * - ARM Generic Timer (CPU internal)
 * - Device tree generation
 * 
 * This machine is designed for kernel debugging with minimal dependencies.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "hw/arm/raspi_platform.h"
#include "hw/core/sysbus.h"
#include "hw/arm/bcm2838.h"
#include "trace.h"


/* Memory map - based on BCM2712 datasheet */
#define BCM2712_RAM_BASE        0x0                 /* RAM starts at 0x0 */
#define BCM2712_RAM_SIZE        (1 * GiB)           /* 1GB for debugging */

/* SoC peripherals base - from device tree */
#define BCM2712_SOC_BASE        0x107C000000ULL     /* SoC peripheral base */
#define BCM2712_SOC_SIZE        0x4000000           /* 64MB for peripherals */

/* GIC-400 registers */
#define BCM2712_GIC_DIST_BASE   0x107C010000ULL     /* GIC distributor */
#define BCM2712_GIC_CPU_BASE    0x107C012000ULL     /* GIC CPU interface */
#define BCM2712_GIC_SIZE        0x2000              /* Each region size */

/* GIC configuration - single core */
#define BCM2712_GIC_NUM_CPU     1                   /* Single core for debugging */
#define BCM2712_GIC_NUM_IRQ     160                 /* Number of interrupts */

/* Machine state */
struct BCM2712State {
    MachineState parent;
    DeviceState *gic;           /* GIC device pointer */
};

#define TYPE_BCM2712_MACHINE MACHINE_TYPE_NAME("bcm2712")
#define BCM2712_MACHINE(obj) \
    OBJECT_CHECK(BCM2712State, (obj), TYPE_BCM2712_MACHINE)

/* 
 * Initialize GIC-400 interrupt controller
 * This creates and configures the GIC, then connects it to the CPU.
 */
static void bcm2712_init_gic(struct BCM2712State *s, MachineState *machine,
                             MemoryRegion *sysmem)
{
    DeviceState *gicdev;
    SysBusDevice *sbd;
    int i;

    /* Create GIC device */
    gicdev = qdev_create(NULL, TYPE_ARM_GIC);
    sbd = SYS_BUS_DEVICE(gicdev);
    s->gic = gicdev;

    /* Set GIC properties */
    qdev_prop_set_uint32(gicdev, "num-cpu", BCM2712_GIC_NUM_CPU);
    qdev_prop_set_uint32(gicdev, "num-irq", BCM2712_GIC_NUM_IRQ);
    qdev_prop_set_bit(gicdev, "has-security-extensions", false);
    qdev_prop_set_bit(gicdev, "has-virtualization-extensions", true);

    /* Realize GIC (create the actual device) */
    qdev_init_nofail(gicdev);

    /* Map GIC registers into system memory */
    sysbus_mmio_map(sbd, 0, BCM2712_GIC_DIST_BASE);  /* Distributor */
    sysbus_mmio_map(sbd, 1, BCM2712_GIC_CPU_BASE);   /* CPU interface */

    /* Connect GIC to CPU core(s) */
    for (i = 0; i < BCM2712_GIC_NUM_CPU; i++) {
        ARMCPU *cp = ARM_CPU(qemu_get_cpu(i));
        if (!cp) {
            continue;
        }

        /* Connect IRQ and FIQ lines from CPU to GIC */
        sysbus_connect_irq(sbd, i, 
                          qdev_get_gpio_in(DEVICE(cp), ARM_CPU_IRQ));
        sysbus_connect_irq(sbd, i + BCM2712_GIC_NUM_CPU,
                          qdev_get_gpio_in(DEVICE(cp), ARM_CPU_FIQ));
        sysbus_connect_irq(sbd, i + 2 * BCM2712_GIC_NUM_CPU,
                          qdev_get_gpio_in(DEVICE(cp), ARM_CPU_VIRQ));
        sysbus_connect_irq(sbd, i + 3 * BCM2712_GIC_NUM_CPU,
                          qdev_get_gpio_in(DEVICE(cp), ARM_CPU_VFIQ));
    }
}

/*
 * Generate device tree for the machine
 * This creates a minimal DT that describes the hardware to the kernel.
 */
static void *bcm2712_create_dtb(struct BCM2712State *s, MachineState *machine,
                                 int *dtb_size)
{
    void *fdt;
    uint64_t addr, soc_addr;
    uint32_t clock_freq = 62500000; /* 62.5MHz timer clock */

    fdt = create_device_tree(&dtb_size);
    if (!fdt) {
        error_report("Failed to create device tree");
        return NULL;
    }

    /* Root node */
    qemu_fdt_setprop_string(fdt, "/", "compatible", "raspberrypi,5-model-b");
    qemu_fdt_setprop_cell(fdt, "/", "#address-cells", 0x2);
    qemu_fdt_setprop_cell(fdt, "/", "#size-cells", 0x2);
    qemu_fdt_setprop_cell(fdt, "/", "interrupt-parent", 
                          qemu_fdt_alloc_phandle(fdt));

    /* CPU nodes - single core version */
    qemu_fdt_add_subnode(fdt, "/cpus");
    qemu_fdt_setprop_cell(fdt, "/cpus", "#address-cells", 0x1);
    qemu_fdt_setprop_cell(fdt, "/cpus", "#size-cells", 0x0);

    /* CPU0 only (single core) */
    qemu_fdt_add_subnode(fdt, "/cpus/cpu@0");
    qemu_fdt_setprop_string(fdt, "/cpus/cpu@0", "device_type", "cpu");
    qemu_fdt_setprop_string(fdt, "/cpus/cpu@0", "compatible", "arm,cortex-a72");
    qemu_fdt_setprop_cell(fdt, "/cpus/cpu@0", "reg", 0);
    /* Simple spin-table for single core (no PSCI needed) */
    qemu_fdt_setprop_string(fdt, "/cpus/cpu@0", "enable-method", "spin-table");
    qemu_fdt_setprop_u64(fdt, "/cpus/cpu@0", "cpu-release-addr", 0x0);

    /* Memory node - describes RAM layout */
    qemu_fdt_add_subnode(fdt, "/memory");
    qemu_fdt_setprop_string(fdt, "/memory", "device_type", "memory");
    qemu_fdt_setprop(fdt, "/memory", "reg", 
                     &(uint64_t){ cpu_to_be64(BCM2712_RAM_BASE),
                                   cpu_to_be64(BCM2712_RAM_SIZE) },
                     16);

    /* Timer node - ARM Generic Timer (CPU internal) */
    qemu_fdt_add_subnode(fdt, "/timer");
    qemu_fdt_setprop_string(fdt, "/timer", "compatible", "arm,armv8-timer");
    /* Only need non-secure physical timer (PPI 14) for single core */
    qemu_fdt_setprop_cells(fdt, "/timer", "interrupts",
                           GIC_FDT_IRQ_TYPE_PPI, 14, 
                           GIC_FDT_IRQ_FLAGS_LEVEL_HI);
    qemu_fdt_setprop_cell(fdt, "/timer", "clock-frequency", clock_freq);

    /* GIC node - interrupt controller */
    addr = BCM2712_GIC_DIST_BASE;
    soc_addr = BCM2712_SOC_BASE;
    
    qemu_fdt_add_subnode(fdt, "/interrupt-controller@107c010000");
    qemu_fdt_setprop_string(fdt, "/interrupt-controller@107c010000", 
                            "compatible", "arm,gic-400");
    qemu_fdt_setprop_cell(fdt, "/interrupt-controller@107c010000", 
                          "#interrupt-cells", 3);
    qemu_fdt_setprop(fdt, "/interrupt-controller@107c010000", 
                     "interrupt-controller", NULL, 0);
    qemu_fdt_setprop(fdt, "/interrupt-controller@107c010000", "reg",
                     &(uint64_t){ cpu_to_be64(addr), cpu_to_be64(0x1000),
                                   cpu_to_be64(addr + 0x2000), cpu_to_be64(0x2000) },
                     32);
    qemu_fdt_setprop_cell(fdt, "/interrupt-controller@107c010000", "phandle",
                          qemu_fdt_alloc_phandle(fdt));

    /* Simple bus for SoC peripherals (placeholder for future devices) */
    qemu_fdt_add_subnode(fdt, "/soc");
    qemu_fdt_setprop_string(fdt, "/soc", "compatible", "simple-bus");
    qemu_fdt_setprop_cell(fdt, "/soc", "#address-cells", 0x2);
    qemu_fdt_setprop_cell(fdt, "/soc", "#size-cells", 0x2);
    qemu_fdt_setprop(fdt, "/soc", "ranges",
                     &(uint64_t){ cpu_to_be64(0x0), cpu_to_be64(soc_addr),
                                   cpu_to_be64(BCM2712_SOC_SIZE) },
                     24);

    return fdt;
}

/*
 * Machine initialization
 * This is the main entry point for our machine.
 */
static void bcm2712_machine_init(MachineState *machine)
{
    BCM2712State *s = BCM2712_MACHINE(machine);
    MemoryRegion *sysmem = get_system_memory();
    MemoryRegion *ram = g_new(MemoryRegion, 1);
    void *dtb;
    int dtb_size;
    struct arm_boot_info binfo = {};

    /* 1. Initialize RAM - from 0x0, size 1GB */
    memory_region_init_ram(ram, NULL, "ram", BCM2712_RAM_SIZE, &error_fatal);
    memory_region_add_subregion(sysmem, BCM2712_RAM_BASE, ram);

    /* 2. Initialize GIC interrupt controller */
    bcm2712_init_gic(s, machine, sysmem);

    /* 3. Create device tree */
    dtb = bcm2712_create_dtb(s, machine, &dtb_size);
    if (!dtb) {
        exit(1);
    }

    /* 4. Setup boot info for arm_load_kernel */
    binfo.ram_size = BCM2712_RAM_SIZE;
    binfo.kernel_filename = machine->kernel_filename;
    binfo.kernel_cmdline = machine->kernel_cmdline;
    binfo.initrd_filename = machine->initrd_filename;
    binfo.nb_cpus = 1;  /* Single core */
    binfo.board_id = -1;
    binfo.loader_start = BCM2712_RAM_BASE;
    binfo.firmware_loaded = 0;
    binfo.psci_conduit = QEMU_PSCI_CONDUIT_DISABLED;  /* No PSCI for single core */
    binfo.modify_dtb = NULL;
    binfo.get_dtb = NULL;
    binfo.write_secondary_boot = NULL;
    binfo.secondary_cpu_reset_hook = NULL;
    binfo.arm_boot_secure = false;
    binfo.gic_version = 2;  /* GIC-400 is GICv2 */

    /* Pass DTB to arm_load_kernel */
    binfo.fdt = dtb;
    binfo.fdt_size = dtb_size;

    /* Load kernel and DTB - this is where the magic happens */
    arm_load_kernel(ARM_CPU(first_cpu), machine, &binfo);
}

/* Machine class initialization */
static void bcm2712_machine_class_init(ObjectClass *oc, void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "Raspberry Pi 5 (BCM2712) - Single Core Debug";
    mc->init = bcm2712_machine_init;
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("cortex-a72");
    mc->default_ram_size = BCM2712_RAM_SIZE;
    mc->min_ram_size = BCM2712_RAM_SIZE;
    mc->max_ram_size = BCM2712_RAM_SIZE;
    mc->default_cpus = 1;           /* Default to single core */
    mc->min_cpus = 1;
    mc->max_cpus = 1;                /* Limit to single core for debugging */
    mc->no_floppy = 1;
    mc->no_cdrom = 1;
    mc->no_parallel = 1;
    mc->default_boot_order = "";
    mc->default_display = "none";
}

/* Machine type registration */
static const TypeInfo bcm2712_machine_type = {
    .name = TYPE_BCM2712_MACHINE,
    .parent = TYPE_MACHINE,
    .instance_size = sizeof(BCM2712State),
    .class_init = bcm2712_machine_class_init,
};

static void bcm2712_machine_register_types(void)
{
    type_register_static(&bcm2712_machine_type);
}

type_init(bcm2712_machine_register_types)
