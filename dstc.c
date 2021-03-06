// Copyright (C) 2018, Jaguar Land Rover
// This program is licensed under the terms and conditions of the
// Mozilla Public License, version 2.0.  The full text of the 
// Mozilla Public License is at https://www.mozilla.org/MPL/2.0/
//
// Author: Magnus Feuer (mfeuer1@jaguarlandrover.com)

// Server that can load and execute lambda functions.
// See README.md for details
#define SYMTAB_SIZE 128
#define MAX_CONNECTIONS 16


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
#include <sys/epoll.h>
#include "dstc.h"
#include "rmc_log.h"

// FIXME: Hash table.
typedef struct dispatch_table {
    char func_name[256];
    void (*server_func)(rmc_node_id_t node_id, uint8_t*);
} dispatch_table_t;

static dispatch_table_t local_func[SYMTAB_SIZE];

typedef void (*callback_t)(rmc_node_id_t node_id, uint8_t*);


static callback_t local_callback[SYMTAB_SIZE];

static struct remote_func_t {
    char func_name[256];
    uint32_t count; // Number of remotes supporting this function
} remote_func[SYMTAB_SIZE];

static uint32_t local_func_ind = 0;
static uint32_t callback_ind = 0;
static uint32_t remote_func_ind = 0;
static int initialized = 0;
static int epoll_fd = -1;


#define MCAST_GROUP_ADDRESS "239.40.41.42" // Completely made up
#define MCAST_GROUP_PORT 4723 // Completely made up

rmc_sub_context_t _dstc_sub_ctx;
rmc_pub_context_t _dstc_pub_ctx;

uint8_t sub_conn_vec_buf[sizeof(rmc_connection_t)*MAX_CONNECTIONS];
uint8_t pub_conn_vec_buf[sizeof(rmc_connection_t)*MAX_CONNECTIONS];

#define USER_DATA_INDEX_MASK 0x0000FFFF
#define USER_DATA_PUB_FLAG   0x00010000


char* _op_res_string(uint8_t res)
{
    switch(res) {
    case RMC_ERROR:
        return "error";
        
    case RMC_READ_MULTICAST:
        return "read multicast";
 
    case RMC_READ_MULTICAST_LOOPBACK:
        return "multicast loopback";
 
    case RMC_READ_MULTICAST_NEW:
        return "new multicast";

    case RMC_READ_MULTICAST_NOT_READY:
        return "multicast not ready";
        
    case RMC_READ_TCP:
        return "read tcp";
        
    case RMC_READ_ACCEPT:
        return "accept";
        
    case RMC_READ_DISCONNECT:
        return "disconnect";

    case RMC_WRITE_MULTICAST:
        return "write multicast";

    case RMC_COMPLETE_CONNECTION:
        return "complete connection";

    case RMC_WRITE_TCP:
        return "tcp write";

    default:
        return "[unknown]";
        
    }
}


// Retrieve a function pointer by name previously registered with
// dstc_register_local_function()
//
static void (*dstc_find_local_function(char* name, int name_len))(rmc_node_id_t node_id, uint8_t*)
{
    int i = local_func_ind;
    while(i--) {
        if (!strncmp(local_func[i].func_name, name, name_len))
            return local_func[i].server_func;
    }
    return (void (*) (rmc_node_id_t, uint8_t*)) 0;
}


// Register a function name - pointer relationship.
// Called by file constructor function _dstc_register_[name]()
// generated by DSTC_SERVER() macro.
//
void dstc_register_local_function(char* name, void (*server_func)(rmc_node_id_t node_id, uint8_t*))
{
    if (local_func_ind == SYMTAB_SIZE - 1) {
        RMC_LOG_FATAL("Out of memory trying to register local function. SYMTAB_SIZE=%d\n", SYMTAB_SIZE);
        exit(255);
    }
        
    strcpy(local_func[local_func_ind].func_name, name);
    local_func[local_func_ind].server_func = server_func;
    local_func_ind++;
}

// Retrieve a callback function. Each time it is invoked, it will be deleted.
// dstc_register_local_function()
//
static void (*dstc_find_callback(uint64_t func_addr))(rmc_node_id_t node_id, uint8_t*)
{
    int i = 0;
    while(i < callback_ind) {
        if ((uint64_t) local_callback[i] == func_addr) {
            callback_t res = local_callback[i];
            // Nill out the callback since it is a one-time shot thing.
            local_callback[i] = 0;
            return res;
        }
        ++i;
    }
    RMC_LOG_COMMENT("Did not find callback [%lX]\n", func_addr);
    return (callback_t) 0;
}


