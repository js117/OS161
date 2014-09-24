#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <thread.h>
#include <curthread.h>
#include <addrspace.h>
#include <vm.h>
#include <machine/spl.h>
#include <machine/tlb.h>
#include <synch.h>
#include <process.h>
#include <syscall.h> //for sbrk
#include <vfs.h>
#include <vnode.h>
#include <kern/unistd.h>
#include <kern/stat.h>
#include <uio.h>

//useful at some point
#define TEXT_REGION1 0;
#define TEXT_REGION2 1;
#define HEAP_REGION 2;
#define STACK_REGION 3; 

// PTentry
#define PAGE_MASK 0xfffff000; //20 bits
#define VALID_MASK 0x00000100;
#define DIRTY_MASK 0x00000200;
#define PID_MASK     0x000000ff; 
#define FAULT_REGION 0x00000c00; 
// PTinfo
#define NEXTTRY_MASK 0xfffc0000; //14 bits
#define PRA_MASK 0x0000ffff; //16 bits, for page replacement algorithm use
#define GLOBAL_BIT 0x00000100; 

//Important global variables relating to VM management:
//To be honest these should all be part of a structure for organizational purposes. Maybe later.
int* RamBitMap;
//u_int32_t* RamCoreMap; //the vaddr at index corresponding to physical frame
int* RamRegionSize;  
int* RamCoreMap; //the inverse page table 
int* RamCoreInfo; //support for the IVT
int* RamCoreInfo2; 
int* RamCoreInfo3; 
int* RamLruTime; 
int BitMapSize; 
int curBitMapPtr; 
int p_base; 
int num_free_pages;
int initial_kern_pages; 
int npages_reserved;
int TOTAL_RAM_SIZE;
struct lock* BitMapLock;
//swapmap support 
struct vnode* swapNode;
int* SwapCoreMap;
int SwapMapSize;
struct lock* SWAP_LOCK;
///////////////////////
int LOW_MEM_THRESHOLD; 
struct cv* memCV; //used for mem sleeping and signalling
int* RamLocks; //integer "locks" to be used in my page faulting routines. 
//Notes:
//-RamRegionSize will be used to track the size of each allocation of getppages within the 
// bitmap; its indices will mirror RamBitMap. E.g. let's say we acquire frames 20 - 29 (inclusive).
// Then RamRegionSize[20] will be 10. Indices in between will be zero, I guess (?).  
// This means when we free a vaddr corresponding to frame 20, 
// we will, in free_kpages, loop from starting frame of the vaddr (frame#), to 
// (frame# + RamRegionSize[frame#], and free the corresponding RamBitMap spots. 
//
// About the in-between entries (e.g. RamRegionSize[22]), we can safely set these to zero and not
// worry about them because of how kmalloc and kfree work for large structures. Basically if we 
// ever alloc something bigger than half a page, we will call getppages(n, n>=1) from kmalloc. 
// In getppages we will set RamRegionSize appropriately. Then when we free we will just acquire 
// this same address and do the looping process described above!   

//More notes:
// 1. Will need to create a similar bitmap for the swap section of a disk. 
// 2. Will need to have file objects: vnode and offset, for data and text sections of a program
// 3. Lock priorities: BitMapLock --> SWAP_LOCK. Always follow this acquisition order to
//    avoid deadlocks. 

//helpers for vm_fault
pid_t curPid;
pid_t prevPid; 


//ram_bootstrap happens first...? 
void
vm_bootstrap(void)
{
  
  curBitMapPtr = 0; 
  

  p_base = return_first_paddr()+ (4096 * (initial_kern_pages)); 
  //whatever gets called after vm_bootstrap seems to take up 1 page..
	
  //now we're calling ram_stealmem directly - need to modify the base. 
  BitMapSize = (return_last_paddr() - p_base) / PAGE_SIZE; //remaining after the kernel starts
  num_free_pages = BitMapSize;

  //Support for swapping pages to disk
  char diskname[] = "lhd0raw:";
  int x = vfs_open(diskname, O_RDWR, &swapNode); 
  if (x) panic("VM system: could not open swap disk\n"); 
  struct stat info; //get info about the disk we're using via VOP_STAT
  VOP_STAT(swapNode, &info);
  SwapMapSize = (int)(info.st_size)/PAGE_SIZE; 
  int size_of_swap_disk = (info.st_size)/PAGE_SIZE;
  int npages_swapmap = (sizeof(int)*size_of_swap_disk + PAGE_SIZE)/PAGE_SIZE;
  SwapCoreMap = (int*) ram_stealmem(npages_swapmap); 
  SwapCoreMap = SwapCoreMap + MIPS_KSEG0/4; 
  kprintf("SwapCoreMap: located @ 0x%x -- size: %d pages\n",SwapCoreMap,SwapMapSize); 
  

  npages_reserved = (sizeof(int)*BitMapSize + PAGE_SIZE)/PAGE_SIZE; 

  RamBitMap = (int*) ram_stealmem(npages_reserved);
  RamRegionSize = (int*) ram_stealmem(npages_reserved); 
  RamCoreMap = (int *) ram_stealmem(npages_reserved);
  RamCoreInfo = (int *) ram_stealmem(npages_reserved); 
  RamCoreInfo2 = (int *) ram_stealmem(npages_reserved); 
  RamCoreInfo3 = (int *) ram_stealmem(npages_reserved); 
  RamLruTime = (int *) ram_stealmem(npages_reserved); 
  RamLocks = (int *) ram_stealmem(npages_reserved);
		
    RamBitMap = RamBitMap + MIPS_KSEG0/4;
  RamRegionSize = RamRegionSize + MIPS_KSEG0/4; 
  RamCoreMap = RamCoreMap + MIPS_KSEG0/4;
  RamCoreInfo = RamCoreInfo + MIPS_KSEG0/4; 
  RamCoreInfo2 = RamCoreInfo2 + MIPS_KSEG0/4; 
  RamCoreInfo3 = RamCoreInfo3 + MIPS_KSEG0/4; 
  RamLruTime = RamLruTime + MIPS_KSEG0/4; 
  RamLocks = RamLocks + MIPS_KSEG0/4;

   

  bzero(RamBitMap, BitMapSize); 
  bzero(RamRegionSize, BitMapSize); 
  bzero(RamCoreMap, BitMapSize); 
  bzero(RamCoreInfo, BitMapSize);
  bzero(RamCoreInfo2, BitMapSize);
  bzero(RamCoreInfo3, BitMapSize);
  bzero(RamLruTime, BitMapSize); 
  bzero(SwapCoreMap, SwapMapSize);
  bzero(RamLocks, BitMapSize);
  

  //make sure to reserve the first 4*npages as special. 
  //4 global structures @ npages each
  int i;
  for (i = 0; i < 8*npages_reserved + (npages_swapmap); i++) {
    RamBitMap[i] = 7; //special
    RamRegionSize[i] = 0;
    RamCoreMap[i] = MIPS_KSEG0; 
    RamCoreInfo[i] = 0;
    num_free_pages--; 
  }

  LOW_MEM_THRESHOLD = 4;//BitMapSize/10; //heuristic guess
  memCV = cv_create("memCV");

  BitMapLock = lock_create("BitMapLock");

  SWAP_LOCK = lock_create("SWAP_LOCK"); 

  if (!BitMapLock || !RamBitMap) {
    kprintf("FAILURE ON ALLOCATING MEMORY MANAGEMENT STRUCTURE(S)\n");
    return; 
  }

  //Create a paging thread:
  struct thread* paging_thread;
  thread_fork("PAGING_THREAD", NULL, 0/*unused*/, paging_thread_routine, &paging_thread);
  paging_thread->t_vmspace = as_create();
  paging_thread->pid = curthread->pid; //I consider it part of the main boot process 
  paging_thread->ppid = curthread->pid; 
  //done.

  kprintf("TOTAL RAMSIZE: %d\n",TOTAL_RAM_SIZE);
  kprintf("EFFECTIVE RAMSIZE = #BITMAP_ENTIES = %d\n",BitMapSize); 
  curPid = 0;
  prevPid = 0; 

}

