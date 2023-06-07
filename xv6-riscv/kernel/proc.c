#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#define max(a, b) (a > b) ? (a) : (b)
#define min(a, b) (a < b) ? (a) : (b)
struct cpu cpus[NCPU];
struct MLFQ_Queue queuetable[QCOUNT];
struct proc proc[NPROC];
// from FreeBSD.
int do_rand(unsigned long *ctx)
{
  /*
   * Compute x = (7^5 * x) mod (2^31 - 1)
   * without overflowing 31 bits:
   *      (2^31 - 1) = 127773 * (7^5) + 2836
   * From "Random number generators: good ones are hard to find",
   * Park and Miller, Communications of the ACM, vol. 31, no. 10,
   * October 1988, p. 1195.
   */
  long hi, lo, x;

  /* Transform to [1, 0x7ffffffe] range. */
  x = (*ctx % 0x7ffffffe) + 1;
  hi = x / 127773;
  lo = x % 127773;
  x = 16807 * lo - 2836 * hi;
  if (x < 0)
    x += 0x7fffffff;
  /* Transform to [0, 0x7ffffffd] range. */
  x--;
  *ctx = x;
  return (x);
}

unsigned long rand_next = 1;

int rand(void)
{
  return (do_rand(&rand_next));
}
uint64 tickettotal(void)
{
  struct proc *p;
  uint64 sum = 0;
  for (p = proc; p < &proc[NPROC]; p++)
  {
    if (p->state == RUNNABLE)
      sum += p->tickets;
  }
  return sum;
}
uint64 changetickets(int tickets)
{
  int pid = myproc()->pid;
  struct proc *p = 0;
  for (p = proc; p < &proc[NPROC]; p++)
  {
    if (p->pid == pid)
    {
      p->tickets = tickets;
      break;
    }
  }
  return pid;
}
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

// Allocate a page for each process's kernel stack.
// Map it high in memory, followed by an invalid
// guard page.
void proc_mapstacks(pagetable_t kpgtbl)
{
  struct proc *p;

  for (p = proc; p < &proc[NPROC]; p++)
  {
    char *pa = kalloc();
    if (pa == 0)
      panic("kalloc");
    uint64 va = KSTACK((int)(p - proc));
    kvmmap(kpgtbl, va, (uint64)pa, PGSIZE, PTE_R | PTE_W);
  }
}

// initialize the proc table at boot time.
void procinit(void)
{
  struct proc *p;

  initlock(&pid_lock, "nextpid");
  initlock(&wait_lock, "wait_lock");
  for (p = proc; p < &proc[NPROC]; p++)
  {
    initlock(&p->lock, "proc");
    p->kstack = KSTACK((int)(p - proc));
  }
}

// Must be called with interrupts disabled,
// to prevent race with process being moved
// to a different CPU.
int cpuid()
{
  int id = r_tp();
  return id;
}

// Return this CPU's cpu struct.
// Interrupts must be disabled.
struct cpu *
mycpu(void)
{
  int id = cpuid();
  struct cpu *c = &cpus[id];
  return c;
}

// Return the current struct proc *, or zero if none.
struct proc *
myproc(void)
{
  push_off();
  struct cpu *c = mycpu();
  struct proc *p = c->proc;
  pop_off();
  return p;
}

int allocpid()
{
  int pid;

  acquire(&pid_lock);
  pid = nextpid;
  nextpid = nextpid + 1;
  release(&pid_lock);

  return pid;
}

