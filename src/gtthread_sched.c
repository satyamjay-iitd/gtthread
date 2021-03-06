/**********************************************************************
gtthread_sched.c.  

This file contains the implementation of the scheduling subset of the
gtthreads library.  A simple round-robin queue should be used.
 **********************************************************************/
/*
  Include as needed
*/

#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <sys/time.h>
#include <unistd.h>
#include <string.h>
#include "gtthread.h"
#include "steque.h"

#define GTTHREAD_RUNNING 0 /* the thread is running */
#define GTTHREAD_CANCEL 1 /* the thread is cancelled */
#define GTTHREAD_DONE 2 /* the thread has done */

typedef struct Thread_t
{
    gtthread_t tid;
    gtthread_t joining;
    int state;
    void* (*proc)(void*);
    void* arg;
    void* retval;
    ucontext_t* ucp; 
} thread_t;   

/* global data section */
static steque_t ready_queue;
static steque_t zombie_queue;
static thread_t* current;
static struct itimerval timer;
sigset_t vtalrm;
static gtthread_t maxtid; 

/* private functions prototypes */
void sigvtalrm_handler(int sig);
void gtthread_start(void* (*start_routine)(void*), void* args);
thread_t* thread_get(gtthread_t tid);

/*
  The gtthread_init() function does not have a corresponding pthread equivalent.
  It must be called from the main thread before any other GTThreads
  functions are called. It allows the caller to specify the scheduling
  period (quantum in micro second), and may also perform any other
  necessary initialization.

  Recall that the initial thread of the program (i.e. the one running
  main() ) is a thread like any other. It should have a
  gtthread_t that clients can retrieve by calling gtthread_self()
  from the initial thread, and they should be able to specify it as an
  argument to other GTThreads functions. The only difference in the
  initial thread is how it behaves when it executes a return
  instruction. You can find details on this difference in the man page
  for pthread_create.
 */
void gtthread_init(long period)
{
    struct sigaction act;

    /* initializing data structures */
    maxtid = 1;
    steque_init(&ready_queue);
    
    /* create main thread and add it to ready queue */  
    /* only main thread is defined on heap and can be freed */
    thread_t* main_thread = (thread_t*) malloc(sizeof(thread_t));
    main_thread->tid = maxtid++;
    main_thread->ucp = (ucontext_t*) malloc(sizeof(ucontext_t)); 
    memset(main_thread->ucp, '\0', sizeof(ucontext_t));
    main_thread->arg = NULL;
    main_thread->state = GTTHREAD_RUNNING;
    main_thread->joining = 0;

    /* must be called before makecontext */
    if (getcontext(main_thread->ucp) == -1)
    {
      perror("getcontext");
      exit(EXIT_FAILURE);
    }

    current = main_thread;
    
    /* setting uo the signal mask */
    sigemptyset(&vtalrm);
    sigaddset(&vtalrm, SIGVTALRM);
    sigprocmask(SIG_UNBLOCK, &vtalrm, NULL); /* in case this is blocked previously */

    /* set alarm signal and signal handler */
    timer.it_interval.tv_usec = period;
    timer.it_interval.tv_sec = 0;
    timer.it_value.tv_usec = period;
    timer.it_value.tv_sec = 0; 
    
    if (setitimer(ITIMER_VIRTUAL, &timer, NULL) < 0)
    {
        perror("setitimer");
        exit(EXIT_FAILURE);
    }

    /* install signal handler for SIGVTALRM */  
    memset(&act, '\0', sizeof(act));
    act.sa_handler = &sigvtalrm_handler;
    if (sigaction(SIGVTALRM, &act, NULL) < 0)
    {
      perror ("sigaction");
      exit(EXIT_FAILURE);
    }
}


/*
  The gtthread_create() function mirrors the pthread_create() function,
  only default attributes are always assumed.
 */