void paging_thread_routine() {
  
  while(1) {
    int spl = splhigh();
    if (num_free_pages > 2*LOW_MEM_THRESHOLD) thread_yield();
    else {
      kprintf("\nPAGING THREAD STARTED\n");
      swap_handler(curthread->t_vmspace);
      if (num_free_pages > LOW_MEM_THRESHOLD) {
	//kprintf("\n\nPAGING THREAD WAKING UP SOME OF THE OTHERS...\n\n");
	thread_wakeup(memCV);
      }
    }
    splx(spl);
    }
  

}

void swap_handler(struct addrspace* as) {
  
  
  if (BitMapLock != NULL) {
 
    int spl = splhigh();
    //lock_acquire(BitMapLock);

    int swapindex; 
    paddr_t swappaddr = find_lru_page(&swapindex, as->PID); 
    //could return a page from ANY process...
      

    if (swappaddr == 0) return; //couldn't do it boss
    //while (swappaddr == 0) {swappaddr = find_lru_page(&swapindex, as->PID); }
    //this indicates an error - the LRU page was part of the kernel. Not supported yet. 

    assert(swappaddr != 0);
    assert(RamLruTime[get_bitmap_index(swappaddr)] >= 0);

    RamLruTime[get_bitmap_index(swappaddr)] = -1;

    int swapvaddr = RamCoreMap[swapindex] & PAGE_MASK; 
    int swappid = RamCoreMap[swapindex] & PID_MASK; 
     
   
    if (swapvaddr == 0) {
      kprintf("HOLD ON A MINUTE...\n");
      kprintf("LRU returned (swappaddr): 0x%x\n",swappaddr);
      //process_debug(); 
      examine_bitmap1();
      while(1) {}
    }

    
    //I don't put text pages on swap
    if (swapvaddr <= as->as_vbase_text+PAGE_SIZE*as->as_npages_text) {
      as_activate(0); //will no longer be a valid mapping
      PT_invalidate(swapvaddr, PID); 
      RamLruTime[get_bitmap_index(swappaddr)] = 0;
      //lock_release(BitMapLock);
      splx(spl);
      return; 
    }
    

    int w = store_to_swap(swapvaddr, swappaddr, swappid);
    if (w != 0) {
      examine_bitmap1();
      examine_swapmap();
      assert(w==0); 
    }
    lock_release(BitMapLock);
    //splx(spl);
    //lock_release(BitMapLock); 

  } 
  
}

int store_to_swap(vaddr_t vpage, paddr_t ppage, int PID) {

  //interrupts: turn them off while dealing with structures and metainfo,
  //but make sure they're on for the actual disk writes/reads
  //lock_acquire(BitMapLock);
  int spl = splhigh();
  int SwapCoreEntry = 0;
  off_t DiskOffset; //to be determined where to write to
  struct uio swap_uio;
  /*
  //first, find a free place
  int first_try = (vpage + 69*PID) % SwapMapSize;
  if (SwapCoreMap[first_try] == 0) { //got a spot
    SwapCoreEntry = vpage | PID; //get the PID in the entry
    SwapCoreMap[first_try] = SwapCoreEntry;
    DiskOffset = first_try * PAGE_SIZE; 
    goto DISK_WRITE;
  }
  int k = 2; int kmax = SwapMapSize/10; int try; //number of quadratic hashing steps we're willing to try
  //before resorting to a linear search. 

  for(k = 2; k < kmax; k++) {
    try = (k*k*(vpage+69*PID))%SwapMapSize; 
    if (SwapCoreMap[try] == 0) {
      SwapCoreEntry = vpage | PID; //get the PID in the entry
      SwapCoreMap[try] = SwapCoreEntry;
      DiskOffset = try * PAGE_SIZE; 
      goto DISK_WRITE;
    }
  }
  */
  

  //OK this sucks we didn't get to hash a spot. Linear seach to find a spot.
  int try = 0;
  for (try = 0; try < SwapMapSize; try++) {
    if (SwapCoreMap[try] == 0) {
      SwapCoreEntry = vpage | PID; //get the PID in the entry
      SwapCoreMap[try] = SwapCoreEntry;
      DiskOffset = try * PAGE_SIZE; 
      goto DISK_WRITE;
    }
  }

  //OK this really sucks, our swap map must be full or something
  //splx(spl);
  //lock_release(BitMapLock);
  return -1; //Swap space was full! 

 DISK_WRITE: 
  /*kprintf("SWAPVADDR: 0x%x -- SWAPPADDR: 0x%x -- SWAPPID: %d -- DiskOffset: 0x%x\n",
  	  vpage,ppage,PID,DiskOffset);
  */
  kprintf("current process: %d -- parent process: %d\n",curthread->pid,curthread->ppid);
  
  //if (as->hasPageOnDisk == 0) as->hasPageOnDisk = 1; //done in PT_invalidate
  mk_kuio(&swap_uio,(void*)PADDR_TO_KVADDR(ppage),PAGE_SIZE,DiskOffset,UIO_WRITE);

  assert(RamLruTime[get_bitmap_index(ppage)] == -1);

  int s = spl0(); //just to make sure
  lock_acquire(SWAP_LOCK);
  int result = VOP_WRITE(swapNode, &swap_uio);
  if (result) panic("VM system: swap to disk failed\n");
  lock_release(SWAP_LOCK);
  splx(s);
  
  //Now we can free this frame completely:
  as_activate(0); //will no longer be a valid mapping
  PT_invalidate(vpage, PID); //not only invalidates from the CoreMap, but marks BitMap as free as well
  RamLruTime[get_bitmap_index(ppage)] = 0; //was -1 before so nobody would swap it out
  //kprintf("done writing to swap...\n");
  PT_invalidate(vpage, PID); 
  TLB_invalidate(); //will no longer be a valid mapping
  //not only invalidates from the CoreMap, but marks BitMap as free as well
  
  lock_release(SWAP_LOCK);
  //splx(spl); 
  //lock_release(BitMapLock);
  return 0;

}

