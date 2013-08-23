// UCLA CS 111 Lab 1 command reading

#include "command.h"
#include "command-internals.h"
#include "alloc.h"
#include <error.h>
#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/*typedef struct command_stream {
  int size;
  int iterator;
  cstatus status;
  char **iplist;
  char **oplist;
  command_stream **dep;
  struct command *m_command;
  struct command_stream* next;
} command_stream;*/

typedef struct token {
  token_type type;
  char* words;
  int linNum;
} token;

typedef struct token_stream {
 token m_token;
 struct token_stream* next;
 struct token_stream* prev;
} token_stream;

token_stream_t tstack = NULL;
command_t *cstack = NULL;

int isWordChar (char input){
  if (isalnum(input))
    return 1;
  
  switch (input) {
    case '!':
    case '%':
    case '+':
    case ',':
    case '-':
    case '.':
    case '/':
    case ':':
    case '@':
    case '^':
    case '_':
	     return 1;
    default: 
	     return 0;
  }
}

char*
read_filestream (int (*get_next_byte) (void *), void *get_next_byte_argument) {
  size_t bufferSize = 1024; // start with default buffer size of 1024
  size_t bufferIterator = 0;
  char value;
  char* buffer = (char*) checked_malloc(bufferSize);

  while ((value = get_next_byte(get_next_byte_argument)) != EOF) { //store into value while it isn't EOF
    buffer[bufferIterator++] = value;
    //    printf("looped once: %c \n", value);
    //    printf("bufferSize: %d \n", (int) bufferSize);
    if (bufferSize == bufferIterator) {
      buffer =  (char*) checked_grow_alloc(buffer, &bufferSize);
      //printf("called checked_realloc: %d\n", (int) bufferSize);
    }
  }
  
  if (bufferSize == bufferIterator) {
    buffer =  (char*) checked_grow_alloc(buffer, &bufferSize);
    //printf("called checked_realloc: %d\n", (int) bufferSize);
  }
  buffer[bufferIterator] = '\0';

  //printf("bufferIterator: %d \n", (int) bufferIterator);
  //puts(buffer);  //will output to stdout
  return buffer;
}