// Register a function name - pointer relationship.
// Called by file constructor function _dstc_register_[name]()
// generated by DSTC_SERVER() macro.
//
void dstc_register_callback(void (*callback)(rmc_node_id_t node_id, uint8_t*))
{
    int ind = 0;
    // Find a previously freed slot, or allocate a new one
    while(ind < callback_ind) {
        if (!local_callback[ind])
            break;
        ++ind;
    }

    // Are we out of memory
    if (ind == SYMTAB_SIZE) {
        RMC_LOG_FATAL("Out of memory trying to register callback. SYMTAB_SIZE=%d\n", SYMTAB_SIZE);
        exit(255);
    }
    local_callback[callback_ind] = callback;;
    RMC_LOG_FATAL("Registered callback [%lX]", (uint64_t) callback);
    callback_ind++;
}

void dstc_cancel_callback(void (*callback)(rmc_node_id_t node_id, uint8_t*))
{
    // Will delete the callback.
    dstc_find_callback((uint64_t) callback);
}

static struct remote_func_t* dstc_find_remote_func(char* func_name)
{
    int ind = remote_func_ind;

    while(ind--) {
        if (!strcmp(func_name, remote_func[ind].func_name))
            return &remote_func[ind];
    }
    return 0;
}

// Register a remote function as provided by the remote DSTC server
// through a control message call processed by
// dstc_subscriber_control_message_cb()
//
void dstc_register_remote_function(char* name)
{
    struct remote_func_t* remote = dstc_find_remote_func(name);

    // Is this an additional registration of an existing function.
    if (remote) {
        remote->count++;
        RMC_LOG_INFO("Remote function [%s] now supported by %d nodes", remote->func_name, remote->count);
        return;
    }
    
    // Are we out of memory
    if (remote_func_ind == SYMTAB_SIZE) {
        RMC_LOG_FATAL("Out of memory trying to register remote func. SYMTAB_SIZE=%d\n", SYMTAB_SIZE);
        exit(255);
    }

    remote = &remote_func[remote_func_ind];
    ++remote_func_ind;

    strncpy(remote->func_name, (uint8_t*) name, sizeof(remote->func_name));
    remote->func_name[sizeof(remote->func_name)-1] = 0;
    remote->count = 1;
    RMC_LOG_INFO("Remote [%s] now supported by one (first) node", remote->func_name, remote->count);
}




static void poll_add(user_data_t user_data,
                     int descriptor,
                     uint32_t event_user_data,
                     rmc_poll_action_t action)
{
    struct epoll_event ev = {
        .data.u32 = event_user_data,
        .events = 0 // EPOLLONESHOT
    };

    if (action & RMC_POLLREAD)
        ev.events |= EPOLLIN;

    if (action & RMC_POLLWRITE)
        ev.events |= EPOLLOUT;

    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, descriptor, &ev) == -1) {
        RMC_LOG_INDEX_FATAL(event_user_data & USER_DATA_INDEX_MASK, "epoll_ctl(add)");
        exit(255);
    }
}


static void poll_add_sub(user_data_t user_data,
                         int descriptor,
                         rmc_index_t index,
                         rmc_poll_action_t action)
{
    poll_add(user_data, descriptor, (uint32_t) index, action);
}

static void poll_add_pub(user_data_t user_data,
                         int descriptor,
                         rmc_index_t index,
                         rmc_poll_action_t action)
{
    poll_add(user_data, descriptor, ((uint32_t) index) | USER_DATA_PUB_FLAG, action);
}



static void poll_modify(user_data_t user_data,
                        int descriptor,
                        uint32_t event_user_data,
                        rmc_poll_action_t old_action,
                        rmc_poll_action_t new_action)
{
    struct epoll_event ev = {
        .data.u32 = event_user_data,
        .events = 0 // EPOLLONESHOT
    };

    if (old_action == new_action)
        return ;
    
    if (new_action & RMC_POLLREAD)
        ev.events |= EPOLLIN;

    if (new_action & RMC_POLLWRITE)
        ev.events |= EPOLLOUT;

    if (epoll_ctl(epoll_fd, EPOLL_CTL_MOD, descriptor, &ev) == -1) {
        RMC_LOG_INDEX_FATAL(event_user_data & USER_DATA_INDEX_MASK, "epoll_ctl(modify): %s", strerror(errno));
        exit(255);
    }
}

