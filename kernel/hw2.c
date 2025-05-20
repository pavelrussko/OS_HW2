#include <linux/errno.h>
#include <linux/irqflags.h>

#include <linux/kernel.h>
#include <linux/uidgid.h>
#include <linux/cred.h>
#include <linux/sched.h>
#include <linux/sched/task.h>
#include <linux/syscalls.h>
#include <linux/pid.h>


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
