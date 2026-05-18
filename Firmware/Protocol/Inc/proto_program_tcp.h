#ifndef PROTO_PROGRAM_TCP_H
#define PROTO_PROGRAM_TCP_H

#include <stdint.h>

int proto_program_tcp_start(uint16_t port);
void proto_program_tcp_stop(void);
void proto_program_tcp_poll(void);

#endif /* PROTO_PROGRAM_TCP_H */
