#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/module.h>
#include <linux/list.h>
#include <linux/cred.h>     // for current_euid()
#include <linux/uidgid.h>
#include <linux/rcupdate.h>
#include <linux/errno.h> // for EINVAL EPERM ERSCH


enum Clearance {
    SWORD = 1,
    MIDNIGHT = 2,
    CLAMP = 4,
    DUTY = 8,
    ISOLATE = 16
};

int to_bool(int a) {
    return a > 0 ? 1 : 0;
}

static int char_to_clr(char c)
{
    switch (c) {
    case 's': return SWORD;
    case 'm': return MIDNIGHT;
    case 'c': return CLAMP;
    case 'd': return DUTY;
    case 'i': return ISOLATE;
    default:  return -1;
    }
}

asmlinkage long sys_hello(void) {
    printk("Hello, World!\n");
    return 0;
}

asmlinkage long sys_set_sec(int sword, int midnight, int clamp, int duty, int isolate){
    // Check correctness of arguments
    if(sword < 0 || midnight < 0 || clamp < 0 || duty < 0 || isolate < 0) {
        return -EINVAL;
    }

    // Check permission
    if(!uid_eq(current_euid(), GLOBAL_ROOT_UID)) {
        return -EPERM;
    }
    current->clearance_flags = 0;
    current->clearance_flags |= to_bool(sword) * SWORD;
    current->clearance_flags |= to_bool(midnight) * MIDNIGHT;
    current->clearance_flags |= to_bool(clamp) * CLAMP;
    current->clearance_flags |= to_bool(duty) * DUTY;
    current->clearance_flags |= to_bool(isolate) * ISOLATE;
    printk("logging: set_sec pid: %d\n", current->pid);
    printk("logging: pcb after set_sec: %d, should be %d, %d, %d, %d, %d\n", current->clearance_flags, sword, midnight, clamp, duty, isolate);

    return 0;
}

asmlinkage long sys_get_sec(char clr)
{
    int clearance = char_to_clr(clr);

    if (clearance < 0)
        return -EINVAL;

    return !!(current->clearance_flags & clearance);
}


asmlinkage long sys_check_sec(pid_t pid, char clr) {
	struct task_struct* pcb;
	int clearance = char_to_clr(clr);
    int result = 0;
        // Check correctness of the input
        if (clearance == -1) {
            return -EINVAL;
        }
        // Check whether process with given pid exists
        rcu_read_lock();
        pcb = find_task_by_vpid(pid);
        if (!pcb) {
            rcu_read_unlock();
            return -ESRCH;
        }
    printk("logging: set_sec called by PID %d, flags set to: %d\n", current->pid, current->clearance_flags);
    printk("logging: pid of found process: %d, should be: %d\n", pcb->pid, pid);
    printk("logging: clearance flag of pcb: %d\n", pcb->clearance_flags);
    printk("logging: clearance got: %d\n", clearance);
    //result = (pcb->clearance_flags & clearance) ? 1 : 0;
    if (pcb->clearance_flags & clearance) {
        result = 1;
    }
    rcu_read_unlock();
    // Check whether calling process has specified clearance
    if(!(current->clearance_flags & clearance)) {
        return -EPERM;
    }
    return result;
}
/* First quick try
asmlinkage long sys_flip_sec_branch(int height, char clr)
{
    int mask, added = 0;
    struct task_struct *p;

    // validate args
    mask = char_to_clr(clr);
    if (height <= 0 || mask < 0)
        return -EINVAL;

    // check clearance
    if (!(current->clearance_flags & mask))
        return -EPERM;

    // climb to the parents
    p = current;

    // go up until we dont have a parent or height reached 0
    while (height-- > 0 && p->parent) {
        p = p->parent;
        if (p->clearance_flags & mask) {
            // if the parent already had the bit, we will change it back by doing AND with the compliment of mask
            p->clearance_flags &= ~mask;
        } else {
            // we will set the new bit and then increment added by one
            p->clearance_flags |= mask;
            added++;
        }
    }

    return added;
}
*/

asmlinkage long sys_flip_sec_branch(int height, char clr) {
    struct task_struct* task;
    int clearance = char_to_clr(clr);
    int gained_count = 0;
    // Check correctness of the input
    if (height <= 0 || clearance == -1) {
        return -EINVAL;
    }
    // Check permission
    task = current;
    if (!(current->clearance_flags & clearance)) {
        return -EPERM;
    }
    while (height-- > 0) {
        task = task->real_parent;
        if (!(task->clearance_flags & clearance)) {
            gained_count++;
        }
        task->clearance_flags ^= clearance;
        // Check whether at init process
        if (task->pid == 1){
            break;
        }
    }
    return gained_count;
}

MODULE_LICENSE("GPL");
