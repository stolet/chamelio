#ifndef UXSOCKET_H_
#define UXSOCKET_H_

#include <stdlib.h>

int uxsocket_send_int(int fd, int64_t i);
int uxsocket_sendfd(int uxfd, int fd, int64_t i);

#endif