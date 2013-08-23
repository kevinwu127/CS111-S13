#include "schedos-kern.h"
#include "x86.h"
#include "lib.h"

/*****************************************************************************
 * schedos-kern
 *
 *   This is the schedos's kernel.
 *   It sets up process descriptors for the 4 applications, then runs
 *   them in some schedule.
 *
 *****************************************************************************/

// The program loader loads 4 processes, starting at PROC1_START, allocating
// 1 MB to each process.
// Each process's stack grows down from the top of its memory space.
// (But note that SchedOS processes, like MiniprocOS processes, are not fully
// isolated: any process could modify any part of memory.)

//minilab2 code begins
//#define NPROCS		5
//minilab2 code ends

#define PROC1_START	0x200000
#define PROC_SIZE	0x100000

// +---------+-----------------------+--------+---------------------+---------/
// | Base    | Kernel         Kernel | Shared | App 0         App 0 | App 1
// | Memory  | Code + Data     Stack | Data   | Code + Data   Stack | Code ...
// +---------+-----------------------+--------+---------------------+---------/
// 0x0    0x100000               0x198000 0x200000              0x300000
//
// The program loader puts each application's starting instruction pointer
// at the very top of its stack.
//
// System-wide global variables shared among the kernel and the four
// applications are stored in memory from 0x198000 to 0x200000.  Currently
// there is just one variable there, 'cursorpos', which occupies the four
// bytes of memory 0x198000-0x198003.  You can add more variables by defining
// their addresses in schedos-symbols.ld; make sure they do not overlap!


// A process descriptor for each process.
// Note that proc_array[0] is never used.
// The first application process descriptor is proc_array[1].
static process_t proc_array[NPROCS];

// A pointer to the currently running process.
// This is kept up to date by the run() function, in mpos-x86.c.
process_t *current;

// The preferred scheduling algorithm.
int scheduling_algorithm;

//minilab2 code begins
#define LOTTERYSIZE RUNCOUNT*NPROCS
static pid_t lottery[LOTTERYSIZE] = {0};

void assign_lottery (int count, pid_t *array, pid_t pid) {
	int i;
	for (i = 0; i < count; ++i)
		array[i] = pid;
}

static unsigned short seed = 0xCDF1u;

int lottery_rand() {
	unsigned randbit = ((seed >> 0) ^ (seed >> 2) ^ (seed >> 3) ^ (seed >> 5)) &  1;
	return (seed = (seed >> 1) | (randbit << 15));
}
//minilab2 code ends

/*****************************************************************************
 * start
 *
 *   Initialize the hardware and process descriptors, then run
 *   the first process.
 *
 *****************************************************************************/

void
start(void)
{
	int i;

	// Set up hardware (schedos-x86.c)
	segments_init();
	interrupt_controller_init(1);
	console_clear();

	// Initialize process descriptors as empty
	memset(proc_array, 0, sizeof(proc_array));
	for (i = 0; i < NPROCS; i++) {
		proc_array[i].p_pid = i;
		proc_array[i].p_state = P_EMPTY;
	}

	// Set up process descriptors (the proc_array[])
	for (i = 1; i < NPROCS; i++) {
		process_t *proc = &proc_array[i];
		uint32_t stack_ptr = PROC1_START + i * PROC_SIZE;

		// Initialize the process descriptor
		special_registers_init(proc);

		// Set ESP
		proc->p_registers.reg_esp = stack_ptr;

		// Load process and set EIP, based on ELF image
		program_loader(i - 1, &proc->p_registers.reg_eip);	

		// Mark the process as runnable!
		proc->p_state = P_RUNNABLE;

//minilab2 code begins
		proc->p_priority = proc->p_share = proc->p_sharedone = 0;
		proc->p_lastrun = 0;

		//assign lottery tickets to processes
		int assigncount = LOTTERYSIZE / (NPROCS - 1);
		int arrayoffset = assigncount * (i - 1);
		assign_lottery (assigncount, lottery + arrayoffset, proc->p_pid);
//minilab2 code ends

	}

	// Initialize the cursor-position shared variable to point to the
	// console's first character (the upper left).
	cursorpos = (uint16_t *) 0xB8000;

	// Initialize the scheduling algorithm.
	scheduling_algorithm = 0;

//minilab2 code begins
	pid_t pid = 1; //highest priority
	++proc_array[pid].p_sharedone;
	run(&proc_array[pid]);
	// Switch to the first process
	//run(&proc_array[1]);
//minilab2 code ends

	// Should never get here!
	while (1)
		/* do nothing */;
}



/*****************************************************************************
 * interrupt
 *
 *   This is the weensy interrupt and system call handler.
 *   The current handler handles 4 different system calls (two of which
 *   do nothing), plus the clock interrupt.
 *
 *   Note that we will never receive clock interrupts while in the kernel.
 *
 *****************************************************************************/