//returns the actual offset, rather than the SwapCoreMap index.
int search_swap_disk(vaddr_t vpage, pid_t PID) {
  
  //lock_acquire(SWAP_LOCK);

  int spl = splhigh(); 
  //first, find a free place
  /*
  int first_try = (vpage + 69*PID) % SwapMapSize;
  vaddr_t test = SwapCoreMap[first_try] & PAGE_MASK;
  if (vpage == test) { //found you
    splx(spl);
    return first_try*PAGE_SIZE;
  }
  
  int k = 2; int kmax = SwapMapSize/10; int try;

  for(k = 2; k < kmax; k++) {
    try = (k*k*(vpage+69*PID))%SwapMapSize; 
    test = SwapCoreMap[try] & PAGE_MASK;
    if (vpage == test) {
      splx(spl);
      return try*PAGE_SIZE; 
    }
  }
  */
  int test; int testPID;
  //Linear search time
  int try = 0;
  for (try = 0; try < SwapMapSize; try++) {
    test = SwapCoreMap[try] & PAGE_MASK;
    testPID = SwapCoreMap[try] & PID_MASK; 
    if (vpage == test && PID == testPID) {
      //if (try == 0) examine_swapmap(); 
      //splx(spl);
      lock_release(SWAP_LOCK);
      //lock_release(SWAP_LOCK);
      return try*PAGE_SIZE;
    }
  }
  splx(spl);
  //lock_release(SWAP_LOCK);
  
  return -1;

}


//How this will be used: given an offset where the appropriate vpage resides on disk, and given
//the allocates ppage where it will go, load that page from the disk back into RAM
//(I say "back" because if it's on the swap disk then it got swapped out at some point)
//If isCopy==1, then don't free the entry.
int load_from_swap(int offset, paddr_t ppage, int isCopy) {
  
  //lock_acquire(BitMapLock);
  int spl = splhigh();

  struct uio swap_uio;
  mk_kuio(&swap_uio,(void*)PADDR_TO_KVADDR(ppage),PAGE_SIZE,offset,UIO_READ);
  
  int s = spl0(); //just to make sure
  lock_acquire(SWAP_LOCK);
  int result = VOP_READ(swapNode, &swap_uio);
  lock_release(SWAP_LOCK);
  splx(s);

  if (result) {
    kprintf("\nLOAD_FROM_SWAP FAILED: offset: 0x%x -- ppage: 0x%x -- \n");
    examine_bitmap1(); 
    panic("VM system: swap from disk failed\n"); 
  }
  
  //splx(s);
  int index = offset/PAGE_SIZE;
  if (isCopy == 1) {
    //kprintf("load_from_swap copy! \n");
  } else {
    SwapCoreMap[index] = 0; //free up the SwapMap index
  }
  lock_release(SWAP_LOCK);
  //splx(spl);
  //lock_release(BitMapLock);

  return 0;

}

//find and return an appropriate page to swap out when VM is full/is getting full
//Precondition: you have acquired BitMapLock
paddr_t find_lru_page(int* spot, int* PTE, int PID) {
  
  //int spl = splhigh();

  int max = 0; int index = 0; int ret_pid;
  int i;

  //lock_acquire(BitMapLock);

  for (i = npages_reserved; i < BitMapSize; i++) {
    int testPID = RamCoreMap[i] & PID_MASK;
    if (RamLruTime[i] > max && RamCoreMap[i] < MIPS_KSEG0
	&& RamLruTime[i] >= 0) {
      max = RamLruTime[i]; 
      index = i;
    }
  }
  
  *PTE = RamCoreMap[index];
  RamLruTime[index] = 0; 

  if (RamCoreMap[index] >= MIPS_KSEG0 || RamCoreMap[index] == 0 || RamLruTime[i] < 0) return 0; 
  //don't wanna swap out a kernel frame by accident, or an empty frame

  *spot = index;

  //lock_release(BitMapLock);

  //splx(spl);
  return (index*PAGE_SIZE + p_base); 
  

}

//get pages for user programs, i.e. VPNs { 0, 0x7ffff000 }
//return 0 if everything went alright, otherwise return -1 

//The virtual page indicated in vaddr is used to allocate (and return) a physical frame.
//Inserts the mapping into the process' page table.  
paddr_t get_upage(vaddr_t vaddr, struct addrspace* as) {
 
 

  paddr_t paddr;
  //int spl = splhigh();
  
  

  //add a corner case
  if (vaddr == 0) {
    //splx(spl);
    return 0; 
  }

  //page fault to insert into the PT
  int x = page_fault(VM_FAULT_WRITE, vaddr, as); 
  int PTentry; 
  lock_acquire(BitMapLock);
  paddr = page_table_lookup(vaddr, as, &PTentry,0);     
  lock_release(BitMapLock);
  if (paddr == 0) { 
    kprintf("ERROR - get_upage returned 0:\n");
    kprintf("vaddr: 0x%x -- pid: %d\n",vaddr,as->PID);
    kprintf("PTE: 0x%x\n",PTentry);
    TLB_dump();
    examine_bitmap1(); 
    while(1) {}
    
  }
  int PID = as->PID; 
  //kprintf("GET_UPAGE for VADDR: 0x%x got PADDR: 0x%x -- PID: %d\n",vaddr,paddr,PID); 

  //splx(spl); 
  assert(paddr != 0); //we will ensure that page_fault handles the case of no ram left
  //lock_release(BitMapLock);
  return paddr; 
 
}


//return the 1st available physical frame in the bitmap; returns the p_addr
//ensures that the addr returned is at most (lastpaddr - PAGE_SIZE)
int get_ram_frame() {

  //int spl = splhigh();
  lock_acquire(BitMapLock);

  if (num_free_pages == 0) return 0; //page fault, no free pages

  paddr_t addr; 
  if (RamBitMap[curBitMapPtr] == 0) { //then alloc this frame
    RamBitMap[curBitMapPtr] = 1;
    RamCoreMap[curBitMapPtr] = curthread; //just so we know it's kernel's  
    RamRegionSize[curBitMapPtr] = 1; //remember, the region we're giving is only size 1
    addr = curBitMapPtr * PAGE_SIZE;
    curBitMapPtr++; //advance to next place for next time
    //splx(spl);
    num_free_pages--; 
    lock_release(BitMapLock);
    return (addr + p_base);  
  }
  //otherwise we have to get searching
  int i;
  for (i = curBitMapPtr; i < BitMapSize; i++) {
    if (RamBitMap[i] == 0) {
      RamBitMap[i] = 1;
      RamCoreMap[i] = curthread; 
      RamRegionSize[i] = 1; //region we're returning is size 1
      curBitMapPtr = i + 1; //advance to next place for next time
      addr = i * PAGE_SIZE;
      //splx(spl);
      num_free_pages--;
      return (addr + p_base);  
    }
  } 
  //only thing left to try is to loop from beginning
  for (i = 0; i < curBitMapPtr; i++) {
    if (RamBitMap[i] == 0) {
      RamBitMap[i] = 1; 
      RamCoreMap[i] = curthread;
      RamRegionSize[i] = 1; //region we're returning is size 1
      curBitMapPtr = i + 1; //advance to next place for next time
      addr = i * PAGE_SIZE;
      num_free_pages--; 
      //splx(spl);
      lock_release(BitMapLock);
      return (addr + p_base);  
    }
  }
  //splx(spl);
  lock_release(BitMapLock);
  //if we've got here, then our physical memory is full meaning we have a page fault 
  return 0; //error value - indicates page fault

}

