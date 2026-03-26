#include "syscall.h"
#include "defs.h"
#include "loader.h"
#include "syscall_ids.h"
#include "timer.h"
#include "trap.h"
#include "proc.h"
#include "vm.h"
#include "riscv.h"

// Define mmap and munmap syscall IDs as specified in the project 
#define SYS_mmap 222
#define SYS_munmap 215

uint64 sys_write(int fd, uint64 va, uint len)
{
    debugf("sys_write fd = %d va = %x, len = %d", fd, va, len);
    if (fd != STDOUT)
        return -1;
    struct proc *p = curr_proc();
    char str[MAX_STR_LEN];
    int size = copyinstr(p->pagetable, str, va, MIN(len, MAX_STR_LEN));
    debugf("size = %d", size);
    for (int i = 0; i < size; ++i) {
        console_putchar(str[i]);
    }
    return size;
}

__attribute__((noreturn)) void sys_exit(int code)
{
    exit(code);
    __builtin_unreachable();
}

uint64 sys_sched_yield()
{
    yield();
    return 0;
}

uint64 sys_gettimeofday(uint64 va, int _tz) 
{
    TimeVal k_val;
    uint64 cycle = get_cycle();
    k_val.sec = cycle / CPU_FREQ;
    k_val.usec = (cycle % CPU_FREQ) * 1000000 / CPU_FREQ;
    
    // Copy from kernel stack to user virtual address
	if (copyout(curr_proc()->pagetable, va, (char *)&k_val, sizeof(TimeVal)) < 0) {
        return -1;
    }
    return 0;
}

uint64 sys_task_info(uint64 user_addr) 
{
    struct proc *p = curr_proc();
    struct TaskInfo info;
    
	info.status = 2;
    
    info.time = (get_cycle() / (CPU_FREQ / 1000)) - p->time;
    
    for(int i = 0; i < MAX_SYSCALL_NUM; i++) {
        info.syscall_times[i] = p->syscall_times[i];
    }
    
    if (copyout(p->pagetable, user_addr, (char *)&info, sizeof(struct TaskInfo)) < 0) {
        return -1;
    }
    return 0;
}

uint64 sys_mmap(uint64 start, uint64 len, int port, int flag, int fd)
{
    if (len == 0) return 0;
    if (len > 1024 * 1024 * 1024) return -1; // Arbitrary 1GiB limit check per specs
    if (!PGALIGNED(start)) return -1;
    
    // port bit 0: readable, bit 1: writable, bit 2: executable
    if ((port & ~0x7) != 0) return -1; // other bits must be 0
    if ((port & 0x7) == 0) return -1;  // must have at least one valid permission

    len = PGROUNDUP(len);
    struct proc *p = curr_proc();
    
    // construct PTE permissions
    int perm = PTE_U | PTE_V;
    if (port & 1) perm |= PTE_R;
    if (port & 2) perm |= PTE_W;
    if (port & 4) perm |= PTE_X;

    // check if any pages in the requested range are already mapped
    for (uint64 a = start; a < start + len; a += PGSIZE) {
        pte_t *pte = walk(p->pagetable, a, 0);
        if (pte != 0 && (*pte & PTE_V)) {
            return -1;
        }
    }

    // allocate and map physical pages page-by-page
    for (uint64 a = start; a < start + len; a += PGSIZE) {
        void *pa = kalloc();
        if (pa == 0) {
            return -1; // insufficient physical memory
        }
        if (mappages(p->pagetable, a, PGSIZE, (uint64)pa, perm) != 0) {
            kfree(pa);
            return -1;
        }
    }
    
    return 0;
}

uint64 sys_munmap(uint64 start, uint64 len)
{
    if (!PGALIGNED(start)) return -1;
    
    len = PGROUNDUP(len);
    struct proc *p = curr_proc();

    // check if unmapped virtual memory exists in the range
    for (uint64 a = start; a < start + len; a += PGSIZE) {
        pte_t *pte = walk(p->pagetable, a, 0);
        if (pte == 0 || (*pte & PTE_V) == 0) {
            return -1;
        }
    }

    // unmap virtual memory. removes mappings and do_free=1 frees physical memory.
    uvmunmap(p->pagetable, start, len / PGSIZE, 1);
    
    return 0;
}

extern char trap_page[];

void syscall()
{
    struct trapframe *trapframe = curr_proc()->trapframe;
    int id = trapframe->a7, ret;
    uint64 args[6] = { trapframe->a0, trapframe->a1, trapframe->a2,
               trapframe->a3, trapframe->a4, trapframe->a5 };
    tracef("syscall %d args = [%x, %x, %x, %x, %x, %x]", id, args[0],
           args[1], args[2], args[3], args[4], args[5]);
           
    // Update syscall counter for task info
    if (id >= 0 && id < MAX_SYSCALL_NUM) {
        curr_proc()->syscall_times[id]++;
    }

    switch (id) {
    case SYS_write:
        ret = sys_write(args[0], args[1], args[2]);
        break;
    case SYS_exit:
        sys_exit(args[0]);
        // __builtin_unreachable();
    case SYS_sched_yield:
        ret = sys_sched_yield();
        break;
    case SYS_gettimeofday:
        // args[0] is the virtual address pointer passed from user space
        ret = sys_gettimeofday(args[0], args[1]);
        break;
    case SYS_task_info:
        ret = sys_task_info(args[0]);
        break;
    case SYS_mmap:
        ret = sys_mmap(args[0], args[1], args[2], args[3], args[4]);
        break;
    case SYS_munmap:
        ret = sys_munmap(args[0], args[1]);
        break;
	case SYS_getpid:
        ret = curr_proc()->pid;
        break;
    default:
        ret = -1;
        errorf("unknown syscall %d", id);
    }
    trapframe->a0 = ret;
    tracef("syscall ret %d", ret);
};