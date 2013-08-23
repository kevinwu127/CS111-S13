#include "schedos-app.h"
#include "x86sync.h"

/*****************************************************************************
 * schedos-1
 *
 *   This tiny application prints red "1"s to the console.
 *   It yields the CPU to the kernel after each "1" using the sys_yield()
 *   system call.  This lets the kernel (schedos-kern.c) pick another
 *   application to run, if it wants.
 *
 *   The other schedos-* processes simply #include this file after defining
 *   PRINTCHAR appropriately.
 *
 *****************************************************************************/

#ifndef PRINTCHAR
#define PRINTCHAR	('1' | 0x0C00)
#endif

//minilab2 code begins
#ifndef RANDSEED
#define RANDSEED	0xACE1u
#endif

unsigned short seed = RANDSEED;

int priority_rand() {
	unsigned randbit = ((seed >> 0) ^ (seed >> 2) ^ (seed >> 3) ^ (seed >> 5)) &  1;
	return (seed = (seed >> 1) | (randbit << 15));
}
//minilab2 code ends

void
start(void)
{
	int i = 0;

//minilab2 code begins
	int priority = 0;
	while (!(priority = priority_rand()%NPROCS)); //priority > 0
	sys_set_selfpriority(priority);
	sys_set_selfshare();

	int prt = PRINTCHAR;
//minilab2 code ends

	for (i = 0; i < RUNCOUNT; i++) {
		// Write characters to the console, yielding after each one.'
//minilab2 code begins
		//here is the code for solving exercise 6 - new system call added
		sys_write_char(prt);
		//below is the original code
		//*cursorpos++ = prt;
		//sys_yield();
//minilab2 code ends
	}

//minilab2 code begins
	sys_exit(0);

	// Yield forever.
	//while (1)
		//sys_yield();
//minilab2 code ends

}
