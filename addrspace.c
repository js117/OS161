#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <addrspace.h>
#include <vm.h>
#include <machine/tlb.h>
#include <machine/spl.h>
#include <curthread.h>
#include <thread.h>

#define PID_MASK     0x000000ff; 

struct addrspace *
as_create(int isCopy)
{

  int spl = splhigh();

  struct addrspace* as = alloc_kpages((sizeof(struct addrspace) + PAGE_SIZE) / PAGE_SIZE); 

  //kprintf("as_create: took this many frames: %d\n",
  //		(sizeof(struct addrspace) + PAGE_SIZE)/PAGE_SIZE);
	if (as==NULL) {
		return NULL;
	}

	as->as_vbase_text = 0;
	as->as_npages_text = 0;

	as->as_vbase_text2 = 0;
	as->as_npages_text2 = 0;
	
	as->as_vbase_heap = 0;
	as->as_vtop_heap = 0;
	
	as->as_stackpbase = 0;
	as->as_npages_stack = 0;

	if (isCopy == 0) {

	  as->PID = curthread->pid; 
	  //special case: as_copy resets it
	  if (PROCESS_LIST != NULL) {
	    assert(PROCESS_LIST[as->PID] == 0);
	    PROCESS_LIST[as->PID] = as;
	  }

	} else {
	  //as_copy will do it
	}
	as->hasPageOnDisk = 0; 
	as->hasLevel3Collisions = 0; 

	as->data_filesz = 0; 
	as->data_offset = 0; 

	int i;
	for(i = 0; i < 400; i++) as->checksumtest[i]=0;
	as->v = NULL; 

	splx(spl); 

	return as;
}