int gtthread_create(gtthread_t *thread,
		    void *(*start_routine)(void *),
		    void *arg)
{
    /* block SIGVTALRM signal */
    sigprocmask(SIG_BLOCK, &vtalrm, NULL);
    
    /* allocate heap for thread, it cannot be stored on stack */
    thread_t* t = malloc(sizeof(thread_t));
    *thread = t->tid = maxtid++; // need to block signal
    t->state = GTTHREAD_RUNNING;
    t->proc = start_routine;
    t->arg = arg;
    t->ucp = (ucontext_t*) malloc(sizeof(ucontext_t));
    t->joining = 0;
    memset(t->ucp, '\0', sizeof(ucontext_t));

    if (getcontext(t->ucp) == -1)
    {
      perror("getcontext");
      exit(EXIT_FAILURE);
    }
    
    /* allocate stack for the newly created context */
    /* here we allocate the stack size using the canonical */
    /* size for signal stack. */
    t->ucp->uc_stack.ss_sp = malloc(SIGSTKSZ);
    t->ucp->uc_stack.ss_size = SIGSTKSZ;
    t->ucp->uc_stack.ss_flags = 0;
    t->ucp->uc_link = NULL;

    makecontext(t->ucp, (void (*)(void)) gtthread_start, 2, start_routine, arg);
    steque_enqueue(&ready_queue, t);

    /* unblock the signal */
    sigprocmask(SIG_UNBLOCK, &vtalrm, NULL);   
    return 0; 
}

/*
  The gtthread_join() function is analogous to pthread_join.
  All gtthreads are joinable.
 */
int gtthread_join(gtthread_t thread, void **status)
{
    /* if a thread tries to join itself */
    if (thread == current->tid)
        return -1;

    thread_t* t;
    /* if a thread is not created */
    if ((t = thread_get(thread)) == NULL)
        return -1;

    /* check if that thread is joining on me */
    if (t->joining == current->tid)
        return -1;

    current->joining = t->tid;
    /* wait on the thread to terminate */
    while (t->state == GTTHREAD_RUNNING)
    {
        sigprocmask(SIG_UNBLOCK, &vtalrm, NULL);
        sigvtalrm_handler(SIGVTALRM);
        sigprocmask(SIG_BLOCK, &vtalrm, NULL);
    }

    if (status == NULL)
        return 0;

    if (t->state == GTTHREAD_CANCEL)
        *status = (void*) GTTHREAD_CANCEL;
    else if (t->state == GTTHREAD_DONE)
        *status = t->retval;

    return 0;
}

/*
  The gtthread_exit() function is analogous to pthread_exit.
 */
void gtthread_exit(void* retval)
{
    /* block alarm signal */
    sigprocmask(SIG_BLOCK, &vtalrm, NULL);

    if (steque_isempty(&ready_queue))
    { 
        sigprocmask(SIG_UNBLOCK, &vtalrm, NULL); 
        exit((long) retval);
    }

    /* if the main thread call gtthread_exit */
    if (current->tid == 1)
    {
        while (!steque_isempty(&ready_queue))
        {
            sigprocmask(SIG_UNBLOCK, &vtalrm, NULL);  
            sigvtalrm_handler(SIGVTALRM);
            sigprocmask(SIG_BLOCK, &vtalrm, NULL);
        }
        sigprocmask(SIG_UNBLOCK, &vtalrm, NULL);   
        exit((long) retval);
    }

    thread_t* prev = current; 
    current = (thread_t*) steque_pop(&ready_queue);
    current->state = GTTHREAD_RUNNING; 

    /* free up memory allocated for exit thread */
    free(prev->ucp->uc_stack.ss_sp); 
    free(prev->ucp);                
    prev->ucp = NULL;

    /* mark the exit thread as DONE and add to zombie_queue */ 
    prev->state = GTTHREAD_DONE; 
    prev->retval = retval;
    prev->joining = 0;
    steque_enqueue(&zombie_queue, prev);

    /* unblock alarm signal and setcontext for next thread */
    sigprocmask(SIG_UNBLOCK, &vtalrm, NULL); 
    setcontext(current->ucp);
}

/*
  The gtthread_yield() function is analogous to pthread_yield, causing
  the calling thread to relinquish the cpu and place itself at the
  back of the schedule queue.
 */
int gtthread_yield(void)
{
    /* block SIGVTALRM signal */
    sigprocmask(SIG_BLOCK, &vtalrm, NULL);
    
    /* if no thread to yield, simply return */
    if (steque_isempty(&ready_queue))
        return 0;

    thread_t* next = (thread_t*) steque_pop(&ready_queue);
    thread_t* prev = current;
    steque_enqueue(&ready_queue, current);
    current = next;

    /* unblock the signal */
    sigprocmask(SIG_UNBLOCK, &vtalrm, NULL); 
    swapcontext(prev->ucp, current->ucp); 
    return 0; 
}