static void poll_modify_pub(user_data_t user_data,
                            int descriptor,
                            rmc_index_t index,
                            rmc_poll_action_t old_action,
                            rmc_poll_action_t new_action)
{
    poll_modify(user_data,
                descriptor,
                ((uint32_t) index) | USER_DATA_PUB_FLAG,
                old_action,
                new_action);
}

static void poll_modify_sub(user_data_t user_data,
                            int descriptor,
                            rmc_index_t index,
                            rmc_poll_action_t old_action,
                            rmc_poll_action_t new_action)
{
    poll_modify(user_data,
                descriptor,
                (uint32_t) index,
                old_action,
                new_action);
}


static void poll_remove(user_data_t user_data,
                        int descriptor,
                        rmc_index_t index)
{
    if (epoll_ctl(epoll_fd, EPOLL_CTL_DEL, descriptor, 0) == -1) {
        RMC_LOG_INDEX_WARNING(index, "epoll_ctl(delete): %s", strerror(errno));
        return;
    }
    RMC_LOG_INDEX_COMMENT(index, "poll_remove()");
}


usec_timestamp_t dstc_get_timeout_timestamp()
{
    usec_timestamp_t sub_event_tout_ts = 0;
    usec_timestamp_t pub_event_tout_ts = 0;

    rmc_pub_timeout_get_next(&_dstc_pub_ctx, &pub_event_tout_ts);
    rmc_sub_timeout_get_next(&_dstc_sub_ctx, &sub_event_tout_ts);

    // Figure out the shortest event timeout between pub and sub context
    if (pub_event_tout_ts == -1 && sub_event_tout_ts == -1)
        return -1;

    if (pub_event_tout_ts == -1 && sub_event_tout_ts != -1)
        return sub_event_tout_ts;

    if (pub_event_tout_ts != -1 && sub_event_tout_ts == -1)
        return pub_event_tout_ts;

    return (pub_event_tout_ts < sub_event_tout_ts)?
        pub_event_tout_ts:sub_event_tout_ts;
}


int dstc_get_timeout_msec(void)
{
    usec_timestamp_t tout = dstc_get_timeout_timestamp();

    if (tout == -1)
        return -1;

    // Convert to relative timestamp.
    tout -= rmc_usec_monotonic_timestamp();
    if (tout < 0)
        return 0;
    
    return tout / 1000 + 1;
}


int dstc_process_single_event(int timeout)
{
    int nfds = 0;
    struct epoll_event events[dstc_get_socket_count()];
        
    nfds = epoll_wait(epoll_fd, events, sizeof(events) / sizeof(events[0]), timeout);

    if (nfds == -1) {
        RMC_LOG_FATAL("epoll_wait(): %s", strerror(errno));
        exit(255);
    }

    // Timeout
    if (nfds == 0) 
        return ETIME;

    // Process all pending events.
    while(nfds--) 
        dstc_process_epoll_result(&events[nfds]);
}

