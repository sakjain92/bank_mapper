// Kernel allocator module
// Allocates contiguous physical pages using mmap

#include <linux/module.h>
#include <linux/version.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/kdev_t.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/cdev.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <asm/page.h>
#include <linux/dma-mapping.h>
#include <linux/uaccess.h>

#define DRIVER_NAME "kam_driver"
#define DEV_NAME    "kam"
#define CLASS_NAME  "chardrv"

static dev_t first; // Global variable for the first device number
static struct cdev c_dev; // Global variable for the character device structure
static struct class *cl; // Global variable for the device class
static struct device *dev;

struct mmap_info {
    int count;
    int using_coherent;
    dma_addr_t dma_addr;
    size_t alloc_size;
    void *pages;
    unsigned long phy_start_addr;
};

// Function to get virtual address of physical page using page table walks
long get_pfn_of_virtual_address(struct vm_area_struct *vma,
        unsigned long address, unsigned long *pfn) { 

    pgd_t *pgd;
    p4d_t *p4d;
    pud_t *pud; 
    pmd_t *pmd; 
    pte_t *ptep; 
    spinlock_t *ptl; 
    struct mm_struct *mm = vma->vm_mm; 

    pgd = pgd_offset(mm, address); 
    if (pgd_none(*pgd) || unlikely(pgd_bad(*pgd))) 
        return -EFAULT; 

    p4d = p4d_offset(pgd, address);
    if (p4d_none(*p4d) || unlikely(p4d_bad(*p4d))) {
        return -EFAULT;
    }

    pud = pud_offset(p4d, address); 
    if (pud_none(*pud) || unlikely(pud_bad(*pud))) 
        return -EFAULT; 

    pmd = pmd_offset(pud, address); 
    if (pmd_none(*pmd)) 
        return -EFAULT; 

    ptep = pte_offset_map_lock(mm, pmd, address, &ptl); 
    *pfn = pte_pfn(*ptep); 
    pte_unmap_unlock(ptep, ptl); 

    return 0; 

} 
static int kam_mmap(struct file *filp, struct vm_area_struct *vma)
{
    struct mmap_info *info;
    size_t size = vma->vm_end - vma->vm_start;
    size_t nrpages = size / PAGE_SIZE;
    size_t alloc_size = size + 2 * PAGE_SIZE;
    unsigned long vaddr;
    int i, ret = 0;

    pr_info("mmap\n");

    // TODO: Do arguments checks
    // TODO: Is there an API to allocate contiguous aligned memory?
    info = vma->vm_private_data = filp->private_data;
    if (info->count != 0) {
        pr_err("Driver allocates memory only once\n");
        return -ENOMEM;
    }

    vma->vm_flags |= VM_DONTEXPAND | VM_DONTCOPY | VM_LOCKED | VM_DONTDUMP;
    
    info->pages = kmalloc(alloc_size, GFP_HIGHUSER);
    if (info->pages == NULL) {
       
        info->pages = dma_alloc_coherent(dev, alloc_size, &info->dma_addr, GFP_HIGHUSER);
        if (info->pages == NULL) { 
            pr_err("Couldn't kmalloc\n");
            return -ENOMEM;
        }
        info->using_coherent = 1;
    }

    info->alloc_size = alloc_size;

    vaddr = ((unsigned long)(info->pages) + PAGE_SIZE - 1) & PAGE_MASK;
    if (remap_pfn_range(vma,
			    vma->vm_start,
			    virt_to_phys((void *)vaddr) >> PAGE_SHIFT,
			    size,
			    PAGE_SHARED)) {

        pr_err("Couldn't map pages");
		ret = -EAGAIN;
        goto err;
	}

    info->phy_start_addr = virt_to_phys((void *)vaddr);

/* TODO: Not working currently
    // Try to pin the pages
    ret = get_user_pages(vma->vm_start, nrpages, 0, NULL, NULL);
    if (ret < 0) {
        pr_err("Couldn't pin pages\n");
        // TODO: Do I need to remove the mapping?
        kfree(info->pages);
        return -EAGAIN;
    }
*/  
    info->count = 1;

    pr_info("mmap done: Virt: %p, Phys %p\n", (void *)vaddr,
            (void *)virt_to_phys((void *)vaddr));

    for (i = 0; i < nrpages; i++) {
        unsigned long v = vma->vm_start + i * PAGE_SIZE;
        unsigned long pfn;
        int ret;
        
        ret = get_pfn_of_virtual_address(vma, v, &pfn); 

        pr_info("User:Virt:%p, Phy:%p, Ret:%d\n", (void *)v,
                (void *)(pfn << PAGE_SHIFT), ret);
    }
    return 0;

err:
    if (info->using_coherent) {
        dma_free_coherent(dev, info->alloc_size, info->pages, info->dma_addr);
    } else {
        kfree(info->pages);
    }
    return ret;
}

