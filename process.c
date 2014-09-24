/*
 * PID table
 */
#include <types.h>
#include <lib.h>
#include <kern/errno.h>
#include <array.h>
#include <machine/spl.h>
#include <machine/pcb.h>
#include <thread.h>
#include <curthread.h>
#include <scheduler.h>
#include <addrspace.h>
#include <vnode.h>
#include <vfs.h>
#include <synch.h>
#include <process.h> 
#include <curthread.h>
#include "opt-synchprobs.h" 
#include <syscall.h>
#include <kern/limits.h>
#include <kern/unistd.h>
#include <vm.h>

struct array *PID; 
struct lock *forkLock; 
struct lock *waitLock; 
struct cv *forkCV; 
struct cv *waitCV; 
struct semaphore *procSem; 

struct array *THREAD_LIST;

struct lock *exitLock; 
int numProcs; 
int maxNumChildren; 

//HOW FORK WORKS:
//Creates a replica process which has a duplicated address space and even runs the same
//code. The difference lies in their unique PIDs. 
//Step 1: use the trap frame of the caller (parent) to set up processor state of child.
//Step 2: copy the address space
//Both steps 1&2 are done in "md_forkentry", which is what the child thread will run.
//Upon completion, both the child and parent will return in sys_fork. The parent must
//return 0, where the child will return its unique PID. 


int sys_fork(struct trapframe *tf) {

  lock_acquire(forkLock);

  //create the child thread
  struct thread *child = NULL; 


  //now create the actual child "process" by creating its thread and then linking up
  //its address space afterwards
   (thread_fork("NEW_CHILD", tf, 0/*unused*/, md_forkentry, &child)); //some error
  //in md_forkentry we will set the child's address space and CPU state accordingly
  

   int x=as_copy(curthread->t_vmspace, &child->t_vmspace, child);
  assert(x==0);
  if (x) return ENOMEM; 

  int childPID = child->pid; 

  //curthread has a child, update it! 
  int i; 
  for (i = 0; i < maxNumChildren; i++) {
    if (curthread->children[i] == -1) {
      curthread->children[i] = childPID;
      //kprintf("\nNEW CHILD: %d\n",childPID); 
      
      break; 
    }
  }
  //note -- haven't yet implemented getting this child off of the parent's list

  kprintf("SYS_FORK: addrspace: 0x%x -- stack; 0x%x -- PID: %d\n",child->t_vmspace,
	  child->t_stack,child->pid);
  assert(child->pid == child->t_vmspace->PID);
  //kprintf("SYS_FORK: epc: 0x%x\n", tf->tf_epc); 

   cv_wait(forkCV,forkLock);
   lock_release(forkLock); 

   if (childPID > 200) {
     int spl = splhigh();
     kprintf("TOO MANY PROCESSES! child process: %d\n",childPID);
     while(1) {} 
   }

   return childPID; 
}

int sys_getpid() {
  
  assert(curthread!=NULL);
  assert(curthread->t_vmspace!=NULL); 
  return curthread->pid; 

}



//flags will not be implemented; should be 0. 
//pointer returnCode gets the value of sys__exit
//waitpid causes the caller to wait for the process specified by pid to exit 
// -- Possible problem with THREAD_LIST and kmalloc using new vm system
int sys_waitpid(pid_t _pid, int* returnCode, int flags) {

  lock_acquire(forkLock);

  pid_t waitpid = curthread->pid; 

  struct thread *t; struct thread *child; child = NULL;
  int isGrandpaSleeping = 0; int i;
  for(i = 0; i < array_getnum(THREAD_LIST); i++) {
    t = array_getguy(THREAD_LIST,i); 
    if (t->pid == curthread->ppid) {
      if (t->isSleeping == 1) isGrandpaSleeping = 1;
    }
    if (t->pid == _pid) child = t; 
  }
  
  if (_pid <= waitpid || isGrandpaSleeping) {
    lock_release(forkLock); 
    return 0; //not a condition I wanna sleep on
  }

  
  if (child != NULL) {
    if (child->isExiting == 0) {
      cv_wait_thread(child, forkLock);
    }
  } else {
    lock_release(forkLock);
    return 0; 
  }

  lock_release(forkLock);

  return _pid; 
  

}