paddr_t getppages(unsigned long npages)
{
  paddr_t addr;
  //int spl = splhigh();

  if (BitMapLock != NULL) { //i.e. our bitmap has been initialized

    if (SWAP_LOCK != NULL && num_free_pages <= npages) {
      int i;
      kprintf("ALLOC_KPAGES SWAPPING SOME STUFF OUT -- npages: %d\n",npages); 
      for(i = 0; i <= 2*npages; i++) {
	swap_handler(curthread->t_vmspace);
      }
      kprintf("ALLOC_KPAGES DONE SWAPPING OUT: %d page(s)\n",npages); 
      thread_wakeup(memCV);
    }


    int i; int count = 0; int didItWork = 0; int test = npages;

    //Currently we loop through, find a 0, then see how many more consecutive zeros we can find.
    //At worst this will be ~O(n^2) which sucks. Very costly for, say, 16384 frames. 
    //there must be a better algorithm for this. Try checking RamBitMap[i+npages] or something
    //optimization: can shift by more than one when looking; shift up to next block
  GET_CONTIGUOUS_PAGES:
    for (i = 0; i < BitMapSize; i++) {
      if (RamBitMap[i] == 0) {
	test = npages;
	for (count = i; count < i+npages; count++) { //fix - count must start at i, not zero
	  if (RamBitMap[count] == 0) test--; 
	}
	if (test == 0) {
	  didItWork = 1;   
	  break;
	}
      
      }
     
    }
    
    //now we need to mark the frames as taken since we found 'em
    if (didItWork) {
      RamRegionSize[i] = npages; //aha. Now we can properly free at this address
      int j;
      for (j = i; j < i+npages; j++) {
	RamBitMap[j] = 1; //taken!
	RamCoreMap[j] = MIPS_KSEG0 + (TOTAL_RAM_SIZE-BitMapSize+i)*PAGE_SIZE;
	//RamLruTime[j] = 0; 
							   //assume kernel at first 
	
      }
      addr = i*PAGE_SIZE + p_base;
      curBitMapPtr = i+npages; //don't forget to advance this
      num_free_pages = num_free_pages - npages; //because we just took npages
      initial_kern_pages += npages;
      splx(spl);
      return addr;
    } else {
      for(i = 0; i <= npages; i++) {
	swap_handler(curthread->t_vmspace);
      }
      goto GET_CONTIGUOUS_PAGES;
    }

    //if it gets here, it didn't work
    //splx(spl); 
    lock_release(BitMapLock);
    return 0; //same error ret val that ram_stealmem(n) returns

  } else { //when the bitmap is not initialized, use normal method

	addr = ram_stealmem(npages);  initial_kern_pages += npages; 
	//splx(spl); 
	return addr;

  }
  

}

/* Allocate/free some kernel-space virtual pages */
//swap_handler & getppages both handle locking themselves
vaddr_t 
alloc_kpages(int npages)
{

  //int spl = splhigh(); 
	u_int32_t pa;
	pa = getppages(npages);

	
	if (pa == 0) { 
	  kprintf("ALLOC_KPAGES CANT GET NO PAGES -- npages: %d\n",npages);
	  examine_bitmap1(); 
	  examine_swapmap();
	  int spl = splhigh();
	  while(1) {}
	} 
	
	 
	//kprintf("alloc_kpages got physical addr: 0x%x\n",pa); 
	//splx(spl); 
	return PADDR_TO_KVADDR(pa);
}

int get_bitmap_index(paddr_t ppp) {
  return (ppp - p_base)/PAGE_SIZE; 
}

//This function is called from kfree when the size to be freed is too large. 
//First convert addr into the appropriate frame. For kernel this is easy
// --> just substract 0x80000000 from the vaddr to get the p frame trying to be freed
void 
free_kpages(vaddr_t addr)
{

  //int spl = splhigh();

  lock_acquire(BitMapLock); 
  
  int paddr = addr - 0x80000000;
  int index = get_bitmap_index(paddr);

  //kprintf("KFREE: paddr: 0x%x  --  index: %d\n",paddr,index);
  //kprintf("numProcs: %d\n",numProcs); 
  
  int i;
  for (i = index; i < index + RamRegionSize[index]; i++) {
    RamBitMap[i] = 0;
    RamCoreMap[i] = 0; 
    RamLruTime[i] = 0;
    num_free_pages++; 
  }
  RamRegionSize[index] = 0;
  
  //splx(spl); 
  lock_release(BitMapLock);

}

//Prototypes needed: 
//void TLB_Write(u_int32_t entryhi, u_int32_t entrylo, u_int32_t index);
//void TLB_Read(u_int32_t *entryhi, u_int32_t *entrylo, u_int32_t index);

//vm fault will happen before a page fault. 
//do we care about faulttype? 
int
vm_fault(int faulttype, vaddr_t faultaddress)
{

  if (faultaddress < curthread->t_vmspace->as_vbase_text) {
    kprintf("BAD fault address: 0x%x; current process: %d\n",faultaddress,curthread->pid);
    TLB_dump();
    examine_bitmap1();
    examine_swapmap();
    assert(faultaddress >= curthread->t_vmspace->as_vbase_text);
  }

  //kprintf("TLB FAULT: 0x%x -- PID: %d\n",faultaddress,curthread->t_vmspace->PID);
  int i;
  int spl = splhigh();

  if (faultaddress < curthread->t_vmspace->as_vbase_text) {
    kprintf("BAD fault address: 0x%x; current process: %d\n",faultaddress,curthread->pid);
    kprintf("curPid: %d -- prevPid: %d\n",curPid,prevPid);
    TLB_dump();
    as_dump(curthread->t_vmspace);
    examine_bitmap1();
    examine_swapmap();
    assert(faultaddress >= curthread->t_vmspace->as_vbase_text);
  }

  
  struct addrspace* as = curthread->t_vmspace; 
  u_int32_t PTentry; 
  
  u_int32_t ehi = 0;
  u_int32_t elo = 0; 

  
  paddr_t paddr = page_table_lookup(faultaddress, as, &PTentry,0);
  lock_release(BitMapLock);

  if(paddr == 0) {
    //kprintf("paddr == 0 because we got here.\n"); 
    int x = page_fault(faulttype, faultaddress, as);
    if (x) {
      kprintf("Uh oh, page_fault returned non-zero: %d\n",x);
      return EUNIMP;
    }
    lock_acquire(BitMapLock);
    paddr = page_table_lookup(faultaddress, as, &PTentry,0);
    lock_release(BitMapLock);
    //should be good after page_fault
    //kprintf("DID WE GET A NEW PADDR? 0x%x\n",paddr); 
  }
  

  
  int ram_index = get_bitmap_index(paddr); 
  RamLruTime[ram_index] = 0;
  //kprintf("TEST: RamLruTime[%d] = %d\n",ram_index,RamLruTime[ram_index]); 

  //set up TLB high entry
  int vpage = faultaddress & PAGE_MASK;
  ehi = ehi | vpage; //set the VPage
  
  
  //ehi = ehi | PID; //put the PID in there

  //now the low entry
  int ppage = paddr & PAGE_MASK;
  elo = elo | ppage; 
  //kprintf("\n:PT entry: 0x%x\n",PTentry); 
  int faultreg = PTentry & FAULT_REGION; 
  faultreg = faultreg >> 10; 

  // kprintf("\nFAULT TYPE: %d\n",faulttype);
  //kprintf("FAULT REGION: %d\n",faultreg); 

  //if (faultreg == 1 || faultreg == 3 || faultreg == 0 || faultreg == 2) {
    elo = elo | TLBLO_DIRTY; //also, entry starts valid so no need to mess with that
    //} //else we'll leave it zero
  int valid = TLBLO_VALID;
  elo = elo | valid; //this new mapping is valid
  
  if (ppage == 0) {
    kprintf("ERROR - ppage is 0x0 -- faultaddress: 0x%x\n",faultaddress);
    process_debug(); 
    examine_bitmap1(); 
     kprintf("HUH\n"); 
     spl = splhigh();
    while(1) {}
    
  }

  

  //kprintf("======== About to write TLB entries: 0x%x||0x%x\n",ehi,elo); 
  //kprintf("VM FAULT TYPE: %d\n",faulttype); 

  u_int32_t test_ehi;
  u_int32_t test_elo; 
  
  spl = splhigh(); //TLB operations should be completely atomic
  	for (i=0; i<NUM_TLB; i++) {
	WRITE_TLB:
		TLB_Read(&test_ehi, &test_elo, i);
		if (test_elo & TLBLO_VALID) {
		  //if (test_ehi == ehi && test_elo == elo) break;
		  continue; //will probably want to update page table on this case of 
		  //removing a TLB entry. Especially if it's writable/dirty
		}
		TLB_Write(ehi, elo, i);
		
		
		if (i == 63) {
		  //kprintf("holy shit batman, the TLB got full\n"); 
		  int l;
		  for (l = 0; l < NUM_TLB; l++) {
		    TLB_Write(TLBHI_INVALID(l), TLBLO_INVALID(), l);
		  }
		}
		goto WRITE_TLB;
		//kprintf("FAULT TYPE: %d\n\n",faulttype); 
		//kprintf("curthread->pid: %d\n",curthread->pid); 
		//process_debug(); 
		//TLB_dump();
		splx(spl);
		return 0;
	}

	//should not have reached here:
	kprintf("ERROR - TLB fault not handled\n");
	while(1) {
	  
	}

}


