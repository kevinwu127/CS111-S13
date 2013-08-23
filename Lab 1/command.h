// UCLA CS 111 Lab 1 command interface

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

typedef enum {
  WORD_TOKEN,
  SEMICOLON_TOKEN,
  PIPE_TOKEN,
  AND_TOKEN,
  OR_TOKEN,
  LEFT_PAREN_TOKEN,
  RIGHT_PAREN_TOKEN,
  GREATER_TOKEN,
  LESS_TOKEN,
  COMMENTS_TOKEN,
  NEWLINE_TOKEN,
  MISC_TOKEN,
  NULL_TOKEN,
} token_type;

typedef enum cstatus {
  NEW,
  WAITING,
  RUNNING,
  DONE,
} cstatus;

typedef struct command command;
typedef struct command *command_t;

struct command_stream;

//typedef struct command_stream command_stream;
typedef struct command_stream *command_stream_t;

typedef struct token *token_t;
typedef struct token_stream *token_stream_t;

typedef struct command_stream {
  int size;
  int iterator;
  int number;
  cstatus c_status;
  char **iplist;
  char **oplist;
  pid_t pid;
  int exit_status;
  command_stream_t *dep;
  struct command *m_command;
  struct command_stream *next; 
}command_stream;

int isWordChar(char input);
int streamPrec (token_type type);
int stackPrec (token_type type);
char* read_filestream(int (*get_next_byte) (void *), void *get_next_byte_argument);
token_stream_t tokenize (char* buffer);
void validate_tokens (token_stream_t tstream);
void display_tokens (token_stream_t tstream);
command_t post_to_command (token_stream_t post);

void tspush (token_stream_t item);
token_type tspeek ();
token_stream_t tspop ();
token_stream_t tsappend (token_stream_t post, token_stream_t item);

command_t new_command ();
void cpush (command_t item, int *top, size_t *);
command_t cpop ();
command_t cmerge (command_t cmd1, command_t cmd2, token_stream_t ts);
command_stream_t csappend (command_stream_t cstream, command_stream_t item);

void execute_and (command_t c);
void execute_or (command_t c);
void execute_sequence (command_t c);
void execute_io_command (command_t c);
void execute_pipe (command_t c);
void execute_gen (command_t c);
void execute_simple (command_t c);
void setup_io (command_t c);

command_t execute_time_travel (command_stream_t stream);
void cs_setup_io_list (command_stream_t item);
void cmd_build_iolist (command_t c, int *icnt, int *ocnt);
void cs_setup_dependancy (command_stream_t cstream, command_stream_t item);
bool cs_check_runnable (command_stream_t item);
command_stream_t get_next_cstream (command_stream_t cstream, command_stream_t from);

/* Create a command stream from GETBYTE and ARG.  A reader of
   the command stream will invoke GETBYTE (ARG) to get the next byte.
   GETBYTE will return the next input byte, or a negative number
   (setting errno) on failure.  */
command_stream_t make_command_stream (int (*getbyte) (void *), void *arg);

/* Read a command from STREAM; return it, or NULL on EOF.  If there is
   an error, report the error and exit instead of returning.  */
command_t read_command_stream (command_stream_t stream);

/* Print a command to stdout, for debugging.  */
void print_command (command_t);

/* Execute a command.  Use "time travel" if the flag is set.  */
void execute_command (command_t);

/* Return the exit status of a command, which must have previously
   been executed.  Wait for the command, if it is not already finished.  */
int command_status (command_t);

/* Insert a token*/
token_stream_t insert_token (token_stream_t,token_t);