token_stream_t
tokenize (char *buffer) {
  int bufferIteratorT = 0;
  int linNumber = 1;
  token_type type;
  token_stream_t tstart = NULL;
  token_stream_t tcur = tstart;
  
  //do a dual parsing here where we handle two characters at the same time. This makes the && and || and comments easy
  //Need to check if the buffer is completely empty (has '\0' as the only character).  Then we don't do anything so we can just return

  if (buffer[bufferIteratorT] == '\0')
    return NULL;
  
  int final = 0; 
  char first;
  char second;
  while (buffer[bufferIteratorT] != '\0') {
    first = buffer[bufferIteratorT];
    if (buffer[bufferIteratorT+1] == '\0')
      final = 1;
    if (!final)
      second = buffer[bufferIteratorT+1];

    switch (first) { //identify the token
      case '&': //Check for &&
		if (!final && second == '&') { 
     	    	  type = AND_TOKEN;
	    	  bufferIteratorT++;//deals with the fact that && is two chars
                }
        	else {
	    	  type = MISC_TOKEN;  // invalid input, only 1 &
      		}
		break;
    
      case '|': //Check for ||
		if (!final && second == '|') {
	    	  type = OR_TOKEN;
	    	  bufferIteratorT++;
          	}
		else {
	    	  type = PIPE_TOKEN;	    
	  	}
		break;
      
      case '#': 
	 	if (bufferIteratorT != 0 && isWordChar(buffer[bufferIteratorT-1])) { 
	    	  fprintf(stderr, "At line %i: # cannot be preceeded by an ordinary token\n", linNumber);
		  exit(1);
		}
		int cSkip = 1;
		while ((buffer[bufferIteratorT+cSkip] != '\n') && (buffer[bufferIteratorT+cSkip] != '\0')) 
		  cSkip++;
		bufferIteratorT	+= cSkip;
		continue;

      case ';':
		type = SEMICOLON_TOKEN;
	        break;
      
      case '(': 
		type = LEFT_PAREN_TOKEN;
      		break;
      
      case ')':
		type = RIGHT_PAREN_TOKEN;
      		break;
      
      case '<':
		type = LESS_TOKEN;
		break;
      
      case '>':
		type = GREATER_TOKEN;
	      	break;
      
      case '\n':
		type = NEWLINE_TOKEN;
		int skip = 0;
		while (buffer[bufferIteratorT+skip] == '\n') {
		  skip++;
		  linNumber++;
		}
	   	bufferIteratorT	+= skip-1;
		break;

      case ' ':
      case '\t':
		bufferIteratorT++;
		continue;

      default: //unknown character for now
		type = MISC_TOKEN;
    }
    
    // figure out how long the word length is and then use token adding part to figure out how far to add the words.  Also, make sure you move ahead in the outer loop.  This will also overwrite MISC_TOKEN if word is found
    int wordlength = 1;
    int placeholder = bufferIteratorT;
    if (isWordChar(first)) {
      type = WORD_TOKEN;
      while (isWordChar(buffer[bufferIteratorT+wordlength]))
        wordlength++;
      bufferIteratorT += wordlength-1;
      //printf("placeholder: %d + \n" , placeholderplaceholder);
      //printf("end: %d \n", bufferIteratorT);
    }

    //token insertion here
    token_stream_t ts_temp = (token_stream*) checked_malloc(sizeof(token_stream));
    ts_temp->next = ts_temp->prev = NULL;
    (ts_temp->m_token).type = type;
    (ts_temp->m_token).linNum = linNumber;

    if (type == WORD_TOKEN) {
      (ts_temp->m_token).words = (char*) checked_malloc ((sizeof(char)*wordlength)+1);
      int i = 0;
      for (; i != wordlength;i++)
        (ts_temp->m_token).words[i] = buffer[placeholder+i]; 
      (ts_temp->m_token).words[i] = '\0';
    }
    else if (type == MISC_TOKEN) {
      fprintf(stderr, "At line %i: Unidentified token %c\n", linNumber, first);
      exit(1);
    }
    else
      (ts_temp->m_token).words = NULL;	

    //now insert into token stream
    if (tcur) {// already few tokens exist, append to end
      tcur->next = ts_temp;
      ts_temp->prev = tcur;      
      tcur = tcur->next;
    }
    else { //NULL, first token
      tstart = ts_temp;
      tcur = tstart;
    }

    bufferIteratorT++;
  }
  return tstart;
}

void
display_tokens (token_stream_t tstream) {
  puts("Legend:\n \tWORD_TOKEN: 0\n \tSEMICOLON_TOKEN: 1\n \tPIPE_TOKEN: 2\n \tAND_TOKEN: 3\n \tOR_TOKEN: 4\n \tLEFT_PAREN_TOKEN: 5\n \tRIGHT_PAREN_TOKEN: 6\n \tGREATER_TOKEN: 7\n \tLESS_TOKEN: 8 \n \tCOMMENTS_TOKEN: 9\n \tNEWLINE_TOKEN: 10\n \tMISC_TOKEN: 11\n");
 
  token_stream_t ts_loop = tstream;
  int counter = 0; 
  printf("List of parsed tokens (Comments ignored)\n");
  while (ts_loop) {
    printf("%3d - %d:%i", counter++, ts_loop->m_token.type, ts_loop->m_token.linNum);
    if (ts_loop->m_token.type == WORD_TOKEN)
      printf(" %s", ts_loop->m_token.words);
    printf("\n");
    ts_loop = ts_loop->next;
  }
}