void
interrupt(registers_t *reg)
{
	// Save the current process's register state
	// into its process descriptor
	current->p_registers = *reg;

	switch (reg->reg_intno) {

	case INT_SYS_YIELD:
		// The 'sys_yield' system call asks the kernel to schedule
		// the next process.
		schedule();

	case INT_SYS_EXIT:
		// 'sys_exit' exits the current process: it is marked as
		// non-runnable.
		// The application stored its exit status in the %eax register
		// before calling the system call.  The %eax register has
		// changed by now, but we can read the application's value
		// out of the 'reg' argument.
		// (This shows you how to transfer arguments to system calls!)
		current->p_state = P_ZOMBIE;
		current->p_exit_status = reg->reg_eax;
		schedule();

	case INT_SYS_USER1:
		// 'sys_user*' are provided for your convenience, in case you
		// want to add a system call.
		/* Your code here (if you want). */
//minilab2 code begins
		current->p_priority = reg->reg_eax;
		if (current->p_pid == (NPROCS - 1)) //last process to get priority, so schedule
			schedule();
		else {
			process_t *new = current + 1;
			run(new);
		}
//minilab2 code ends

	case INT_SYS_USER2:
		/* Your code here (if you want). */
//minilab2 code begins
		current->p_share = current->p_pid;
		run(current);
//minilab2 code ends

	case INT_CLOCK:
		// A clock interrupt occurred (so an application exhausted its
		// time quantum).
		// Switch to the next runnable process.
		schedule();

//minilab2 code begins
	case INT_SYS_USER3:
		*cursorpos++ = reg->reg_eax;
		schedule();
//minilab2 code ends

	default:
		while (1)
			/* do nothing */;

	}
}



/*****************************************************************************
 * schedule
 *
 *   This is the weensy process scheduler.
 *   It picks a runnable process, then context-switches to that process.
 *   If there are no runnable processes, it spins forever.
 *
 *   This function implements multiple scheduling algorithms, depending on
 *   the value of 'scheduling_algorithm'.  We've provided one; in the problem
 *   set you will provide at least one more.
 *
 *****************************************************************************/

void
schedule(void)
{
	pid_t pid = current->p_pid;

	if (scheduling_algorithm == 0) //Round Robin scheduling, starting from 1
		while (1) {
			pid = (pid + 1) % NPROCS;

			// Run the selected process, but skip
			// non-runnable processes.
			// Note that the 'run' function does not return.
			if (proc_array[pid].p_state == P_RUNNABLE)
				run(&proc_array[pid]);
		}

//minilab2 code begins
	else if (scheduling_algorithm == 1) { //Strict Priority scheduling, 1 is highest
		pid_t pid2 = 1; //highest priority
		while (1) {
			if (proc_array[pid2].p_state == P_RUNNABLE)
				run(&proc_array[pid2]);
			else
				pid2 = (pid2 + 1) % NPROCS;
		}
	}

	else if (scheduling_algorithm == 2) { //Similar to 1, priority set by self process
		pid_t cand_pid, last_pid = 0, pid2;
		int flag = 0;
		while (1) {
			//find first highest priority RUNNABLE process - cand_pid
			for (cand_pid = 1; cand_pid < NPROCS; ++cand_pid)
				if (proc_array[cand_pid].p_state == P_RUNNABLE)
					break;

			for (pid2 = cand_pid + 1; pid2 < NPROCS; ++pid2) 
				if ((proc_array[pid2].p_state == P_RUNNABLE) && proc_array[pid2].p_priority && (proc_array[pid2].p_priority < proc_array[cand_pid].p_priority))
					cand_pid = pid2;

			//search for same priority process with lastrun flag set
			if (proc_array[cand_pid].p_lastrun == 1)
				last_pid = cand_pid;
			else
				for (pid2 = (cand_pid + 1) % NPROCS; pid2 != cand_pid; pid2 = (pid2 + 1) % NPROCS)
					if ((proc_array[pid2].p_priority == proc_array[cand_pid].p_priority) && (proc_array[pid2].p_lastrun == 1)) {
						last_pid = pid2;
						break;
					}

			if (last_pid) { //find next process of same priority
				cand_pid = last_pid;
				for (pid2 = (last_pid + 1) % NPROCS; pid2 != last_pid; pid2 = (pid2 + 1) % NPROCS)
					if ((proc_array[pid2].p_state == P_RUNNABLE) && (proc_array[pid2].p_priority == proc_array[last_pid].p_priority)) {
						cand_pid = pid2;
						break;
					}
				proc_array[last_pid].p_lastrun = 0;
			}
			proc_array[cand_pid].p_lastrun = 1;
			run(&proc_array[cand_pid]);
		}
	}

	else if (scheduling_algorithm == 3) { //Proportional scheduling
		pid_t pid2;
		while (1) {
			if (proc_array[pid].p_state == P_RUNNABLE) {
				if (proc_array[pid].p_sharedone < proc_array[pid].p_share) {
					++proc_array[pid].p_sharedone;
					run(&proc_array[pid]);
				}
				else {
					proc_array[pid].p_sharedone = 0;
					for (pid2 = (pid + 1) % NPROCS; pid2 != pid; pid2 = (pid2 + 1) % NPROCS)
						if (proc_array[pid2].p_state == P_RUNNABLE) {
							++proc_array[pid2].p_sharedone;
							run(&proc_array[pid2]);
						}
				}
			}
			else
				for (pid2 = (pid + 1) % NPROCS; pid2 != pid; pid2 = (pid2 + 1) % NPROCS)
					if (proc_array[pid2].p_state == P_RUNNABLE) {
						++proc_array[pid2].p_sharedone;
						run(&proc_array[pid2]);
					}
		}
	}

	else if (scheduling_algorithm == 4) { //Lottery
		while (1) {
			int win = lottery_rand() % LOTTERYSIZE;
			pid_t pid2 = lottery[win];
			if (proc_array[pid2].p_state == P_RUNNABLE) { //unused ticket
				run(&proc_array[pid2]);
			}
		}
	}
//minilab2 code ends

	// If we get here, we are running an unknown scheduling algorithm.
	cursorpos = console_printf(cursorpos, 0x100, "\nUnknown scheduling algorithm %d\n", scheduling_algorithm);
	while (1)
		/* do nothing */;
}
