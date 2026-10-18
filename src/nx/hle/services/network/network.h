/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The network services, on the stack in src/common/network: bsd (bsd.c), sfdnsres and nsd (dns.c), nifm (nifm.c),
   ssl (ssl.c), ldn (ldn.c) */
#ifndef SNX_HLE_NETWORK_H
#define SNX_HLE_NETWORK_H
#include "nx/hle/services/services_int.h"
#include "common/network/net.h"

/* bsd.c */
void network_init(void); /* the stack, with its messages in the log */
SvcObj* new_bsd_u(void);
SvcObj* new_bsd_s(void);
SvcObj* new_bsdcfg(void);
/* the connection the game is told about: the host's, unless the network option is off */
int network_online(void);
int network_interface(SnxNetInterface* out); /* 0 and the host's interface when online */
/* whether a guest may reach addr: with the network option off, loopback only */
int network_reachable(const SnxSockAddr* addr);
/* the socket behind descriptor fd in the table of the bsd session process pid registered (RegisterClient), with a
   reference for the caller, or NULL: what ssl duplicates, as the console's does with bsd:s's DuplicateSocket */
SnxSocket* bsd_client_socket(u64 pid, i32 fd);
/* the address a game's bind to INADDR_ANY binds instead, while ldn gives this console one, and its subnet's broadcast
   address, which a datagram socket bound there keeps hearing; NULL clears it */
void bsd_set_bind_address(const u8* ip, const u8* bcast);

/* dns.c */
SvcObj* new_sfdnsres(void);
SvcObj* new_nsd(void);

/* nifm.c */
SvcObj* new_nifm(void);

/* ssl.c */
SvcObj* new_ssl(void);

/* ldn.c */
SvcObj* new_ldn(void);

#endif