void
validate_tokens (token_stream_t tstream) {
  int counter = 0, paren_open = 0;
  token_stream_t ts_loop = tstream;
  token_stream_t ts_prev = tstream;

  while (ts_loop) {
counter++;

    token nxtToken, prvToken;
    FILE *file;
    if (ts_loop->next) //next exists
      nxtToken = (ts_loop->next)->m_token;
      prvToken = (ts_prev)->m_token;

    switch ((ts_loop->m_token).type) {
      case LESS_TOKEN: //Redirect always followed by a word (existing file if input)
      case GREATER_TOKEN:     
	if ((ts_loop->next == NULL) || (nxtToken.type != WORD_TOKEN)) { // order or comparison not to change - if NULL, second is not executed, so no error
	  fprintf(stderr, "At line %i: Redirect needs a file word following it\n", (ts_loop->m_token).linNum);
	  exit(1);
	}
	if ((ts_loop->m_token).type == LESS_TOKEN) {
	  if ((file = fopen(nxtToken.words, "r")))
       	    fclose(file);
	  else { 
	    printf("At line %i %d : Input redirect needs an existing filename following it, file %s not found.\n", (ts_loop->m_token).linNum, counter, nxtToken.words);
	    ts_loop = ts_loop->next;
 	  }
	}
	break;
   
      case NEWLINE_TOKEN: //Newlines have some specific places they can appear in.
	// can be before (, ) or filenames of simple commands
	if (ts_loop->next) { //token exist
	  if ((nxtToken.type == LEFT_PAREN_TOKEN) || (nxtToken.type == RIGHT_PAREN_TOKEN) || (nxtToken.type == WORD_TOKEN)) {
	    if ((prvToken.type == LESS_TOKEN) || (prvToken.type == GREATER_TOKEN)) {
	      fprintf(stderr, "At line %i: Newline can follow ;,|,&&,||,(,) or simple commands only.\n", (ts_loop->m_token).linNum);
	      exit(1);
	    }
            else if ((prvToken.type == SEMICOLON_TOKEN) || (prvToken.type == AND_TOKEN) || (prvToken.type == PIPE_TOKEN) || (prvToken.type == OR_TOKEN)) { //skip this newline
	      ts_prev->next = ts_loop->next;
  	      ts_loop = ts_loop->next;
	      continue;
	    }
	    else if (paren_open && (prvToken.type != LEFT_PAREN_TOKEN)) //with ( i.e. subshell, so change to ; 
	      ts_loop->m_token.type = SEMICOLON_TOKEN;
	  }
	  else if (nxtToken.type == NEWLINE_TOKEN) {
	    ts_prev->next = ts_loop->next;
  	    ts_loop = ts_loop->next;
	    continue;
	  }
	  else {  
	    fprintf(stderr, "At line %i: Newline can be followed by (,), or Simple commands (filenames) only.\n", (ts_loop->m_token).linNum);
	    exit(1);
	  }
	}
	else
	  ts_prev->next = NULL;
        if ((prvToken.type == LESS_TOKEN) || (prvToken.type == GREATER_TOKEN)) {
	  fprintf(stderr, "At line %i: Newline can follow ;,|,&&,||,(,), or Simple commands only.\n", (ts_loop->m_token).linNum);
	  exit(1);
	}
	break;

      case SEMICOLON_TOKEN: //Semi Colons have some specific places they can appear in.
	//cannot be the first token or follow itself
	if (ts_loop == tstream) {
	  fprintf(stderr, "At line %i: Semicolon cannot be in the beginning\n", (ts_loop->m_token).linNum);
	  exit(1);
	}
	if (ts_loop->next) {
	  if (nxtToken.type == SEMICOLON_TOKEN) {
	    fprintf(stderr, "At line %i: Semicolon cannot be followed by itself\n", (ts_loop->m_token).linNum);
	    exit(1);
	  }
	  if (!paren_open)
	    ts_loop->m_token.type = NEWLINE_TOKEN;
	}
	else
	  ts_prev->next = NULL;
	break;

      case LEFT_PAREN_TOKEN:
	++paren_open;
	break;

      case RIGHT_PAREN_TOKEN:
	if (paren_open)
	  --paren_open;
	break;

      default:
	break;
    }
    ts_prev = ts_loop;
    ts_loop = ts_loop->next;
  }
}

int
streamPrec (token_type type) {
  switch (type) {
    case SEMICOLON_TOKEN:
    case NEWLINE_TOKEN:
	return 1;
    case AND_TOKEN:
    case OR_TOKEN:
	return 3;
    case PIPE_TOKEN:
	return 5;
    case GREATER_TOKEN:
    case LESS_TOKEN:
	return 7;
    case LEFT_PAREN_TOKEN:
	return 9;
    default:
	return -1;
  }
}

