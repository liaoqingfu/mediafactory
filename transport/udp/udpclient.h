#ifndef __TCP_CLIENT__
#define __TCP_CLIENT__

#include <unistd.h>
#include <stdio.h>

void* udp_client_new(const char * server,int serverPort);

int udp_client_write(void* handle, const char * sendBuff, int length);

int udp_client_read(void* handle, char* buffer, int length);

int udp_client_free(void* handle);

#endif