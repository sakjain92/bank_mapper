// Kernel allocator module

#include <linux/module.h>
#include <linux/version.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/kdev_t.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/cdev.h>

#define DRIVER_NAME "kam_driver"
#define DEV_NAME    "kam"
#define CLASS_NAME  "chardrv"

static dev_t first; // Global variable for the first device number
static struct cdev c_dev; // Global variable for the character device structure
static struct class *cl; // Global variable for the device class

static struct file_operations fops = 
{
    .owner = THIS_MODULE,
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
    
    if (device_create(cl, NULL, first, NULL, DEV_NAME) == NULL) {
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