int page_fault(int faulttype, vaddr_t faultaddress, struct addrspace* as) {
  
  //lock_acquire(BitMapLock); 
  int spl = splhigh(); //because we will allow other functions to call page_fault
  //not just the vm_fault handler
  int s; //to turn interrupts back on for the disk...?


  paddr_t paddr; int i; int fault_region;
  u_int32_t ehi, elo;
  vaddr_t vbase_text, vtop_text, vbase_heap, vtop_heap, stackbase, stacktop;
  vaddr_t vbase_text2, vtop_text2;   
  
        vbase_text = as->as_vbase_text;
	vtop_text = vbase_text + as->as_npages_text * PAGE_SIZE;
	vbase_text2 = as->as_vbase_text2;
	vtop_text2 = vbase_text2 + as->as_npages_text2 * PAGE_SIZE;
	vbase_heap = as->as_vbase_heap;
	vtop_heap = as->as_vtop_heap; 
	stackbase = USERSTACK - (as->as_npages_stack  * PAGE_SIZE);
	stacktop = USERSTACK;

     vaddr_t useraddr = (faultaddress/PAGE_SIZE)*PAGE_SIZE; 
     off_t offset; 
     
     //kprintf("Faultaddress: 0x%x -- process: %d\n",faultaddress,as->PID);

	//first, find out what section the faultaddress lies in: 
   if (faultaddress >= vbase_text && faultaddress <= vtop_text) {
     fault_region = TEXT_REGION1; 
     
     offset = (faultaddress - vbase_text)/PAGE_SIZE*PAGE_SIZE;
     assert(offset>=0); 
    
     int fz = as->text_size - (useraddr - as->as_vbase_text);
     if (as->text_size<PAGE_SIZE) fz = as->text_size;
     if (fz >= PAGE_SIZE) fz = PAGE_SIZE;
     //int fz = PAGE_SIZE; 

     //kprintf("OFFSET TO LOAD FROM (text): 0x%x -- fz: %d\n",offset,fz); 
     int ppage;
     
     int x = insert_new_PTentry(faultaddress, as, &ppage, fault_region);
     while (x == -1) {
       thread_sleep(memCV);
       int x = insert_new_PTentry(faultaddress, as, &ppage, fault_region);
     }
     assert(x!=-1);
     //bzero(PADDR_TO_KVADDR(ppage),PAGE_SIZE);
      

     int vpage = faultaddress & PAGE_MASK; 
     ppage = ppage & PAGE_MASK; 
     
     //make sure the frame can't be swapped out while we're reading from disk
     RamLruTime[get_bitmap_index(ppage)] = -1;

       int ls = load_segment(as->v, offset, PADDR_TO_KVADDR(ppage), 
			     PAGE_SIZE, fz,1); 
      
       assert(RamLruTime[get_bitmap_index(ppage)] == -1);
       RamLruTime[get_bitmap_index(ppage)] = 1000;
     
   }
   else if (faultaddress >= vbase_text2 && faultaddress <= vtop_text2) {
     fault_region = TEXT_REGION2;
     
     //offset = as->text_size;  
     offset = (faultaddress - vbase_text2)/PAGE_SIZE*PAGE_SIZE + as->data_offset; 
     int offset_page = (faultaddress - vbase_text2)/PAGE_SIZE*PAGE_SIZE; 

     assert(offset>=as->data_offset);

    
     int fz = as->data_size -(useraddr-as->as_vbase_text2);
     if (as->data_filesz < PAGE_SIZE) fz = as->data_filesz;
     if (fz>=PAGE_SIZE) fz=PAGE_SIZE;
     if (as->data_filesz == 0) fz = 0;
     
     //as->data_filesz = as->data_filesz - fz; //because we've loaded it. 
     
     //kprintf("(DATA) faultaddress -- data: 0x%x -- offset: %d -- fz: %d\n",faultaddress,offset,fz);

     //DONE_FZ:
     int ppage;
     int x = insert_new_PTentry(faultaddress, as, &ppage, fault_region);
     while (x == -1) {
       thread_sleep(memCV);
       x = insert_new_PTentry(faultaddress, as, &ppage, fault_region);
     }
     assert(x!=-1);
     //bzero(PADDR_TO_KVADDR(ppage),PAGE_SIZE);

     int vpage = faultaddress & PAGE_MASK; 
     ppage = ppage & PAGE_MASK; 

     //make sure the frame can't be swapped out while we're reading from disk
     RamLruTime[get_bitmap_index(ppage)] = -1;

     if (as->hasPageOnDisk == 1) {
       int query = search_swap_disk(useraddr, as->PID); 
       if (query >= 0) {
	 //kprintf("FOUND DATA PAGE ON DISK; OFFSET: 0x%x -- vaddr: 0x%x -- ppage: 0x%x -- PID: %d\n",
	 //	 query,useraddr,ppage,as->PID);
	 //bzero(PADDR_TO_KVADDR(ppage),PAGE_SIZE); //no difference
	 int u = load_from_swap(query, ppage, 0);

	 
       } else {
	 int ls = load_segment(as->v, offset, PADDR_TO_KVADDR(ppage), 
	 		     PAGE_SIZE, fz,1); 
       }
     } else {
       //kprintf("DATA loading filesz: %d\n",fz);
       int ls = load_segment(as->v, offset, PADDR_TO_KVADDR(ppage), 
			     PAGE_SIZE, fz,1); 
       
     
     }
     
     assert(RamLruTime[get_bitmap_index(ppage)] == -1);
     RamLruTime[get_bitmap_index(ppage)] = 1000;

   }
   else if (faultaddress >= vbase_heap && faultaddress <= vtop_heap) {
     fault_region = HEAP_REGION;
     
     int ppage;
     int x = insert_new_PTentry(faultaddress, as, &ppage, fault_region);
     while (x == -1) {
       thread_sleep(memCV);
       x = insert_new_PTentry(faultaddress, as, &ppage, fault_region);
     }
     bzero(PADDR_TO_KVADDR(ppage),PAGE_SIZE);
     
     //make sure the frame can't be swapped out while we're reading from disk
     RamLruTime[get_bitmap_index(ppage)] = -1;

     if (as->hasPageOnDisk == 1) {
       int query = search_swap_disk(useraddr, as->PID); 
       if (query >= 0) {
	 //kprintf("FOUND STACK PAGE ON DISK; OFFSET: 0x%x -- vaddr: 0x%x -- ppage: 0x%x\n",
	 //	 query,useraddr,ppage);
	 int uu = load_from_swap(query, ppage, 0); 
       }
     }

     assert(RamLruTime[get_bitmap_index(ppage)]== -1);
     RamLruTime[get_bitmap_index(ppage)] = 1000;

   }
   
   else if (faultaddress >= vtop_heap && faultaddress <= stacktop) {
     fault_region = STACK_REGION; 
     //kprintf("STACK REGION FAULT: 0x%x -- as->hasPageOnDisk: %d\n",faultaddress,as->hasPageOnDisk); 

     int ppage;
     int x = insert_new_PTentry(faultaddress, as, &ppage, fault_region);
     while (x == -1) {
       thread_sleep(memCV);
       x = insert_new_PTentry(faultaddress, as, &ppage, fault_region);
     }
     assert(x!=-1);

     //make sure the frame can't be swapped out while we're reading from disk
     RamLruTime[get_bitmap_index(ppage)] = -1;
     
     if (as->hasPageOnDisk == 1) {
       int query = search_swap_disk(useraddr, as->PID); 
       if (query >= 0) {
	 //kprintf("FOUND STACK PAGE ON DISK; OFFSET: 0x%x -- vaddr: 0x%x -- ppage: 0x%x\n",
	 //	 query,useraddr,ppage);
	 int uu = load_from_swap(query, ppage, 0); 
       }
     }

     assert(RamLruTime[get_bitmap_index(ppage)]== -1);
     RamLruTime[get_bitmap_index(ppage)] = 1000;
     
   }
     
   
   else { 

     fault_region = 9001;
     kprintf("FAUL_REGION FAIL @ 0x%x -- PID %d\n\n\n",faultaddress,as->PID);
     /*
	kprintf("\n\n=========== Addrspace params ==============\n");
	kprintf("vbase_text: 0x%x -- npages: %d\n",vbase_text,as->as_npages_text);
	kprintf("vtop_text: 0x%x\n",vtop_text);
	kprintf("vbase_text2: 0x%x -- %d\n",vbase_text2,as->as_npages_text2);
	kprintf("vtop_text2: 0x%x\n",vtop_text2);
	kprintf("vbase_heap: 0x%x\n",vbase_heap);
	kprintf("vtop_heap: 0x%x\n",vtop_heap);
	kprintf("stackbase: 0x%x -- npages_stack: %d\n",stackbase, as->as_npages_stack);
	kprintf("stacktop: 0x%x\n", stacktop);
	kprintf("===============================================\n"); 
     */
	//sys_debug(NULL);
     TLB_dump();
     as_dump(as);
	examine_bitmap1();
	examine_swapmap();
      
     TLB_dump(); 
     while(1) {} 
     
   }
   
 
   //By the end of this, our process better have some pages in RAM.
   int pagetest = 0;
   for(i = 0; i < BitMapSize; i++) {
     int PIDtest = RamCoreMap[i] & PID_MASK;
     if (PIDtest == as->PID) pagetest++;
   }
   assert(pagetest != 0);

   //lock_release(BitMapLock);
   splx(spl); 
   return 0;
}



