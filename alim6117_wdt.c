/** 
 *   ALi M6117 Watchdog timer driver.
 *
 *   (c) Copyright 2003 Federico Bareilles <fede@fcaglp.unlp.edu.ar>,
 *   Instituto Argentino de Radio Astronomia (IAR).
 *
 *   This program is free software; you can redistribute it and/or
 *   modify it under the terms of the GNU General Public License as
 *   published by the Free Software Foundation; either version 2 of
 *   the License, or (at your option) any later version.
 *     
 *   The author does NOT admit liability nor provide warranty for any
 *   of this software. This material is provided "AS-IS" in the hope
 *   that it may be useful for others.
 *
 *   Based on alim1535_wdt.c by Alan Cox and other WDT by several
 *   authors...
 *
 *   ALi (Acer Labs) M6117 is an i386 that has the watchdog timer
 *   built in.  Watchdog uses a 32.768KHz clock with a 24 bits
 *   counter. The timer ranges is from 30.5u sec to 512 sec with
 *   resolution 30.5u sec. When the timer times out; a system reset,
 *   NMI or IRQ may happen. This can be decided by the user's
 *   programming.
 **/

#define ALI_WDT_VERSION "0.2.0"

#include <linux/module.h>
#include <linux/miscdevice.h>
#include <linux/watchdog.h>
#include <asm/io.h>
#include <asm/uaccess.h>
#include <linux/reboot.h>
#include <linux/init.h>
#include <linux/proc_fs.h>

#define OUR_NAME "alim6117_wdt"

/* Port definitions: */
#define M6117_PORT_INDEX 0x22
#define M6117_PORT_DATA  0x23
/* YES, the two unused ports of 8259:
 * 0020-003f : pic1 
 *
 * The 8259 Interrup Controller uses four port addresses (0x20 through
 * 0x23). Although IBM documentation indicates that these four port
 * addresses are reserved for the 8259, only the two lower ports (0x20
 * and 0x21) ar documented as usable by programers. The two ports
 * (0x22 and 0x23) are used only when reprogramming the 8259 for
 * special dedicated systems that operate in modes which are not
 * compatible with normal IBM PC operation (this case).
 **/

/* Index for ALI M6117: */
#define ALI_LOCK_REGISTER 0x13
#define ALI_WDT           0x37
#define ALI_WDT_SELECT    0x38
#define ALI_WDT_DATA0     0x39
#define ALI_WDT_DATA1     0x3a
#define ALI_WDT_DATA2     0x3b
#define ALI_WDT_CTRL      0x3c

/* Time out generates signal select: */
#define WDT_SIGNAL_IRQ3  0x10
#define WDT_SIGNAL_IRQ4  0x20
#define WDT_SIGNAL_IRQ5  0x30
#define WDT_SIGNAL_IRQ6  0x40
#define WDT_SIGNAL_IRQ7  0x50
#define WDT_SIGNAL_IRQ9  0x60
#define WDT_SIGNAL_IRQ10 0x70
#define WDT_SIGNAL_IRQ11 0x80
#define WDT_SIGNAL_IRQ12 0x90
#define WDT_SIGNAL_IRQ14 0xa0
#define WDT_SIGNAL_IRQ15 0xb0
#define WDT_SIGNAL_NMI   0xc0
#define WDT_SIGNAL_SRSET 0xd0
/* set signal to use: */
#define WDT_SIGNAL       WDT_SIGNAL_SRSET

/* ALI_WD_TIME_FACTOR is 1000000/30.5 */
#define ALI_WD_TIME_FACTOR 32787	/* (from seconds to ALi counter) */

static unsigned long wdt_is_open;
static char ali_expect_close;
static int wdt_run = 0;


static bool nowayout = WATCHDOG_NOWAYOUT;
module_param(nowayout, bool, 0);
MODULE_PARM_DESC(nowayout, "Watchdog cannot be stopped once started "
	"(default=" __MODULE_STRING(WATCHDOG_NOWAYOUT) ")");

static unsigned wdt_timeout = 60;
module_param(wdt_timeout, int, 0);
MODULE_PARM_DESC(wdt_timeout, "initial watchdog timeout (in seconds)");


static int alim6117_read(int index)
{
	outb(index, M6117_PORT_INDEX);
	return inb(M6117_PORT_DATA);
}

static void alim6117_write(int index, int data)
{
	outb(index, M6117_PORT_INDEX);
	outb(data, M6117_PORT_DATA);
}

static void alim6117_ulock_conf_register(void)
{
	alim6117_write(ALI_LOCK_REGISTER, 0xc5);
}

