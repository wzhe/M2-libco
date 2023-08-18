#include "co.h"
#include "list.h"
#include <stdint.h>
#include <stdlib.h>
#include <setjmp.h>
#include <assert.h>

/* #define dbg(...) printf(__VA_ARGS__) */
#define dbg(...)

enum co_status {
  CO_NEW = 1, // 新创建，还未执行过
  CO_RUNNING, // 已经执行过
  CO_WAITING, // 在 co_wait 上等待
  CO_DEAD,    // 已经结束，但还未释放资源
};

static const char * status_str[]=
  {
    "123",
    "CO_NEW",
    "CO_RUNNING", // 已经执行过
    "CO_WAITING", // 在 co_wait 上等待
    "CO_DEAD",    // 已经结束，但还未释放资源
  };

#define K (1024)
#define STACK_SIZE  (64 * K)


struct co {
  const char *name;
  void (*func)(void *); // co_start 指定的入口地址和参数
  void *arg;

  volatile enum co_status status;  // 协程的状态
  struct co *    waiter;  // 是否有其他协程在等待当前协程
  jmp_buf        context; // 寄存器现场 (setjmp.h)
  uint8_t        stack[STACK_SIZE]; // 协程的堆栈
};

static struct co* current = NULL;
static Queue *queue = NULL;

struct co *co_start(const char *name, void (*func)(void *), void *arg) {
  struct co* coroutine = (struct co*)malloc(sizeof(struct co));
  assert(coroutine);

  coroutine->name = name;
  coroutine->func = func;
  coroutine->arg = arg;
  coroutine->status = CO_NEW;
  coroutine->waiter = NULL;

  Item *item = (Item*)malloc(sizeof(Item));
  if (!item) {
    fprintf(stderr, "New item failure\n");
    return NULL;
  }

  item->data = coroutine;

  assert(!q_is_full(queue));
  q_push(queue, item);

  dbg("insert co[%s] stats : %s\n",coroutine->name, status_str[coroutine->status]);
  return coroutine;
}


// 设置函数栈，函数开始运行
// movq %%rcx, 0(0%); // 将rsp保存至rcx中
// movq %0, %%rsp;    // 设置 sp 为rsp
// movq %2, %%rdi;    // 将参数存入rdic寄存器中
// call *%1;          // 执行函数调用， 此处不能用jmp，否则函数就跳飞了，回不来了。
static inline void stack_switch_call(void *sp, void *entry, void* arg) {
  asm volatile (
#if __x86_64__
                "movq %%rcx, 0(%0); movq %0, %%rsp; movq %2, %%rdi; call *%1"
                : : "b"((uintptr_t)sp - 16), "d"((uintptr_t)entry), "a"((uintptr_t)arg)
#else
                  "movl %%ecx, 4(%0); movl %0, %%esp; movl %2, 0(%0); call *%1"
                  : : "b"((uintptr_t)sp - 8), "d"((uintptr_t)entry), "a"((uintptr_t)arg)
#endif
                );
}
/*
 * 从调用的指定函数返回，并恢复相关的寄存器。
 * 此时协程执行结束，以后再也不会执行该协程的上下文。
 * 这里需要注意的是，其和上面并不是对称的，
 * 因为调用协程给了新创建的选中协程的堆栈，
 * 则选中协程以后就在自己的堆栈上执行，永远不会返回到调用协程的堆栈。
 */
static inline void restore_return() {
  asm volatile (
#if __x86_64__
                "movq 0(%%rsp), %%rcx" : :
#else
                "movl 4(%%esp), %%ecx" : :
#endif
                );
}


//yield函数有两个功能
//1.让渡CPU给其他协程继续执行
//2.从其他协程挑战过来继续执行
void co_yield() {
  int ret = setjmp(current->context);
  if (ret == 0) {
    //让渡CPU给其他协程
    assert(!q_is_empty(queue));
    // 查找一个可以运行的线程
    Item *item = q_pop(queue);
    assert(item);
    current = (struct co *)(item->data);
    while(!(current->status == CO_NEW || current->status == CO_RUNNING)) {
      dbg("co[%s] stats : %s\n",current->name, status_str[current->status]);
      q_push(queue, item);
      item=q_pop(queue);
      current = (struct co *)(item->data);
    }
    // 将当前协程排到最后去
    q_push(queue, item);

    dbg("chose co[%s] stats : %s\n",current->name, status_str[current->status]);
    assert(current);
    assert(current->status == CO_NEW || current->status == CO_RUNNING);

    if (current->status == CO_RUNNING) {
      //跳转到其他协程，继续执行
      longjmp(current->context, 1);
    } else if (current->status == CO_NEW) {
      // 设置状态为运行中
      current->status = CO_RUNNING;
      // 设置堆栈，并开始执行
      dbg("co[%s] start run \n",current->name);
      stack_switch_call(current->stack + STACK_SIZE, current->func, current->arg);
      dbg("co[%s] run over \n",current->name);

      //函数执行完毕，恢复相关寄存器
      restore_return();

      // 协程执行完毕，等待资源回收
      current->status = CO_DEAD;

      //如果有wait此协程的，可以继续下去了
      if (current->waiter) {
        current->waiter->status = CO_RUNNING;
      }
      co_yield(); //让渡CPU，之后，不会再回来了。
      assert(0);

    }
  }
  dbg("co[%s] countine run \n",current->name);
  // 从其他协程跳转过来的，什么也不需要做，继续执行即可
  assert(ret && current->status == CO_RUNNING);
}

void co_wait(struct co *coroutine) {
  assert(coroutine);

  //如果等待的协程还未执行完成，则继续等待
  if (coroutine->status != CO_DEAD) {
    coroutine->waiter = current;
    current->status = CO_WAITING;
    co_yield();//让渡CPU，继续等待
  }

  assert(!q_is_empty(queue));
  Item *item = q_pop(queue);
  assert(item);
  while (item->data != coroutine) {
    q_push(queue, item);
    item=q_pop(queue);
  }

  dbg("wait release co[%s] stats : %s\n",coroutine->name, status_str[coroutine->status]);
  free(item->data);
  free(item);
}

static __attribute__((constructor)) void co_constructor(void) {
  queue = q_new();
  assert(queue);
  current = co_start("main", NULL, NULL);
  assert(current);
  current->status = CO_RUNNING;
}

static __attribute__((destructor)) void co_destructor(void) {
  dbg("start destructor");
  while(!q_is_empty(queue)) {
    Item *item = q_pop(queue);
    assert(item);
    struct co *coroutine = item->data;
    dbg("release co[%s] stats : %s\n",coroutine->name, status_str[coroutine->status]);
    free(item->data);
    free(item);
  }
}
