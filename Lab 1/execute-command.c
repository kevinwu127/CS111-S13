// UCLA CS 111 Lab 1 command execution

#include "command.h"
#include "command-internals.h"
#include "alloc.h"

#include <sys/types.h>
#include <sys/wait.h> 
#include <unistd.h>  
#include <stdlib.h>  
#include <stdio.h>
#include <string.h>
#include <error.h>
#include <errno.h>

#include <fcntl.h>
#include <sys/stat.h>

char **ip, **op;
size_t isize, osize, iosizedf = 1024*sizeof(char*); //default of 8

int
command_status (command_t c)
{
  return c->status;
}

void execute_gen(command_t c)
{
  switch(c->type)
    {
    case AND_COMMAND:
      execute_and(c);
      break;
    case OR_COMMAND:
      execute_or(c);
      break;
    case SEQUENCE_COMMAND:
      execute_sequence(c);
      break;
    case PIPE_COMMAND:
      execute_pipe(c);
      break;
    case SIMPLE_COMMAND:
    case SUBSHELL_COMMAND:
      execute_io_command(c);
      break;
    default:
      error(1, 0, "Invalid command type");
    }
}

void execute_and(command_t c)
{
  execute_gen(c->u.command[0]);
  if(c->u.command[0]->status == 0)
    {
      execute_gen(c->u.command[1]);
      c->status = c->u.command[1]->status;
    }
  else
    c->status = c->u.command[0]->status;
}

void execute_or(command_t c)
{
  execute_gen(c->u.command[0]);
  if(c->u.command[0]->status != 0)
    {
      execute_gen(c->u.command[1]);
      c->status = c->u.command[1]->status;
    }
  else
    c->status = c->u.command[0]->status;
}

void execute_sequence(command_t c)
{
  int status;
  pid_t pid = fork();
  if(pid > 0)
    {
      // Parent process
      waitpid(pid, &status, 0);
      c->status = status;
    }
  else if(pid == 0)
    {
      //Child process
      pid = fork();
      if( pid > 0)
	{
	  waitpid(pid, &status, 0);
	  execute_gen(c->u.command[1]);
	  _exit(c->u.command[1]->status);
	}
      else if( pid == 0)
	{
	  // The child of the child now runs
	  execute_gen(c->u.command[0]);
	  _exit(c->u.command[0]->status);
	}
      else
	error(1, 0, "Could not fork");
    }
  else
    error(1, 0, "Could not fork");
}

// If a command has particular input/output characteristics,
// this function will open and redirect the appropriate i/o locations
void setup_io(command_t c)
{
  // Check for an input characteristic, which can be read
  if(c->input != NULL)
    {
      int fd_in = open(c->input, O_RDWR);
      if( fd_in < 0)
	error(1, 0, "Unable to read input file: %s", c->input);

      if( dup2(fd_in, 0) < 0)
	error(1, 0, "Problem doing dup2 for input");

      if( close(fd_in) < 0)
	error(1, 0, "Problem closing input");
    }

  // Check for an output characteristic, which can be read and written on,
  // and if it doesn't exist yet, should be created
  if(c->output != NULL)
    {
      // Be sure to set flags
      int fd_out = open(c->output, O_CREAT | O_WRONLY | O_TRUNC,
			S_IRUSR | S_IRGRP | S_IWGRP | S_IWUSR);
      if( fd_out < 0)
	error(1, 0, "Problem reading output file: %s", c->output);

      if( dup2(fd_out, 1) < 0)
	error(1, 0, "Problem using dup2 for output");

      if( close(fd_out) < 0)
	error(1, 0, "Problem closing output file");
    }
}


void execute_simple(command_t c)
{
  int status;
  pid_t pid = fork();

  if(pid > 0)
    {
      // Parent waits for child, then stores status
      waitpid(pid, &status, 0);
      c->status = status;
    }
  else if(pid == 0)
    {
      // Set input-output characteristics
      setup_io(c);

      // In a semicolon simple command, exit as fast as possible
      if(c->u.word[0][0] == ':')
	_exit(0);

      // Execute the simple command program
      execvp(c->u.word[0], c->u.word );
      error(1, 0, "Invalid simple command");
    }
  else
    error(1, 0, "Could not fork");
}


