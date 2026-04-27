#include "syscall.h"
#include "console.h"
#include "defs.h"
#include "loader.h"
#include "syscall_ids.h"
#include "timer.h"
#include "trap.h"

uint64 console_write(uint64 va, uint64 len)
{
	struct proc *p = curr_proc();
	char str[MAX_STR_LEN];
	int size = copyinstr(p->pagetable, str, va, MIN(len, MAX_STR_LEN));
	tracef("write size = %d", size);
	for (int i = 0; i < size; ++i) {
		console_putchar(str[i]);
	}
	return len;
}

uint64 console_read(uint64 va, uint64 len)
{
	struct proc *p = curr_proc();
	char str[MAX_STR_LEN];
	tracef("read size = %d", len);
	for (int i = 0; i < len; ++i) {
		int c = consgetc();
		str[i] = c;
	}
	copyout(p->pagetable, va, str, len);
	return len;
}

uint64 sys_write(int fd, uint64 va, uint64 len)
{
	if (fd < 0 || fd > FD_BUFFER_SIZE)
		return -1;
	struct proc *p = curr_proc();
	struct file *f = p->files[fd];
	if (f == NULL) {
		errorf("invalid fd %d\n", fd);
		return -1;
	}
	switch (f->type) {
	case FD_STDIO:
		return console_write(va, len);
	case FD_INODE:
		return inodewrite(f, va, len);
	default:
		panic("unknown file type %d\n", f->type);
	}
}