static void alim6117_lock_conf_register(void)
{
	alim6117_write(ALI_LOCK_REGISTER, 0x00);
}

static void alim6117_set_timeout(int time)
{
	u32 timeout_bits;

	timeout_bits = time * ALI_WD_TIME_FACTOR;
	alim6117_write(ALI_WDT_DATA0, timeout_bits & 0xff);
	alim6117_write(ALI_WDT_DATA1, (timeout_bits & 0xff00) >> 8);
	alim6117_write(ALI_WDT_DATA2, (timeout_bits & 0xff0000) >> 16);

	return;
}

static void alim6117_wdt_disable(void)
{
	int val = alim6117_read(ALI_WDT);

	val &= 0xbf;		/* 1011|1111 */
	alim6117_write(ALI_WDT, val);
}

static void alim6117_wdt_enable(void)
{
	int val = alim6117_read(ALI_WDT);

	val |= 0x40;		/* 0100|0000 */
	alim6117_write(ALI_WDT, val);
}

static void alim6117_wdt_signal_select(int signal)
{
	int val = alim6117_read(ALI_WDT_SELECT);

	val &= 0xf0;
	val |= signal;
	alim6117_write(ALI_WDT_SELECT, val);
}

static void ali_wdt_ping(void)
{
	int val;

	/* if no run, no ping; wdt start when ping it. */ 
	if (wdt_run) {
		alim6117_ulock_conf_register();
		val = alim6117_read(ALI_WDT);
		val &= ~0x40; /* 0100|0000 */
		alim6117_write(ALI_WDT, val);
		val |= 0x40;  /* 0100|0000 */
		alim6117_write(ALI_WDT, val);
		alim6117_lock_conf_register();
		/*
		printk(KERN_INFO OUR_NAME ": WDT ping...\n");
		*/
	} else { 
		printk(KERN_WARNING OUR_NAME ": WDT is stopped\n");
	}
}

static void ali_wdt_start(void)
{
	alim6117_ulock_conf_register();
	alim6117_wdt_disable();
	alim6117_set_timeout(wdt_timeout);
	alim6117_wdt_signal_select(WDT_SIGNAL);
	alim6117_wdt_enable();
	alim6117_lock_conf_register();
	wdt_run = 1;
}

static void ali_wdt_stop(void)
{
	int val;
	if ( wdt_run ) {
		alim6117_ulock_conf_register();
		val = alim6117_read(ALI_WDT);
		val &= ~0x40;  /* 0100|0000 */
		alim6117_write(ALI_WDT, val);
		alim6117_lock_conf_register();
		wdt_run = 0;
		/*
		printk(KERN_INFO OUR_NAME ": WDT stop...\n");
		*/
	}
}

/**
 *      ali_wdt_notify_sys:
 *      @this: our notifier block
 *      @code: the event being reported
 *      @unused: unused
 *
 *      Our notifier is called on system shutdowns. We want to turn the timer
 *      off at reboot otherwise the machine will reboot again during memory
 *      test or worse yet during the following fsck.
 *
 */

static int ali_wdt_notify_sys(struct notifier_block *this,
			      unsigned long code, void *unused)
{
	if (code == SYS_DOWN || code == SYS_HALT) {
		/* Turn the timer off */
		ali_wdt_stop();
	}
	return NOTIFY_DONE;
}

/**
 *      ali_write       -       writes to ALi watchdog
 *      @file: file handle to the watchdog
 *      @data: user address of data
 *      @len: length of data
 *      @ppos: pointer to the file offset
 *
 *      Handle a write to the ALi watchdog. Writing to the file pings
 *      the watchdog and resets it. Writing the magic 'V' sequence allows
 *      the next close to turn off the watchdog.
 */

static ssize_t ali_write(struct file *file, const char *data,
			 size_t len, loff_t * ppos)
{
	/*  Can't seek (pwrite) on this device  */
	if (ppos != &file->f_pos)
		return -ESPIPE;

	/* Check if we've got the magic character 'V' and reload the timer */
	if (len) {
		size_t i;

		ali_expect_close = 0;

		/* scan to see wether or not we got the magic character */
		for (i = 0; i != len; i++) {
			u8 c;
			if (get_user(c, data + i))
				return -EFAULT;
			if (c == 'V')
				ali_expect_close = 42;
		}
		ali_wdt_ping();
		return 1;
	}
	return 0;
}

/**
 *      ali_ioctl       -       handle watchdog ioctls
 *      @inode: inode of the device
 *      @file: file handle to the device
 *      @cmd: watchdog command
 *      @arg: argument pointer
 *
 *      Handle the watchdog ioctls supported by the ALi driver.
 */