//something wrong with forking!
int
as_copy(struct addrspace *old, struct addrspace **ret, struct thread* newThread)
{

  lock_acquire(BitMapLock);
  //int spl = splhigh();

	struct addrspace *newas;
	
	newas = as_create(1); //is a copy!
	if (newas==NULL) {
	  kprintf("DAMN HOMIE YOU OUTTA MEMORY!\n"); 
		return ENOMEM;
	} 
	
	newas->PID = newThread->pid; 
	PROCESS_LIST[newas->PID] = newas; 

	newas->v = old->v; 
	
	newas->text_size = old->text_size;
	newas->data_size = old->data_size; 
	newas->data_filesz = old->data_filesz; 
	newas->data_offset = old->data_offset;

	int i; int pt; paddr_t p, o;

	//note - we need different vaddrs here
	newas->as_vbase_text = old->as_vbase_text;
	newas->as_npages_text = old->as_npages_text; 
	newas->as_vbase_text2 = old->as_vbase_text2;
	newas->as_npages_text2 = old->as_npages_text2;

	newas->as_npages_stack = old->as_npages_stack; 

	
	int num_pages_needed=old->as_npages_text+old->as_npages_text2+old->as_npages_stack+1;
	/*
	if (num_pages_needed > num_free_pages) {
	  kprintf("AS_COPY switching some frames out\n");
	  lock_acquire(SWAP_LOCK);
	  for(i = 0; i < num_pages_needed; i++) {
	    swap_handler(newas);
	  }
	  lock_release(SWAP_LOCK);
	  assert(num_free_pages >= num_pages_needed);
	}
	*/
	//spl = spl0();
	lock_acquire(SWAP_LOCK);
	while(num_free_pages < num_pages_needed) swap_handler(newas);
	
	//splx(spl); //back to high
	assert(num_free_pages >= num_pages_needed);
	

	//kprintf("TEST: newas->as_npages_stack: %d\n",newas->as_npages_stack); 
	//as_prepare_load(newas); //give it its physical frames 
	//kprintf("TEST: newas->as_npages_stack: %d\n",newas->as_npages_stack); 

	//now do a deep copy via memmove, page-by-page
	//memmove(dest,src,length)
	//OPTIMIZE: don't copy every damn page...(e.g. data pages with filesz < PAGE_SIZE)

	for (i = 0; i < newas->as_npages_text; i++) {	  
	  
	  if (!page_table_lookup(old->as_vbase_text+(i*PAGE_SIZE),old,&pt,0)) {
	    //uh oh, the page we want to copy isn't in RAM. Get it from the disk:
	    int offset = search_swap_disk(old->as_vbase_text+(i*PAGE_SIZE), old->PID);
	    if (offset >= 0) { 
	      p = get_upage(newas->as_vbase_text + i*PAGE_SIZE, newas); 
	      RamLruTime[get_bitmap_index(p)] = -1;
	      int ww = load_from_swap(offset, p, 1);
	      RamLruTime[get_bitmap_index(p)] = 0;
	    }//done
	    else {
	      //parent frame not in the page table, not on swap, so don't do anything
	    }
	  } else {
	    p = get_upage(newas->as_vbase_text + i*PAGE_SIZE, newas); 
	    RamLruTime[get_bitmap_index(p)] = -1;
	    memmove((void*)PADDR_TO_KVADDR(p),old->as_vbase_text+(i*PAGE_SIZE),PAGE_SIZE); 
	    RamLruTime[get_bitmap_index(p)] = 0;
	  }
	}
	

	
	for (i = 0; i < newas->as_npages_text2; i++) {
	  
	  if (!page_table_lookup(old->as_vbase_text2+(i*PAGE_SIZE),old,&pt,0)) {
	    //uh oh, the page we want to copy isn't in RAM. Get it from the disk:
	    int offset = search_swap_disk(old->as_vbase_text2+(i*PAGE_SIZE), old->PID);
	    if (offset >= 0) { 	      
	      p = get_upage(newas->as_vbase_text2 + i*PAGE_SIZE, newas);
	      RamLruTime[get_bitmap_index(p)] = -1;
	      int ww = load_from_swap(offset, p, 1); //done	   
	      RamLruTime[get_bitmap_index(p)] = 0;
	    }
	    else {
	      //parent frame not in table or swap, don't copy anything
	    }
	  } else {
	    p = get_upage(newas->as_vbase_text2 + i*PAGE_SIZE, newas);
	    RamLruTime[get_bitmap_index(p)] = -1;
	    memmove((void*)PADDR_TO_KVADDR(p),old->as_vbase_text2+(i*PAGE_SIZE),PAGE_SIZE); 
	    RamLruTime[get_bitmap_index(p)] = 0;
	  }
	}
	

	//kprintf("TEST: newas->as_npages_stack: %d\n",newas->as_npages_stack); 
	for (i = newas->as_npages_stack; i > 0; i--) {
	 
	  if (!page_table_lookup(USERSTACK-(i*PAGE_SIZE),old,&pt,0)) {
	    //uh oh, the page we want to copy isn't in RAM. Get it from the disk:
	    int offset = search_swap_disk(USERSTACK-(i*PAGE_SIZE), old->PID);
	    if (offset >= 0) { 
	      p = get_upage(USERSTACK-(i*PAGE_SIZE),newas);
	      RamLruTime[get_bitmap_index(p)] = -1;
	      int ww = load_from_swap(offset, p, 1); //done
	      RamLruTime[get_bitmap_index(p)] = 0;
	    }
	    else {
	      //this probably shouldn't happen. 
	      kprintf("Lost a parent's stack page...?\n");
	    }
	  } else {
	    p = get_upage(USERSTACK-(i*PAGE_SIZE),newas);
	    RamLruTime[get_bitmap_index(p)] = -1;
	    memmove((void*)PADDR_TO_KVADDR(p),USERSTACK-(i*PAGE_SIZE),PAGE_SIZE);
	    RamLruTime[get_bitmap_index(p)] = 0;
	  }
	}
	
	
	*ret = newas;
	
	lock_release(BitMapLock);
	
	//as_activate(newas);

	//as_activate(newas);

	//kprintf("AS_COPY DONE! newchild: %d -- parent: %d\n",newas->PID,old->PID);
	
	//examine_bitmap1();
	//examine_swapmap();
	//while(1) {}
	
	//splx(spl); 

	lock_release(SWAP_LOCK);
	lock_release(BitMapLock);
	//splx(spl); 

	return 0;
}


void
as_destroy(struct addrspace *as)
{

  //int spl = splhigh();
  //kprintf("\n\n === AS_DESTROY GETS CALLED === PID: %d\n",as->PID);
  //kprintf("curthread: 0x%x  pid: %d  addrspace: 0x%x\n",curthread,curthread->pid,as); 
  lock_acquire(BitMapLock);
  int i;
  for (i = 0; i < BitMapSize; i++) {

    int testPID = RamCoreMap[i] & PID_MASK;
    if (testPID == as->PID && RamBitMap[i]!=7 && RamCoreMap[i] < MIPS_KSEG0) 
      { RamCoreMap[i] = 0; RamBitMap[i] = 0; num_free_pages++; }

    
  }
  lock_release(BitMapLock);

  lock_acquire(SWAP_LOCK);
  for(i = 0; i < SwapMapSize; i++) {
    int test = SwapCoreMap[i] & PID_MASK;
    if (test == as->PID) SwapCoreMap[i] = 0; 
  }
  lock_release(SWAP_LOCK);
  free_kpages((void*)as);

  //splx(spl); 
 
}

