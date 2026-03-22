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
#include "qemu/units.h"
#include "qemu/error-report.h"
#include "hw/arm/raspi_platform.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev.h"
#include "hw/arm/bcm2838.h"
#include "hw/arm/boot.h"
#include "hw/intc/arm_gic.h"
#include "hw/arm/fdt.h"
#include "system/device_tree.h"
#include "trace.h"
#include "hw/arm/machines-qom.h"


/* Memory map - based on BCM2712 datasheet */
#define BCM2712_RAM_BASE        0x0
#define BCM2712_RAM_SIZE        (1 * GiB)

/* SoC peripherals base - from device tree */
#define BCM2712_SOC_BASE        0x107C000000ULL
#define BCM2712_SOC_SIZE        0x4000000

/* GIC-400 registers */
#define BCM2712_GIC_DIST_BASE   0x107C010000ULL
#define BCM2712_GIC_CPU_BASE    0x107C012000ULL
#define BCM2712_GIC_SIZE        0x2000

/* GIC configuration - single core */
#define BCM2712_GIC_NUM_CPU     1
#define BCM2712_GIC_NUM_IRQ     160

/* Machine state */
typedef struct BCM2712State BCM2712State;
struct BCM2712State {
    MachineState parent;
    DeviceState *gic;
};

#define TYPE_BCM2712_MACHINE MACHINE_TYPE_NAME("bcm2712")
OBJECT_DECLARE_TYPE(BCM2712State, MachineClass, BCM2712_MACHINE)

/* 
 * Initialize GIC-400 interrupt controller
 */