int
stackPrec (token_type type) {
  switch (type) {
    case SEMICOLON_TOKEN:
    case NEWLINE_TOKEN:
	return 2;	
    case AND_TOKEN:
    case OR_TOKEN:
	return 4;
    case PIPE_TOKEN:
	return 6;
    case GREATER_TOKEN:
    case LESS_TOKEN:
	return 8;
    case LEFT_PAREN_TOKEN:
	return 0;
    default:
	return -1;
  }
}

command_stream_t
token_to_command_stream (token_stream *tstream) {
  token_stream_t ts_cur, ts_next;
  command_t ct_temp, ct_temp2, cmd1, cmd2;
  command_stream_t cs_temp, cstream;

  ts_cur = tstream;
  ct_temp = ct_temp2 = cmd1 = cmd2 = NULL; 
  cs_temp = cstream = NULL;

  char **sca = NULL;
  int paren_open = 0;

  int top = -1;
  size_t ctstacksize = 8*sizeof(command_t); //initial size for stack used for building command stream
  cstack = (command_t*) checked_malloc(ctstacksize);

  while (ts_cur) {
    ts_next = ts_cur->next;
    
    switch ((ts_cur->m_token).type) {
      case LEFT_PAREN_TOKEN:
		cpush (ct_temp, &top, &ctstacksize);
		ct_temp = NULL;
		sca = NULL;
		++paren_open;
		tspush(ts_cur);
		break;
	
      case RIGHT_PAREN_TOKEN:
		if (!paren_open) {
		  fprintf(stderr, "Stray ')' in script\n");
		  exit(1);		
  		}
		cpush (ct_temp, &top, &ctstacksize);
		--paren_open;
    		while (tspeek() != LEFT_PAREN_TOKEN) {
		  if (tspeek() == NULL_TOKEN) {
		    fprintf(stderr, "Shell command syntax error, unmatched ')'\n");
		    exit(1);
		  }
		  cmd2 = cpop(&top);
		  cmd1 = cpop(&top);
		  ct_temp = cmerge(cmd1, cmd2, tspop());
		  cpush (ct_temp, &top, &ctstacksize);
		  ct_temp = NULL;
	        }
    		ct_temp2 = new_command();
		ct_temp2->type = SUBSHELL_COMMAND;
		ct_temp2->u.subshell_command = cpop(&top); //ct_temp;
		cpush (ct_temp2, &top, &ctstacksize);
		ct_temp = ct_temp2 = NULL;
		sca = NULL;
		tspop();
		break;

      case WORD_TOKEN:
		if (!ct_temp) {
		  ct_temp = new_command();
		  sca = (char **) checked_malloc(150*sizeof(char*));
		  ct_temp->u.word = sca;
		}
		*sca = (ts_cur->m_token).words;
		*++sca = NULL;
		break;

      case GREATER_TOKEN:
		if (!ct_temp) {
		  ct_temp = cpop(&top);
		  if (!ct_temp) {
		    fprintf(stderr,"Syntax incorrect for OP REDIR command\n");
	    	    exit(1);
		  }
		}
		if (ts_next && (ts_next->m_token).type == WORD_TOKEN) {
		  ct_temp->output = (ts_next->m_token).words;
		cpush (ct_temp, &top, &ctstacksize);
		ct_temp = NULL;
		sca = NULL;
		ts_cur = ts_next;
		if (ts_cur)
		  ts_next = ts_cur->next;
		}
		break;

      case LESS_TOKEN:
		if (!ct_temp) {
		  ct_temp = cpop(&top);
		  if (!ct_temp) {
		    fprintf(stderr,"Syntax incorrect for IP REDIR command\n");
	    	    exit(1);
		  }
		}
		if (ts_next && (ts_next->m_token).type == WORD_TOKEN) {
		  ct_temp->input = (ts_next->m_token).words;
		cpush (ct_temp, &top, &ctstacksize);
		ct_temp = NULL;
		sca = NULL;
		ts_cur = ts_next;
		if (ts_cur)
		  ts_next = ts_cur->next;
		}
		break;	     

      case PIPE_TOKEN:
      case AND_TOKEN:
      case OR_TOKEN:
      case SEMICOLON_TOKEN:
		cpush (ct_temp, &top, &ctstacksize);
		while (stackPrec(tspeek()) > streamPrec((ts_cur->m_token).type)) {
		  cmd2 = cpop(&top);
		  cmd1 = cpop(&top);
		  ct_temp = cmerge(cmd1, cmd2, tspop());
		  cpush (ct_temp, &top, &ctstacksize);
		}
		ct_temp = NULL;
		sca = NULL;
		tspush(ts_cur);
		break;

      case NEWLINE_TOKEN:
		cpush (ct_temp, &top, &ctstacksize);
		while (stackPrec(tspeek()) > streamPrec((ts_cur->m_token).type)) {
		  cmd2 = cpop(&top);
		  cmd1 = cpop(&top);
		  ct_temp = cmerge(cmd1, cmd2, tspop());
		  cpush (ct_temp, &top, &ctstacksize);
		}
		if (!paren_open) {
		  cs_temp = (command_stream_t) checked_malloc(sizeof(command_stream));
		  cs_temp->m_command = cpop(&top);
		  cs_temp->c_status = NEW;
		  cstream = csappend(cstream, cs_temp);
		}
		ct_temp = NULL;
		sca = NULL;
	  	break;

      default:
	break;
    }
    ts_cur = ts_next;
  }
  cpush (ct_temp, &top, &ctstacksize);
  while (tspeek() != NULL_TOKEN) {
    cmd2 = cpop(&top);
    cmd1 = cpop(&top);
    ct_temp = cmerge(cmd1, cmd2, tspop());
    cpush (ct_temp, &top, &ctstacksize);
  }
  cs_temp = (command_stream_t) checked_malloc(sizeof(command_stream));
  cs_temp->m_command = cpop(&top);

  cstream = csappend(cstream, cs_temp);
  ct_temp = NULL;
  if (cpop(&top)) {
   fprintf(stderr,"Syntax error\n");
   exit(1);
  }
  return cstream;
}

