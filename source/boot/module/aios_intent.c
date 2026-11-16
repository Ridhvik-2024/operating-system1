// SPDX-License-Identifier: GPL-2.0
/* aios_intent.c — AI-OS kernel bridge (M6): the intent device.
 *
 * AI-OS treats AI workloads as first-class OS citizens. This module is the
 * kernel side of that contract:
 *
 *   - /dev/aios  char device (misc). Userspace (aios-init, aiosh, the agent
 *     runtime) registers *intents*: "this pid is an INTERACTIVE shell",
 *     "this pid is a BATCH tool worker", "this pid is an IDLE background
 *     agent". The kernel keeps the registry and counts real scheduler
 *     activity per class via the sched_switch tracepoint.
 *
 *   - /proc/aios human-readable view of the registry + per-class context
 *     switch counters. That counter is the kernel's honest opinion about
 *     how much CPU each AI workload class is actually consuming.
 *
 * Userspace still applies the actual scheduling policy (sched_setattr,
 * cgroup v2) — the kernel cannot be trusted to invent policy, only to
 * observe and enforce. This module observes and accounts.
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/spinlock.h>
#include <linux/hashtable.h>
#include <linux/tracepoint.h>
#include <linux/sched.h>
#include <linux/pid.h>
#include <linux/version.h>
#include <linux/kprobes.h>

#define AIOS_INTENT_VERSION "0.6.0"

/* intent classes — must mirror include/aios/ksched.h */
#define AIOS_CLASS_INTERACTIVE 0
#define AIOS_CLASS_BATCH       1
#define AIOS_CLASS_IDLE        2
#define AIOS_CLASS_COUNT       3

#define AIOS_IOC_MAGIC 'A'
#define AIOS_IOC_REGISTER   _IOW(AIOS_IOC_MAGIC, 1, struct aios_intent_req)
#define AIOS_IOC_UNREGISTER _IOW(AIOS_IOC_MAGIC, 2, struct aios_intent_req)
#define AIOS_IOC_QUERY      _IOWR(AIOS_IOC_MAGIC, 3, struct aios_intent_req)

struct aios_intent_req {
        __s32 pid;
        __u8  aiclass;      /* AIOS_CLASS_* */
        __u8  pad[3];
        __u64 wakeups;      /* out: wakeups observed for this pid */
};

struct aios_entry {
        struct hlist_node node;
        pid_t pid;
        u8    aiclass;
        u64   wakeups;
        char  comm[TASK_COMM_LEN];
};

#define AIOS_HASH_BITS 7
static DEFINE_HASHTABLE(aios_table, AIOS_HASH_BITS);
static DEFINE_SPINLOCK(aios_lock);

static atomic_t aios_registered = ATOMIC_INIT(0);
static atomic64_t aios_wakeups[AIOS_CLASS_COUNT];

/* ------------------------------------------------------------------ */
/* wakeup accounting via kprobe on try_to_wake_up                       */
/*                                                                      */
/* The sched_switch tracepoint is not exported to modules, but kprobes  */
/* resolve through kallsyms: any non-blacklisted symbol can be probed.  */
/* try_to_wake_up's first argument is the task being woken; counting    */
/* its wakeups gives the kernel's view of how often each intent class   */
/* becomes runnable. x86_64 System V ABI: first arg in %di.             */

static int aios_wake_pre(struct kprobe *p, struct pt_regs *regs)
{
        struct aios_entry *e;
#ifdef CONFIG_X86_64
        struct task_struct *t = (struct task_struct *)regs->di;
        pid_t wpid;

        if (!t)
                return 0;
        wpid = t->pid;

        rcu_read_lock();
        hash_for_each_possible_rcu(aios_table, e, node, wpid) {
                if (e->pid == wpid) {
                        e->wakeups++;
                        atomic64_inc(&aios_wakeups[e->aiclass]);
                        break;
                }
        }
        rcu_read_unlock();
#else
        (void)e;
#endif
        return 0;
}

static struct kprobe aios_wake_kp = {
        .symbol_name = "try_to_wake_up",
        .pre_handler = aios_wake_pre,
};

/* ------------------------------------------------------------------ */
/* registry operations                                                  */

static struct aios_entry *aios_lookup(pid_t pid)
{
        struct aios_entry *e;

        hash_for_each_possible_rcu(aios_table, e, node, pid)
                if (e->pid == pid)
                        return e;
        return NULL;
}

static long aios_register(struct aios_intent_req __user *ureq)
{
        struct aios_intent_req req;
        struct aios_entry *e;
        unsigned long flags;

        if (copy_from_user(&req, ureq, sizeof req))
                return -EFAULT;
        if (req.aiclass >= AIOS_CLASS_COUNT)
                return -EINVAL;

        spin_lock_irqsave(&aios_lock, flags);
        e = aios_lookup(req.pid);
        if (e) {
                e->aiclass = req.aiclass;
        } else {
                struct task_struct *t;
                struct pid *ph;

                rcu_read_lock();
                ph = find_vpid(req.pid);
                t = ph ? pid_task(ph, PIDTYPE_PID) : NULL;
                if (!t) {
                        rcu_read_unlock();
                        spin_unlock_irqrestore(&aios_lock, flags);
                        return -ESRCH;
                }
                rcu_read_unlock();
                e = kzalloc(sizeof *e, GFP_ATOMIC);
                if (!e) {
                        spin_unlock_irqrestore(&aios_lock, flags);
                        return -ENOMEM;
                }
                e->pid = req.pid;
                e->aiclass = req.aiclass;
                get_task_comm(e->comm, t);
                hash_add_rcu(aios_table, &e->node, e->pid);
                atomic_inc(&aios_registered);
        }
        spin_unlock_irqrestore(&aios_lock, flags);
        return 0;
}

