
#ifndef _PEERLIST_H_
#define _PEERLIST_H_

#include <arpa/inet.h>

void set_local_peer(const char *local_id, const char *local_ip);

int add_peer(const char *id, const char *dest_ip, const uint16_t port,
    const char *key);

int get_dest_info(const char *local_ip, char *source_id, char *dest_id, 
    struct sockaddr_in *addr, char *key, int *idx);

int get_source_info(const char *id, char *source, char *dest, char *key);

#endif