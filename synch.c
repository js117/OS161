/*
 * Synchronization primitives.
 * See synch.h for specifications of the functions.
 */

#include <types.h>
#include <lib.h>
#include <synch.h>
#include <thread.h>
#include <curthread.h>
#include <machine/spl.h>
#include <array.h>
#include <addrspace.h>

////////////////////////////////////////////////////////////
//
// Semaphore.

struct semaphore *
sem_create(const char *namearg, int initial_count)
{
	struct semaphore *sem;

	assert(initial_count >= 0);

	sem = kmalloc(sizeof(struct semaphore));
	if (sem == NULL) {
		return NULL;
	}

	sem->name = kstrdup(namearg);
	if (sem->name == NULL) {
		kfree(sem);
		return NULL;
	}

	sem->count = initial_count;
	return sem;
}

void
sem_destroy(struct semaphore *sem)
{
	int spl;
	if (sem == NULL) {
	  //kprintf("SEMAPHORE NULL. SEM IN QUESTION: %s\n",sem->name); 
	  kprintf("CURTHREAD: %s\n",curthread->t_name); 
	  assert(sem != NULL);
	}

	spl = splhigh();
	assert(thread_hassleepers(sem)==0);
	splx(spl);

	/*
	 * Note: while someone could theoretically start sleeping on
	 * the semaphore after the above test but before we free it,
	 * if they're going to do that, they can just as easily wait
	 * a bit and start sleeping on the semaphore after it's been
	 * freed. Consequently, there's not a whole lot of point in 
	 * including the kfrees in the splhigh block, so we don't.
	 */

	kfree(sem->name);
	kfree(sem);
}

void 
P(struct semaphore *sem)
{
	int spl;
	if (sem == NULL) {
	  //kprintf("SEMAPHORE NULL. SEM IN QUESTION: %s\n",sem->name); 
	  kprintf("CURTHREAD: %s\n",curthread->t_name); 
	  assert(sem != NULL);
	}

	/*
	 * May not block in an interrupt handler.
	 *
	 * For robustness, always check, even if we can actually
	 * complete the P without blocking.
	 */
	assert(in_interrupt==0);

	spl = splhigh();
	while (sem->count==0) {
		thread_sleep(sem);
	}
	assert(sem->count>0);
	sem->count--;
	splx(spl);
}

void
V(struct semaphore *sem)
{
	int spl;
	if (sem == NULL) {
	  //kprintf("SEMAPHORE NULL. SEM IN QUESTION: %s\n",sem->name); 
	  kprintf("CURTHREAD: %s\n",curthread->t_name); 
	  assert(sem != NULL);
	}
	spl = splhigh();
	sem->count++;
	assert(sem->count>0);
	thread_wakeup(sem);
	splx(spl);
}


////////////////////////////////////////////////////////////
//
// Lock.

struct lock *
lock_create(const char *name)
{
	struct lock *lock;
	lock = kmalloc(sizeof(struct lock));
	if (lock == NULL) {
		return NULL;
	}

	lock->name = kstrdup(name);
	if (lock->name == NULL) {
		kfree(lock);
		return NULL;
	}
	
	lock->thisThread = NULL; 

	return lock;
}

void
lock_destroy(struct lock *lock)
{
	int spl;
	assert(lock != NULL);
        spl = splhigh();
	assert(thread_hassleepers(lock)==0); //make sure nobody is sleeping on the lock before we destroy it; 
	splx(spl);
	kfree(lock->name);
	kfree(lock);
}


void
lock_acquire(struct lock *lock)
{

  int spl;
  assert(lock != NULL);

  if (lock_do_i_hold(lock)) return; //do nothing if you already have the lock
  
  spl = splhigh(); //note - doesn't work for multiple processors
    while (lock->thisThread != NULL) { //while you don't have the lock
      thread_sleep(lock); //sleep on the address of this lock; scheduler puts in the sleep queue
	  //thread_sleep requires interrupts be off or it can mess up the context switch; 
	  //don't want to interrupt the kernel (OS) code while it's changing program states
    }
  
  lock-> thisThread = curthread;
  splx(spl); 
}

void
lock_release(struct lock *lock)
{
  int spl;
  spl = splhigh();
  if (lock_do_i_hold(lock)){	
    lock->thisThread = NULL; 
    thread_wakeup(lock);		
  } //else do nothing
  splx(spl);
}

int
lock_do_i_hold(struct lock *lock)
{
   return (lock->thisThread == curthread );
}

////////////////////////////////////////////////////////////
//
// CV


struct cv *
cv_create(const char *name)
{
	struct cv *cv;

	cv = kmalloc(sizeof(struct cv));
	if (cv == NULL) {
		return NULL;
	}

	cv->name = kstrdup(name);
	if (cv->name==NULL) {
		kfree(cv);
		return NULL;
	}
	
	// add stuff here as needed
	
	return cv;
}

void
cv_destroy(struct cv *cv)
{
	assert(cv != NULL);

	// add stuff here as needed
	
	kfree(cv->name);
	kfree(cv);
}

/* Operations:
 *    cv_wait      - Release the supplied lock, go to sleep, and, after
 *                   waking up again, re-acquire the lock.
 *    cv_signal    - Wake up one thread that's sleeping on this CV.
 *    cv_broadcast - Wake up all threads sleeping on this CV.
 *
 * For all three operations, the current thread must hold the lock passed 
 * in. Note that under normal circumstances the same lock should be used
 * on all operations with any particular CV.
 *
 * These operations must be atomic. You get to write them.
 *
 * These CVs are expected to support Mesa semantics, that is, no
 * guarantees are made about scheduling.
 *
 * The name field is for easier debugging. A copy of the name is made
 * internally.
 */

void
cv_wait(struct cv *cv, struct lock *lock)
{
        
    int spl =splhigh();
        if(lock_do_i_hold(lock)){
			lock_release(lock);
			thread_sleep(cv);
			lock_acquire(lock);
        }

	splx(spl);
	
	//About the lock - consider the alternative: without the lock, once the threads are woken up via signal,
	//they will engage in an unordered "free for all" to use the condition variable (e.g. x, the bank account)
	//While we could have done locking separately, this is the convention for implementing POSIX threads in Unix,
	//probably coming from common use cases.

}

void
cv_wait_thread(struct thread *t, struct lock *lock) {
  int spl =splhigh();
        if(lock_do_i_hold(lock)){
		lock_release(lock);
		thread_sleep(t);
		lock_acquire(lock);
        }

	splx(spl);
}

void
cv_signal_thread(struct thread *t, struct lock *lock)
{
	int spl=splhigh();
	thread_wakeup_single(t);
        splx(spl);
}

//wake up one thread waiting on this condition variable
void
cv_signal(struct cv *cv, struct lock *lock)
{
	int spl=splhigh();
	thread_wakeup_single(cv);
        splx(spl);
}

//wake up all threads waiting on this condition variable 
void
cv_broadcast(struct cv *cv, struct lock *lock)
{
	int spl=splhigh();
	thread_wakeup(cv);
        splx(spl);
}

void cv_broadcast_thread(struct thread *t, struct lock *lock)
{
	int spl=splhigh();
	thread_wakeup(t);
        splx(spl);
}
