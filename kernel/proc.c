#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

struct cpu cpus[NCPU];

struct proc proc[NPROC];

struct proc *initproc;

int nextpid = 1;
struct spinlock pid_lock;

extern void forkret(void);
static void freeproc(struct proc *p);

extern char trampoline[]; // trampoline.S

// helps ensure that wakeups of wait()ing
// parents are not lost. helps obey the
// memory model when using p->parent.
// must be acquired before any p->lock.
struct spinlock wait_lock;

extern uint64 cas(volatile void *addr, int expected, int newval);

/*
  Process: Each process will have a field: <next_proc> that will hold the index (in proc[] array) of the next process of it's current list.
           (The value of the field will be between (0 - NPROC), or -1 if this process is the last one in the list).
           In addition, each process will have a field <index> that will hold the index in <proc[]> array of the process.
  CPU: There will be an array <cpus_list> of size <NCPU> that will hold the index of the firt process in the runnable list of the coresponding cpu.
       If the list is empty <cpus_lists> will be set to -1.
  States lists: Three global variables: <sleeping_list>, <zombie_list>, <unused_list> 
                Each variable will hold the index (in proc[] array) of the first process/ entry in the respective list.
                If the list is empty, the value of the global variable will be -1.
  ## All of the above fields and global variables will be initialized to -1.
*/

int sleeping_list;
int zombie_list;
int unused_list;

int cpus_lists[NCPU] = { [ 0 ... (NCPU - 1) ] = -1}; // List initialised to -1

// Allocate a page for each process's kernel stack.
// Map it high in memory, followed by an invalid
// guard page.
void
proc_mapstacks(pagetable_t kpgtbl) {
  struct proc *p;
  
  for(p = proc; p < &proc[NPROC]; p++) {
    char *pa = kalloc();
    if(pa == 0)
      panic("kalloc");
    uint64 va = KSTACK((int) (p - proc));
    kvmmap(kpgtbl, va, (uint64)pa, PGSIZE, PTE_R | PTE_W);
  }
}

// initialize the proc table at boot time.
void
procinit(void)
{
  struct proc *p;
  unused_list = -1;
  sleeping_list = -1;
  zombie_list = -1;
  
  initlock(&pid_lock, "nextpid");
  initlock(&wait_lock, "wait_lock");

  // Task 3.1.5.10
  for(p = proc; p < &proc[NPROC]; p++) {
    initlock(&p->lock, "proc");
    p->kstack = KSTACK((int) (p - proc));
    p->index = (p - proc);
    p->next_proc = -1;
    p->cpu_num = -1;
    add_link(&unused_list, p->index, -1);
  }
}

// Must be called with interrupts disabled,
// to prevent race with process being moved
// to a different CPU.
int
cpuid()
{
  int id = r_tp();
  return id;
}

// Return this CPU's cpu struct.
// Interrupts must be disabled.
struct cpu*
mycpu(void) {
  int id = cpuid();
  struct cpu *c = &cpus[id];
  return c;
}

// Return the current struct proc *, or zero if none.
struct proc*
myproc(void) {
  push_off();
  struct cpu *c = mycpu();
  struct proc *p = c->proc;
  pop_off();
  return p;
}

int
allocpid() { // Changed as required
  push_off();
  int pid;    
  do {
    pid = nextpid;
  }
  while(cas(&nextpid, pid, pid + 1));
  pop_off();
  return pid;
}

