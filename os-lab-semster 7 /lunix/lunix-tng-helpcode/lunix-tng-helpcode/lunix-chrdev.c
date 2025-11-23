/*
 * lunix-chrdev.c
 *
 * Implementation of character devices
 * for Lunix:TNG
 *
 * vasiliki tsiplakidi - el22636
 *
 */


#include <linux/mm.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/list.h>
#include <linux/cdev.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/ioctl.h>
#include <linux/types.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/mmzone.h>
#include <linux/vmalloc.h>
#include <linux/spinlock.h>

#include <asm/io.h>

#include "lunix.h"
#include "lunix-chrdev.h"
#include "lunix-lookup.h"

/*
 * Global data
 */
struct cdev lunix_chrdev_cdev;

/*
 * Just a quick [unlocked] check to see if the cached
 * chrdev state needs to be updated from sensor measurements.
 */
/*
 * Declare a prototype so we can define the "unused" attribute and keep
 * the compiler happy. This function is not yet used, because this helpcode
 * is a stub.
 */
static int lunix_chrdev_state_needs_refresh(struct lunix_chrdev_state_struct *state)
{
	struct lunix_sensor_struct *sensor;
	
	WARN_ON ( !(sensor = state->sensor));
	/* ? */

    /* MY CODE */
    spin_lock(&sensor->lock);
    uint32_t last_update = sensor->msr_data[state->type]->last_update;
    spin_unlock(&sensor->lock);

	return state->buf_timestamp != last_update; 
    /* END OF MY CODE */
}

/*
 * Updates the cached state of a character device
 * based on sensor data. Must be called with the
 * character device state lock held.
 */
static int lunix_chrdev_state_update(struct lunix_chrdev_state_struct *state)
{
    struct lunix_sensor_struct *sensor = state->sensor;	
    // debug("leaving\n");

	/*
	 * Grab the raw data quickly, hold the
	 * spinlock for as little as possible.
	 */
	/* ? */
	/* Why use spinlocks? See LDD3, p. 119 */
 
    /* MY CODE */ 
    // Acquire sensor lock
    spin_lock(&sensor->lock);

    // Copy over: No need to copy over everything, just the new data
    uint32_t last_update = sensor->msr_data[state->type]->last_update;
    uint32_t raw_value = sensor->msr_data[state->type]->values[0];
    // Release sensor lock
    spin_unlock(&sensor->lock);

    if (state->buf_timestamp == last_update)
        return -EAGAIN;

    /* END OF MY CODE */
	/*
	 * Now we can take our time to format them,
	 * holding only the private state semaphore
	 */
	/* ? */

    /* MY CODE */
    long lookup_value;
    switch(state->type) {
    case BATT:
        lookup_value = lookup_voltage[raw_value];
        break;
    case TEMP: 
        lookup_value = lookup_temperature[raw_value];
        break;
    case LIGHT:
        lookup_value = lookup_light[raw_value];
        break;
    default:
        return -EINVAL;
    } 
    state->buf_lim = sprintf(state->buf_data, "%ld.%03ld", lookup_value / 1000, lookup_value % 1000); 
    state->buf_timestamp = last_update;
    /* END OF MY CODE */

    // debug("leaving\n");
	return 0;
}

/*************************************
 * Implementation of file operations
 * for the Lunix character device
 *************************************/

static int lunix_chrdev_open(struct inode *inode, struct file *filp)
{
	/* Declarations */
	/* ? */

    /* MY CODE */
    struct lunix_chrdev_state_struct *state;
    /* END OF MY CODE */

	int ret;

	debug("entering\n");
	ret = -ENODEV;
	if ((ret = nonseekable_open(inode, filp)) < 0)
		goto out;
	
	/* Allocate a new Lunix character device private state structure */
	/* ? */

    /* MY CODE */
    // GFP_DMA for continuous
    state = kzalloc(sizeof(*state), GFP_KERNEL);
    if (!state)
        return -ENOMEM;
    // make the file's private_data point to it's state object
    filp->private_data = state;
 
	/*
	 * Associate this open file with the relevant sensor based on
	 * the minor number of the device node [/dev/sensor<NO>-<TYPE>]
	 */
    unsigned int minor = iminor(inode);
    unsigned int sensor_id = minor >> 3;
    unsigned int msr_type = minor & 0x07;
    if(sensor_id >= lunix_sensor_cnt || msr_type >= N_LUNIX_MSR) {
        ret = -ENODEV;
        goto out;
    }

    state->type = msr_type;
    state->sensor = &lunix_sensors[sensor_id];

    // buf_lim, buf_timestamp and eof_flag already initialized to 0
    sema_init(&state->lock, 1);
    /* END OF MY CODE */

out:
	debug("leaving, with ret = %d\n", ret);
	return ret;
}