int dstc_process_events(usec_timestamp_t timeout_arg)
{
    usec_timestamp_t timeout_ts = 0;
    usec_timestamp_t now = 0;
    
    if (!initialized)
        dstc_setup();

    // Calculate an absolute timeout timestamp based on relative
    // timestamp provided in argument.
    
    timeout_ts = (timeout_arg == -1)?-1:(rmc_usec_monotonic_timestamp() + timeout_arg);

    // Process evdents until we reach the timeout therhold.
    while((now = rmc_usec_monotonic_timestamp()) < timeout_ts || timeout_ts == -1) {
        usec_timestamp_t timeout = 0;
        char is_arg_timeout = 0;
        usec_timestamp_t event_tout_ts = 0;

        event_tout_ts = dstc_get_timeout_timestamp();

        // Figure out the shortest timeout between argument and event timeout
        if (timeout_ts == -1 && event_tout_ts == -1) {
            RMC_LOG_DEBUG("Both argument and event timeout are -1 -> -1");
            timeout = -1;
        }

        if (timeout_ts == -1 && event_tout_ts != -1) {
            timeout = (event_tout_ts - now) / 1000 + 1 ;
            RMC_LOG_DEBUG("arg timeout == -1. Event timeout != -1 -> %ld", timeout);
        }

        if (timeout_ts != -1 && event_tout_ts == -1) {
            timeout = (timeout_ts - now) / 1000 + 1 ;
            RMC_LOG_DEBUG("arg timeout != -1. Event timeout == -1 -> %ld", timeout);
        }

        if (timeout_ts != -1 && event_tout_ts != -1) {
            if (event_tout_ts < timeout_ts) {
                timeout = (event_tout_ts - now) / 1000 + 1;
                
                is_arg_timeout = 0;
            } else {
                timeout = (timeout_ts - now) / 1000 + 1;
                is_arg_timeout = 1;
            }
                
            if (event_tout_ts < timeout_ts) {
                RMC_LOG_DEBUG("event timeout is less than arg timeout -> %ld", timeout);
            }
            else {
                RMC_LOG_DEBUG("arg timeout is less than event timeout -> %ld", timeout);
            }
        }

        if (dstc_process_single_event((int) timeout) == ETIME) {
            // Did we time out on an RMC event to be processed, or did
            // we time out on the argument provided to
            // dstc_process_events()?
            if (is_arg_timeout) {
                RMC_LOG_DEBUG("Timed out on argument. returning" );
                return ETIME;
            }

            // Make rmc calls to handle scheduled events.
            dstc_process_timeout();
            continue;
        }
    }

    return 0;
}

extern void dstc_process_epoll_result(struct epoll_event* event)
{
    int res = 0;
    uint8_t op_res = 0;
    rmc_index_t c_ind = (rmc_index_t) event->data.u32 & USER_DATA_INDEX_MASK;
    int is_pub = (event->data.u32 & USER_DATA_PUB_FLAG)?1:0;

    RMC_LOG_INDEX_DEBUG(c_ind, "%s: %s%s%s",
                        (is_pub?"pub":"sub"),
                        ((event->events & EPOLLIN)?" read":""),
                        ((event->events & EPOLLOUT)?" write":""),
                        ((event->events & EPOLLHUP)?" disconnect":""));


    if (event->events & EPOLLIN) {
        if (is_pub)
            res = rmc_pub_read(&_dstc_pub_ctx, c_ind, &op_res);
        else
            res = rmc_sub_read(&_dstc_sub_ctx, c_ind, &op_res);

        RMC_LOG_INDEX_DEBUG(c_ind, "read result: %s - %s", _op_res_string(op_res),   strerror(res));
    }

    if (event->events & EPOLLOUT) {
        if (is_pub) {
            if (rmc_pub_write(&_dstc_pub_ctx, c_ind, &op_res) != 0) 
                rmc_pub_close_connection(&_dstc_pub_ctx, c_ind);
        } else {
            if (rmc_sub_write(&_dstc_sub_ctx, c_ind, &op_res) != 0)
                rmc_sub_close_connection(&_dstc_sub_ctx, c_ind);
        }
    }
}

extern void dstc_process_timeout(void)
{
    rmc_pub_timeout_process(&_dstc_pub_ctx);
    rmc_sub_timeout_process(&_dstc_sub_ctx);
}

static uint32_t dstc_process_function_call(uint8_t* data, uint32_t data_len)
{
    dstc_header_t* call = (dstc_header_t*) data;
    void (*local_func_ptr)(rmc_node_id_t node_id, uint8_t*) = 0;

    if (data_len < sizeof(dstc_header_t)) {
        RMC_LOG_WARNING("Packet header too short! Wanted %ld bytes, got %d",
                        sizeof(dstc_header_t), data_len);
        return data_len; // Emtpy buffer
    }

    if (data_len - sizeof(dstc_header_t) < call->payload_len) {
        RMC_LOG_WARNING("Packet payload too short! Wanted %d bytes, got %d",
                        call->payload_len, data_len - sizeof(dstc_header_t));
        return data_len; // Emtpy buffer
    }

    // Retrieve function pointer from name, as previously
    // registered with dstc_register_local_function()
    RMC_LOG_DEBUG("DSTC Serve: node_id[%lu] name_len[%d] name[%.*s] payload_len[%d]",
                  call->node_id, 
                  call->name_len,
                  call->name_len, call->payload, call->payload_len - call->name_len);
    if (call->name_len)
        local_func_ptr = dstc_find_local_function(call->payload, call->name_len);
    else
        local_func_ptr = dstc_find_callback(*(uint64_t*)call->payload);
        

    if (!local_func_ptr) {
        RMC_LOG_COMMENT("Function [%.*s] not loaded. Ignored", call->name_len, call->payload);
        return sizeof(dstc_header_t) + call->payload_len;
    }

    (*local_func_ptr)(call->node_id, call->payload + (call->name_len?call->name_len:sizeof(uint64_t))); 
    return sizeof(dstc_header_t) + call->payload_len;
}