//NOTE - this function is called when we want the processor to see this 
//addrspace. 
void
as_activate(struct addrspace *as)
{
  
  int i, spl;
  spl = splhigh();

    u_int32_t test_ehi, test_elo; int PTentry; paddr_t p; 
    //Loop through the TLB, invalidate any entry in new as's page_table 
    //so as to avoid duplicates. 
    for (i=0; i<NUM_TLB; i++) {
        TLB_Read(&test_ehi, &test_elo, i);
	TLB_Write(TLBHI_INVALID(i), TLBLO_INVALID(), i);
    	
      }

    	splx(spl);
	return 0; 
  
}

/*
 * Set up a segment at virtual address VADDR of size MEMSIZE. The
 * segment in memory extends from VADDR up to (but not including)
 * VADDR+MEMSIZE.
 *
 * The READABLE, WRITEABLE, and EXECUTABLE flags are set if read,
 * write, or execute permission should be set on the segment. At the
 * moment, these are ignored. When you write the VM system, you may
 * want to implement them.
 */
int
as_define_region(struct addrspace *as, vaddr_t vaddr, size_t sz,
		 int readable, int writeable, int executable, int textHeap, struct vnode* v)
{ //textHeap: = 0 for text section, = 1 for heap. Should help redefine heap later
	size_t npages; 
	
	size_t copy_sz = sz;
	
	//I don't get this but I trust it for now
	copy_sz += vaddr & ~(vaddr_t)PAGE_FRAME;
	vaddr &= PAGE_FRAME;
	copy_sz = (copy_sz + PAGE_SIZE - 1) & PAGE_FRAME;
	npages = copy_sz / PAGE_SIZE;
	

	/* We don't use these - all pages are read-write */
	(void)readable;
	(void)writeable;
	(void)executable;

	//npages = sz/PAGE_SIZE + 1; 

	//text
	if (textHeap == 0 && as->as_npages_text == 0) {
	  as->v = v;
		as->as_vbase_text = vaddr;
		as->as_npages_text = npages;
		as->text_size = sz;
		//kprintf("as_define_region gave text memsz as: %d\n",sz);
		return 0;
	} 

	//data
	if (textHeap == 0 && as->as_npages_text2 == 0) {
		as->as_vbase_text2 = vaddr;
		as->as_npages_text2 = npages;
		as->data_size = sz;
		//kprintf("as_define_region gave data memsz as: %d\n",sz);
		//also, let's figure out where the heap should start
		as->as_vbase_heap = vaddr + PAGE_SIZE*(npages+1);
		as->as_vtop_heap = as->as_vbase_heap; //starts empty
		return 0;
	} 


	/*
	 * Support for more than two regions is not available.
	 */
	kprintf("dumbvm: Warning: too many regions\n");
	return EUNIMP;

}


//give physical frames for all parts of the process
int
as_prepare_load (struct addrspace *as)
{
  int spl = splhigh();

  kprintf("AS_PREPARE_LOAD: 0x%x\n",as);

  int i;
  paddr_t p;

  
  for (i = 0; i < as->as_npages_text; i++) {
    p = get_upage(as->as_vbase_text + i*PAGE_SIZE, as); 
  }
  

  
  for (i = 0; i < as->as_npages_text2; i++) {
    p = get_upage(as->as_vbase_text2 + i*PAGE_SIZE, as); 
  }
  
  

  for (i = as->as_npages_stack; i > 0; i--) {
    p = get_upage(USERTOP - i*PAGE_SIZE, as); 
    //kprintf("GOT STACKPAGE: 0x%x\n",USERTOP-i*PAGE_SIZE);
  }
  
  splx(spl); 
  //kprintf("AS_PREPARE_LOAD FINISHED!\n"); 
	return 0;
}

//called right after loadelf loads the segment(s) 
//the plan: grab some physical frames, put the program in there
int
as_complete_load(struct addrspace *as)
{

  kprintf("\n\n AS_COMPLETE_LOAD NEVER GET CALLED...RIGHT? \n\n"); 
  /*
  paddr_t p;
  p = get_upage(as->as_vbase_text, as);
  //kprintf("AS_COMPLETE_LOAD (text) got paddr: 0x%x\n",p);

  p = get_upage(as->as_vbase_text2, as);
  //kprintf("AS_COMPLETE_LOAD (data) got paddr: 0x%x\n",p);
  */
	return 0;
}

//Should be fine as-is for now. Later on we may want to create another similar function to
//Modify the stack and grow/shrink it as needed, page-by-page. 
int
as_define_stack(struct addrspace *as, vaddr_t *stackptr)
{

	(void)as;

	/* Initial user-level stack pointer */
	*stackptr = USERSTACK;
	
	
	return 0;
}