uint64 sys_read(int fd, uint64 va, uint64 len)
{
	if (fd < 0 || fd > FD_BUFFER_SIZE)
		return -1;
	struct proc *p = curr_proc();
	struct file *f = p->files[fd];
	if (f == NULL) {
		errorf("invalid fd %d\n", fd);
		return -1;
	}
	switch (f->type) {
	case FD_STDIO:
		return console_read(va, len);
	case FD_INODE:
		return inoderead(f, va, len);
	default:
		panic("unknown file type %d\n", f->type);
	}
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

uint64 sys_gettimeofday(uint64 val, int _tz)
{
	struct proc *p = curr_proc();
	uint64 cycle = get_cycle();
	TimeVal t;
	t.sec = cycle / CPU_FREQ;
	t.usec = (cycle % CPU_FREQ) * 1000000 / CPU_FREQ;
	copyout(p->pagetable, val, (char *)&t, sizeof(TimeVal));
	return 0;
}

uint64 sys_getpid()
{
	return curr_proc()->pid;
}

uint64 sys_getppid()
{
	struct proc *p = curr_proc();
	return p->parent == NULL ? IDLE_PID : p->parent->pid;
}

uint64 sys_clone()
{
	debugf("fork!");
	return fork();
}

static inline uint64 fetchaddr(pagetable_t pagetable, uint64 va)
{
	uint64 *addr = (uint64 *)useraddr(pagetable, va);
	return *addr;
}

uint64 sys_exec(uint64 path, uint64 uargv)
{
	struct proc *p = curr_proc();
	char name[MAX_STR_LEN];
	copyinstr(p->pagetable, name, path, MAX_STR_LEN);
	uint64 arg;
	static char strpool[MAX_ARG_NUM][MAX_STR_LEN];
	char *argv[MAX_ARG_NUM];
	int i;
	for (i = 0; uargv && (arg = fetchaddr(p->pagetable, uargv));
	     uargv += sizeof(char *), i++) {
		copyinstr(p->pagetable, (char *)strpool[i], arg, MAX_STR_LEN);
		argv[i] = (char *)strpool[i];
	}
	argv[i] = NULL;
	return exec(name, (char **)argv);
}

uint64 sys_wait(int pid, uint64 va)
{
	struct proc *p = curr_proc();
	int *code = (int *)useraddr(p->pagetable, va);
	return wait(pid, code);
}

uint64 sys_spawn(uint64 va)
{
	struct proc *p = curr_proc();
	char name[200];
	if (copyinstr(p->pagetable, name, va, sizeof(name)) < 0)
		return -1;
	return spawn(name);
}

uint64 sys_set_priority(long long prio)
{
	return set_priority(prio);
}

uint64 sys_task_info(uint64 va)
{
	struct proc *p = curr_proc();
	struct TaskInfo info;
	info.status = p->state;
	info.time = (get_cycle() / (CPU_FREQ / 1000)) - p->time;
	for (int i = 0; i < MAX_SYSCALL_NUM; i++) {
		info.syscall_times[i] = p->syscall_times[i];
	}
	if (copyout(p->pagetable, va, (char *)&info, sizeof(info)) < 0)
		return -1;
	return 0;
}

uint64 sys_openat(uint64 va, uint64 omode, uint64 _flags)
{
	struct proc *p = curr_proc();
	char path[200];
	copyinstr(p->pagetable, path, va, 200);
	return fileopen(path, omode);
}

uint64 sys_close(int fd)
{
	if (fd < 0 || fd > FD_BUFFER_SIZE)
		return -1;
	struct proc *p = curr_proc();
	struct file *f = p->files[fd];
	if (f == NULL) {
		errorf("invalid fd %d", fd);
		return -1;
	}
	fileclose(f);
	p->files[fd] = 0;
	return 0;
}

int sys_fstat(int fd, uint64 stat)
{
	if (fd < 0 || fd >= FD_BUFFER_SIZE)
		return -1;
	struct proc *p = curr_proc();
	struct file *f = p->files[fd];
	if (f == NULL || f->type != FD_INODE || f->ip == NULL)
		return -1;

	ivalid(f->ip);

	Stat s;
	s.dev = f->ip->dev;
	s.ino = f->ip->inum;
	s.mode = (f->ip->type == T_DIR) ? DIR : FILE;
	s.nlink = f->ip->nlink;
	for (int i = 0; i < 7; i++)
		s.pad[i] = 0;

	if (copyout(p->pagetable, stat, (char *)&s, sizeof(s)) < 0)
		return -1;
	return 0;
}

int sys_linkat(int olddirfd, uint64 oldpath, int newdirfd, uint64 newpath,
	       uint64 flags)
{
	struct proc *p = curr_proc();
	char old[MAXPATH], new[MAXPATH];
	if (copyinstr(p->pagetable, old, oldpath, sizeof(old)) < 0)
		return -1;
	if (copyinstr(p->pagetable, new, newpath, sizeof(new)) < 0)
		return -1;
	if (strncmp(old, new, MAXPATH) == 0)
		return -1;

	struct inode *ip = namei(old);
	if (ip == 0)
		return -1;
	ivalid(ip);

	struct inode *dp = root_dir();
	if (dirlink(dp, new, ip->inum) < 0) {
		iput(ip);
		iput(dp);
		return -1;
	}

	ip->nlink++;
	iupdate(ip);
	iput(ip);
	iput(dp);
	return 0;
}

int sys_unlinkat(int dirfd, uint64 name, uint64 flags)
{
	struct proc *p = curr_proc();
	char path[MAXPATH];
	if (copyinstr(p->pagetable, path, name, sizeof(path)) < 0)
		return -1;

	struct inode *dp = root_dir();
	struct inode *ip = dirlookup(dp, path, 0);
	if (ip == 0) {
		iput(dp);
		return -1;
	}
	ivalid(ip);

	if (ip->nlink < 1) {
		iput(ip);
		iput(dp);
		return -1;
	}
	ip->nlink--;
	iupdate(ip);

	if (dirunlink(dp, path) < 0) {
		iput(ip);
		iput(dp);
		return -1;
	}

	iput(ip);
	iput(dp);
	return 0;
}

uint64 sys_mmap(uint64 start, uint64 len, int prot)
{
	if ((start & (PGSIZE - 1)) != 0)
		return -1;
	if ((prot & ~0x7) != 0 || (prot & 0x7) == 0)
		return -1;
	if (len == 0)
		return 0;

	struct proc *p = curr_proc();
	int perm = PTE_U;
	if (prot & 0x1) perm |= PTE_R;
	if (prot & 0x2) perm |= PTE_W;
	if (prot & 0x4) perm |= PTE_X;

	uint64 npages = PGROUNDUP(len) / PGSIZE;

	for (uint64 i = 0; i < npages; i++) {
		if (walkaddr(p->pagetable, start + i * PGSIZE) != 0)
			return -1;
	}

	for (uint64 i = 0; i < npages; i++) {
		uint64 va = start + i * PGSIZE;
		void *page = kalloc();
		if (page == 0) {
			uvmunmap(p->pagetable, start, i, 1);
			return -1;
		}
		memset(page, 0, PGSIZE);
		if (mappages(p->pagetable, va, PGSIZE, (uint64)page, perm) != 0) {
			kfree(page);
			uvmunmap(p->pagetable, start, i, 1);
			return -1;
		}
	}

	uint64 end_pg = (start + npages * PGSIZE) / PAGE_SIZE;
	if (end_pg > p->max_page)
		p->max_page = end_pg;

	return 0;
}

int sys_munmap(uint64 start, uint64 len)
{
	if ((start & (PGSIZE - 1)) != 0)
		return -1;
	if (len == 0)
		return 0;

	struct proc *p = curr_proc();
	uint64 npages = PGROUNDUP(len) / PGSIZE;

	for (uint64 i = 0; i < npages; i++) {
		if (walkaddr(p->pagetable, start + i * PGSIZE) == 0)
			return -1;
	}

	uvmunmap(p->pagetable, start, npages, 1);
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
	if (id >= 0 && id < MAX_SYSCALL_NUM) {
		curr_proc()->syscall_times[id]++;
	}
	switch (id) {
	case SYS_write:
		ret = sys_write(args[0], args[1], args[2]);
		break;
	case SYS_read:
		ret = sys_read(args[0], args[1], args[2]);
		break;
	case SYS_openat:
		ret = sys_openat(args[0], args[1], args[2]);
		break;
	case SYS_close:
		ret = sys_close(args[0]);
		break;
	case SYS_exit:
		sys_exit(args[0]);
		// __builtin_unreachable();
	case SYS_sched_yield:
		ret = sys_sched_yield();
		break;
	case SYS_gettimeofday:
		ret = sys_gettimeofday(args[0], args[1]);
		break;
	case SYS_getpid:
		ret = sys_getpid();
		break;
	case SYS_getppid:
		ret = sys_getppid();
		break;
	case SYS_clone: // SYS_fork
		ret = sys_clone();
		break;
	case SYS_execve:
		ret = sys_exec(args[0], args[1]);
		break;
	case SYS_wait4:
		ret = sys_wait(args[0], args[1]);
		break;
	case SYS_fstat:
	    ret = sys_fstat(args[0],args[1]);
		break;
	case SYS_linkat:
	    ret = sys_linkat(args[0],args[1],args[2],args[3],args[4]);
		break;
	case SYS_unlinkat:
	    ret = sys_unlinkat(args[0],args[1],args[2]);
		break;
	case SYS_spawn:
		ret = sys_spawn(args[0]);
		break;
	case SYS_set_priority:
		ret = sys_set_priority((long long)args[0]);
		break;
	case SYS_task_info:
		ret = sys_task_info(args[0]);
		break;
	case SYS_mmap:
		ret = sys_mmap(args[0], args[1], args[2]);
		break;
	case SYS_munmap:
		ret = sys_munmap(args[0], args[1]);
		break;
	default:
		ret = -1;
		errorf("unknown syscall %d", id);
	}
	trapframe->a0 = ret;
	tracef("syscall ret %d", ret);
}
