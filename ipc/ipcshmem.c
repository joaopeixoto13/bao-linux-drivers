/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Bao Hypervisor IPC Through Shared-memory Sample Driver
 *
 * Copyright (c) Bao Project and Contributors. All rights reserved.
 *
 * Authors:
 *	David Cerdeira and José Martins
 */

#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/uaccess.h>
#include <linux/fs.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/interrupt.h>
#include <linux/of.h>
#include <asm/io.h>
#include <linux/mutex.h>
#include <linux/poll.h>
#include <linux/platform_device.h>
#include <linux/ioctl.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/spinlock.h>
#include <linux/mutex.h>
#include <linux/wait.h>
#include <linux/mm.h>

#if defined(CONFIG_ARM64) || defined(CONFIG_ARM)
#include <linux/arm-smccc.h>
#include <asm/memory.h>
#elif CONFIG_RISCV
#include <asm/sbi.h>
#endif

#define DEV_NAME    "baoipc"
#define MAX_DEVICES 16
#define NAME_LEN    32

static dev_t bao_ipcshmem_devt;
struct class* cl;

struct bao_ipcshmem {
    struct cdev cdev;
    struct device* dev;

    int id;
    char label[NAME_LEN];
    void* read_base;
    size_t read_size;
    void* write_base;
    size_t write_size;
    void* physical_base;
    size_t shmem_size;
};

#ifdef CONFIG_ARM64
static uint64_t bao_ipcshmem_notify(struct bao_ipcshmem* dev)
{
    register uint64_t x0 asm("x0") =
        ARM_SMCCC_CALL_VAL(ARM_SMCCC_FAST_CALL, ARM_SMCCC_SMC_64, ARM_SMCCC_OWNER_VENDOR_HYP, 1);
    register uint64_t x1 asm("x1") = dev->id;
    register uint64_t x2 asm("x2") = 0;

    asm volatile("hvc 0\t\n" : "=r"(x0) : "r"(x0), "r"(x1), "r"(x2));

    return x0;
}
#elif CONFIG_ARM
static uint32_t bao_ipcshmem_notify(struct bao_ipcshmem* dev)
{
    register uint32_t r0 asm("r0") =
        ARM_SMCCC_CALL_VAL(ARM_SMCCC_FAST_CALL, ARM_SMCCC_SMC_32, ARM_SMCCC_OWNER_VENDOR_HYP, 1);
    register uint32_t r1 asm("r1") = dev->id;
    register uint32_t r2 asm("r2") = 0;

    asm volatile("hvc #0\t\n" : "=r"(r0) : "r"(r0), "r"(r1), "r"(r2));

    return r0;
}
#elif CONFIG_RISCV
static uint64_t bao_ipcshmem_notify(struct bao_ipcshmem* dev)
{
    struct sbiret ret = sbi_ecall(0x08000ba0, 1, dev->id, 0, 0, 0, 0, 0);

    return ret.error;
}
#endif

static int bao_ipcshmem_mmap_fops(struct file* filp, struct vm_area_struct* vma)
{
    struct bao_ipcshmem* bao = filp->private_data;

    unsigned long vsize = vma->vm_end - vma->vm_start;

    if (remap_pfn_range(vma, vma->vm_start, (unsigned long)bao->physical_base >> PAGE_SHIFT, vsize,
            vma->vm_page_prot)) {
        return -EFAULT;
    }

    return 0;
}

static ssize_t bao_ipcshmem_read_fops(struct file* filp, char* buf, size_t count, loff_t* ppos)
{
    struct bao_ipcshmem* bao_ipcshmem = filp->private_data;
    unsigned long missing = 0;
    size_t len = 0;

    len = strnlen(bao_ipcshmem->read_base, bao_ipcshmem->read_size);

    if (*ppos >= len) {
        return 0;
    }
    if ((len - *ppos) < count) {
        count = len - *ppos;
    }

    missing = copy_to_user(buf, bao_ipcshmem->read_base + *ppos, count);
    if (missing != 0) {
        count = count - missing;
    }
    *ppos += count;

    return count;
}

static ssize_t bao_ipcshmem_write_fops(struct file* filp, const char* buf, size_t count,
    loff_t* ppos)
{
    struct bao_ipcshmem* bao_ipcshmem = filp->private_data;
    unsigned long missing = 0;

    if (*ppos >= bao_ipcshmem->write_size) {
        return 0;
    }
    if (count > bao_ipcshmem->write_size) {
        count = bao_ipcshmem->write_size;
    }
    if ((*ppos + count) > bao_ipcshmem->write_size) {
        count = bao_ipcshmem->write_size - *ppos;
    }

    missing = copy_from_user(bao_ipcshmem->write_base + *ppos, buf, count);
    if (missing != 0) {
        count = count - missing;
    }
    *ppos += count;

    bao_ipcshmem_notify(bao_ipcshmem);

    return count;
}

static int bao_ipcshmem_open_fops(struct inode* inode, struct file* filp)
{
    struct bao_ipcshmem* bao_ipcshmem = container_of(inode->i_cdev, struct bao_ipcshmem, cdev);
    filp->private_data = bao_ipcshmem;

    kobject_get(&bao_ipcshmem->dev->kobj);

    return 0;
}

static int bao_ipcshmem_release_fops(struct inode* inode, struct file* filp)
{
    struct bao_ipcshmem* bao_ipcshmem = container_of(inode->i_cdev, struct bao_ipcshmem, cdev);
    filp->private_data = NULL;

    kobject_put(&bao_ipcshmem->dev->kobj);

    return 0;
}

