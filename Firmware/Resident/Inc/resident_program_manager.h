#ifndef RESIDENT_PROGRAM_MANAGER_H
#define RESIDENT_PROGRAM_MANAGER_H

#include <stdint.h>

typedef enum
{
  RESIDENT_PROGRAM_STATE_ERASING = 0,
  RESIDENT_PROGRAM_STATE_PROGRAMMING_READY,
  RESIDENT_PROGRAM_STATE_STOPPED,
  RESIDENT_PROGRAM_STATE_PAUSED,
  RESIDENT_PROGRAM_STATE_RUNNING,
} ResidentProgramState;

void resident_program_manager_init(void);
int resident_program_manager_request_state(ResidentProgramState state);
ResidentProgramState resident_program_manager_state(void);
int resident_program_manager_tcp_port(void);
void resident_program_manager_mark_stopped(void);
void resident_program_manager_mark_programming_ready(void);

#endif /* RESIDENT_PROGRAM_MANAGER_H */