// Look in the process table for an UNUSED proc.
// If found, initialize state required to run in the kernel,
// and return with p->lock held.
// If there are no free procs, or a memory allocation fails, return 0.
static struct proc*
allocproc(void)
{
  // Task 3.1.5.11
  struct proc *p;
  int index = unused_list;
  if (index == -1){ return 0;} // No free entry in <proc[]> array

  p = proc + index;
  acquire(&p->lock);
  p->pid = allocpid();
  p->state = USED;
  remove_link(&unused_list, index);

  // Allocate a trapframe page.
  if((p->trapframe = (struct trapframe *)kalloc()) == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // An empty user page table.
  p->pagetable = proc_pagetable(p);
  if(p->pagetable == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // Set up new context to start executing at forkret,
  // which returns to user space.
  memset(&p->context, 0, sizeof(p->context));
  p->context.ra = (uint64)forkret;
  p->context.sp = p->kstack + PGSIZE;

  return p;
}


// free a proc structure and the data hanging from it,
// including user pages.
// p->lock must be held.
static void
freeproc(struct proc *p)
{
  if(p->trapframe)
    kfree((void*)p->trapframe);
  p->trapframe = 0;
  if(p->pagetable)
    proc_freepagetable(p->pagetable, p->sz);
  // Task 3.1.5.6
  remove_link(&zombie_list, p->index);
  add_link(&unused_list, p->index, -1);
  p->pagetable = 0;
  p->sz = 0;
  p->pid = 0;
  p->parent = 0;
  p->name[0] = 0;
  p->chan = 0;
  p->killed = 0;
  p->xstate = 0;
  p->cpu_num = -1;
  p->next_proc = -1;
  p->state = UNUSED;
  
}

// Create a user page table for a given process,
// with no user memory, but with trampoline pages.
pagetable_t
proc_pagetable(struct proc *p)
{
  pagetable_t pagetable;

  // An empty page table.
  pagetable = uvmcreate();
  if(pagetable == 0)
    return 0;

  // map the trampoline code (for system call return)
  // at the highest user virtual address.
  // only the supervisor uses it, on the way
  // to/from user space, so not PTE_U.
  if(mappages(pagetable, TRAMPOLINE, PGSIZE,
              (uint64)trampoline, PTE_R | PTE_X) < 0){
    uvmfree(pagetable, 0);
    return 0;
  }

  // map the trapframe just below TRAMPOLINE, for trampoline.S.
  if(mappages(pagetable, TRAPFRAME, PGSIZE,
              (uint64)(p->trapframe), PTE_R | PTE_W) < 0){
    uvmunmap(pagetable, TRAMPOLINE, 1, 0);
    uvmfree(pagetable, 0);
    return 0;
  }

  return pagetable;
}

// Free a process's page table, and free the
// physical memory it refers to.
void
proc_freepagetable(pagetable_t pagetable, uint64 sz)
{
  uvmunmap(pagetable, TRAMPOLINE, 1, 0);
  uvmunmap(pagetable, TRAPFRAME, 1, 0);
  uvmfree(pagetable, sz);
}

// a user program that calls exec("/init")
// od -t xC initcode
uchar initcode[] = {
  0x17, 0x05, 0x00, 0x00, 0x13, 0x05, 0x45, 0x02,
  0x97, 0x05, 0x00, 0x00, 0x93, 0x85, 0x35, 0x02,
  0x93, 0x08, 0x70, 0x00, 0x73, 0x00, 0x00, 0x00,
  0x93, 0x08, 0x20, 0x00, 0x73, 0x00, 0x00, 0x00,
  0xef, 0xf0, 0x9f, 0xff, 0x2f, 0x69, 0x6e, 0x69,
  0x74, 0x00, 0x00, 0x24, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00
};

// Set up first user process.
void
userinit(void)
{
  struct proc *p;
  p = allocproc();
  if(p == 0){ return;}
  initproc = p;
  
  // allocate one user page and copy init's instructions
  // and data into it.
  uvminit(p->pagetable, initcode, sizeof(initcode));
  p->sz = PGSIZE;

  // prepare for the very first "return" from kernel to user.
  p->trapframe->epc = 0;      // user program counter
  p->trapframe->sp = PGSIZE;  // user stack pointer

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  cas(&p->state, USED, RUNNABLE);
  add_link(&cpus_lists[0], p->index, 0); // Task 3.1.5.9
  release(&p->lock);
}

// Grow or shrink user memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint sz;
  struct proc *p = myproc();

  sz = p->sz;
  if(n > 0){
    if((sz = uvmalloc(p->pagetable, sz, sz + n)) == 0) {
      return -1;
    }
  } else if(n < 0){
    sz = uvmdealloc(p->pagetable, sz, sz + n);
  }
  p->sz = sz;
  return 0;
}

// Create a new process, copying the parent.
// Sets up child kernel stack to return as if from fork() system call.
int
fork(void)
{
  int i, j, pid;
  struct proc *np;
  struct proc *p = myproc();

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }
  // holding np->lock

  // Copy user memory from parent to child.
  if(uvmcopy(p->pagetable, np->pagetable, p->sz) < 0){
    freeproc(np);
    release(&np->lock);
    return -1;
  }
  np->sz = p->sz;

  // copy saved user registers.
  *(np->trapframe) = *(p->trapframe);

  // Cause fork to return 0 in the child.
  np->trapframe->a0 = 0;

  // increment reference counts on open file descriptors.
  for(i = 0; i < NOFILE; i++)
    if(p->ofile[i])
      np->ofile[i] = filedup(p->ofile[i]);
  np->cwd = idup(p->cwd);

  safestrcpy(np->name, p->name, sizeof(p->name));
  pid = np->pid;
  np->cpu_num = p->cpu_num;
  for(i = 0; i < NPROC; i++){
    if((proc + i)-> pid == pid){
      do{
        j = np->index;
      } while(cas(&(np->index), j, i));
      break;
    }
  }
  release(&np->lock);

  acquire(&wait_lock);
  np->parent = p;
  release(&wait_lock);

  add_link((cpus_lists + np->cpu_num), np->index, np->cpu_num); // 3.1.5.3 Add new process to the parent's CPU's RUNNABLE list
  cas(&np->state, USED, RUNNABLE);

  return pid;
}