int insert_new_PTentry(vaddr_t faultaddress, struct addrspace* as, int* ppage, int faultreg) {
  //int spl = splhigh();

  lock_acquire(BitMapLock);

   int newPTentry=0; //build a new page table entry
   int nextPTinfo=0;

   int first_try; //to set PTinfo of the first try if things work out and we need to

   int offset; 
   int newPTspot = PT_hash_function(faultaddress, &first_try, as, &offset); 
   *ppage = newPTspot * PAGE_SIZE + p_base; 

   while (newPTspot == -1) {
     newPTspot = PT_hash_function(faultaddress, &first_try, as, &offset);
   }
   *ppage = newPTspot * PAGE_SIZE + p_base;

   //the PTE
   int VPN = faultaddress & PAGE_MASK; //0x#####000
   newPTentry = newPTentry | VPN; 
   int valid = 1; valid = valid << 8; int dirty = 1; dirty = dirty << 9;
   //newPTentry = newPTentry | valid; newPTentry = newPTentry | dirty;
   int PID = as->PID; 
   //PID is the bottom 8 bits now I decided.
   if (PID > 255) PID = 255; //cap at 255 processes
   newPTentry = newPTentry | PID; 
   faultreg = faultreg << 10; //a number 0,1,2 or 3; move it over to bits 11,12
   newPTentry = newPTentry | faultreg;

   //the PT info
   offset = offset << 18; //because we're saying it'll be 14 bits max
   nextPTinfo = nextPTinfo | offset; 
   nextPTinfo = nextPTinfo | PID; 
   
   

   RamCoreMap[newPTspot] = newPTentry;
   
   //kprintf("About to insert new entry: 0x%x -- ppage: 0x%x\n",newPTentry,*ppage); 

   if (newPTentry < 0x400000) {
     kprintf("Found a small PTE: 0x%x (faultaddr: 0x%x) for process %d (ppid: %d)\n",
	     newPTentry,faultaddress,as->PID,curthread->ppid);
     //process_debug();
     examine_bitmap1();
     examine_swapmap();
     
     while(1){}
   }
   
   /*
   //now trying to overwrite the CoreInfo entry... If we need to do that, then we need to invalidate
   //the corresponding CoreMap entry.
   /*
   if (offset != 0) {

     assert(nextPTinfo != 0);

     if (RamCoreInfo[first_try] == 0) {
       RamCoreInfo[first_try] = nextPTinfo; 
       //kprintf("ADDING LVL1 PT INFO ENTRY: 0x%x -- %d\n",nextPTinfo,first_try);
       goto DONE;
     }
     if (RamCoreInfo[first_try] != 0 && RamCoreInfo2[first_try] == 0) {
       //kprintf("LEVEL2 COLLISION -- adding: 0x%x -- %d\n",nextPTinfo,first_try); 
       RamCoreInfo2[first_try] = nextPTinfo;
       goto DONE;
     }  
       int k;
       for (k = 0; k < BitMapSize; k++) {
	 if (RamCoreInfo3[k] == 0) {
	   RamCoreInfo3[k] = nextPTinfo;
	   if (as->hasLevel3Collisions == 0) as->hasLevel3Collisions = 1; 
	   goto DONE;
	 }
       }
   */
   
   }
   */
 DONE: 
   splx(spl); 
   return 0;
}