static void dstc_subscription_complete(rmc_sub_context_t* sub_ctx,
                                       uint32_t listen_ip,
                                       in_port_t listen_port,
                                       rmc_node_id_t node_id)
{
    int ind = local_func_ind;
    RMC_LOG_COMMENT("Subscription complete. Sending supported functions.");

    // Retrieve function pointer from name, as previously
    // registered with dstc_register_local_function()
    // Include null terminator for an easier life.
    while(ind--) {
        RMC_LOG_COMMENT("  [%s]", local_func[ind].func_name);
        rmc_sub_write_control_message_by_node_id(sub_ctx, 
                                                 node_id, 
                                                 local_func[ind].func_name, 
                                                 strlen(local_func[ind].func_name) + 1);
        
    }
    RMC_LOG_COMMENT("Done sending functions");
    return;
}

static void dstc_process_incoming(rmc_sub_context_t* sub_ctx)
{
    sub_packet_t* pack = rmc_sub_get_next_dispatch_ready(sub_ctx);
    RMC_LOG_DEBUG("Processing incoming");
    while(pack) {
        uint32_t ind = 0;

        RMC_LOG_DEBUG("Got packet. payload_len[%d]", pack->payload_len);
        while(ind < pack->payload_len) {
            RMC_LOG_DEBUG("Processing function call. ind[%d]", ind);
            ind += dstc_process_function_call(((uint8_t*) pack->payload) + ind, pack->payload_len - ind);
        }

        rmc_sub_packet_dispatched(sub_ctx, pack);
        pack = rmc_sub_get_next_dispatch_ready(sub_ctx);            
    }
    return;
}


static void dstc_subscriber_control_message_cb(rmc_pub_context_t* ctx,
                                               uint32_t publisher_address,
                                               uint16_t publisher_port,
                                               rmc_node_id_t node_id,
                                               void* payload,
                                               payload_len_t payload_len)
{
    dstc_register_remote_function((char*) payload);
    return;
}


uint32_t dstc_get_socket_count(void)
{
    if (!initialized)
        return 0;
  
    // Grab the count of all open sockets.
    return rmc_sub_get_socket_count(&_dstc_sub_ctx) + 
        rmc_pub_get_socket_count(&_dstc_pub_ctx);
}


static void free_published_packets(void* pl, payload_len_t len, user_data_t dt)
{
    RMC_LOG_DEBUG("Freeing %p", pl);
    free(pl);
}