static struct file_operations bao_ipcshmem_fops = { .owner = THIS_MODULE,
    .read = bao_ipcshmem_read_fops,
    .write = bao_ipcshmem_write_fops,
    .mmap = bao_ipcshmem_mmap_fops,
    .open = bao_ipcshmem_open_fops,
    .release = bao_ipcshmem_release_fops };

static int bao_ipcshmem_register(struct platform_device* pdev)
{
    int ret = 0;
    struct device* dev = &(pdev->dev);
    struct device_node* np = dev->of_node;
    struct module* owner = THIS_MODULE;
    struct resource* r;
    dev_t devt;
    resource_size_t shmem_size;
    u32 write_offset, read_offset, write_size, read_size;
    bool rd_in_range, wr_in_range, disjoint;
    void* shmem_base_addr = NULL;
    int id = -1;
    struct bao_ipcshmem* bao;

    r = platform_get_resource(pdev, IORESOURCE_MEM, 0);
    if (r == NULL) {
        return -EINVAL;
    }
    of_property_read_u32_index(np, "read-channel", 0, &read_offset);
    of_property_read_u32_index(np, "read-channel", 1, &read_size);
    of_property_read_u32_index(np, "write-channel", 0, &write_offset);
    of_property_read_u32_index(np, "write-channel", 1, &write_size);

    rd_in_range = (r->start + read_offset + read_size) < r->end;
    wr_in_range = (r->start + write_offset + write_size) < r->end;
    disjoint =
        ((read_offset + read_size) <= write_offset) || ((write_offset + write_size) <= read_offset);

    if (!rd_in_range || !wr_in_range || !disjoint) {
        dev_err(&pdev->dev, "invalid channel layout\n");
        dev_err(&pdev->dev, "rd_in_range = %d, wr_in_range = %d, disjoint = %d\n", rd_in_range,
            wr_in_range, disjoint);
        return -EINVAL;
    }

    shmem_size = r->end - r->start + 1;
    shmem_base_addr = memremap(r->start, shmem_size, MEMREMAP_WB);
    if (shmem_base_addr == NULL) {
        return -ENOMEM;
    }

    of_property_read_u32(np, "id", &id);
    if (id >= MAX_DEVICES) {
        dev_err(&pdev->dev, "invalid id %d\n", id);
        ret = -EINVAL;
        goto err_unmap;
    }

    bao = devm_kzalloc(&pdev->dev, sizeof(struct bao_ipcshmem), GFP_KERNEL);
    if (bao == NULL) {
        ret = -ENOMEM;
        goto err_unmap;
    }
    snprintf(bao->label, NAME_LEN, "%s%d", DEV_NAME, id);
    bao->id = id;
    bao->read_size = read_size;
    bao->write_size = write_size;
    bao->read_base = shmem_base_addr + read_offset;
    bao->write_base = shmem_base_addr + write_offset;
    bao->physical_base = (void*)r->start;
    bao->shmem_size = shmem_size;

    cdev_init(&bao->cdev, &bao_ipcshmem_fops);
    bao->cdev.owner = owner;

    devt = MKDEV(MAJOR(bao_ipcshmem_devt), id);
    ret = cdev_add(&bao->cdev, devt, 1);
    if (ret) {
        goto err_unmap;
    }

    bao->dev = device_create(cl, &pdev->dev, devt, bao, bao->label);
    if (IS_ERR(bao->dev)) {
        ret = PTR_ERR(bao->dev);
        goto err_cdev;
    }
    dev_set_drvdata(bao->dev, bao);

    return 0;

err_cdev:
    cdev_del(&bao->cdev);
err_unmap:
    memunmap(shmem_base_addr);

    dev_err(&pdev->dev, "failed initialization\n");
    return ret;
}

static void bao_ipcshmem_unregister(struct platform_device* pdev)
{
    /* TODO */
    return;
}

static const struct of_device_id of_bao_ipcshmem_match[] = { {
                                                                 .compatible = "bao,ipcshmem",
                                                             },
    { /* sentinel */ } };
MODULE_DEVICE_TABLE(of, of_bao_ipcshmem_match);

static struct platform_driver bao_ipcshmem_driver = {
    .probe = bao_ipcshmem_register,
    .remove = bao_ipcshmem_unregister,
    .driver = {
        .name = DEV_NAME,
        .of_match_table = of_bao_ipcshmem_match,
    },
};

static int __init bao_ipcshmem_init(void)
{
    int ret;

    if ((cl = class_create(DEV_NAME)) == NULL) {
        ret = -1;
        pr_err("unable to class_create " DEV_NAME " device\n");
        return ret;
    }

    ret = alloc_chrdev_region(&bao_ipcshmem_devt, 0, MAX_DEVICES, DEV_NAME);
    if (ret < 0) {
        pr_err("unable to alloc_chrdev_region " DEV_NAME " device\n");
        return ret;
    }

    return platform_driver_register(&bao_ipcshmem_driver);
}

static void __exit bao_ipcshmem_exit(void)
{
    platform_driver_unregister(&bao_ipcshmem_driver);
    unregister_chrdev(bao_ipcshmem_devt, DEV_NAME);
    class_destroy(cl);
}

module_init(bao_ipcshmem_init);
module_exit(bao_ipcshmem_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("David Cerdeira");
MODULE_AUTHOR("José Martins");
MODULE_DESCRIPTION("bao ipc through shared-memory sample driver");
