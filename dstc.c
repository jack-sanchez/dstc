// Copyright (C) 2018, Jaguar Land Rover
// This program is licensed under the terms and conditions of the
// Mozilla Public License, version 2.0.  The full text of the 
// Mozilla Public License is at https://www.mozilla.org/MPL/2.0/
//
// Author: Magnus Feuer (mfeuer1@jaguarlandrover.com)

// Server that can load and execute lambda functions.
// See README.md for details

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <sys/time.h>

#include <sys/resource.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "dstc.h"

#define SYMTAB_SIZE 128

// FIXME: Hash table.
static struct symbol_table_t {
    char func_name[256];
    void (*server_func)(uint8_t*);
} symtab[SYMTAB_SIZE];

static uint32_t symind = 0;
extern int _dstc_mcast_sock;



int dstc_setup_mcast_sub(void)
{
    unsigned sinlen;
    struct sockaddr_in sock_addr;
    struct ip_mreq mreq;
    int flag = 1;

    sinlen = sizeof(struct sockaddr_in);
    memset(&sock_addr, 0, sinlen);

    _dstc_mcast_sock = socket (PF_INET, SOCK_DGRAM, 0);
    sock_addr.sin_family = AF_INET;
    sock_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    sock_addr.sin_port = htons(DSTC_MCAST_PORT);

    if (setsockopt(_dstc_mcast_sock, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag)) < 0) {
        perror("ABORT: dstc_setup_mcast_sub(): setsockopt(REUSEADDR)");
        exit(1);
    }

    if (setsockopt(_dstc_mcast_sock, SOL_SOCKET, SO_REUSEPORT, &flag, sizeof(flag)) < 0) {
        perror("ABORT: dstc_setup_mcast_sub(): setsockopt(SO_REUSEPORT)");
        exit(1);
    }

    // Join multicast group
    mreq.imr_multiaddr.s_addr = inet_addr(DSTC_MCAST_GROUP);         
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);         
    if (setsockopt(_dstc_mcast_sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
        perror("ABORT: dstc_mcast_group(): setsockopt(IP_ADD_MEMBERSHIP)");
        exit(1);
    }         

    // Bind to local endpoint.
    if (bind(_dstc_mcast_sock, (struct sockaddr *) &sock_addr, sizeof(sock_addr)) < 0) {        
        perror("bind");
        exit(1);
    }    


    return _dstc_mcast_sock;
}

// Called by DSTC_CLIENT()-generated code
// to send out data to multicast socket.
void _dstc_send(uint8_t* buf, uint32_t sz)
{
    static struct sockaddr_in dstc_addr;                                
    static char first_call = 1;                                         

    if (first_call) {
        memset(&dstc_addr, 0, sizeof(dstc_addr));                                 
        dstc_addr.sin_family = AF_INET;                                
        dstc_addr.sin_addr.s_addr = inet_addr(DSTC_MCAST_GROUP);       
        dstc_addr.sin_port = htons(DSTC_MCAST_PORT);                   
    }

    sendto(_dstc_mcast_sock, buf, sz, 0,                            
           (struct sockaddr*) &dstc_addr, sizeof(dstc_addr));
}

// Register a function name - pointer relationship.
// Called by file constructor function _dstc_register_[name]()
// generated by DSTC_SERVER() macro.
//
void dstc_register_function(char* name, void (*server_func)(uint8_t*))
{
    printf("dstc_register_function(%s): Called\n", name);
    strcpy(symtab[symind].func_name, name);
    symtab[symind].server_func = server_func;
    symind++;
}

// Retrieve a function pointer by name previously registered with
// dstc_register_function()
//
static void (*dstc_get_function(char* name))(uint8_t*)
{
    int i = symind;
    while(i--) {
        if (!strcmp(symtab[i].func_name, name))
            return symtab[i].server_func;
    }
    return (void (*) (uint8_t*)) 0;
}

int dstc_get_socket()
{
    if (_dstc_mcast_sock == -1)
        dstc_setup_mcast_sub();
    
    return _dstc_mcast_sock;
}

void dstc_read(void)
{
    uint8_t rcv_buf[64*1024]; // FIXME better buffer management.
    uint8_t *payload = 0;
    ssize_t cnt = 0;
    int i = symind;
    
    // Setup socket if not already done.
    if (_dstc_mcast_sock == -1)
        dstc_setup_mcast_sub();
    
    // Buffer format:
    // [function_name]\0[data]
    // [function_name] matches prior function registration by DSTC_SERVER() macro.
    // [data] gets fet to the (macro-generated) deserializer of the function
    cnt = recvfrom(_dstc_mcast_sock, rcv_buf, sizeof(rcv_buf), 0, (struct sockaddr *) 0, 0);

    if (cnt < 0) {
        perror("ABORT: recvfrom");
        exit(1);
    }

    // Place payload on the byte after the function name.
    payload = rcv_buf + strlen(rcv_buf) + 1;

    // Retrieve function pointer from name, as previously
    // registered with dstc_register_function()
    while(i--) {
        if (!strcmp(symtab[i].func_name, rcv_buf))
            break;
    }

    if (i == -1) {
        printf("[Info]: Function [%s] not loaded. Ignored\n", rcv_buf);
        return;
    }

    (*symtab[i].server_func)(payload); // Read arguments from stdin and run function.
}
