#ifndef PROC_H
#define PROC_H

#include "riscv.h"
#include "types.h"

// Define the max syscall number
#define MAX_SYSCALL_NUM 500

#define NPROC (16)

// Saved registers for kernel context switches.
struct context {
    uint64 ra;
    uint64 sp;
    // callee-saved
    uint64 s0;
    uint64 s1;
    uint64 s2;
    uint64 s3;
    uint64 s4;
    uint64 s5;
    uint64 s6;
    uint64 s7;
    uint64 s8;
    uint64 s9;
    uint64 s10;
    uint64 s11;
};

enum procstate { UNUSED, USED, SLEEPING, RUNNABLE, RUNNING, ZOMBIE };

// Per-process state
struct proc {
    enum procstate state;
    int pid; // Process ID
    pagetable_t pagetable; // User page table
    uint64 ustack;
    uint64 kstack; // Virtual address of kernel stack
    struct trapframe *trapframe;
    struct context context; // swtch() here to run process
    uint64 max_page;
    
    // LAB1: newly added fields
    uint32 syscall_times[MAX_SYSCALL_NUM];
    uint64 time;
};

/*
* LAB1: define struct for TaskInfo here
*/
struct TaskInfo {
    int status;
    uint32 syscall_times[MAX_SYSCALL_NUM];
    int time;
};

struct proc *curr_proc();
void exit(int);
void yield();
void freeproc(struct proc *p);
void proc_init(void);
struct proc *allocproc(void);
void scheduler(void);
int threadid();
void swtch(struct context *old, struct context *new);

#endif // PROC_H