//executed by child to die
void sys__exit(int code) {

  lock_acquire(forkLock);
  int x = code; //unused for now

  curthread->isExiting = 1;

  //kprintf("ARE YOU GUYS GONNA WAKE UP???\n"); 
  cv_broadcast_thread(curthread,forkLock); 

  lock_release(forkLock); 

  thread_exit();

}


//progname and argv are USERLAND pointers, so to get them we need copyin functions
//isKernel: pass in 1 if we're calling from the kernel directly, to avoid unnecessary 
//copyin of the arguments from userland. 
int sys_execv(char* progname, char** argv, int isKernel) {
  lock_acquire(forkLock);
  //lock_acquire(process_lock); 

  //int spl = splhigh();

  struct vnode *v;
  vaddr_t entrypoint, stack_ptr; 
 
  if (progname == NULL) {
    kprintf("EXECV: progname was NULL\n"); 
    return EFAULT; 
  }
  //kprintf("TEST2\n"); 
  char* filename = (char *)alloc_kpages(1);
  size_t size;
  //copyin from userland to kernel space - i.e. get the filename/path we want to run
  if (isKernel == 0) {
    if(copyinstr((userptr_t)progname, filename, PATH_MAX, &size)) return -1; 
  } else {
    free_kpages(filename);  
  }

  //get the argument count
  int argc = 0;
  while(argv[argc] != NULL) argc++;

  //kprintf("TEST1\n"); 
  char** arguments;
  arguments = (char**) alloc_kpages(1); //probably not many arguments 
  //THIS WAS A BUG.....
  
    size_t arg_length; int i; 
    for(i = 0; i < argc; i++) {
      int j = strlen(argv[i]);
      j++;
      arguments[i] = (char*)alloc_kpages(1);
      if (isKernel == 0) {
	copyinstr((userptr_t)argv[i], arguments[i], j, &arg_length); 
      } else {

	strcpy(arguments[i], argv[i]); 

      }
      
    }

  arguments[argc] = NULL; //terminate with null
  

  ///THIS CODE WAS BELOW VFS_OPEN
  //Open up a new addrspace so we can load the elf:
  if (curthread->t_vmspace) { //we need to make a new one
    as_destroy(curthread->t_vmspace);
    curthread->t_vmspace = NULL; 
  }

  curthread->t_vmspace = as_create(0); 
  if (curthread->t_vmspace == NULL) {
    //vfs_close(v); 
    return ENOMEM;
  }
  ////////////// END CODE THAT WAS BELOW VFS_OPEN ////////
  

  //Now we can open the file:

  int x = vfs_open(isKernel ? progname: filename, O_RDONLY, &v); 
  //now v points to the file in kernel land
  if (x) return x; //error

  

  //activate it by getting those TLB regs ready
  //as_activate(curthread->t_vmspace); //now the page tables exist

  //we can now load the executable ELF:
  x += load_elf(v, &entrypoint); //load_elf calls as_define_region
  if (x) { vfs_close(v); return x; }

  
  curthread->t_vmspace->as_npages_stack = 1; //assume 1 page of stack

  //temporary fix for now; just to let matmult work, which barely fits in our memory space
  //if (curthread->t_vmspace->as_npages_text2 < 3) {

    //as_prepare_load(curthread->t_vmspace); 
  //}

  //vfs_close(v); //done with file

  //set up the stack:
  x += as_define_stack(curthread->t_vmspace, &stack_ptr);
  if (x) return x; 
  stack_ptr = stack_ptr - 4; //just to be safe, don't start right at the top

  //stack_ptr = stack; //let's try copying out to physical frame

  //setting up the new stack is tricky
  unsigned int prog_stack[argc];
  for(i = argc-1; i >= 0; i--) { //backwards
    
      int l = strlen(arguments[i]);
      int k = l%4;
    
    if(k == 3) k = 1; //the fix - final addr must be div/4
    else if(k == 0) k = 4; 
    else if (k == 1) k = 3; 
    stack_ptr = stack_ptr - l - k; 
    //copyout from kernel source to user dest:
    //memcpy(arguments[i],stack,
    //kprintf("ARGUMENT[%d]: %s   ---   SP: %x\n",i,arguments[i],stack_ptr);
    copyoutstr(arguments[i], (userptr_t)stack_ptr, l, &arg_length); 
    //free_kpages(arguments[i]); //freee
    prog_stack[i] = stack_ptr; 
  }
  stack_ptr = stack_ptr - 8; 
  prog_stack[argc] = (int) NULL; 
  for(i = argc-1; i >= 0; i--) {
    stack_ptr = stack_ptr - 4; 
    //copy a block of memory from kernel to userland
    copyout(&prog_stack[i], (userptr_t)stack_ptr, sizeof(prog_stack[i])); 
  }

  kfree(arguments); 
  lock_release(forkLock);

  for(i = 0; i < argc; i++) {
    free_kpages(arguments[i]); 
  }
  free_kpages(arguments); 
  free_kpages(filename); 
			   
  //kprintf("CURTHREAD VNODE: 0x%x\n",curthread->t_vmspace->v); 

  //splx(spl); 

  md_usermode(argc, (userptr_t)stack_ptr, stack_ptr, entrypoint);

  panic("md_usermode returned!\n"); 
  
  return EINVAL;

}