void execute_io_command(command_t c)
{

  if(c->type == SIMPLE_COMMAND)
    {
      execute_simple(c);
    }
  else if(c->type == SUBSHELL_COMMAND)
    {
      setup_io(c);
      execute_gen(c->u.subshell_command);
    }
  else
    error(1,0, "Error with processing i/o command");
}

void
execute_pipe (command_t c)
{
  int status;
  int buf[2];
  pid_t return_pid;
  pid_t pid_1;
  pid_t pid_2;

  if ( pipe(buf) == -1 )
    error (1, errno, "cannot create pipe");
  pid_1 = fork();
  if( pid_1 > 0 )
    {
      // Parent process
      pid_2 = fork();
      if( pid_2 > 0 )
	{
	  //close parent ios
	  close(buf[0]);
	  close(buf[1]);
	  // Wait for any process to finish
	  return_pid = waitpid(-1, &status, 0);
	  if( return_pid == pid_1 )
	    {
	      c->status = status;
	      waitpid(pid_2, &status, 0);
	      return;
	    }
	  else if(return_pid == pid_2)
	    {
	      waitpid(pid_1, &status, 0);
	      c->status = status;
	      return;
	    }
	}
      else if( pid_2 == 0 )
	{
	  // The 2nd child now runs, first part of the pipe
	  close(buf[0]);
	  if( dup2(buf[1], 1) == -1 )
	    error (1, errno,  "dup2 error");
	  execute_gen(c->u.command[0]);
	  _exit(c->u.command[0]->status);
	}
      else
	error(1, 0, "Could not fork");
    }
  else if( pid_1 == 0)
    {
      // First child, command 2nd in the pipe
      close(buf[1]);
      if( dup2(buf[0], 0)== -1 )
        error (1, errno,  "dup2 error");
      execute_gen(c->u.command[1]);
      _exit(c->u.command[1]->status);
    }
  else
    error(1, 0, "Could not fork");
}

void
execute_command (command_t c)
{
  //  c->status = 1;
  //if (time_travel == true)
    //return;
  execute_gen(c);
}

command_t
execute_time_travel (command_stream_t cstream) {
puts("in function execute time travel");
  command_stream_t cs_next = NULL, cs_loop = NULL;
  pid_t pid1 = 0, pid2 = 0;
  int status = 0;
  command_t last_command = NULL;
  cs_next = get_next_cstream(cstream, cstream);
//printf("next is %d\n", cs_next->number);
  while (cs_next) {
    if (cs_next->c_status == NEW) {
      cs_setup_io_list(cs_next);
      cs_setup_dependancy(cstream, cs_next);
    }

    if (cs_check_runnable(cs_next)) {
      last_command = cs_next->m_command;
printf("fork by parent for command %d\n", cs_next->number);
      pid1 = fork();
      if (pid1 == 0) { //child
printf("in child for command %d\n", cs_next->number);
      	execute_gen(cs_next->m_command);
printf("exiting child process now for command %d\n", cs_next->number);
	_exit(cs_next->m_command->status);
      }
      else if (pid1 > 0) { //parent
printf("in parent after forking for command %d\n", cs_next->number);
	cs_next->pid = pid1;
	cs_next->c_status = RUNNING;
	while ((pid2 = waitpid(-1, &status, WNOHANG|WUNTRACED)) > 0) { //wait for any child which is done
	  cs_loop = cstream;
	  while (cs_loop) {
	    if (cs_loop->pid == pid2) {
printf("1 completed process for command %d\n", cs_loop->number);
	      cs_loop->c_status = DONE;
	      if (status)
	        cs_loop->exit_status = WEXITSTATUS(status);
	      break;
	    }
	    cs_loop = cs_loop->next;
	  }
        }
      }
      else
	error(1, 0, "Could not fork");
    }
    else 
      cs_next->c_status = WAITING;
    
    while ((pid2 = waitpid(-1, &status, WNOHANG|WUNTRACED)) > 0) { //wait for any child which is done
      cs_loop = cstream;
      while (cs_loop) {
        if (cs_loop->pid == pid2) {
printf("1 completed process for command %d\n", cs_loop->number);
          cs_loop->c_status = DONE;
          if (status)
            cs_loop->exit_status = WEXITSTATUS(status);
          break;
        }
        cs_loop = cs_loop->next;
      }
    }

    cs_next = get_next_cstream(cstream, cs_next->next);
  }

  while ((pid2 = waitpid(-1, &status, 0)) != -1) { //wait till all children are done
    cs_loop = cstream;
    while (cs_loop) {
      if (cs_loop->pid == pid2) {
printf("3 completed process for command %d\n", cs_loop->number);
        cs_loop->c_status = DONE;
        if (status)
          cs_loop->exit_status = WEXITSTATUS(status);
        break;
      }
      cs_loop = cs_loop->next;
    }
  }

puts("returning from execute time travel");
  return last_command;
}