void
tspush (token_stream_t item) {
  //printf("tspushed %d\n", item->m_token.type);
  if (!item)
    return;
  if (!tstack) { //empty
    tstack = item;
    tstack->prev = tstack->next = NULL;
  }
  else {
    item->next = tstack;
    item->prev = NULL;
    tstack->prev = item;
    tstack = item;
  }
}

token_type 
tspeek () {
  return (tstack ? (tstack->m_token).type : NULL_TOKEN);
}

token_stream_t 
tspop () {
  token_stream_t ts_top = NULL;
  if (tstack) { //not empty
    ts_top = tstack;
    tstack = tstack->next;
    ts_top->next = ts_top->prev = NULL;
    if(tstack)
      tstack->prev = NULL;
  }
  //printf("tspopped %d\n", ts_top->m_token.type);
  return ts_top;
}

token_stream_t
tsappend (token_stream_t post, token_stream_t item) {
  if (!item)
    return post;
  if (!post) { //empty
    post = item;
    item->next = item->prev = NULL;
  }
  else {
    token_stream_t ts_loop = post;
    while (ts_loop->next) 
      ts_loop = ts_loop->next;
    ts_loop->next = item;
    item->prev = ts_loop;
    item->next = NULL;
  }
  //printf("tsappended %d\n", item->m_token.type);
  return post;
}

command_t
new_command () {
  command_t ret = (command_t) checked_malloc(sizeof(command));
  ret->type = SIMPLE_COMMAND; //default
  ret->input = ret->output = NULL;
  ret->u.command[0] = ret->u.command[1] = NULL;
  ret->u.word = NULL;
  ret->u.subshell_command = NULL;
  return ret;
}

void
cpush (command_t item, int *top, size_t *size) {
  if (!item)
    return;
  if (*size == ((*top+1)*sizeof(command_t)))
    cstack = (command_t*) checked_grow_alloc(cstack, size);
  cstack[++*top] = item;
  //printf("cpushed type - %d, top %d\n", item->type, *top);
}

command_t 
cpop (int *top) {
  command_t ret = NULL;
  if (*top >= 0) { //not empty
    ret = cstack[*top];
    if (*top)
	--*top;
    else
	*top = -1;
    //printf("cpoped type - %d, top %d\n", ret->type, *top);
  }
  return ret;
}