// Look in the process table for an UNUSED proc.
// If found, initialize state required to run in the kernel,
// and return with p->lock held.
// If there are no free procs, or a memory allocation fails, return 0.
static struct proc *
allocproc(void)
{
  struct proc *p;

  for (p = proc; p < &proc[NPROC]; p++)
  {
    acquire(&p->lock);
    if (p->state == UNUSED)
    {
      goto found;
    }
    else
    {
      release(&p->lock);
    }
  }
  return 0;

found:
  p->pid = allocpid();
  p->state = USED;

  // Allocate a trapframe page.
  if ((p->trapframe = (struct trapframe *)kalloc()) == 0)
  {
    freeproc(p);
    release(&p->lock);
    return 0;
  }
  if ((p->trapframe_copy = (struct trapframe *)kalloc()) == 0)
  {
    release(&p->lock);
    return 0;
  }
  // An empty user page table.
  p->pagetable = proc_pagetable(p);
  if (p->pagetable == 0)
  {
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // Set up new context to start executing at forkret,
  // which returns to user space.
  memset(&p->context, 0, sizeof(p->context));
  // sigalarm
  p->is_sigalarm = 0;
  // p->ticks = 0;
  // p->ticks_passed = 0;
  p->handler = 0;
  // sigalarm end
  p->context.ra = (uint64)forkret;
  p->context.sp = p->kstack + PGSIZE;
  p->timeofcreation = ticks;
  p->staticpriority = 60;
  p->starttime = 0;
  p->integermask = 0;
  p->scheduletick = 0;
  p->pbsrtime = 0;
  p->pbsstime = 0;
  p->tickets = 1;
  p->numscheduled = 0;
  p->totalrtime = 0;
  p->qlevel = 0;
  p->qstate = DEQUED;
  p->qentrytime = 0;
  p->rtime = 0;
  p->etime = 0;
  p->ctime = ticks;
  p->q[0] = 0;
  p->q[1] = 0;
  p->q[2] = 0;
  p->q[3] = 0;
  p->q[4] = 0;

  return p;
}

// free a proc structure and the data hanging from it,
// including user pages.
// p->lock must be held.
static void
freeproc(struct proc *p)
{
  if (p->trapframe)
  {
    kfree((void *)p->trapframe);
  }
  // difference
  if (p->trapframe_copy)
  {
    kfree((void *)p->trapframe_copy);
  }
  //
  p->trapframe = 0;
  if (p->pagetable)
  {
    proc_freepagetable(p->pagetable, p->sz);
  }
  p->pagetable = 0;
  p->sz = 0;
  p->pid = 0;
  p->parent = 0;
  p->name[0] = 0;
  p->chan = 0;
  p->killed = 0;
  p->xstate = 0;
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
  if (pagetable == 0)
    return 0;

  // map the trampoline code (for system call return)
  // at the highest user virtual address.
  // only the supervisor uses it, on the way
  // to/from user space, so not PTE_U.
  if (mappages(pagetable, TRAMPOLINE, PGSIZE,
               (uint64)trampoline, PTE_R | PTE_X) < 0)
  {
    uvmfree(pagetable, 0);
    return 0;
  }

  // map the trapframe just below TRAMPOLINE, for trampoline.S.
  if (mappages(pagetable, TRAPFRAME, PGSIZE,
               (uint64)(p->trapframe), PTE_R | PTE_W) < 0)
  {
    uvmunmap(pagetable, TRAMPOLINE, 1, 0);
    uvmfree(pagetable, 0);
    return 0;
  }

  return pagetable;
}

// Free a process's page table, and free the
// physical memory it refers to.
void proc_freepagetable(pagetable_t pagetable, uint64 sz)
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
    0x00, 0x00, 0x00, 0x00};

// Set up first user process.
void userinit(void)
{
  struct proc *p;

  p = allocproc();
  initproc = p;

  // allocate one user page and copy init's instructions
  // and data into it.
  uvmfirst(p->pagetable, initcode, sizeof(initcode));
  p->sz = PGSIZE;

  // prepare for the very first "return" from kernel to user.
  p->trapframe->epc = 0;     // user program counter
  p->trapframe->sp = PGSIZE; // user stack pointer

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  p->state = RUNNABLE;
  p->tickets = 1;
  release(&p->lock);
}

// Grow or shrink user memory by n bytes.
// Return 0 on success, -1 on failure.
int growproc(int n)
{
  uint sz;
  struct proc *p = myproc();

  sz = p->sz;
  if (n > 0)
  {
    if ((sz = uvmalloc(p->pagetable, sz, sz + n, PTE_W)) == 0)
    {
      return -1;
    }
  }
  else if (n < 0)
  {
    sz = uvmdealloc(p->pagetable, sz, sz + n);
  }
  p->sz = sz;
  return 0;
}