static long ali_ioctl(struct file *file,
		     unsigned int cmd, unsigned long arg)
{
	int options;

	static struct watchdog_info ident = {
		.options          = WDIOF_KEEPALIVEPING | WDIOF_SETTIMEOUT,
		.firmware_version = 0,
		.identity         = "ALi M6117 WDT",
	};
	
	switch (cmd) {
	case WDIOC_KEEPALIVE:
		ali_wdt_ping();
		return 0;
	case WDIOC_SETTIMEOUT:
		if (get_user(options, (int *) arg))
			return -EFAULT;
		if (options < 1 || options > 512)
			return -EFAULT;
		wdt_timeout = options;
		ali_wdt_start();
	case WDIOC_GETTIMEOUT:
		return put_user(wdt_timeout, (int *) arg);
	case WDIOC_GETSUPPORT:
		if (copy_to_user
		    ((struct watchdog_info *) arg, &ident, sizeof(ident)))
			return -EFAULT;
		return 0;
	case WDIOC_GETSTATUS:
	case WDIOC_GETBOOTSTATUS:
		return put_user(0, (int *) arg);
	case WDIOC_SETOPTIONS:
		if (get_user(options, (int *) arg))
			return -EFAULT;
		if (options & WDIOS_DISABLECARD) {
			ali_wdt_stop();
			return 0;
		}
		if (options & WDIOS_ENABLECARD) {
			ali_wdt_start();
			return 0;
		}
		return -EINVAL;

	default:
		return -ENOTTY;

	}
}

/**
 *      ali_open        -       handle open of ali watchdog
 *      @inode: inode of device
 *      @file: file handle to device
 *
 *      Open the ALi watchdog device. Ensure only one person opens it
 *      at a time. Also start the watchdog running.
 */

static int ali_open(struct inode *inode, struct file *file)
{
	if(test_and_set_bit(0, &wdt_is_open))
                return -EBUSY;
	ali_wdt_start();

	return 0;
}

/**
 *      ali_release     -       close an ALi watchdog
 *      @inode: inode from VFS
 *      @file: file from VFS
 *
 *      Close the ALi watchdog device. Actual shutdown of the timer
 *      only occurs if the magic sequence has been set or nowayout is 
 *      disabled.
 */

static int ali_release(struct inode *inode, struct file *file)
{
	if (ali_expect_close == 42 && !nowayout) {
		ali_wdt_stop();
	} else {
		printk(KERN_CRIT OUR_NAME
		       ": Unexpected close, not stopping watchdog!\n");
	}
	ali_expect_close = 0;
	clear_bit(0, &wdt_is_open);

	return 0;
}

static struct file_operations ali_fops = {
	.owner          = THIS_MODULE,
	.write          = ali_write,
	.unlocked_ioctl = ali_ioctl,
	.open           = ali_open,
	.release        = ali_release,
};

static struct miscdevice ali_miscdev = {
	.minor          = WATCHDOG_MINOR,
	.name           = "watchdog",
	.fops           = &ali_fops,
};

/*
 *   The WDT needs to learn about soft shutdowns in order to turn the
 *   timebomb registers off.
 */

static struct notifier_block ali_notifier = {
	.notifier_call = ali_wdt_notify_sys,
	.next          = NULL,
	.priority      = 0
};

static int __init alim6117_init(void)
{
	if (wdt_timeout < 1 || wdt_timeout > 512){
		printk(KERN_ERR OUR_NAME
		       ": Timeout out of range (0 < wdt_timeout <= 512)\n");
		return -EIO;
	}

	if (misc_register(&ali_miscdev) != 0) {
		printk(KERN_ERR OUR_NAME
		       ": cannot register watchdog device node.\n");
		return -EIO;
	}

	register_reboot_notifier(&ali_notifier);

	printk(KERN_INFO "WDT driver for ALi M6117 v(" 
	       ALI_WDT_VERSION ") initialising.\n");

	return 0;
}

static void __exit alim6117_exit(void)
{
	misc_deregister(&ali_miscdev);
	unregister_reboot_notifier(&ali_notifier);

	ali_wdt_stop();		/* Stop the timer */
}

module_init(alim6117_init);
module_exit(alim6117_exit);

MODULE_AUTHOR("Federico Bareilles <fede@fcaglp.unlp.edu.ar>");
MODULE_DESCRIPTION("Driver for watchdog timer in ALi M6117 chip.");
MODULE_LICENSE("GPL");
MODULE_SUPPORTED_DEVICE("watchdog");