//interrupt atomicity because others can call this.
//kill parameter: set to 1 to invalidate the particular entry
//Precondition: you have the BitMapLock
paddr_t page_table_lookup(vaddr_t vaddr, struct addrspace* as, u_int32_t* PTentry, int kill) {

  int spl = splhigh();
  //lock_acquire(BitMapLock);

  if (vaddr == 0) {
    //splx(spl);
    //lock_release(BitMapLock);
    return 0; 
  }

  vaddr = vaddr & PAGE_MASK;
  int i;
  for(i = npages_reserved; i < BitMapSize; i++) {
    int testVPAGE = RamCoreMap[i] & PAGE_MASK;
    int testPID = RamCoreMap[i] & PID_MASK;
    if (testVPAGE == vaddr && as->PID == testPID) {
      splx(spl);
      return (i*PAGE_SIZE + p_base);
    }
  }

  /*
  int num_tries = 0; 
  int next_try_offset, next_try_up, next_try_down; 
  int VPN = vaddr & PAGE_MASK; //0x#####000 
  VPN = VPN >> 12; 
  int first_try = (VPN+69*as->PID) % BitMapSize;
  //first_try is the index into the bitmap
  int PID = as->PID;
  int vpage = vaddr & PAGE_MASK;
  int i;
  for(i = 0; i < BitMapSize; i++) {
    int testVPAGE = RamCoreMap[i] & PAGE_MASK;
    int testPID = RamCoreMap[i] & PID_MASK;
    if (testVPAGE == vpage && testPID == PID) {
      if (kill == 0) {
	//kprintf("Found in page_table_lookup: 0x%x -- PID: %d\n",testVPAGE,testPID);
	return (i*PAGE_SIZE + p_base);
      }
      else {
	RamCoreMap[i] = 0;
	RamBitMap[i] = 0; 
	RamLruTime[i] = 0;
	num_free_pages++;
	return 0;
	
      }
    }
  }

  /*
  ////////////////// TRY THE LEVEL1 HASH TABLE ///////////////////////////

  int pt_try = RamCoreMap[first_try] & PAGE_MASK; pt_try = pt_try >> 12; 
  int PID = RamCoreMap[first_try] & PID_MASK; 
  //kprintf("...pt_try: 0x%x\n",pt_try);
  if (pt_try == VPN && as->PID == PID) {
    *PTentry = RamCoreMap[first_try]; 
    if (!kill) {
      //splx(spl); 
      lock_release(BitMapLock);
      //lock_release(BitMapLock);
      return (first_try*PAGE_SIZE + p_base); 
    } else {
      RamCoreMap[first_try] = 0;
      RamBitMap[first_try] = 0; num_free_pages++;
      *PTentry = 0; //represents zero offset
      //splx(spl);
      //lock_release(BitMapLock);
      return 0;
    }
  }
  next_try_offset = RamCoreInfo[first_try] & NEXTTRY_MASK;
  next_try_offset = next_try_offset >> 18; 
  //kprintf("...next_try_offset: %x\n",next_try_offset);

 TRY_OFFSETS:

  next_try_up = RamCoreMap[first_try + next_try_offset] & PAGE_MASK;
  PID = RamCoreMap[first_try + next_try_offset] & PID_MASK; 
  next_try_up = next_try_up >> 12;
  //kprintf("...next_try_up: 0x%x\n",next_try_up);
  if (next_try_up == VPN && as->PID == PID) {
    *PTentry = RamCoreMap[first_try + next_try_offset];
    //if (num_tries > 1) kprintf("success on num_tries: %d\n");
    if (!kill) {
      //splx(spl);
      //lock_release(BitMapLock);
      return ((first_try + next_try_offset)*PAGE_SIZE + p_base); 
    } else {
      RamCoreMap[first_try + next_try_offset] = 0;
      RamBitMap[first_try + next_try_offset] = 0; num_free_pages++; 
      *PTentry = next_try_offset << 18; //aha. We can use this to help invalidate offsets
      //splx(spl);
      //lock_release(BitMapLock);
      return 0;
    }
  }

  next_try_down = RamCoreMap[first_try - next_try_offset] & PAGE_MASK; 
  PID = RamCoreMap[first_try - next_try_offset] & PID_MASK; 
  next_try_down = next_try_down >> 12;
  //kprintf("...next_try_down: 0x%x\n",next_try_down);
  if (next_try_down == VPN && as->PID == PID) {
    *PTentry = RamCoreMap[first_try - next_try_offset];
    //if (num_tries > 1) kprintf("success on num_tries: %d\n");
    if (!kill) {
      //splx(spl);
      //lock_release(BitMapLock);
      return ((first_try - next_try_offset)*PAGE_SIZE + p_base); 
    } else {
      RamCoreMap[first_try - next_try_offset] = 0;
      RamBitMap[first_try - next_try_offset] = 0; num_free_pages++; 
      *PTentry = next_try_offset << 18; //aha. We can use this to help invalidate offsets
      //splx(spl);
      //lock_release(BitMapLock); 
      return 0;
    }
  }

  //if (num_tries > 1) kprintf("num_tries: %d\n",num_tries); 

  num_tries++; 
  ////////////////////////////////////////////////////////////////////////////

  ////////////// OK, TRY THE LEVEL2 HASH TABLE ///////////////////////////////

  if (num_tries == 1) {
    //kprintf("TRYING LEVEL2 TABLE\n"); 
    next_try_offset = RamCoreInfo2[first_try] & NEXTTRY_MASK;
    next_try_offset = next_try_offset >> 18; 
    goto TRY_OFFSETS;
  } else { //num_tries >= 2.
    if (as->hasLevel3Collisions == 0) goto DONE; 
    //kprintf("TRYING LEVEL3 OFFSET - num_tries-2: %d\n",num_tries-2);
    next_try_offset = RamCoreInfo3[num_tries - 2]; 
    if (next_try_offset == 0) {
      goto DONE;
    }
    next_try_offset = next_try_offset >> 18; 
    goto TRY_OFFSETS;
  }
  */
 DONE:
  //you don't exist in this page table then.
  *PTentry = 0;
  //splx(spl);

  */
  return 0; 


}

//returns the index of the bitmap that it was able to acquire; -1 if not able
//We will use ________ probing, and we will modify j to tell us how many steps
//we had to take. This will allow us to set the "check next if wrong" field for the PTE.
//PRECONDITION: you have the BitMapLock
int PT_hash_function(vaddr_t vaddr, int* first_try, struct addrspace* as, int* offset) { 
  
  //int spl = splhigh();

  //Check for low memory threshold:
     if (num_free_pages < LOW_MEM_THRESHOLD) {
       int o;
       //kprintf("PT_hash_fcn swapping some pages out\n");
       lock_acquire(SWAP_LOCK);
       lock_acquire(SWAP_LOCK);
       for(o = 0; o < LOW_MEM_THRESHOLD+1; o++) swap_handler(as);
       //assert(num_free_pages >= LOW_MEM_THRESHOLD);
       lock_release(SWAP_LOCK);
       //assert(num_free_pages >= LOW_MEM_THRESHOLD);
       lock_release(SWAP_LOCK);
       //kprintf("NUM_FREE_PAGES after PT_hash_function swaps: %d\n",num_free_pages);
       thread_wakeup(memCV);
     }
     
     /*
  int VPN = vaddr & PAGE_MASK; //0x#####000
  VPN = VPN >> 12; //shift right 12 times to get 0x000#####
  int j;
  //example of what we want to do: take 0xfffff virtual pages and map them to 80 p frames. 
  //i.e. 1,048,575 possible virtual frames to 80 physical frames of ram

  /*

  int try = (VPN+69*as->PID) % BitMapSize; //a number from 0 to BitMapSize-1;
  int init_try = try; 
  *first_try = init_try; 

  //kprintf("BITMAP SIZE: %d\n",BitMapSize); 
  //kprintf("HASH FUNCTION FIRST TRY: %d -- VPN: 0x%x -- PID: %d\n",init_try,VPN,as->PID); 

  if (RamBitMap[try] == 0) { //take it
    *offset = 0; 
    RamBitMap[try] = 1; RamRegionSize[try] = 1; num_free_pages--; //taking a frame
    //kprintf("HASH FUNCTION GOT: %d -- with offset: %d\n",try,*offset);
    //splx(spl);
    return try; 
  } else { 
     */
     int j, try, init_try; 
    for(j = npages_reserved; j < BitMapSize; (j)++) {
      try = j; 
      if (RamBitMap[try] == 0 && RamLruTime[try] >= 0) {
	*offset = init_try - j;
	if (*offset < 0) *offset = *offset * -1; //make it positive... 
	RamBitMap[try] = 1; RamRegionSize[try] = 1; num_free_pages--; //taking a frame
	//kprintf("HASH FUNCTION GOT: %d -- with offset: %d\n",try,*offset);
		splx(spl);
	return try; 
      }
    }

 
    //}
  
  splx(spl);
   return -1; //we couldn't find an empty spot. 

}