/*
  The gtthread_yield() function is analogous to pthread_equal,
  returning zero if the threads are the same and non-zero otherwise.
 */
int  gtthread_equal(gtthread_t t1, gtthread_t t2)
{
    return t1 == t2;
}

/*
  The gtthread_cancel() function is analogous to pthread_cancel,
  allowing one thread to terminate another asynchronously.
 */
int gtthread_cancel(gtthread_t thread)
{
    /* if a thread cancel itself */
    if (gtthread_equal(current->tid, thread))
        gtthread_exit(0);

    sigprocmask(SIG_BLOCK, &vtalrm, NULL);
    thread_t* t = thread_get(thread);
    if (t == NULL)
    {
        sigprocmask(SIG_UNBLOCK, &vtalrm, NULL);    
        return -1;
    }
    if (t->state == GTTHREAD_DONE)
    {
        sigprocmask(SIG_UNBLOCK, &vtalrm, NULL); 
        return -1;
    }
    if (t->state == GTTHREAD_CANCEL)
    {
        sigprocmask(SIG_UNBLOCK, &vtalrm, NULL);  
        return -1;
    }
    else
        t->state = GTTHREAD_CANCEL;

    free(t->ucp->uc_stack.ss_sp);
    free(t->ucp);
    t->ucp = NULL;
    t->joining = 0;
    steque_enqueue(&zombie_queue, t);
    sigprocmask(SIG_UNBLOCK, &vtalrm, NULL);
    return 0;
}

/*
  Returns calling thread.
 */
gtthread_t gtthread_self(void)
{
    return current->tid;
}


/*
 * helper functions to install the signal handler 
 */

/* 
 * A wrapper function to start a routine.
 * The reason we need this is because we need to call gtthread_exit
 * when the thread finish executing.
 */
void gtthread_start(void* (*start_routine)(void*), void* args)
{
    /* unblock signal comes from gtthread_create */
    sigprocmask(SIG_UNBLOCK, &vtalrm, NULL);

    /* start executing the start routine*/
    current->retval = (*start_routine)(args);

    /* when start_rountine returns, call gtthread_exit*/
    gtthread_exit(current->retval);
}

/*
 * A signal handler for SIGVTALRM
 * Comes here when a thread runs up its time slot. This handler implements
 * a preemptive thread scheduler. It looks at the global ready queue, pop
 * the thread in the front, save the current thread context and switch context. 
 */
void sigvtalrm_handler(int sig)
{
    /* block the signal */
    sigprocmask(SIG_BLOCK, &vtalrm, NULL);

    /* if no thread in the ready queue, resume execution */
    if (steque_isempty(&ready_queue))
        return;

    /* get the next runnable thread and use preemptive scheduling */
    thread_t* next = (thread_t*) steque_pop(&ready_queue);
    while (next->state == GTTHREAD_CANCEL)
    {
        steque_enqueue(&zombie_queue, next);
        next = (thread_t*) steque_pop(&ready_queue); 
    }
    thread_t* prev = current;
    steque_enqueue(&ready_queue, current);
    next->state = GTTHREAD_RUNNING; 
    current = next;

    /* unblock the signal */
    sigprocmask(SIG_UNBLOCK, &vtalrm, NULL); 
    swapcontext(prev->ucp, current->ucp);
}

/*
 * Given a thread ID, search the thread in ready_queue and zombie queue 
 * This helper method is useful when we try to determine whether the thread
 * user wants to join is created before. 
 */
thread_t* thread_get(gtthread_t tid)
{
    steque_node_t* current = ready_queue.front;   
    while (current != NULL)
    {
        thread_t* t= (thread_t*) current->item;
        if (t->tid == tid)
            return t;
        current = current->next;
    }

    current = zombie_queue.front;
    while (current != NULL)
    {
        thread_t* t= (thread_t*) current->item;
        if (t->tid == tid)
            return t;
        current = current->next;
    } 
    return NULL;
}
