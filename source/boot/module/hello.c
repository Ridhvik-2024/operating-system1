// SPDX-License-Identifier: GPL-2.0
/* hello.c — AI-OS boot/module: canary module to validate the LKM toolchain. */
#include <linux/module.h>
#include <linux/init.h>

static int __init hello_init(void)
{
	pr_info("aios: hello module loaded — kernel bridge alive\n");
	return 0;
}

static void __exit hello_exit(void)
{
	pr_info("aios: hello module unloaded\n");
}

module_init(hello_init);
module_exit(hello_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("AI-OS");
MODULE_DESCRIPTION("AI-OS toolchain canary");
