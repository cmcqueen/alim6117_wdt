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

#define ALI_WDT_VERSION "0.3.0"

#include <linux/module.h>
#include <linux/watchdog.h>
#include <asm/io.h>
#include <linux/reboot.h>
#include <linux/init.h>

#define OUR_NAME "alim6117_wdt"

#define TIMEOUT_DEFAULT		60

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

static int wdt_run = 0;

static bool nowayout = WATCHDOG_NOWAYOUT;
module_param(nowayout, bool, 0);
MODULE_PARM_DESC(nowayout, "Watchdog cannot be stopped once started "
	"(default=" __MODULE_STRING(WATCHDOG_NOWAYOUT) ")");

static unsigned timeout = TIMEOUT_DEFAULT;
module_param(timeout, int, 0);
MODULE_PARM_DESC(timeout, "Initial watchdog timeout in seconds "
	"(default=" __MODULE_STRING(TIMEOUT_DEFAULT) ")");

static bool early_enable;
module_param(early_enable, bool, 0);
MODULE_PARM_DESC(early_enable,
	"Watchdog is started on module insertion (default=0)");

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
		printk(KERN_WARNING OUR_NAME ": Watchdog is stopped\n");
	}
}

static void ali_wdt_start(unsigned int wdt_timeout)
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

static int ali_m6117_wdt_start(struct watchdog_device *wdog)
{
	ali_wdt_start(wdog->timeout);

	return 0;
}

static int ali_m6117_wdt_stop(struct watchdog_device *wdog)
{
	ali_wdt_stop();

	return 0;
}

static int ali_m6117_wdt_ping(struct watchdog_device *wdog)
{
	ali_wdt_ping();

	return 0;
}

static int ali_m6117_wdt_set_timeout(struct watchdog_device *wdog,
				unsigned int wdt_timeout)
{
	ali_wdt_start(wdt_timeout);

	return 0;
}

static struct watchdog_info ali_wdt_info = {
	.options          = WDIOF_KEEPALIVEPING | WDIOF_MAGICCLOSE | WDIOF_SETTIMEOUT,
	.identity         = "ALi M6117 Watchdog",
};

static const struct watchdog_ops ali_wdt_ops = {
	.owner		= THIS_MODULE,
	.start		= ali_m6117_wdt_start,
	.stop		= ali_m6117_wdt_stop,
	.ping		= ali_m6117_wdt_ping,
	.set_timeout	= ali_m6117_wdt_set_timeout,
};

static struct watchdog_device ali_m6117_wdt = {
	.info = &ali_wdt_info,
	.ops = &ali_wdt_ops,
	.min_timeout = 1,
	.max_timeout = 512,
	.timeout = TIMEOUT_DEFAULT,
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
	int ret;

	printk(KERN_INFO "Watchdog driver for ALi M6117 v"
	       ALI_WDT_VERSION " initialising.\n");

	ret = watchdog_init_timeout(&ali_m6117_wdt, timeout, NULL);
	if (ret < 0)
		return ret;

	watchdog_set_nowayout(&ali_m6117_wdt, nowayout);

	ret = watchdog_register_device(&ali_m6117_wdt);
	if (ret != 0) {
		printk(KERN_ERR OUR_NAME
		       ": cannot register watchdog device.\n");
		return -EIO;
	}

	register_reboot_notifier(&ali_notifier);

	if (early_enable)
		ali_wdt_start(ali_m6117_wdt.timeout);

	return 0;
}

static void __exit alim6117_exit(void)
{
	unregister_reboot_notifier(&ali_notifier);

	watchdog_unregister_device(&ali_m6117_wdt);

	ali_wdt_stop();		/* Stop the timer */
}

module_init(alim6117_init);
module_exit(alim6117_exit);

MODULE_AUTHOR("Federico Bareilles <fede@fcaglp.unlp.edu.ar>");
MODULE_DESCRIPTION("Driver for watchdog timer in ALi M6117 chip.");
MODULE_LICENSE("GPL");
MODULE_SUPPORTED_DEVICE("watchdog");
MODULE_VERSION(ALI_WDT_VERSION);
