#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

void push(struct MLFQ_Queue *q, struct proc *p)
{
  q->queue[q->front] = p;
  q->front++;
  if (q->front == QSIZE)
  {
    q->front = 0;
  }
  if (q->front == q->back)
  {
    panic("Can't push in full queue");
  }
  p->qstate = QUEUED;
}
struct proc *
pop(struct MLFQ_Queue *q)
{
  if (q->back == q->front)
  {
    panic("Can't pop empty queue");
  }
  struct proc *p = q->queue[q->back];
  p->qstate = DEQUED;
  if (q->back == NPROC)
  {
    q->back = 0;
  }
  else
  {
    q->back++;
  }
  return p;
}
void remove(struct MLFQ_Queue *q, struct proc *p)
{
  if (p->qstate == DEQUED)
  {
    return;
  }
  int i = q->back;
  while (i != q->front)
  {
    if (p == q->queue[i])
    {
      p->qstate = DEQUED;
      int j = i + 1;
      while (j != q->front)
      {
        q->queue[(j + QSIZE - 1) % QSIZE] = q->queue[j];
        j = (j + 1) % QSIZE;
      }
      q->front--;
      if (q->front < 0)
        q->front += QSIZE;
      break;
    }
    i = (i + 1) % QSIZE;
  }
}
int empty(struct MLFQ_Queue q)
{
  if (q.front == q.back)
  {
    return 1;
  }
  else
  {
    return 0;
  }
}