static int dstc_setup_internal(rmc_pub_context_t* pub_ctx,
                               rmc_sub_context_t* sub_ctx,
                               int epoll_fd_arg,
                               user_data_t user_data)
{
    uint8_t* sub_conn_vec_mem = 0;
    uint8_t* pub_conn_vec_mem = 0;


    // Already intialized?
    if (initialized) 
        return EBUSY;

    epoll_fd = epoll_fd_arg,
    rmc_log_set_start_time();
    pub_conn_vec_mem = malloc(sizeof(rmc_connection_t)*MAX_CONNECTIONS);
    memset(pub_conn_vec_mem, 0, sizeof(rmc_connection_t)*MAX_CONNECTIONS);

    rmc_pub_init_context(&_dstc_pub_ctx,
                         0, // Random node_id
                         MCAST_GROUP_ADDRESS, MCAST_GROUP_PORT, 
                         "0.0.0.0", // Bind to any address for tcp control listen
                         0, // Use ephereal tcp port for tcp control
                         user_data,
                         poll_add_pub, poll_modify_pub, poll_remove,
                         pub_conn_vec_mem, MAX_CONNECTIONS,
                         free_published_packets);


    // Setup a subscriber callback, allowing us to know when a subscribe that can
    // execute the function has attached.
    rmc_pub_set_control_message_callback(&_dstc_pub_ctx, dstc_subscriber_control_message_cb);

    // Subscriber init.

    sub_conn_vec_mem = malloc(sizeof(rmc_connection_t)*MAX_CONNECTIONS);
    memset(sub_conn_vec_mem, 0, sizeof(rmc_connection_t)*MAX_CONNECTIONS);

    rmc_sub_init_context(&_dstc_sub_ctx,
                         // Reuse pub node id to detect and avoid loopback messages
                         rmc_pub_node_id(&_dstc_pub_ctx), 
                         MCAST_GROUP_ADDRESS,
                         "0.0.0.0", // Any interface for multicast address
                         MCAST_GROUP_PORT,  
                         user_data,
                         poll_add_sub, poll_modify_sub, poll_remove,
                         sub_conn_vec_mem, MAX_CONNECTIONS,
                         0,0);
    
    rmc_sub_set_packet_ready_callback(&_dstc_sub_ctx, dstc_process_incoming);
    rmc_sub_set_subscription_complete_callback(&_dstc_sub_ctx, dstc_subscription_complete);

    rmc_pub_activate_context(&_dstc_pub_ctx);
    rmc_sub_activate_context(&_dstc_sub_ctx);

    // Start ticking announcements as a client that the server will connect back to.
    rmc_pub_set_announce_interval(&_dstc_pub_ctx, 200000); // Start ticking announces.

    initialized = 1;
    return 0;
}

rmc_node_id_t dstc_get_node_id(void)
{
    if (!initialized)
        return 0;

    return rmc_pub_node_id(&_dstc_pub_ctx);
}

int dstc_setup_epoll(int epoll_fd_arg)
{
    return dstc_setup_internal(&_dstc_pub_ctx,
                               &_dstc_sub_ctx,
                               epoll_fd_arg,
                               // user_data to be provided to poll_add, poll_modify, and poll_remove
                               user_data_nil());
}


int dstc_setup(void)
{
    if (initialized)
        return EBUSY;

    epoll_fd = epoll_create(1);

    return dstc_setup_epoll(epoll_fd);
}

uint32_t dstc_get_remote_count(char* function_name)
{
    struct remote_func_t* remote = dstc_find_remote_func(function_name);

    // Is this an additional registration of an existing function.
    if (!remote)
        return 0;
    
    return remote->count;
}

static void dstc_queue(uint8_t* name, uint8_t name_len, uint8_t* arg, uint32_t arg_sz)
{
    // Will be freed by RMC on confirmed delivery
    dstc_header_t *call = (dstc_header_t*) malloc(sizeof(dstc_header_t) + strlen(name) + arg_sz) ;
    uint16_t actual_name_len = name_len?name_len:sizeof(uint64_t);

    // FIXME: Stuff multiple calls into a single packet.
    //        Queue packet either at timeout (1-2 msec) or when packet is full (RMC_MAX_PAYLOAD)
    //
    if (!initialized)
        dstc_setup();

    call->name_len = name_len; // May be zero to indicate thtat this is an address.
    call->payload_len = arg_sz + actual_name_len;
    call->node_id = dstc_get_node_id();

    memcpy(call->payload, name, actual_name_len);
    memcpy(call->payload + actual_name_len, arg, arg_sz);

    RMC_LOG_DEBUG("DSTC Queue: node_id[%lu] name_len[%d/%d] name[%.*s] payload_len[%d]",
                  call->node_id,
                  call->name_len, actual_name_len,
                  call->name_len?call->name_len:10,
                  call->name_len?call->payload:((uint8_t*)"[callback]"),
                  call->payload_len - actual_name_len);
    rmc_pub_queue_packet(&_dstc_pub_ctx, call, sizeof(dstc_header_t) + actual_name_len + arg_sz, 0);
    return;
}


void dstc_queue_callback(uint64_t addr, uint8_t* arg, uint32_t arg_sz)
{
    // Call with zero namelen to treat name as a 64bit integer.
    // This integer will be mapped by the received through the local_callback
    // table to a pending callback function.
    dstc_queue((uint8_t*) &addr, 0, arg, arg_sz);
}

void dstc_queue_func(uint8_t* name, uint8_t* arg, uint32_t arg_sz)
{
    dstc_queue(name, strlen(name), arg, arg_sz);
}