command_t
cmerge (command_t cmd1, command_t cmd2, token_stream_t ts) {
  if (!ts) {
    fprintf(stderr, "Syntax error - command type unidentified\n");
    exit(1);
  }
  command_t ret = NULL;
  if (!(cmd1 && cmd2)) {
    char *ctype = NULL;
    if (ts->m_token.type == SEMICOLON_TOKEN)
      ctype = ";";
    else if (ts->m_token.type == AND_TOKEN)
      ctype = "&&";
    if (ts->m_token.type == PIPE_TOKEN)
      ctype = "|";
    if (ts->m_token.type == OR_TOKEN)
      ctype = "||";
    else
      fprintf(stderr, "Syntax error - command type incorrect\n");
    
    if (cmd1)
      fprintf(stderr, "Syntax error - RHS of %s command missing\n",ctype);
    else
      fprintf(stderr, "Syntax error - LHS of %s command missing\n",ctype);
    exit(1);
  } 

  ret = new_command();
  ret->u.command[0] = cmd1;
  ret->u.command[1] = cmd2;

  if (ts->m_token.type == SEMICOLON_TOKEN)
    ret->type = SEQUENCE_COMMAND;
  else if (ts->m_token.type == AND_TOKEN)
    ret->type = AND_COMMAND;
  if (ts->m_token.type == PIPE_TOKEN)
    ret->type = PIPE_COMMAND;
  if (ts->m_token.type == OR_TOKEN)
    ret->type = OR_COMMAND;
  else {
      //fprintf(stderr, "Syntax error - iicommand type incorrect\n");
      //exit(1);
  }
  return ret;
}

command_stream_t 
csappend (command_stream_t cstream, command_stream_t item) {
  //printf("cappend\n");
  if (!item)
    return cstream;
  if (!cstream) { //empty
    cstream = item;
    item->next = NULL;
    item->number = 1;
  }
  else {
    command_stream_t cs_loop = cstream;
    int counter = 1;
    while (cs_loop->next) {
      cs_loop->number = counter++;
      cs_loop = cs_loop->next;
    }
    cs_loop->number = counter++;
    cs_loop->next = item;
    item->number = counter;
    item->next = NULL;
  }
  return cstream;
}

command_stream_t
make_command_stream (int (*get_next_byte) (void *), void *get_next_byte_argument) {
  // logic:
  // 	step 1: read input by character into buffer
  // 	step 2: form tokens and append to token stream
  //	step 3: validate token stream
  // 	step 4: convert token stream to commands based on precedence
 
  // step 1: read input by character into buffer
  char *buffer = read_filestream (get_next_byte, get_next_byte_argument);
  
  // step 2: form tokens and append to token stream
  //same as before, except input from buffer instead of filestream and 
  //output to token stream instead of buffer
  token_stream* tstream = NULL;
  tstream = tokenize (buffer);

  if (tstream == NULL) 
    return NULL;   

  // display token for testing
  //display_tokens (tstream);

  // step 3: validate token stream
  validate_tokens (tstream);

  // step 4: convert token stream to commands based on precedence
  command_stream_t cstream = token_to_command_stream (tstream);

  return cstream;
}

command_t
read_command_stream (command_stream_t s)
{
  if (s->iterator == 0){
    s->iterator = 1;
    return s->m_command;
  }
  else if (s->next != NULL){
    return read_command_stream(s->next);
  }
  else return NULL;
}

command_stream_t
get_next_cstream (command_stream_t cstream, command_stream_t from) {
  command_stream_t cs_loopbk, cs_loop = cstream;
//printf("in get next stream %d\n", cstream->number);
  if (from) {
    while (cs_loop && (cs_loop != from))
      cs_loop = cs_loop->next;
    if (!cs_loop)
      return NULL;
  }
  cs_loopbk = cs_loop;

  while (cs_loop) {
    if (cs_loop->c_status == NEW)
      return cs_loop;
    cs_loop = cs_loop->next;
  }

  cs_loop = cs_loopbk;
  while (cs_loop) {
    if (cs_loop->c_status == WAITING)
      return cs_loop;
    cs_loop = cs_loop->next;
  }
  if (cs_loopbk != cstream) { //wrap around required only for WAITING
    cs_loop = cstream;
    while (cs_loop && (cs_loop != cs_loopbk)) {
      if (cs_loop->c_status == WAITING)
	return cs_loop;
      cs_loop = cs_loop->next;
    }
  }
  return NULL;

}