static int lunix_chrdev_release(struct inode *inode, struct file *filp)
{
	/* ? */
    /* MY CODE */
    kfree(filp->private_data);
    /* END OF MY CODE */
	return 0;
}

static long lunix_chrdev_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    unsigned char val;
    struct lunix_chrdev_state_struct *state = filp->private_data;
    
    switch(cmd) {
    case LUNIX_IOC_SET_REWIND:
        if (get_user(val, (unsigned char __user *)arg))
            return -EFAULT;    
        if (val != 0 && val != 1)
            return -EINVAL;

        if (down_interruptible(&state->lock))
            return -ERESTARTSYS;
        state->auto_rewind_flag = val;
        up(&state->lock);

        return 0;
    
    case LUNIX_IOC_GET_REWIND: 
        if (down_interruptible(&state->lock))
            return -ERESTARTSYS;
        val = state->auto_rewind_flag;
        up(&state->lock);
        
        if (put_user(val, (unsigned char __user *)arg))
            return -EFAULT;
        return 0;
    
    default:
        return -EINVAL;
    }
}

static ssize_t lunix_chrdev_read(struct file *filp, char __user *usrbuf, size_t cnt, loff_t *f_pos)
{
	ssize_t ret;

	struct lunix_sensor_struct *sensor;
	struct lunix_chrdev_state_struct *state;

	state = filp->private_data;
	WARN_ON(!state);

	sensor = state->sensor;
	WARN_ON(!sensor);

	/* MY CODE - Lock */
    if (down_interruptible(&state->lock))
        return -ERESTARTSYS;
	
    /* Auto-rewind on EOF mode? */
	/* ? */
    if (state->auto_rewind_flag && *f_pos >= state->buf_lim)
        *f_pos = 0;

    /*
	 * If the cached character device state needs to be
	 * updated by actual sensor data (i.e. we need to report
	 * on a "fresh" measurement, do so
	 */
	if (*f_pos == 0) {
        // Non blocking mode
        if (filp->f_flags & O_NONBLOCK) {
            if (lunix_chrdev_state_update(state) == -EAGAIN) {
                ret = -EAGAIN;
                goto out;
            }
        }
        // Blocking mode
        else {
            while (lunix_chrdev_state_update(state) == -EAGAIN) {
                /* ? */
                /* The process needs to sleep */
                /* See LDD3, page 153 for a hint */

                up(&state->lock);        

                // Wait until new data has come
                if(wait_event_interruptible(sensor->wq, lunix_chrdev_state_needs_refresh(state)))
                    return -ERESTARTSYS;

                if (down_interruptible(&state->lock))
                    return -ERESTARTSYS;
            }
        }
	}

	/* End of file */
	/* ? */
    /* MY CODE */
    if (*f_pos >= state->buf_lim) {
        ret = 0;
        goto out;
    }
    /* END OF MY CODE */

	/* Determine the number of cached bytes to copy to userspace */
	/* ? */
    /* MY CODE */ 
	ssize_t bytes_to_copy = min_t(ssize_t, cnt, state->buf_lim - *f_pos);
    if (copy_to_user(usrbuf, state->buf_data + *f_pos, bytes_to_copy)) {
        ret = -EFAULT;
        goto out;
    }
    *f_pos += bytes_to_copy;
    ret = bytes_to_copy;
    /* END OF MY CODE */
	
out:
	/* MY CODE - Unlock? */
    up(&state->lock);
	return ret;
}