static void bcm2712_init_gic(BCM2712State *s, MachineState *machine,
                             MemoryRegion *sysmem)
{
    DeviceState *gicdev;
    SysBusDevice *sbd;
    int i;

    gicdev = qdev_new(TYPE_ARM_GIC);
    sbd = SYS_BUS_DEVICE(gicdev);
    s->gic = gicdev;

    qdev_prop_set_uint32(gicdev, "num-cpu", BCM2712_GIC_NUM_CPU);
    qdev_prop_set_uint32(gicdev, "num-irq", BCM2712_GIC_NUM_IRQ);
    qdev_prop_set_bit(gicdev, "has-security-extensions", false);
    qdev_prop_set_bit(gicdev, "has-virtualization-extensions", false);

    sysbus_realize_and_unref(sbd, &error_fatal);

    sysbus_mmio_map(sbd, 0, BCM2712_GIC_DIST_BASE);
    sysbus_mmio_map(sbd, 1, BCM2712_GIC_CPU_BASE);

    for (i = 0; i < BCM2712_GIC_NUM_CPU; i++) {
        ARMCPU *cp = ARM_CPU(qemu_get_cpu(i));
        if (!cp) {
            continue;
        }

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
 */
static void *bcm2712_create_dtb(BCM2712State *s, MachineState *machine,
                                 int *dtb_size)
{
    void *fdt;
    uint64_t addr, soc_addr;
    uint32_t clock_freq = 62500000;
    uint64_t reg_mem[2];
    uint64_t reg_gic[4];
    uint64_t reg_soc[3];

    fdt = create_device_tree(dtb_size);
    if (!fdt) {
        error_report("Failed to create device tree");
        return NULL;
    }

    qemu_fdt_setprop_string(fdt, "/", "compatible", "raspberrypi,5-model-b");
    qemu_fdt_setprop_cell(fdt, "/", "#address-cells", 0x2);
    qemu_fdt_setprop_cell(fdt, "/", "#size-cells", 0x2);
    qemu_fdt_setprop_cell(fdt, "/", "interrupt-parent", 
                          qemu_fdt_alloc_phandle(fdt));

    qemu_fdt_add_subnode(fdt, "/cpus");
    qemu_fdt_setprop_cell(fdt, "/cpus", "#address-cells", 0x1);
    qemu_fdt_setprop_cell(fdt, "/cpus", "#size-cells", 0x0);

    qemu_fdt_add_subnode(fdt, "/cpus/cpu@0");
    qemu_fdt_setprop_string(fdt, "/cpus/cpu@0", "device_type", "cpu");
    qemu_fdt_setprop_string(fdt, "/cpus/cpu@0", "compatible", "arm,cortex-a72");
    qemu_fdt_setprop_cell(fdt, "/cpus/cpu@0", "reg", 0);
    qemu_fdt_setprop_string(fdt, "/cpus/cpu@0", "enable-method", "spin-table");
    qemu_fdt_setprop_u64(fdt, "/cpus/cpu@0", "cpu-release-addr", 0x0);

    qemu_fdt_add_subnode(fdt, "/memory");
    qemu_fdt_setprop_string(fdt, "/memory", "device_type", "memory");
    reg_mem[0] = cpu_to_be64(BCM2712_RAM_BASE);
    reg_mem[1] = cpu_to_be64(BCM2712_RAM_SIZE);
    qemu_fdt_setprop(fdt, "/memory", "reg", reg_mem, sizeof(reg_mem));

    qemu_fdt_add_subnode(fdt, "/timer");
    qemu_fdt_setprop_string(fdt, "/timer", "compatible", "arm,armv8-timer");
    qemu_fdt_setprop_cells(fdt, "/timer", "interrupts",
                           GIC_FDT_IRQ_TYPE_PPI, 14, 
                           GIC_FDT_IRQ_FLAGS_LEVEL_HI);
    qemu_fdt_setprop_cell(fdt, "/timer", "clock-frequency", clock_freq);

    addr = BCM2712_GIC_DIST_BASE;
    soc_addr = BCM2712_SOC_BASE;
    
    qemu_fdt_add_subnode(fdt, "/interrupt-controller@107c010000");
    qemu_fdt_setprop_string(fdt, "/interrupt-controller@107c010000", 
                            "compatible", "arm,gic-400");
    qemu_fdt_setprop_cell(fdt, "/interrupt-controller@107c010000", 
                          "#interrupt-cells", 3);
    qemu_fdt_setprop(fdt, "/interrupt-controller@107c010000", 
                     "interrupt-controller", NULL, 0);
    reg_gic[0] = cpu_to_be64(addr);
    reg_gic[1] = cpu_to_be64(0x1000);
    reg_gic[2] = cpu_to_be64(addr + 0x2000);
    reg_gic[3] = cpu_to_be64(0x2000);
    qemu_fdt_setprop(fdt, "/interrupt-controller@107c010000", "reg",
                     reg_gic, sizeof(reg_gic));
    qemu_fdt_setprop_cell(fdt, "/interrupt-controller@107c010000", "phandle",
                          qemu_fdt_alloc_phandle(fdt));

    qemu_fdt_add_subnode(fdt, "/soc");
    qemu_fdt_setprop_string(fdt, "/soc", "compatible", "simple-bus");
    qemu_fdt_setprop_cell(fdt, "/soc", "#address-cells", 0x2);
    qemu_fdt_setprop_cell(fdt, "/soc", "#size-cells", 0x2);
    reg_soc[0] = cpu_to_be64(0x0);
    reg_soc[1] = cpu_to_be64(soc_addr);
    reg_soc[2] = cpu_to_be64(BCM2712_SOC_SIZE);
    qemu_fdt_setprop(fdt, "/soc", "ranges", reg_soc, sizeof(reg_soc));

    return fdt;
}

/*
 * Machine initialization
 */
static void bcm2712_machine_init(MachineState *machine)
{
    BCM2712State *s = BCM2712_MACHINE(machine);
    int i;

    /* Create CPU explicitly */
    for (i = 0; i < machine->smp.cpus; i++) {
        Object *cpuobj;
        

        cpuobj = object_new(machine->cpu_type);
        ARM_CPU(cpuobj); // CPU realized

        object_property_add_child(OBJECT(machine), "cpu[*]", cpuobj);
        object_property_set_int(cpuobj, "psci-conduit", QEMU_PSCI_CONDUIT_DISABLED,
                                &error_abort);
        qdev_realize(DEVICE(cpuobj), NULL, &error_fatal);
        object_unref(cpuobj);
    }
    MemoryRegion *sysmem = get_system_memory();
    MemoryRegion *ram = g_new(MemoryRegion, 1);
    void *dtb;
    int dtb_size;
    struct arm_boot_info binfo = {};

    memory_region_init_ram(ram, NULL, "ram", BCM2712_RAM_SIZE, &error_fatal);
    memory_region_add_subregion(sysmem, BCM2712_RAM_BASE, ram);

    bcm2712_init_gic(s, machine, sysmem);

    dtb = bcm2712_create_dtb(s, machine, &dtb_size);
    if (!dtb) {
        exit(1);
    }

    binfo.ram_size = BCM2712_RAM_SIZE;
    binfo.kernel_filename = machine->kernel_filename;
    binfo.kernel_cmdline = machine->kernel_cmdline;
    binfo.initrd_filename = machine->initrd_filename;
    binfo.board_id = -1;
    binfo.loader_start = BCM2712_RAM_BASE;
    binfo.firmware_loaded = 0;
    binfo.psci_conduit = QEMU_PSCI_CONDUIT_DISABLED;

    machine->fdt = dtb;
    /* machine->dtb = g_strdup("bcm2712.dtb"); */ // Generated internally

    arm_load_kernel(ARM_CPU(first_cpu), machine, &binfo);
}

/* Machine class initialization */
static void bcm2712_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "Raspberry Pi 5 (BCM2712) - Single Core Debug";
    mc->init = bcm2712_machine_init;
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("cortex-a72");
    mc->default_ram_size = BCM2712_RAM_SIZE;
    mc->default_cpus = 1;
    mc->min_cpus = 1;
    mc->max_cpus = 1;
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
    .interfaces = aarch64_machine_interfaces,
};

static void bcm2712_machine_register_types(void)
{
    type_register_static(&bcm2712_machine_type);
}

type_init(bcm2712_machine_register_types)