static int kam_open(struct inode *inode, struct file *filp)
{
    struct mmap_info *info;

    pr_info("open\n");
    
    info = kmalloc(sizeof(struct mmap_info), GFP_KERNEL);
    if (info == NULL)
        return -EPERM;

    info->count = 0;
    info->using_coherent = 0;
    filp->private_data = info;
    
    return 0;
}
static int kam_release(struct inode *inode, struct file *filp)
{
    struct mmap_info *info;

    pr_info("release\n");
    
    info = filp->private_data;
    if (info->count != 0) {
        if (info->using_coherent) {
            dma_free_coherent(dev, info->alloc_size, info->pages, info->dma_addr);
        } else {
            kfree(info->pages);
        }   
    }

    kfree(info);
    filp->private_data = NULL;
    return 0;
}

static long kam_ioctl(struct file *filp, unsigned int ioctl_num,
        unsigned long ioctl_param)
{
    uint64_t *user_phy_addr_buf;
    struct mmap_info *info;
    int ret;

    info = filp->private_data;

    // Only '0' ioctl is supported
    if (ioctl_num != 0)
        return -ENOSYS;

    // Check if mmap() has been called yet?
    if (info == NULL || info->pages == NULL)
        return -EAGAIN;

    // Return the physical address of the start of mmap region
    user_phy_addr_buf = (uint64_t *)ioctl_param;
    ret = copy_to_user(user_phy_addr_buf, &info->phy_start_addr, 
                        sizeof(info->phy_start_addr));
    if (ret < 0) {
        pr_err("Couldn't copy to user\n");
        return -EINVAL;
    }

    return 0;
}

static struct file_operations fops = 
{
    .owner = THIS_MODULE,
    .mmap = kam_mmap,
    .open = kam_open,
    .unlocked_ioctl = kam_ioctl,
    .release = kam_release,
};

static int __init kam_init(void) /* Constructor */
{
    if (alloc_chrdev_region(&first, 0, 1, DRIVER_NAME) < 0) {
        return -1;
    }
    
    if ((cl = class_create(THIS_MODULE, CLASS_NAME)) == NULL) {
        unregister_chrdev_region(first, 1);
        return -1;
    }
    
    dev = device_create(cl, NULL, first, NULL, DEV_NAME);
    if (dev == NULL) {
        class_destroy(cl);
        unregister_chrdev_region(first, 1);
        return -1;
    }

    cdev_init(&c_dev, &fops);

    if (cdev_add(&c_dev, first, 1) < 0)
    {
        device_destroy(cl, first);
        class_destroy(cl);
        unregister_chrdev_region(first, 1);
        return -1;
    }

    pr_info("Device:%s registered\n", DEV_NAME);

    return 0;
}

static void __exit kam_exit(void) /* Destructor */
{
    cdev_del(&c_dev);
    device_destroy(cl, first);
    class_destroy(cl);
    unregister_chrdev_region(first, 1);
    pr_info("Removed device:%s\n", DEV_NAME);
}

module_init(kam_init);
module_exit(kam_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Saksham Jain>");
MODULE_DESCRIPTION("Allocator for contigous memory");