//GOAL: grow the vtop_heap by "change" if we can. Try not to do everything at a page
//granularity because that's wasteful (e.g. malloc(10); malloc(20); malloc(30); there's
//no reason these small allocations should require 3 pages)

void* sys_sbrk(size_t change) {
  //int s = splhigh();
  kprintf("SBRK TIME\n"); 
  struct addrspace* as = curthread->t_vmspace; 
  vaddr_t old_heap_top = as->as_vtop_heap;
  size_t npages_new_heap = (change + PAGE_SIZE)/PAGE_SIZE;

  vaddr_t stackbottom = USERTOP - (as->as_npages_stack*PAGE_SIZE); 
  //check for error first to avoid doing other stuff if we don't need to
  if (stackbottom-PAGE_SIZE < (as->as_vtop_heap + npages_new_heap*PAGE_SIZE)) goto ERROR;

  as->as_vtop_heap += change; 

  //int ppage;
  //int x = insert_new_PTentry(, curthread->t_vmspace, &ppage, HEAP_REGION);

  //splx(s); 
  return (void*) old_heap_top; 

 ERROR:
  //splx(s);
  return ((void*)-1); 

  

}

void examine_bitmap1() {

  int spl = splhigh();
  int i; 
  for (i = 0; i < BitMapSize; i++) {
    if (RamBitMap[i] != 0 || RamCoreInfo[i] != 0 || RamCoreInfo2[i] != 0) {
      int PID = RamCoreMap[i] & PID_MASK;
      int offset1 = RamCoreInfo[i] & PID_MASK;
      //offset1 = offset1 >> 18; 
      int offset2 = RamCoreInfo2[i] & PID_MASK;
      //offset2 = offset2 >> 18; 
      kprintf("BitMap @ %d: %d - CoreMap: 0x%x - LRU_TIME: %d - paddr: 0x%x - PID: %d\n",i,
	      RamBitMap[i],RamCoreMap[i],RamLruTime[i],i*PAGE_SIZE + p_base, PID); 
    }
  }
  kprintf("NUM_FREE_PAGES: %d\n",num_free_pages); int l,k;
  paddr_t swappaddr = find_lru_page(&l, &k, 9999); 
  vaddr_t swapvaddr = RamCoreMap[l] & PAGE_MASK; 
  kprintf("LRU test: paddr: 0x%x -- vaddr: 0x%x\n",swappaddr,swapvaddr); 
  splx(spl); 

}

void examine_swapmap() {
  int spl = splhigh();
  int i;
  for (i = 0; i < SwapMapSize; i++) {
    if (SwapCoreMap[i] != 0) {
      int vpage = SwapCoreMap[i] & PAGE_MASK;
      int PID = SwapCoreMap[i] & PID_MASK;
      kprintf("SwapCoreMap[%d]: vpage: 0x%x -- PID: %d\n",
	      i,vpage,PID);
    }
				     
  }
}

void examine_raminfo() {

  int spl = splhigh(); 
  int i;
  for (i = 0; i < BitMapSize; i++) {
    kprintf("index %d: RamCoreInfo: 0x%x -- RamCoreInfo2: 0x%x -- RamCoreInfo3: 0x%x\n",
	    i,RamCoreInfo[i],RamCoreInfo2[i],RamCoreInfo3[i]);
  }
  splx(spl); 

}


//Precondition: you have the BitMapLock (and you will have the SWAP_LOCK as well)
void PT_invalidate(vaddr_t vpage, int PID) {

  int spl = splhigh();
  
  if (PROCESS_LIST != NULL) {
    as = PROCESS_LIST[PID];
    if (as == NULL) {
      PROCESS_LIST_dump();
      examine_bitmap1();
      examine_swapmap();
      assert(as!=NULL);
    }
  } else {
    kprintf("Does this happen?\n");
    while(1);
  }
  
  //kprintf("PT_invalidate for process %d -- vpage: 0x%x -- as: 0x%x\n",PID,vpage,as); 

  as->hasPageOnDisk = 1; 

  vpage = vpage & PAGE_MASK;
  for(i = npages_reserved; i < BitMapSize; i++) {
    int testVPAGE = RamCoreMap[i] & PAGE_MASK;
    int testPID = RamCoreMap[i] & PID_MASK;
    if (testVPAGE == vpage && testPID == PID) {
      RamCoreMap[i] = 0;
      RamBitMap[i] = 0;
      RamLruTime[i] = 0;
      num_free_pages++;
      //kprintf("PT_invalidate worked for: 0x%x -- PID:%d\n",vpage,PID);
      splx(spl);
      return;
    }
  }

  /*
  int offset; //note the PTLookup gives us an offset due to hashing if we put kill=1
  //int x = page_table_lookup(vpage, as, &offset, 1);
  if (x != 0) kprintf("Warning: tried deleting a PT entry that didn't exist.\n"); 

}

//flush dat TLB
void TLB_invalidate() {

   int s = splhigh();

  assert(page_table_lookup(vpage,as,&offset,0) == 0);
  */
  //kprintf("PT_invalidate didn't invalidate anything!\n");
  splx(spl);

}

void TLB_invalidate(vaddr_t vpage) {
   int s = splhigh();
   int x = TLB_Probe(vpage, 0);
   if (x >= 0) {
     kprintf("\n\n TLB INVALIDATE FOR VPAGE: 0x%x\n\n",vpage);
       TLB_Write(TLBHI_INVALID(x), TLBLO_INVALID(), x);
   }
   splx(s);
}

void TLB_dump() {

  int i; 
  u_int32_t test_ehi, test_elo; 
  for (i=0; i<NUM_TLB; i++) {
		TLB_Read(&test_ehi, &test_elo, i);
		if (test_elo & TLBLO_VALID) {
		  kprintf("TLB entry @[%d]: 0x%x||0x%x\n",i,test_ehi,test_elo);
		}
	
	}
  

}


int checksum(int* a, int size) {

  int spl = splhigh();
  int i;
  int cksum = 0;
  for(i = 0; i < size; i++) {
    cksum += a[i];
  }
  splx(spl);
  return cksum;
}

void PROCESS_LIST_dump() {
  int i;
  for(i=0; i<240; i++) {
    if (PROCESS_LIST[i] != 0) kprintf("PROCESS_LIST[%d]: 0x%x\n",i,PROCESS_LIST[i]);
  }

}