void
cs_setup_io_list (command_stream_t item) {
//printf("in function setup io for %d - ", item->number);
  int icnt = 0, ocnt = 0;
  isize = osize = iosizedf;
  item->iplist = ip = (char **) checked_malloc (isize);
  item->oplist = op = (char **) checked_malloc (osize);
  *ip = NULL;
  *op = NULL;
  cmd_build_iolist (item->m_command, &icnt, &ocnt);
  *ip = NULL;
  *op = NULL;
//printf("%d i and %d o\n",icnt, ocnt);
}

void 
cmd_build_iolist (command_t c, int *icnt, int *ocnt) {
  char **loop;
  int len = 0;

  if (c->input) {
    *icnt += 1;
    if (isize <= ((*icnt)*sizeof(char*)))
      ip = (char **) checked_grow_alloc(ip, &isize);

    len = strlen(c->input) + 1;
    *ip = (char *) checked_malloc(len*sizeof(char));
    strcpy(*ip, c->input);
    *++ip = NULL;
  }
  
  if (c->output) {
    *ocnt += 1;
    if (osize <= ((*ocnt)*sizeof(char*)))
      op = (char **) checked_grow_alloc(op, &osize);
    len = strlen(c->output) + 1;
    *op = (char *) checked_malloc(len*sizeof(char));
    strcpy(*op, c->output);
    *++op = NULL;
  }

  switch (c->type) {
    case AND_COMMAND:
    case PIPE_COMMAND:
    case OR_COMMAND:
    case SEQUENCE_COMMAND:
	cmd_build_iolist (c->u.command[0], icnt, ocnt);	
	cmd_build_iolist (c->u.command[1], icnt, ocnt);	
	break;

    case SUBSHELL_COMMAND:
	cmd_build_iolist (c->u.subshell_command, icnt, ocnt);	
	break;

    case SIMPLE_COMMAND:
	loop = c->u.word;
	while(*++loop) {
	  *icnt += 1;
	  if (isize <= ((*icnt)*sizeof(char*)))
	    ip = (char **) checked_grow_alloc(ip, &isize);
    	  len = strlen(*loop) + 1;
          *ip = (char *) checked_malloc(len*sizeof(char));
          strcpy(*ip, *loop);
          *++ip = NULL;
	}
	break;
  }
}

void
cs_setup_dependancy (command_stream_t cstream, command_stream_t item) {
printf("in function setup dependancy for %d -", item->number);
  int cnt = 0, icnt, ocnt;
  command_stream **temp = NULL;
  if (cstream && item && item->iplist && *item->iplist) {
    char **otemp, **itemp;
    size_t size = 5*sizeof(command_stream *); //default 5
    bool flag = true;
    temp = item->dep = (command_stream **) checked_malloc(size);
    *item->dep = NULL;
    command_stream_t cs_loop = cstream;
    while (cs_loop && (cs_loop != item)) {
      otemp = cs_loop->oplist;
      ocnt = 0;
      while (*otemp && strlen(*otemp)) {
	itemp = item->iplist;
	icnt = 0;
	while (*itemp && strlen(*itemp)) {
	  if (!strcmp(*itemp, *otemp)) {
	    cnt += 1;
	    if (size <= (cnt*sizeof(command_stream *)))
	      item->dep = (command_stream **) checked_grow_alloc(item->dep, &size);
	    *temp = cs_loop;
	    *++temp = NULL;
	    flag = false;
	  }
	  *itemp = *(itemp + ++icnt);
	}
	if(!*itemp)
	  break;
	*otemp = *(otemp + ++ocnt);
      }
      cs_loop = cs_loop->next;
    }
    if (flag)
      item->dep = NULL;
  }
printf("%d -%d\n", cnt, item->number);
}

bool
cs_check_runnable (command_stream_t item) {
//printf("in function check runnable for %d - ", item->number);
  if (item->dep)
    while (*(item->dep)) {
      if ((*(item->dep++))->c_status != DONE)
	return false;
    }
  return true;
}