// Create a new process, copying the parent.
// Sets up child kernel stack to return as if from fork() system call.
int fork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *p = myproc();

  // Allocate process.
  if ((np = allocproc()) == 0)
  {
    return -1;
  }

  // Copy user memory from parent to child.
  if (uvmcopy(p->pagetable, np->pagetable, p->sz) < 0)
  {
    freeproc(np);
    release(&np->lock);
    return -1;
  }
  np->sz = p->sz;
  np->integermask = p->integermask;
  // copy saved user registers.
  *(np->trapframe) = *(p->trapframe);

  // Cause fork to return 0 in the child.
  np->tickets = p->tickets;
  np->trapframe->a0 = 0;

  // increment reference counts on open file descriptors.
  for (i = 0; i < NOFILE; i++)
    if (p->ofile[i])
      np->ofile[i] = filedup(p->ofile[i]);
  np->cwd = idup(p->cwd);

  safestrcpy(np->name, p->name, sizeof(p->name));

  pid = np->pid;

  release(&np->lock);

  acquire(&wait_lock);
  np->parent = p;
  release(&wait_lock);

  acquire(&np->lock);
  np->state = RUNNABLE;
  release(&np->lock);

  return pid;
}

// Pass p's abandoned children to init.
// Caller must hold wait_lock.
void reparent(struct proc *p)
{
  struct proc *pp;

  for (pp = proc; pp < &proc[NPROC]; pp++)
  {
    if (pp->parent == p)
    {
      pp->parent = initproc;
      wakeup(initproc);
    }
  }
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait().
void exit(int status)
{
  struct proc *p = myproc();

  if (p == initproc)
    panic("init exiting");

  // Close all open files.
  for (int fd = 0; fd < NOFILE; fd++)
  {
    if (p->ofile[fd])
    {
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
  p->etime = ticks;
  release(&wait_lock);

  // Jump into the scheduler, never to return.
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int wait(uint64 addr)
{
  struct proc *np;
  int havekids, pid;
  struct proc *p = myproc();

  acquire(&wait_lock);

  for (;;)
  {
    // Scan through table looking for exited children.
    havekids = 0;
    for (np = proc; np < &proc[NPROC]; np++)
    {
      if (np->parent == p)
      {
        // make sure the child isn't still in exit() or swtch().
        acquire(&np->lock);

        havekids = 1;
        if (np->state == ZOMBIE)
        {
          // Found one.
          pid = np->pid;
          if (addr != 0 && copyout(p->pagetable, addr, (char *)&np->xstate,
                                   sizeof(np->xstate)) < 0)
          {
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
    if (!havekids || p->killed)
    {
      release(&wait_lock);
      return -1;
    }

    // Wait for a child to exit.
    sleep(p, &wait_lock); // DOC: wait-sleep
  }
}
int waitx(uint64 addr, uint *wtime, uint *rtime)
{
  struct proc *np;
  int havekids, pid;
  struct proc *p = myproc();

  acquire(&wait_lock);

  for (;;)
  {
    // Scan through table looking for exited children.
    havekids = 0;
    for (np = proc; np < &proc[NPROC]; np++)
    {
      if (np->parent == p)
      {
        // make sure the child isn't still in exit() or swtch().
        acquire(&np->lock);

        havekids = 1;
        if (np->state == ZOMBIE)
        {
          // Found one.
          pid = np->pid;
          *rtime = np->rtime;
          *wtime = np->etime - np->ctime - np->rtime;
          if (addr != 0 && copyout(p->pagetable, addr, (char *)&np->xstate,
                                   sizeof(np->xstate)) < 0)
          {
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
    if (!havekids || p->killed)
    {
      release(&wait_lock);
      return -1;
    }

    // Wait for a child to exit.
    sleep(p, &wait_lock); // DOC: wait-sleep
  }
}
int set_priority(int priority, int pid)
{
  for (struct proc *p = proc; p < (&proc[NPROC]); p++)
  {
    acquire(&p->lock);
    if (p->pid != pid)
    {
      release(&p->lock);
    }
    else
    {
      int val = p->staticpriority;
      p->staticpriority = priority;
      p->pbsstime = 0;
      p->pbsrtime = 0;
      release(&p->lock);
      int diff = val - priority;
      if (diff > 0)
      {
        yield();
        return val;
      }
      else
      {
        return val;
      }
    }
  }
  return -1;
}
void initqueues(void)
{
  int i=0;
  while(i<5)
  {
    queuetable[i].front=0;
    queuetable[i].back=0;
    i++;
  }
}
void updatetime()
{
  struct proc *p;
  for (p = proc; p < &proc[NPROC]; p++)
  {
    acquire(&p->lock);
    if (p->state == RUNNING)
    {
      p->pbsrtime++;
      p->rtime++;
      p->totalrtime++;
    }
    if (p->state == SLEEPING)
      p->pbsstime++;
#ifdef MLFQ
    if (p->qstate == QUEUED)
    {
      p->q[p->qlevel]++;
      p->qruntime++;
    }
#endif
    release(&p->lock);
  }
}

// Lock before calling this function
// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run.
//  - swtch to start running that process.
//  - eventually that process transfers control
//    via swtch back to the scheduler.
void scheduler(void)
{
  struct cpu *c = mycpu();
  c->proc = 0;
  int r;
  r = -1;
  if (r == 0)
    printf("To Remove Unused Variable Error\n");
  for (;;)
  {
    // Avoid deadlock by ensuring that devices can interrupt.
    intr_on();
#ifdef RR
    struct proc *p;
    for (p = proc; p < &proc[NPROC]; p++)
    {
      acquire(&p->lock);
      if (p->state == RUNNABLE)
      {
        // Switch to chosen process.  It is the process's job
        // to release its lock and then reacquire it
        // before jumping back to us.
        p->state = RUNNING;
        c->proc = p;
        swtch(&c->context, &p->context);

        // Process is done running for now.
        // It should have changed its p->state before coming back.
        c->proc = 0;
      }
      release(&p->lock);
    }
#endif
#ifdef FCFS
    struct proc *p;
    struct proc *temp = 0;
    for (p = proc; p < &proc[NPROC]; p++)
    {
      acquire(&p->lock);
      uint64 diff = p->timeofcreation - temp->timeofcreation;
      if (p->state == RUNNABLE && (!temp || diff < 0))
      {
        if (temp)
        {
          release(&temp->lock);
        }
        temp = p;
      }
      else
      {
        release(&p->lock);
      }
    }
    if (temp != 0)
    {
      temp->state = RUNNING;
      c->proc = temp;
      p->numscheduled++;
      swtch(&c->context, &temp->context);
      c->proc = 0;
      release(&temp->lock);
    }
#endif
#ifdef LBS
    struct proc *p;
    for (p = proc; p < &proc[NPROC]; p++)
    {
      acquire(&p->lock);
      if (p->state != RUNNABLE)
      {
        release(&p->lock);
      }
      else
      {
        int ticktotal = tickettotal();
        if (ticktotal)
        {
          r = (rand() % ticktotal) + 1;
        }
        else if (r <= 0)
        {
          r = (rand() % ticktotal) + 1;
        }
        r -= p->tickets;
        if (r > 0)
        {
          release(&p->lock);
        }
        else
        {
          if (p->state == RUNNABLE)
          {
            p->state = RUNNING;
            c->proc = p;
            swtch(&c->context, &p->context);
            c->proc = 0;
          }
          release(&p->lock);
        }
      }
    }
#endif
#ifdef PBS
    struct proc *p;
    int cnt = 101;
    struct proc *temp = 0;
    for (p = proc; p < &proc[NPROC]; p++)
    {
      acquire(&p->lock);
      int niceness = 5;
      if (p->numscheduled != 0)
      {
        uint64 var = p->pbsrtime + p->pbsstime;
        if (!var)
        {
          niceness = 5;
        }
        else
        {
          niceness = (p->pbsstime / var);
          niceness = niceness * 10;
        }
      }
      int val = p->staticpriority;
      val = val - (niceness - 5);
      int tmp, totalproc;
      if (val < 100)
      {
        tmp = val;
      }
      else
      {
        tmp = 100;
      }
      if (tmp < 0)
      {
        totalproc = 0;
      }
      else
      {
        totalproc = tmp;
      }
      if (p->state == RUNNABLE)
      {
        if (!temp || (totalproc == cnt && p->numscheduled < temp->numscheduled) || (totalproc == cnt && p->numscheduled == temp->numscheduled && p->timeofcreation < temp->timeofcreation))
        {
          if (temp)
          {
            release(&temp->lock);
            temp = p;
            cnt = totalproc;
            continue;
          }
          else
          {
            temp = p;
            cnt = totalproc;
            continue;
          }
        }
      }
      release(&p->lock);
    }
    if (temp)
    {
      temp->numscheduled++;
      temp->starttime = ticks;
      temp->state = RUNNING;
      temp->pbsrtime = 0;
      temp->pbsstime = 0;
      c->proc = temp;
      swtch(&c->context, &temp->context);
      c->proc = 0;
      release(&temp->lock);
    }
#endif
#ifdef MLFQ
    // agingproc
    struct proc *p;
    for (p = proc; p < &proc[NPROC]; p++)
    {
      if (p->state == RUNNABLE && ticks >= p->qentrytime + AGE)
      {
        remove(&queuetable[p->qlevel], p);
        p->qlevel = p->qlevel - 1;
        if (p->qlevel > 0)
        {
          p->qentrytime = ticks;
        }
        else
        {
          p->qlevel = 0;
          p->qentrytime = ticks;
        }
      }
    }
    for (p = proc; p < &proc[NPROC]; p++)
    {
      if (p->state == RUNNABLE)
      {
        if (p->qstate == DEQUED)
        {
          push(&queuetable[p->qlevel], p);
        }
      }
    }
    struct proc *temp = 0;
    for (int i = 0; i <= 4; i++)
    {
      while (empty(queuetable[i]) == 0)
      {
        struct proc *p = pop(&queuetable[i]);
        if (p->state == RUNNABLE)
        {
          temp = p;
          break;
        }
      }
      if (temp)
      {
        i = 5;
      }
    }
    if (temp == 0)
    {
      continue;
    }
    acquire(&temp->lock);
    temp->scheduletick = ticks;
    temp->qentrytime = ticks;
    temp->qruntime = 0;
    temp->state = RUNNING;
    temp->starttime = ticks;
    temp->pbsrtime = 0;
    temp->pbsstime = 0;
    temp->numscheduled++;
    c->proc = temp;
    swtch(&c->context, &temp->context);
    temp->qentrytime = ticks;
    c->proc = 0;
    release(&temp->lock);
#endif
  }
}

// Switch to scheduler.  Must hold only p->lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->noff, but that would
// break in the few places where a lock is held but
// there's no process.
void sched(void)
{
  int intena;
  struct proc *p = myproc();

  if (!holding(&p->lock))
    panic("sched p->lock");
  if (mycpu()->noff != 1)
    panic("sched locks");
  if (p->state == RUNNING)
    panic("sched running");
  if (intr_get())
    panic("sched interruptible");

  intena = mycpu()->intena;
  swtch(&p->context, &mycpu()->context);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void yield(void)
{
  struct proc *p = myproc();
  acquire(&p->lock);
  p->state = RUNNABLE;
  sched();
  release(&p->lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch to forkret.
void forkret(void)
{
  static int first = 1;

  // Still holding p->lock from scheduler.
  release(&myproc()->lock);

  if (first)
  {
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
void sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();

  // Must acquire p->lock in order to
  // change p->state and then call sched.
  // Once we hold p->lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup locks p->lock),
  // so it's okay to release lk.

  acquire(&p->lock); // DOC: sleeplock1
  release(lk);

  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;

  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  release(&p->lock);
  acquire(lk);
}

// Wake up all processes sleeping on chan.
// Must be called without any p->lock.
void wakeup(void *chan)
{
  struct proc *p;

  for (p = proc; p < &proc[NPROC]; p++)
  {
    if (p != myproc())
    {
      acquire(&p->lock);
      if (p->state == SLEEPING && p->chan == chan)
      {
        p->state = RUNNABLE;
      }
      release(&p->lock);
    }
  }
}

// Kill the process with the given pid.
// The victim won't exit until it tries to return
// to user space (see usertrap() in trap.c).
int kill(int pid)
{
  struct proc *p;

  for (p = proc; p < &proc[NPROC]; p++)
  {
    acquire(&p->lock);
    if (p->pid == pid)
    {
      p->killed = 1;
      if (p->state == SLEEPING)
      {
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
int either_copyout(int user_dst, uint64 dst, void *src, uint64 len)
{
  struct proc *p = myproc();
  if (user_dst)
  {
    return copyout(p->pagetable, dst, src, len);
  }
  else
  {
    memmove((char *)dst, src, len);
    return 0;
  }
}

// Copy from either a user address, or kernel address,
// depending on usr_src.
// Returns 0 on success, -1 on error.
int either_copyin(void *dst, int user_src, uint64 src, uint64 len)
{
  struct proc *p = myproc();
  if (user_src)
  {
    return copyin(p->pagetable, dst, src, len);
  }
  else
  {
    memmove(dst, (char *)src, len);
    return 0;
  }
}

// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void procdump(void)
{
#if defined(FCFS) || defined(RR)
  static char *states[] = {
      [UNUSED] "unused",
      [USED] "used",
      [SLEEPING] "sleep ",
      [RUNNABLE] "runble",
      [RUNNING] "run   ",
      [ZOMBIE] "zombie"};
  struct proc *p;

  printf("\n");
  char *state;
  for (p = proc; p < &proc[NPROC]; p++)
  {
    if (p->state == UNUSED)
      continue;
    if (p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    printf("%d %s %s rtime=%d", p->pid, state, p->name, p->rtime);
    printf("\n");
  }
#endif
#ifdef PBS
  static char *states[] = {
      [UNUSED] "unused",
      [USED] "used",
      [SLEEPING] "sleep ",
      [RUNNABLE] "runble",
      [RUNNING] "run   ",
      [ZOMBIE] "zombie"};
  struct proc *p;

  printf("\n");
  char *state;
  for (p = proc; p < &proc[NPROC]; p++)
  {
    int time;
    if (p->etime)
      time = p->etime - (p->timeofcreation + p->rtime);
    else
      time = ticks - (p->timeofcreation + p->rtime);
    if (p->state == UNUSED)
      continue;
    if (p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    printf("%d\t%s    %d\t  %s\trtime=%d\twtime=%d", p->pid, state, p->staticpriority, p->name, p->rtime, time);
    printf("\n");
  }
#endif
#ifdef MLFQ
  static char *states[] = {
      [UNUSED] "unused",
      [USED] "used",
      [SLEEPING] "sleep ",
      [RUNNABLE] "runble",
      [RUNNING] "run   ",
      [ZOMBIE] "zombie"};

  struct proc *p;
  printf("\n");
  printf("PID\t");
  char *state;
  printf("Priority\t");
  printf("\trtime\twtime\tnrun\t");
  for (int i = 0; i < 5; i++)
  {
    printf("q%d\t", i);
  }
  printf("\n");
  for (p = proc; p < &proc[NPROC]; p++)
  {
    if (p->state == UNUSED)
      continue;
    if (p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    printf("%d\t", p->pid);
    int priority = (p->qlevel == DEQUED) ? -1 : p->qlevel;
    printf("%d\t\t", priority);
    printf("%s\t", state);
    uint waittime = (ticks - p->qentrytime);
    printf("%d\t%d\t%d\t", p->totalrtime, waittime, p->numscheduled);
    for (int i = 0; i < 5; i++)
    {
      printf("%d\t", p->q[i]);
    }
    printf("\n");
  }
#endif
}
uint64 sys_sigalarm(void)
{
  int ticks;
  argint(0, &ticks);
  if (ticks < 0)
  {
    return -1;
  }
  uint64 handler;
  argaddr(1, &handler);
  if (handler < 0)
  {
    return -1;
  }
  struct proc *p = myproc();
  p->handler = handler;
  p = myproc();
  p->ticks = ticks;
  p = myproc();
  p->ticks_passed = 0;
  p = myproc();
  p->is_sigalarm = 0;
  return 0;
}