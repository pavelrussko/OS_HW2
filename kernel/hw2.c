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
        int clearance = char_to_clr(clr);
        struct task_struct *pcb;
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
        int result = (pcb->clearance_flags & clearance) ? 1 : 0;
        rcu_read_unlock();
        
        // Check whether calling process has specified clearance
        if(!(current->clearance_flags & clearance)) {
            return -EPERM;
        }
        return result;
}

MODULE_LICENSE("GPL");