//do some kprintf business
void process_debug() {
  
  int s = splhigh();

  kprintf("\n\n --- curthread: %x\n",curthread);
  print_run_queue();
  print_sleep_queue(); 
  int i; struct thread *t;
  for(i = 0; i < array_getnum(THREAD_LIST); i++) {
    t = array_getguy(THREAD_LIST,i);
    kprintf("THREAD: %s     -     %x\n",t->t_name, t);
    kprintf("     pid: %d -- addrspace pid: %d\n",t->pid, t->t_vmspace->PID);
    kprintf("     ppid: %d", t->ppid); 
    as_dump(t->t_vmspace); 
    kprintf("\n\n"); 
  }

  kprintf("\n\n");

  //while(1) {}
  splx(s); 

}

void as_dump(struct addrspace* as) {
  int spl = splhigh();
  int x;
  int texttest, datatest, stacktest;
  texttest = page_table_lookup(as->as_vbase_text,as,&x,0);
	if (texttest == 0) texttest = search_swap_disk(as->as_vbase_text,as->PID);
	
	datatest = page_table_lookup(as->as_vbase_text2,as,&x,0);
  if (datatest == 0) datatest = search_swap_disk(as->as_vbase_text2,as->PID);
  
  stacktest = page_table_lookup(MIPS_KSEG0-PAGE_SIZE,as,&x,0);
  if (stacktest == 0) stacktest = search_swap_disk(MIPS_KSEG0-PAGE_SIZE,as->PID);

  kprintf("Addrspace: 0x%x -- PID: %d\n",as,as->PID);
  kprintf("Text region: 0x%x -- paddr: 0x%x --  npages: %d\n",as->as_vbase_text,
	  texttest,as->as_npages_text);
  kprintf("Data region: 0x%x -- paddr: 0x%x -- npages: %d\n",as->as_vbase_text2,
	  datatest,as->as_npages_text2);
  /*
  kprintf("Heap region: 0x%x -- paddr: 0x%x -- npages: %d\n",as->as_vbase_heap,
	  page_table_lookup(as->as_vbase_heap,as,&x),as->as_npages_heap);
  */
  kprintf("First stack paddr: 0x%x -- Stack npages: %d\n",
	  stacktest,as->as_npages_stack);
  splx(spl); 
}

void print_sleep_queue() {

  int i;
  for (i = 0; i < array_getnum(sleepers); i++) {
    kprintf("Sleeper thread: %x\n",array_getguy(sleepers,i));
  }

}

void sys_getpid_test() {
  kprintf("\nCurrent PID: %d\n",sys_getpid());
  kprintf("Current thread: %x\n\n",curthread);
}

void sys_debug(struct trapframe* tf) {
  int s = splhigh();
  PROCESS_LIST_dump();
  TLB_dump(); 
  examine_bitmap1();
  examine_swapmap();
  examine_raminfo();
  while(1) {}
  splx(s); 
}
