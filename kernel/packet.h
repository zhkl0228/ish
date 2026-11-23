#ifndef KERNEL_PACKET_H
#define KERNEL_PACKET_H

// AF_PACKET sockets (kernel/packet.c), on /dev/netcap's made up packets

#include <stdbool.h>
#include "misc.h"
#include "fs/fd.h"

fd_t packet_socket(int type, int protocol);
bool fd_is_packet(struct fd *fd);
int_t packet_bind(struct fd *fd, addr_t sockaddr_addr, uint_t sockaddr_len);
int_t packet_getsockname(struct fd *fd, addr_t sockaddr_addr, addr_t sockaddr_len_addr);
int_t packet_setsockopt(struct fd *fd, int level, int option, const void *value, uint_t len);
// value has *len bytes (in and out)
int_t packet_getsockopt(struct fd *fd, int level, int option, void *value, uint_t *len);
// A call that is not here, logged: EOPNOTSUPP
int_t packet_unsupported(struct fd *fd, const char *call);

#endif