static long aios_unregister(struct aios_intent_req __user *ureq)
{
        struct aios_intent_req req;
        struct aios_entry *e;
        unsigned long flags;

        if (copy_from_user(&req, ureq, sizeof req))
                return -EFAULT;

        spin_lock_irqsave(&aios_lock, flags);
        e = aios_lookup(req.pid);
        if (!e) {
                spin_unlock_irqrestore(&aios_lock, flags);
                return -ESRCH;
        }
        hash_del_rcu(&e->node);
        spin_unlock_irqrestore(&aios_lock, flags);

        synchronize_rcu();
        kfree(e);
        atomic_dec(&aios_registered);
        return 0;
}

static long aios_query(struct aios_intent_req __user *ureq)
{
        struct aios_intent_req req;
        struct aios_entry *e;
        long ret = 0;
        unsigned long flags;

        if (copy_from_user(&req, ureq, sizeof req))
                return -EFAULT;

        spin_lock_irqsave(&aios_lock, flags);
        e = aios_lookup(req.pid);
        if (e) {
                req.aiclass = e->aiclass;
                req.wakeups = e->wakeups;
        } else {
                ret = -ESRCH;
        }
        spin_unlock_irqrestore(&aios_lock, flags);

        if (!ret && copy_to_user(ureq, &req, sizeof req))
                ret = -EFAULT;
        return ret;
}

static long aios_unlocked_ioctl(struct file *f, unsigned int cmd,
                                unsigned long arg)
{
        switch (cmd) {
        case AIOS_IOC_REGISTER:   return aios_register((void __user *)arg);
        case AIOS_IOC_UNREGISTER: return aios_unregister((void __user *)arg);
        case AIOS_IOC_QUERY:      return aios_query((void __user *)arg);
        default:                  return -ENOTTY;
        }
}

static const struct file_operations aios_fops = {
        .owner          = THIS_MODULE,
        .unlocked_ioctl = aios_unlocked_ioctl,
        .llseek         = noop_llseek,
};

static struct miscdevice aios_misc = {
        .minor = MISC_DYNAMIC_MINOR,
        .name  = "aios",
        .mode  = 0666,
        .fops  = &aios_fops,
};

/* ------------------------------------------------------------------ */
/* /proc/aios                                                           */

static const char *const class_names[AIOS_CLASS_COUNT] = {
        "INTERACTIVE", "BATCH", "IDLE"
};

static int aios_proc_show(struct seq_file *m, void *v)
{
        struct aios_entry *e;
        unsigned long flags;
        int c, bkt;

        seq_printf(m, "# AI-OS intent registry (kernel bridge %s)\n",
                   AIOS_INTENT_VERSION);
        seq_printf(m, "# pid\tclass\twakeups\tcommand\n");

        spin_lock_irqsave(&aios_lock, flags);
        hash_for_each_rcu(aios_table, bkt, e, node) {
                seq_printf(m, "%d\t%s\t%llu\t%s\n",
                           e->pid, class_names[e->aiclass],
                           (unsigned long long)e->wakeups, e->comm);
        }
        spin_unlock_irqrestore(&aios_lock, flags);

        seq_printf(m, "# per-class wakeups\n");
        for (c = 0; c < AIOS_CLASS_COUNT; c++)
                seq_printf(m, "%s\t%lld\n", class_names[c],
                           (long long)atomic64_read(&aios_wakeups[c]));
        seq_printf(m, "registered\t%d\n", atomic_read(&aios_registered));
        return 0;
}

static int aios_proc_open(struct inode *inode, struct file *file)
{
        return single_open(file, aios_proc_show, NULL);
}

static const struct proc_ops aios_proc_ops = {
        .proc_open    = aios_proc_open,
        .proc_read    = seq_read,
        .proc_lseek   = seq_lseek,
        .proc_release = single_release,
};

/* ------------------------------------------------------------------ */

static int __init aios_intent_init(void)
{
        int ret, c;

        for (c = 0; c < AIOS_CLASS_COUNT; c++)
                atomic64_set(&aios_wakeups[c], 0);

        ret = misc_register(&aios_misc);
        if (ret) {
                pr_err("aios: cannot register /dev/aios: %d\n", ret);
                return ret;
        }

        ret = register_kprobe(&aios_wake_kp);
        if (ret) {
                pr_warn("aios: wakeup kprobe unavailable: %d "
                        "(registry only)\n", ret);
        }

        proc_create("aios", 0444, NULL, &aios_proc_ops);
        pr_info("aios: intent device ready — /dev/aios, /proc/aios "
                "(kernel bridge %s)\n", AIOS_INTENT_VERSION);
        return 0;
}

static void __exit aios_intent_exit(void)
{
        struct aios_entry *e;
        struct hlist_node *tmp;
        unsigned long flags;
        int bkt;

        remove_proc_entry("aios", NULL);
        if (aios_wake_kp.addr)
                unregister_kprobe(&aios_wake_kp);

        misc_deregister(&aios_misc);

        spin_lock_irqsave(&aios_lock, flags);
        hash_for_each_safe(aios_table, bkt, tmp, e, node) {
                hash_del(&e->node);
                kfree(e);
        }
        spin_unlock_irqrestore(&aios_lock, flags);
        pr_info("aios: intent device removed\n");
}

module_init(aios_intent_init);
module_exit(aios_intent_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("AI-OS");
MODULE_DESCRIPTION("AI-OS kernel bridge: intent registry + per-class scheduler accounting");
MODULE_VERSION(AIOS_INTENT_VERSION);
