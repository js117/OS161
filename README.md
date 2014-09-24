This repository contains select code from the OS161 Operating Systems project. 

The goal of this project is to implement features of a modern Linux-like kernel
(system calls, synchronization primitives, virtual memory, etc). It is run on 
a hardware emulator representing a MIPS uniprocessor.
For more info, see http://www.eecs.harvard.edu/syrah/os161/ 

Of particular relevance is virtual_memory.txt, which describes my VM system 
implementing an inverted page table as a way to handle high-thread loads in a 
RAM-constrained environment. Code is in vm.c. 