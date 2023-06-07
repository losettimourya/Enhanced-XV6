#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run
{
  struct run *next;
};

struct
{
  struct spinlock lock;
  struct run *freelist;
  // cow
  // int numFreePages;;
} kmem;

void kinit()
{
  initlock(&kmem.lock, "kmem");
  freerange(end, (void *)PHYSTOP);
}
/*
cow change
void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
    kfree(p);
}
*/
int refcnt[PHYSTOP / PGSIZE];
void freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char *)PGROUNDUP((uint64)pa_start);
  for (; p + PGSIZE <= (char *)pa_end; p = p + PGSIZE)
  {
    int index = (uint64)p / PGSIZE;
    refcnt[index] = 1;
    kfree(p);
  }
}
/*
cow change
// Free the page of physical memory pointed at by pa,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  struct run *r;
  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");
  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);
  r = (struct run*)pa;
  acquire(&kmem.lock);
  r->next = kmem.freelist;
  kmem.freelist = r;
  release(&kmem.lock);
}
*/
void increse(uint64 pa)
{
  // acquire the lock
  acquire(&kmem.lock);
  int pn = pa / PGSIZE;
  int flag = (pa > PHYSTOP || refcnt[pn] < 1);
  if (flag)
  {
    panic("increase ref cnt");
  }
  refcnt[pn]++;
  release(&kmem.lock);
}

void kfree(void *pa)
{
  struct run *r;
  r = (struct run *)pa;
  if (((uint64)pa % PGSIZE) != 0 || (char *)pa < end || (uint64)pa >= PHYSTOP)
  {
    panic("kfree");
  }
  // when we free the page decraese the refcnt of the pa
  // we need to acquire the lock
  // and get the really current cnt for the current fucntion
  acquire(&kmem.lock);
  int pn = (uint64)r / PGSIZE;
  if (refcnt[pn] < 1)
  {
    panic("kfree panic");
  }
  refcnt[pn] -= 1;
  int tmp = refcnt[pn];
  release(&kmem.lock);

  if (tmp > 0)
  {
    return;
  }
  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);
  acquire(&kmem.lock);
  r->next = kmem.freelist;
  kmem.freelist = r;
  release(&kmem.lock);
}
/*
cow change
// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;
  acquire(&kmem.lock);
  r = kmem.freelist;
  if(r)
    kmem.freelist = r->next;
  release(&kmem.lock);
  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk
  return (void*)r;
}
*/
void *
kalloc(void)
{
  struct run *r;

  acquire(&kmem.lock);
  r = kmem.freelist;

  if (r)
  {
    int pn = (uint64)r / PGSIZE;
    if (refcnt[pn] != 0)
    {
      panic("refcnt kalloc");
    }
    refcnt[pn] = 1;
    kmem.freelist = r->next;
  }

  release(&kmem.lock);

  if (r)
    memset((char *)r, 10, PGSIZE); // fill with junk
  return (void *)r;
}
// int
// getNumFreePages(void)
// {
// if(kmem.lock)
// acquire(&kmem.lock);
// int r = kmem.numFreePages;
// if(kmem.lock)
// release(&kmem.lock);
// return (r);
// }
int cowfault(pagetable_t pagetable, uint64 va)
{
  if (va >= MAXVA)
    return -1;
  pte_t *pte = walk(pagetable, va, 0);
  if (pte == 0)
    return -1;
  if ((*pte & PTE_U) == 0 || (*pte & PTE_V) == 0)
    return -1;
  uint64 pa1 = PTE2PA(*pte);
  uint64 pa2 = (uint64)kalloc();
  if (pa2 == 0)
  {
    // panic("cow panic kalloc");
    return -1;
  }

  memmove((void *)pa2, (void *)pa1, PGSIZE);
  *pte = PA2PTE(pa2) | PTE_U | PTE_V | PTE_W | PTE_X | PTE_R;
  kfree((void *)pa1);
  return 0;
}