static int lunix_chrdev_mmap(struct file *filp, struct vm_area_struct *vma)
{
    /* 
     * What needs to be mapped to userspace: the correct msr of the correct sensor
     * So state->sensor->msr_data[state->type]
    */
 
	struct lunix_chrdev_state_struct *state = filp->private_data;
    struct lunix_msr_data_struct *data = state->sensor->msr_data[state->type];

    unsigned long size = vma->vm_end - vma->vm_start;
    // User should have passed exactly one page
    if (size != PAGE_SIZE) {
        debug("mmap: Invalid size vma\n");
        return -EINVAL;
    }
    // User vma should allign with page
    if (vma->vm_pgoff != 0) {
        debug("mmap: Vma should have 0 page offset\n");
        return -EINVAL;
    }

    // Get data's physical address
    unsigned long data_pfn = virt_to_phys(data) >> PAGE_SHIFT;

    // Remap user's vma to point to data's physical
	int ret = remap_pfn_range(vma, vma->vm_start, data_pfn, size, vma->vm_page_prot);    
    if (ret) {
        debug("lunix_chrdev_mmap - remap_pfn_range failed with %d\n", ret);
        return ret;
    }
    return 0;
}

static struct file_operations lunix_chrdev_fops = 
{
	.owner          = THIS_MODULE,
	.open           = lunix_chrdev_open,
	.release        = lunix_chrdev_release,
	.read           = lunix_chrdev_read,
	.unlocked_ioctl = lunix_chrdev_ioctl,
	.mmap           = lunix_chrdev_mmap
};

int lunix_chrdev_init(void)
{
	/*
	 * Register the character device with the kernel, asking for
	 * a range of minor numbers (number of sensors * 8 measurements / sensor)
	 * beginning with LINUX_CHRDEV_MAJOR:0
	 */
	int ret;
	dev_t dev_no;
	unsigned int lunix_minor_cnt = lunix_sensor_cnt << 3;

	debug("initializing character device\n");
	cdev_init(&lunix_chrdev_cdev, &lunix_chrdev_fops);
	lunix_chrdev_cdev.owner = THIS_MODULE;
	
	dev_no = MKDEV(LUNIX_CHRDEV_MAJOR, 0);
	/* ? */
	/* register_chrdev_region? */

    /* MY CODE */
    // This just marks the region as taken
    // (All are registered with the same major since lunix_minor_cnt < (1 << 20))
    ret = register_chrdev_region(dev_no, lunix_minor_cnt, "sensor");
    /* END OF MY CODE */

    // return 0

	if (ret < 0) {
		debug("failed to register region, ret = %d\n", ret);
		goto out;
	}
	/* ? */
	/* cdev_add? */

    /* MY CODE */
    // This maps the continuous range of [dev_no, dev_no + lunix_minor_cnt) to the lunix_chrdev_cdev object we just created
    /*
        Now after opp(fd, ...) -> ... -> inode->i_rdev = MKDEV(60, minor)
        1) struct cdev *cdev = cdev_map_lookup(60, minor)
            Access the cdev object we just created to retrieve cdev->ops
        2) struct file *file = alloc_file(); file->f_op = cdev->ops
            Create a file with f_op = cdev->ops
        3) file->f_op->opp(inode, file)
            Call my opp function 
        But why create a file and not just call the function I need from cdev->ops?
        Lets take read for example. After reading a measurement, the next read should return the next measurement
        But where would I store the position of the current measurement to know what to return next?
        Also what if multiple processes open the same device? Meaning, I cannot use a global 'position' attribute
    */ 
    ret = cdev_add(&lunix_chrdev_cdev, dev_no, lunix_minor_cnt);
    /* END OF MY CODE*/

	if (ret < 0) {
		debug("failed to add character device\n");
		goto out_with_chrdev_region;
	}
	debug("completed successfully\n");
	return 0;

out_with_chrdev_region:
	unregister_chrdev_region(dev_no, lunix_minor_cnt);
out:
	return ret;
}

void lunix_chrdev_destroy(void)
{
	dev_t dev_no;
	unsigned int lunix_minor_cnt = lunix_sensor_cnt << 3;

	debug("entering\n");
	dev_no = MKDEV(LUNIX_CHRDEV_MAJOR, 0);
	cdev_del(&lunix_chrdev_cdev);
	unregister_chrdev_region(dev_no, lunix_minor_cnt);
	debug("leaving\n");
}