// Pass p's abandoned children to init.
// Caller must hold wait_lock.
void
reparent(struct proc *p)
{
  struct proc *pp;

  for(pp = proc; pp < &proc[NPROC]; pp++){
    if(pp->parent == p){
      pp->parent = initproc;
      wakeup(initproc);
    }
  }
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait().
void
exit(int status)
{
  struct proc *p = myproc();

  if(p == initproc)
    panic("init exiting");

  // Close all open files.
  for(int fd = 0; fd < NOFILE; fd++){
    if(p->ofile[fd]){
      struct file *f = p->ofile[fd];
      fileclose(f);
      p->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(p->cwd);
  end_op();
  p->cwd = 0;

  acquire(&wait_lock);

  // Give any children to init.
  reparent(p);

  // Parent might be sleeping in wait().
  wakeup(p->parent);
  
  acquire(&p->lock);

  p->xstate = status;
  p->state = ZOMBIE;
  add_link(&zombie_list, p->index, -1); // Task 3.1.5.5

  release(&wait_lock);

  // Jump into the scheduler, never to return.
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(uint64 addr)
{
  struct proc *np;
  int havekids, pid;
  struct proc *p = myproc();
  acquire(&wait_lock);

  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(np = proc; np < &proc[NPROC]; np++){
      if(np->parent == p){
        // make sure the child isn't still in exit() or swtch().
        acquire(&np->lock);

        havekids = 1;
        if(np->state == ZOMBIE){
          // Found one.
          pid = np->pid;
          if(addr != 0 && copyout(p->pagetable, addr, (char *)&np->xstate,
                                  sizeof(np->xstate)) < 0) {
            release(&np->lock);
            release(&wait_lock);
            return -1;
          }
          freeproc(np);
          release(&np->lock);
          release(&wait_lock);
          return pid;
        }
        release(&np->lock);
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || p->killed){
      release(&wait_lock);
      return -1;
    }
    // Wait for a child to exit.
    sleep(p, &wait_lock);  //DOC: wait-sleep
  }
}

// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run.
//  - swtch to start running that process.
//  - eventually that process transfers control
//    via swtch back to the scheduler.
void
scheduler(void)
{
  struct proc *p = 0;
  struct cpu *c = mycpu();
  int index, i;
  int cpu_id = cpuid();
  int *first_link_loc;

  c->proc = 0;
  for(;;){
    // Avoid deadlock by ensuring that devices can interrupt.
    intr_on();

    // 3.1.5.4
    first_link_loc = cpus_lists + cpu_id;
    index = cpus_lists[cpu_id];
    //printf("scheduler: index = %d, cpu_id = %d\n", index, cpu_id); // TODO
    if(index != -1){ // CPU's RUNNABLE list isn't empty
      i = index;
      do {
        p = proc + index;
        acquire(&p->lock);
        if(remove_link(first_link_loc, index)){
          cas(&p->state, RUNNABLE, RUNNING);
          c->proc = p;
          swtch(&c->context, &p->context);
          add_link(first_link_loc, index, cpu_id);
          c->proc = 0;
        }
        release(&p->lock);
        i = index;
      } while(!cas(&index, i, *first_link_loc) && index != -1);
        // <first_link_loc> had changed in <remove_link>
    }
  }
}

// Switch to scheduler.  Must hold only p->lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->noff, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  int intena;
  struct proc *p = myproc();

  if(!holding(&p->lock))
    panic("sched p->lock");
  if(mycpu()->noff != 1)
    panic("sched locks");
  if(p->state == RUNNING)
    panic("sched running");
  if(intr_get())
    panic("sched interruptible");

  intena = mycpu()->intena;
  swtch(&p->context, &mycpu()->context);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  struct proc *p = myproc();
  acquire(&p->lock);
  p->state = RUNNABLE;
  add_link((cpus_lists + p->cpu_num), p->index, p->cpu_num); // Task 3.1.5.12
  sched();
  release(&p->lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch to forkret.
void
forkret(void)
{
  static int first = 1;

  // Still holding p->lock from scheduler.
  release(&myproc()->lock);

  if (first) {
    // File system initialization must be run in the context of a
    // regular process (e.g., because it calls sleep), and thus cannot
    // be run from main().
    first = 0;
    fsinit(ROOTDEV);
  }

  usertrapret();
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();
  int temp;
  
  // Must acquire p->lock in order to
  // change p->state and then call sched.
  // Once we hold p->lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup locks p->lock),
  // so it's okay to release lk.

  acquire(&p->lock);
  release(lk);

  p->chan = chan;
  remove_link(&(cpus_lists[p->cpu_num]), p->index);
  temp = p->state;
  cas(&p->state, temp, SLEEPING);
  add_link(&sleeping_list, p->index, -1);

  // Go to sleep.
  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  release(&p->lock);
  acquire(lk);
}

// Wake up all processes sleeping on chan.
// Must be called without any p->lock.
void
wakeup(void *chan)
{
  // Task 3.1.5.8
  struct proc *p;
  int index = sleeping_list;
  int old;
  if(index == -1){ // No sleeping processes
    return;
  }

  do{
    p = (proc + index);
    if(p != myproc()){
      acquire(&(p->lock));
      if (p->chan == chan){ // p is sleeping on <chan>
        printf("wakeup: p->chan = chan, p->pid = %d\n", p->pid); // TODO
        remove_link(&sleeping_list, index);
        p->chan = 0;
        if(!cas(&p->state, SLEEPING, RUNNABLE)){
          add_link(&(cpus_lists[p->cpu_num]), index, p->cpu_num);
        }
      }
      release(&p->lock);
    }
    old = index;
  } while(!cas(&index, old, sleeping_list) && index != -1) ;
}

// Kill the process with the given pid.
// The victim won't exit until it tries to return
// to user space (see usertrap() in trap.c).
int
kill(int pid)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++){
    acquire(&p->lock);
    if(p->pid == pid){
      p->killed = 1;
      if(p->state == SLEEPING){
        // Wake process from sleep().
        p->state = RUNNABLE;
      }
      release(&p->lock);
      return 0;
    }
    release(&p->lock);
  }
  return -1;
}

// Copy to either a user address, or kernel address,
// depending on usr_dst.
// Returns 0 on success, -1 on error.
int
either_copyout(int user_dst, uint64 dst, void *src, uint64 len)
{
  struct proc *p = myproc();
  if(user_dst){
    return copyout(p->pagetable, dst, src, len);
  } else {
    memmove((char *)dst, src, len);
    return 0;
  }
}

// Copy from either a user address, or kernel address,
// depending on usr_src.
// Returns 0 on success, -1 on error.
int
either_copyin(void *dst, int user_src, uint64 src, uint64 len)
{
  struct proc *p = myproc();
  if(user_src){
    return copyin(p->pagetable, dst, src, len);
  } else {
    memmove(dst, (char*)src, len);
    return 0;
  }
}

// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    "unused",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  struct proc *p;
  char *state;

  printf("\n");
  for(p = proc; p < &proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    printf("%d %s %s", p->pid, state, p->name);
    printf("\n");
  }
}

int
add_link(int *first_link, int new_proc_index, int cpu_num){
  /*If the list is a CPU's RUNNABLE list, <first_link> is a pointer to <cpus_lists[cpu id]>*/
  /*If the list is a global variable, <first_link> is a pointer to <state_list>, <cpu_num> is -1*/
  
  int curr_link;
  int next_link;
  int cpu;

  if (new_proc_index < 0 || new_proc_index > NPROC){ // Imposible index
    return -1;
  }

  // Empty list
  if(!cas(first_link, -1, new_proc_index)){ // Set the pointer to the new first link index
    cpu = (proc + new_proc_index)->cpu_num;
    if(cpu_num > 0){ // CPU's RUNNABLE list
      cas(&((proc + new_proc_index)->cpu_num), cpu, cpu_num);
    }
    curr_link = (proc + new_proc_index)->next_proc;
    return !cas(&((proc + new_proc_index)->next_proc), curr_link, -1); // Set last link <next_proc> to -1
  }
  else{
    // Find the last link
    curr_link = *first_link; // Will hold the index in <proc[]> array of the current link
    do {
      next_link = curr_link;
    } while (((proc + next_link)->next_proc >= 0) && !cas(&curr_link, next_link, (proc + next_link)->next_proc)) ;
    // <curr_link> holds the last link

    if (!cas(&((proc + curr_link)->next_proc), -1, new_proc_index)){ // Set last link <next_proc> to <new_proc_index>
       cpu = (proc + new_proc_index)->cpu_num;
      if(cpu_num > 0){ // CPU's RUNNABLE list
        cas(&((proc + new_proc_index)->cpu_num), cpu, cpu_num);
      } 
      curr_link = (proc + new_proc_index)->next_proc;
      return !cas(&((proc + new_proc_index)->next_proc), curr_link, -1); // Set last link <next_proc> to -1
    }
    return -1;
  }
}


int
remove_link(int *first_link, int proc_to_remove_index){

  int curr_link = *first_link;
  int prev_link;
  int next_link;

  // TODO: Check if return value of a list that doesn't contain <pid_to_remove> should be 0 / -1
  if (*first_link == -1) { // Empty list
    return -1;
  }

  if(*first_link == proc_to_remove_index){ // Remove first link
    next_link = (proc + proc_to_remove_index)->next_proc; // Can be -1
    return !(cas(first_link, curr_link, next_link));
  }

  do {
    prev_link = curr_link;
  } while(!cas(&curr_link, prev_link, (proc + prev_link)->next_proc) && curr_link != proc_to_remove_index) ;
  // <prev_link> holds the index of the link "pointing" at <pid_to_remove>
  // <curr_link> holds the index of <pid_to_remove>
  next_link = (proc + curr_link)->next_proc; // Holds the location of the link <pid_to_remove> "points" at.
  return !cas(&((proc + prev_link)->next_proc), curr_link, next_link); // Set the next link <prev_link> "points" at to <next_link>
}

// For debugging
int
print_list(int loop_size){
  struct  proc *p;
  int list = -1;
  
  for (p = proc; p < &proc[NPROC]; p++){
      add_link(&list, p->pid, -1);
      //printf("link added: %d\n", p->pid);
  }
  int link = list;
  int count = 0;
  int temp;
  do {
    temp = link;
    count++;
    //printf("link index: %d\n", temp);
  }
  while (!cas(&link, temp, (proc + link)->next_proc)) ; 
  printf("list size: %d\n", count);
  return 0;
}

// 3.1.5.1
int
set_cpu(int cpu_num){
  struct  proc *p = myproc();
  remove_link((cpus_lists + p->cpu_num), p->index);
  // TODO: find out how to yield the cpu
  add_link((cpus_lists + cpu_num), p->index, cpu_num);

  
  return -1;
}

// 3.1.5.2
int
get_cpu(void){
  int cpu_num = -1;
  intr_off();
  cpu_num = cpuid();
  return cpu_num;
}