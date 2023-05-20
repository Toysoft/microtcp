#include <time.h> // time()
#include <ctype.h>
#include <errno.h>
#include <string.h> // strerror()
#include <stdint.h>
#include <stdlib.h>

#ifndef MICROTCP_AMALGAMATION
#   include "ip.h"
#   include "arp.h"
#   include "tcp.h"
#   include "endian.h"
#   include "microtcp.h"
#   ifdef MICROTCP_BACKGROUND_THREAD
#       include "tinycthread.h"
#   endif
#endif

#ifdef MICROTCP_USING_TAP
#include <tuntap.h>
#endif

#ifdef MICROTCP_DEBUG
#include <stdio.h>
#define MICROTCP_DEBUG_LOG(fmt, ...) do { fprintf(stderr, "MICROTCP :: " fmt "\n", ## __VA_ARGS__); } while (0);
#else
#define MICROTCP_DEBUG_LOG(...) do {} while (0);
#endif

#ifdef MICROTCP_BACKGROUND_THREAD
#define   LOCK_WHEN_THREADED(mtcp) do {   mtx_lock(&(mtcp)->lock); } while (0);
#define UNLOCK_WHEN_THREADED(mtcp) do { mtx_unlock(&(mtcp)->lock); } while (0);
#else
#define   LOCK_WHEN_THREADED(mtcp) do { (void) (mtcp); } while (0);
#define UNLOCK_WHEN_THREADED(mtcp) do { (void) (mtcp); } while (0);
#endif

#ifdef MICROTCP_USING_MUX
typedef struct mux_entry_t mux_entry_t;
struct mux_entry_t {
    mux_entry_t **mux_prev;
    mux_entry_t  *mux_next;
    mux_entry_t **sock_prev;
    mux_entry_t  *sock_next;
    microtcp_mux_t *mux; // This is set on initialization
                         // of the parent microtcp_mux_t
                         // and never changed.
    microtcp_socket_t *sock;
    void *userp;
    int triggered_events;
    int events_of_interest;
};

struct microtcp_mux_t {
    microtcp_t *mtcp;
#ifdef MICROTCP_BACKGROUND_THREAD
    cnd_t queue_not_empty;
#endif
    mux_entry_t *free_list;
    mux_entry_t *idle_list;
    mux_entry_t *ready_queue_head;
    mux_entry_t *ready_queue_tail;
    mux_entry_t entries[MICROTCP_MAX_MUX_ENTRIES];
};
#endif

typedef struct buffer_t buffer_t;
struct buffer_t {
    microtcp_t *mtcp;
    buffer_t   *prev;
    buffer_t   *next;
    size_t      used;
    char        data[1518];
};

typedef enum {
    SOCKET_LISTENER,
    SOCKET_CONNECTION,
} socket_type_t;

struct microtcp_socket_t {
    microtcp_t *mtcp;
    microtcp_socket_t *prev;
    microtcp_socket_t *next;
    socket_type_t type;
    union {
        tcp_listener_t   *listener;
        tcp_connection_t *connection;
    };

#ifdef MICROTCP_BACKGROUND_THREAD
    union {
        cnd_t something_to_accept;
        struct {
            cnd_t something_to_recv;
            cnd_t something_to_send;
        };
    };
#endif

#ifdef MICROTCP_USING_MUX
    mux_entry_t *mux_list;
#endif
};

struct microtcp_t {

    time_t last_update_time;

#ifdef MICROTCP_BACKGROUND_THREAD
    bool thread_should_stop;
    thrd_t thread_id;
    mtx_t lock;
#endif

    microtcp_callbacks_t callbacks;

    ip_address_t ip;
    mac_address_t mac;

    ip_state_t   ip_state;
    arp_state_t arp_state;
    tcp_state_t tcp_state;

    buffer_t *used_buffer;
    buffer_t *wait_buffer_list;
    buffer_t *free_buffer_list;
    buffer_t buffer_pool[MICROTCP_MAX_BUFFERS];

    microtcp_socket_t *used_socket_list;
    microtcp_socket_t *free_socket_list;
    microtcp_socket_t socket_pool[MICROTCP_MAX_SOCKETS];
};

const char *microtcp_strerror(microtcp_errcode_t errcode)
{
    switch (errcode) {
        case MICROTCP_ERRCODE_NONE: return "No error occurred";
        case MICROTCP_ERRCODE_SOCKETLIMIT: return "Can't create a socket because the socket limit per microtcp instance was reached";
        case MICROTCP_ERRCODE_TCPERROR: return "An error occurred at the TCP layer";
        case MICROTCP_ERRCODE_BADCONDVAR: return "Condition variable error";
        case MICROTCP_ERRCODE_NOTLISTENER: return "Invalid operation on a non-listener socket";
        case MICROTCP_ERRCODE_CANTBLOCK: return "Can't execute a blocking call for this function";
        case MICROTCP_ERRCODE_WOULDBLOCK: return "Can't executa e non-blocking call for this function";
        case MICROTCP_ERRCODE_NOTCONNECTION: return "Invalid operation on a non-connection socket";
    }
    return "???";
}

typedef enum {
    ETHERNET_PROTOCOL_ARP = 0x0806,
    ETHERNET_PROTOCOL_IP  = 0x0800,
} ethernet_protocol_t;

typedef struct {
    mac_address_t dst;
    mac_address_t src;
    uint16_t    proto;
} __attribute__((packed)) ethernet_frame_t;

static_assert(sizeof(ethernet_frame_t) == 14);

#ifdef MICROTCP_DEBUG
static bool is_valid_buffer_pointer(microtcp_t *mtcp, buffer_t *buffer)
{
    for (size_t i = 0; i < MICROTCP_MAX_BUFFERS; i++)
        if (buffer == mtcp->buffer_pool + i)
            return true;
    return false;
}
#endif

static void send_arp_packet(void *data, mac_address_t dst)
{
    microtcp_t *mtcp = data;
    buffer_t *buffer = mtcp->used_buffer;

#ifdef MICROTCP_DEBUG
    assert(is_valid_buffer_pointer(mtcp, buffer));
#endif

    buffer->used = sizeof(ethernet_frame_t) + sizeof(arp_packet_t);

    ethernet_frame_t *frame = (ethernet_frame_t*) buffer->data;
    frame->dst = dst;
    frame->src = mtcp->mac;
    frame->proto = cpu_to_net_u16(ETHERNET_PROTOCOL_ARP);

    // TODO: What about the CRC?
    #warning "TODO: Calculate Ethernet CRC"

    int n = mtcp->callbacks.send(mtcp->callbacks.data, buffer->data, buffer->used);
    if (n < 0)
        MICROTCP_DEBUG_LOG("Couldn't send (%s)", strerror(errno));

    // Now reset the used buffer
    mtcp->used_buffer->used = 0;
}

static int send_tcp_segment(void *data, ip_address_t ip, 
                            const slice_list_t *slices, 
                            size_t num_slices)
{
    microtcp_t *mtcp = data;
    return ip_send_2(&mtcp->ip_state, IP_PROTOCOL_TCP, ip, true, slices, num_slices);
}

static void move_wait_buffer_to_free_list(buffer_t *buffer)
{
    microtcp_t *mtcp = buffer->mtcp;

#ifdef MICROTCP_DEBUG
    assert(is_valid_buffer_pointer(mtcp, buffer));
    assert(buffer->prev == NULL || is_valid_buffer_pointer(mtcp, buffer->prev));
    assert(buffer->next == NULL || is_valid_buffer_pointer(mtcp, buffer->next));
#endif
    
    if (buffer->prev)
        buffer->prev->next = buffer->next;
    else
        mtcp->wait_buffer_list = buffer->next;

    if (buffer->next)
        buffer->next->prev = buffer->prev;

#ifdef MICROTCP_DEBUG
    assert(mtcp->free_buffer_list == NULL || is_valid_buffer_pointer(mtcp, mtcp->free_buffer_list));
    assert(mtcp->free_buffer_list == NULL || mtcp->free_buffer_list->prev == NULL);
    assert(mtcp->free_buffer_list == NULL || mtcp->free_buffer_list->next == NULL || is_valid_buffer_pointer(mtcp, mtcp->free_buffer_list->next));
#endif

    buffer->prev = NULL;
    buffer->next = mtcp->free_buffer_list;
    mtcp->free_buffer_list = buffer;
}

static void mac_resolved(void *data, arp_resolution_status_t status, mac_address_t mac)
{
    buffer_t *buffer = data;
    microtcp_t *mtcp = buffer->mtcp;

#ifdef MICROTCP_DEBUG
    assert(is_valid_buffer_pointer(mtcp, buffer));
#endif

    switch (status) {
        case ARP_RESOLUTION_OK:
        {
            ethernet_frame_t *frame = (ethernet_frame_t*) buffer->data;
            frame->dst = mac;

            MICROTCP_DEBUG_LOG("sending %d", buffer->used);
            int n = mtcp->callbacks.send(mtcp->callbacks.data, buffer->data, buffer->used);
            if (n < 0)
                MICROTCP_DEBUG_LOG("Couldn't send (%s)", strerror(errno));
        }
        break;

        case ARP_RESOLUTION_FAILED:
        MICROTCP_DEBUG_LOG("MAC resolution failed");
        break;

        case ARP_RESOLUTION_TIMEOUT:
        MICROTCP_DEBUG_LOG("MAC resolution timeout");
        break;
    }

    move_wait_buffer_to_free_list(buffer);
}

static void move_used_buffer_to_wait_list(microtcp_t *mtcp)
{
    buffer_t *buffer = mtcp->used_buffer;
    mtcp->used_buffer = NULL;

#ifdef MICROTCP_DEBUG
    assert(is_valid_buffer_pointer(mtcp, buffer));
#endif

    buffer->next = mtcp->wait_buffer_list;
    if (mtcp->wait_buffer_list)
        mtcp->wait_buffer_list->prev = buffer;
    mtcp->wait_buffer_list = buffer;
    
    ip_change_output_buffer(&mtcp->ip_state, NULL, 0);
    arp_change_output_buffer(&mtcp->arp_state, NULL, 0);
}

static void use_a_buffer(microtcp_t *mtcp)
{

#ifdef MIRCOTCP_DEBUG
    assert(mtcp->free_buffer_list == NULL || is_valid_buffer_pointer(mtcp, mtcp->free_buffer_list));
    assert(mtcp->free_buffer_list == NULL || mtcp->free_buffer_list->prev == NULL);
    assert(mtcp->free_buffer_list == NULL || mtcp->free_buffer_list->next == NULL || is_valid_buffer_pointer(mtcp, mtcp->free_buffer_list->next));
#endif

    //  At this moment the network stack has no allocated
    //  output buffer but wants to allocate one (by calling
    //  this function).
    //  It's assumed there is no output buffer, hence:
    //
    assert(mtcp->used_buffer == NULL);
    //
    //  To allocate a buffer, we need to pop it from the
    //  buffer free list, which is a singly-linked list of
    //  unused buffers. Once it's been popped off the list,
    //  we need to tell the upper layers of the stack that
    //  this is the new output buffer.
    //
    //  If the free list is empty, no buffer is allocated.
    //
    if (!mtcp->free_buffer_list)
        return; // No free buffers available in the free list.
    //
    // Pop a buffer from the free list
    buffer_t *buffer = mtcp->free_buffer_list;
    mtcp->free_buffer_list = buffer->next;
    //
    // Initialize the buffer
    buffer->mtcp = mtcp;
    buffer->used = 0;
    buffer->prev = NULL;
    buffer->next = NULL;
    //
    // Set it as the output buffer
    mtcp->used_buffer = buffer;
    //
    // Now tell the upper layers where they'll output 
    // the data, but reserve the first bytes of the buffer
    // for the ethernet header.
    //
    void  *output_ptr =        buffer->data  + sizeof(ethernet_frame_t);
    size_t output_max = sizeof(buffer->data) - sizeof(ethernet_frame_t);
    ip_change_output_buffer(&mtcp->ip_state,   output_ptr, output_max);
    arp_change_output_buffer(&mtcp->arp_state, output_ptr, output_max);
}

static void send_ip_packet(void *data, ip_address_t ip, size_t len)
{
    microtcp_t *mtcp = data;
    
    buffer_t *buffer = mtcp->used_buffer;
    if (buffer == NULL)
        // The IP layer wants to send something, but no output
        // buffer was associated to it. This function should not
        // have been called by the IP layer without a buffer.
        return;

    buffer->used = sizeof(ethernet_frame_t) + len;

    move_used_buffer_to_wait_list(mtcp);
    use_a_buffer(mtcp);

    ethernet_frame_t *frame = (ethernet_frame_t*) buffer->data;
    frame->src = mtcp->mac;
    frame->dst = MAC_ZERO; // We need to determine it
    frame->proto = cpu_to_net_u16(ETHERNET_PROTOCOL_IP);

    arp_resolve_mac(&mtcp->arp_state, ip, buffer, mac_resolved);
}

static void 
tcp_process_segment_wrapper(void *data, ip_address_t ip, const void *packet, size_t len)
{
    if (len >= sizeof(tcp_segment_t))
        tcp_process_segment((tcp_state_t*) data, ip, (tcp_segment_t*) packet, len);
}

static void 
process_packet(microtcp_t *mtcp, const void *packet, size_t len)
{
    if (len < sizeof(ethernet_frame_t))
        return;

    const ethernet_frame_t *frame = packet;
    
    switch (net_to_cpu_u16(frame->proto)) {
        case ETHERNET_PROTOCOL_ARP: 
        arp_process_packet(&mtcp->arp_state, frame+1, len - sizeof(ethernet_frame_t)); 
        break;
        
        case ETHERNET_PROTOCOL_IP:
        ip_process_packet(&mtcp->ip_state, frame+1, len - sizeof(ethernet_frame_t)); 
        break;

        default:
        // Unsupported ethertype
        //MICROTCP_DEBUG_LOG("Ignoring packet with ethertype %4x", frame->proto);
        break;
    }
}

void microtcp_process_packet(microtcp_t *mtcp, const void *packet, size_t len)
{
    LOCK_WHEN_THREADED(mtcp);
    process_packet(mtcp, packet, len);
    UNLOCK_WHEN_THREADED(mtcp);
}

void microtcp_step(microtcp_t *mtcp)
{
    char packet[1024]; // This buffer is the bottleneck for the 
                       // maximum packet size that can be processed.

    // The call to [recv] (which is assumed to be blocking)
    // needs to be out of the critical section to give other
    // threads the ability to progress in the mean time.
    int size = mtcp->callbacks.recv(mtcp->callbacks.data, packet, sizeof(packet));
    if (size < 0)
        return;

    LOCK_WHEN_THREADED(mtcp);
    {
        process_packet(mtcp, packet, size);
        
        time_t current_time = time(NULL);
        int secs = (float) (current_time - mtcp->last_update_time);
                    
        if (secs > 0) {
            ip_seconds_passed(&mtcp->ip_state, secs);
            arp_seconds_passed(&mtcp->arp_state, secs);
            tcp_seconds_passed(&mtcp->tcp_state, secs);
            mtcp->last_update_time = current_time;
        }
    }
    UNLOCK_WHEN_THREADED(mtcp);
}

#ifdef MICROTCP_BACKGROUND_THREAD
static int loop(void *data)
{
    microtcp_t *mtcp = data;
    while (!mtcp->thread_should_stop)
        microtcp_step(mtcp);
    return 0;
}
#endif

static bool is_hex_digit(char c)
{
    return (c >= '0' && c <= '9')
        || (c >= 'a' && c <= 'f')
        || (c >= 'A' && c <= 'F');
}

static int int_from_hex_digit(char c)
{
    assert(is_hex_digit(c));
    if (c >= 'A' || c <= 'F')
        return c - 'A' + 10;
    if (c >= 'a' || c <= 'f')
        return c - 'a' + 10;
    return c - '0';
}

static bool parse_mac(const char *src, size_t len, 
                      mac_address_t *mac)
{
    if (src == NULL || len != 17
     || !is_hex_digit(src[0]) 
     || !is_hex_digit(src[1])
     || src[2] != ':'
     || !is_hex_digit(src[3])
     || !is_hex_digit(src[4])
     || src[5] != ':'
     || !is_hex_digit(src[6])
     || !is_hex_digit(src[7])
     || src[8] != ':'
     || !is_hex_digit(src[9])
     || !is_hex_digit(src[10])
     || src[11] != ':'
     || !is_hex_digit(src[12])
     || !is_hex_digit(src[13])
     || src[14] != ':'
     || !is_hex_digit(src[15])
     || !is_hex_digit(src[16]))
        return false;

    static const char max_char_map[] = "0123456789ABCDEF";

    if (mac) {
        mac->data[0] = max_char_map[int_from_hex_digit(src[ 0])] << 4
                     | max_char_map[int_from_hex_digit(src[ 1])];
        mac->data[1] = max_char_map[int_from_hex_digit(src[ 3])] << 4
                     | max_char_map[int_from_hex_digit(src[ 4])];
        mac->data[2] = max_char_map[int_from_hex_digit(src[ 6])] << 4
                     | max_char_map[int_from_hex_digit(src[ 7])];
        mac->data[3] = max_char_map[int_from_hex_digit(src[ 9])] << 4
                     | max_char_map[int_from_hex_digit(src[10])];
        mac->data[4] = max_char_map[int_from_hex_digit(src[12])] << 4
                     | max_char_map[int_from_hex_digit(src[13])];
        mac->data[5] = max_char_map[int_from_hex_digit(src[15])] << 4
                     | max_char_map[int_from_hex_digit(src[16])];
    }
    return true;
}

static mac_address_t generate_random_mac()
{
    mac_address_t mac = {
        .data = {
            rand() & 0xff,
            rand() & 0xff,
            rand() & 0xff,
            rand() & 0xff,
            rand() & 0xff,
            rand() & 0xff,
        },
    };
    return mac;
}

static bool parse_ip(const char *ip, ip_address_t *parsed_ip)
{
    size_t len = strlen(ip);
    size_t i = 0;

    uint32_t value = 0;
    
    for (size_t k = 0; k < 4; k++) {
        if (i == len || !isdigit(ip[i]))
            return false;
        int n = 0; // Used to represent a byte, but it's larger
                   // to detect overflows.
        do {
            // Convert character to number
            int digit = ip[i] - '0';
            if (n > (UINT8_MAX - digit)/10)
                // Adding this digit would make the
                // byte overflow, so it can't be part
                // of the octet.
                break;
            n = n * 10 + digit;
            i++;
        } while (i < len && isdigit(ip[i]));
        
        assert(n >= 0 && n <= UINT8_MAX);
        value = (value << 8) | (uint8_t) n;
        
        // If this isn't the last octet and there is no
        // dot following it, the address is invalid.
        if (k < 3) {
            if (i == len || ip[i] != '.')
                return false;
            i++; // Consume the dot.
        }
    }
    if (i < len)
        // source string contains something 
        // other than the address in it.
        return false;

    *parsed_ip = cpu_to_net_u32(value);
    return true;
}

microtcp_t *microtcp_create_using_callbacks(const char *ip, const char *mac,
                                            microtcp_callbacks_t callbacks)
{
    mac_address_t parsed_mac;
    if (mac == NULL) {
        // Generate a random MAC
        parsed_mac = generate_random_mac();
    } else {
        if (!parse_mac(mac, mac ? strlen(mac) : 0, &parsed_mac))
            return NULL;
    }

    ip_address_t parsed_ip;
    if (!parse_ip(ip, &parsed_ip))
        return NULL;

    microtcp_t *mtcp = malloc(sizeof(microtcp_t));
    if (mtcp == NULL)
        return NULL;

    mtcp->ip = parsed_ip;
    mtcp->mac = parsed_mac;
    mtcp->callbacks = callbacks;
    mtcp->last_update_time = time(NULL);

    mtcp->used_buffer = NULL;
    mtcp->wait_buffer_list = NULL;
    mtcp->free_buffer_list = mtcp->buffer_pool;
    for (size_t i = 0; i < MICROTCP_MAX_BUFFERS-1; i++) {
        mtcp->buffer_pool[i].mtcp = NULL;
        mtcp->buffer_pool[i].prev = NULL;
        mtcp->buffer_pool[i].next = mtcp->buffer_pool + i+1;
    }
    mtcp->buffer_pool[MICROTCP_MAX_BUFFERS-1].mtcp = NULL;
    mtcp->buffer_pool[MICROTCP_MAX_BUFFERS-1].prev = NULL;
    mtcp->buffer_pool[MICROTCP_MAX_BUFFERS-1].next = NULL;

    mtcp->used_socket_list = NULL;
    mtcp->free_socket_list = mtcp->socket_pool;
    for (size_t i = 0; i < MICROTCP_MAX_SOCKETS-1; i++) {
        mtcp->socket_pool[i].mtcp = NULL;
        mtcp->socket_pool[i].prev = NULL;
        mtcp->socket_pool[i].next = mtcp->socket_pool + i + 1;
    }
    mtcp->socket_pool[MICROTCP_MAX_SOCKETS-1].mtcp = NULL;
    mtcp->socket_pool[MICROTCP_MAX_SOCKETS-1].prev = NULL;
    mtcp->socket_pool[MICROTCP_MAX_SOCKETS-1].next = NULL;

    ip_init(&mtcp->ip_state, parsed_ip, mtcp, send_ip_packet);
    if (!ip_plug_protocol(&mtcp->ip_state, IP_PROTOCOL_TCP, &mtcp->tcp_state, tcp_process_segment_wrapper)) {
        free(mtcp);
        return NULL;
    }

    arp_init(&mtcp->arp_state, parsed_ip, parsed_mac, mtcp, send_arp_packet);
    
    tcp_init(&mtcp->tcp_state, parsed_ip, (tcp_callbacks_t) {
        .data = mtcp,
        .send = send_tcp_segment,
    });
    
    use_a_buffer(mtcp);

#ifdef MICROTCP_BACKGROUND_THREAD
    {
        if (mtx_init(&mtcp->lock, mtx_plain) != thrd_success) {
            ip_free(&mtcp->ip_state);
            arp_free(&mtcp->arp_state);
            tcp_free(&mtcp->tcp_state);
            free(mtcp);
            return NULL;
        }
        mtcp->thread_should_stop = false;
        if (thrd_create(&mtcp->thread_id, loop, mtcp) != thrd_success) {
            ip_free(&mtcp->ip_state);
            arp_free(&mtcp->arp_state);
            tcp_free(&mtcp->tcp_state);
            mtx_destroy(&mtcp->lock);
            free(mtcp);
            return NULL;
        }
    }
#endif

    MICROTCP_DEBUG_LOG("Instanciated ("
        "debug="
#ifdef MICROTCP_DEBUG
        "yes"
#else
        "no"
#endif
        ", thread="
#ifdef MICROTCP_BACKGROUND_THREAD
        "yes"
#else
        "no"
#endif
        ")");

    return mtcp;
}


#ifdef MICROTCP_USING_TAP

static void log_callback_for_tuntap_library(int level, const char *errmsg) 
{
    const char *name;

    switch(level) {
        case TUNTAP_LOG_DEBUG : name = "Debug";   break;
        case TUNTAP_LOG_INFO  : name = "Info";    break;
        case TUNTAP_LOG_NOTICE: name = "Notice";  break;
        case TUNTAP_LOG_WARN  : name = "Warning"; break;
        case TUNTAP_LOG_ERR   : name = "Error";   break;
        case TUNTAP_LOG_NONE:
        default:
            name = NULL;
            break;
    }
    if (name == NULL) {
        MICROTCP_DEBUG_LOG("%s (from the tap library)", errmsg);
    } else {
        MICROTCP_DEBUG_LOG("[%s] %s (from the tap library)", name, errmsg);
    }
}

bool microtcp_callbacks_create_for_tap(const char *ip, const char *mac,
                                       microtcp_callbacks_t *callbacks)
{
    assert(ip);

    struct device *dev = tuntap_init();
    if (!dev)
        return false;

    // This must be set AFTER tuntap_init because
    // it sets the callback function to the default
    // callback which writes to stderr.
    tuntap_log_set_cb(log_callback_for_tuntap_library);

    int netmask = 24; // TODO: Make this configurable

    if (tuntap_start(dev, TUNTAP_MODE_ETHERNET, TUNTAP_ID_ANY))
        goto cleanup;

    tuntap_set_ip(dev, ip, netmask);
    tuntap_set_hwaddr(dev, mac ? mac : "random");

    if (tuntap_up(dev))
        goto cleanup;

    *callbacks = (microtcp_callbacks_t) {
        .data = dev,
        .free = (void(*)(void*)) tuntap_release,
        .recv = (int(*)(void*, void*, size_t)) tuntap_read,
        .send = (int(*)(void*, const void*, size_t)) tuntap_write,
    };
    
    return true;

cleanup:
    tuntap_release(dev);
    return false;
}

microtcp_t *microtcp_create(const char *tap_ip, const char *stack_ip, 
                            const char *tap_mac, const char *stack_mac)
{
    microtcp_callbacks_t callbacks;
    if (!microtcp_callbacks_create_for_tap(tap_ip, tap_mac, &callbacks))
        return NULL;
    microtcp_t *mtcp = microtcp_create_using_callbacks(stack_ip, stack_mac, callbacks);
    if (!mtcp)
        callbacks.free(callbacks.data);
    return mtcp;
}
#endif

void microtcp_destroy(microtcp_t *mtcp)
{
#ifdef MICROTCP_BACKGROUND_THREAD
    MICROTCP_DEBUG_LOG("Stopping thread");
    mtcp->thread_should_stop = true;
    thrd_join(mtcp->thread_id, NULL);
    mtx_destroy(&mtcp->lock);
    MICROTCP_DEBUG_LOG("Thread stopped");
#endif

    ip_free(&mtcp->ip_state);
    arp_free(&mtcp->arp_state);
    tcp_free(&mtcp->tcp_state);

    if (mtcp->callbacks.free)
        mtcp->callbacks.free(mtcp->callbacks.data);
}

static microtcp_socket_t*
pop_socket_struct_from_free_list(microtcp_t *mtcp)
{
    microtcp_socket_t *socket = mtcp->free_socket_list;
    mtcp->free_socket_list = socket->next;
    return socket;
}

static void
push_unlinked_socket_into_used_list(microtcp_socket_t *socket)
{
    microtcp_t *mtcp = socket->mtcp;

    socket->next = mtcp->used_socket_list;
    if (mtcp->used_socket_list)
        mtcp->used_socket_list->prev = socket;
    mtcp->used_socket_list = socket;
}

static void 
unlink_socket_from_used_socket_list(microtcp_socket_t *socket)
{
    microtcp_t *mtcp = socket->mtcp;

    if (socket->prev)
        socket->prev->next = socket->next;
    else
        mtcp->used_socket_list = socket->next;

    if (socket->next)
        socket->next->prev = socket->prev;

    socket->prev = NULL;
    socket->next = NULL;
}

static void
push_unlinked_socket_into_free_list(microtcp_t *mtcp, microtcp_socket_t *socket)
{
    socket->prev = NULL;
    socket->next = mtcp->free_socket_list;
    mtcp->free_socket_list = socket;
}

#ifdef MICROTCP_USING_MUX
static void
signal_events_to_muxes_associated_to_socket(microtcp_socket_t *socket, int events);
#endif

static void ready_to_accept(void *data)
{
    microtcp_socket_t *socket = data;
    (void) socket;

#ifdef MICROTCP_BACKGROUND_THREAD
    cnd_signal(&socket->something_to_accept);
#endif

#ifdef MICROTCP_USING_MUX
    MICROTCP_DEBUG_LOG("Signaling ACCEPT to muxes");
    signal_events_to_muxes_associated_to_socket(socket, MICROTCP_MUX_ACCEPT);
#endif
}

microtcp_socket_t *microtcp_open(microtcp_t *mtcp, uint16_t port,
                                 microtcp_errcode_t *errcode)
{
    microtcp_errcode_t errcode2 = MICROTCP_ERRCODE_NONE;
    microtcp_socket_t *socket = NULL;
    LOCK_WHEN_THREADED(mtcp);
    {
        socket = pop_socket_struct_from_free_list(mtcp);
        if (!socket) {
            errcode2 = MICROTCP_ERRCODE_SOCKETLIMIT;
            goto unlock_and_exit; // Socket limit reached
        }

        tcp_listener_t *listener = tcp_listener_create(&mtcp->tcp_state, port, socket, ready_to_accept);
        if (listener == NULL) {
            #warning "This error code should be more specific, but the TCP module isn't stable yet"
            errcode2 = MICROTCP_ERRCODE_TCPERROR;
            push_unlinked_socket_into_free_list(mtcp, socket);
            goto unlock_and_exit;
        }

        socket->mtcp = mtcp;
        socket->prev = NULL;
        socket->next = NULL;
        socket->type = SOCKET_LISTENER;
        socket->listener = listener;

#ifdef MICROTCP_USING_MUX
        socket->mux_list = NULL;
#endif

#ifdef MICROTCP_BACKGROUND_THREAD
        if (cnd_init(&socket->something_to_accept) != thrd_success) {
            errcode2 = MICROTCP_ERRCODE_BADCONDVAR;
            push_unlinked_socket_into_free_list(mtcp, socket);
            tcp_listener_destroy(listener);
            goto unlock_and_exit;
        }
#endif
        push_unlinked_socket_into_used_list(socket);
    }
unlock_and_exit:
    UNLOCK_WHEN_THREADED(mtcp);
    if (errcode)
        *errcode = errcode2;
    return socket;
}

void microtcp_close(microtcp_socket_t *socket)
{
    if (!socket)
        return;

    microtcp_t *mtcp = socket->mtcp;

    #warning "sockets should unregister from all multiplexers"

    LOCK_WHEN_THREADED(mtcp);
    {
        switch (socket->type) {
            
            case SOCKET_LISTENER:
#ifdef MICROTCP_BACKGROUND_THREAD
            cnd_destroy(&socket->something_to_accept);
#endif
            tcp_listener_destroy(socket->listener);
            break;
                
            case SOCKET_CONNECTION:
            tcp_connection_destroy(socket->connection);
            break;
        }

        unlink_socket_from_used_socket_list(socket);
        push_unlinked_socket_into_free_list(mtcp, socket);
    }
    UNLOCK_WHEN_THREADED(mtcp);
}

static void ready_to_recv(void *data)
{
    microtcp_socket_t *socket = data;
    (void) socket;

#ifdef MICROTCP_BACKGROUND_THREAD
    cnd_signal(&socket->something_to_recv);
#endif

#ifdef MICROTCP_USING_MUX
    MICROTCP_DEBUG_LOG("Signaling RECV to muxes");
    signal_events_to_muxes_associated_to_socket(socket, MICROTCP_MUX_RECV);
#endif
}

static void ready_to_send(void *data)
{
    microtcp_socket_t *socket = data;
    (void) socket;

#ifdef MICROTCP_BACKGROUND_THREAD
    cnd_signal(&socket->something_to_send);
#endif

#ifdef MICROTCP_USING_MUX
    MICROTCP_DEBUG_LOG("Signaling SEND to muxes");
    signal_events_to_muxes_associated_to_socket(socket, MICROTCP_MUX_SEND);
#endif
}

microtcp_socket_t *microtcp_accept(microtcp_socket_t *socket, 
                                   bool no_block,
                                   microtcp_errcode_t *errcode)
{
    microtcp_errcode_t errcode2 = MICROTCP_ERRCODE_NONE;
    microtcp_t *mtcp = socket->mtcp;
    microtcp_socket_t *socket2 = NULL;

    LOCK_WHEN_THREADED(mtcp);
    {
        if (socket->type != SOCKET_LISTENER) {
            errcode2 = MICROTCP_ERRCODE_NOTLISTENER;
            goto unlock_and_exit; // Can't accept from a non-listening socket
        }

        socket2 = pop_socket_struct_from_free_list(mtcp);
        if (!socket2) {
            errcode2 = MICROTCP_ERRCODE_SOCKETLIMIT;
            goto unlock_and_exit; // Socket limit reached
        }

        tcp_connection_t *connection = tcp_listener_accept(socket->listener, socket2, ready_to_recv, ready_to_send);

#ifdef MICROTCP_BACKGROUND_THREAD
        while (!connection && !no_block) {
            if (cnd_wait(&socket->something_to_accept, &mtcp->lock) != thrd_success) {
                errcode2 = MICROTCP_ERRCODE_BADCONDVAR;
                push_unlinked_socket_into_free_list(mtcp, socket2);
                goto unlock_and_exit;
            }
            connection = tcp_listener_accept(socket->listener, socket2, ready_to_recv, ready_to_send);
        }
#else
        if (!connection) {
            if (no_block)
                errcode2 = MICROTCP_ERRCODE_WOULDBLOCK;
            else
                errcode2 = MICROTCP_ERRCODE_CANTBLOCK;
            push_unlinked_socket_into_free_list(mtcp, socket2);
            goto unlock_and_exit;
        }
#endif

        socket2->mtcp = mtcp;
        socket2->prev = NULL;
        socket2->next = NULL;
        socket2->type = SOCKET_CONNECTION;
        socket2->connection = connection;

#ifdef MICROTCP_USING_MUX
        socket2->mux_list = NULL;
#endif

#ifdef MICROTCP_BACKGROUND_THREAD
        if (cnd_init(&socket2->something_to_recv) != thrd_success) {
            errcode2 = MICROTCP_ERRCODE_BADCONDVAR;
            push_unlinked_socket_into_free_list(mtcp, socket2);
            goto unlock_and_exit;
        }
        if (cnd_init(&socket2->something_to_send) != thrd_success) {
            errcode2 = MICROTCP_ERRCODE_BADCONDVAR;
            cnd_destroy(&socket2->something_to_recv);
            push_unlinked_socket_into_free_list(mtcp, socket2);
            goto unlock_and_exit;
        }
#endif

        push_unlinked_socket_into_used_list(socket2);
    }

unlock_and_exit:
    UNLOCK_WHEN_THREADED(mtcp);

    if (errcode)
        *errcode = errcode2;

    return socket2;
}

size_t microtcp_recv(microtcp_socket_t *socket, 
                     void *dst, size_t len,
                     bool no_block,
                     microtcp_errcode_t *errcode)
{
    if (!socket || socket->type != SOCKET_CONNECTION) {
        if (errcode)
            *errcode = MICROTCP_ERRCODE_NOTCONNECTION;
        return 0;
    }

    size_t num;
    microtcp_t *mtcp = socket->mtcp;
    microtcp_errcode_t errcode2 = MICROTCP_ERRCODE_NONE;

    LOCK_WHEN_THREADED(mtcp);
    {
        num = tcp_connection_recv(socket->connection, dst, len);

#ifdef MICROTCP_BACKGROUND_THREAD
        while (num == 0 && !no_block) {
            if (cnd_wait(&socket->something_to_recv, &mtcp->lock) != thrd_success) {
                errcode2 = MICROTCP_ERRCODE_BADCONDVAR;
                goto unlock_and_exit;
            }
            num = tcp_connection_recv(socket->connection, dst, len);
        }
#endif
        if (num == 0) {
            if (no_block)
                errcode2 = MICROTCP_ERRCODE_WOULDBLOCK;
            else
                errcode2 = MICROTCP_ERRCODE_CANTBLOCK;
        }
    }
unlock_and_exit:
    UNLOCK_WHEN_THREADED(mtcp);

    if (errcode)
        *errcode = errcode2;
    return num;
}

size_t microtcp_send(microtcp_socket_t *socket, 
                     const void *src, size_t len, 
                     bool no_block,
                     microtcp_errcode_t *errcode)
{
    if (!socket || socket->type != SOCKET_CONNECTION) {
        if (errcode) 
            *errcode = MICROTCP_ERRCODE_NOTCONNECTION;
        return 0;
    }


    size_t num;
    microtcp_t *mtcp = socket->mtcp;
    microtcp_errcode_t errcode2 = MICROTCP_ERRCODE_NONE;

    LOCK_WHEN_THREADED(mtcp);
    {
        num = tcp_connection_send(socket->connection, src, len);

#ifdef MICROTCP_BACKGROUND_THREAD
        while (num == 0 && !no_block) {
            if (cnd_wait(&socket->something_to_send, &mtcp->lock) != thrd_success) {
                errcode2 = MICROTCP_ERRCODE_BADCONDVAR;
                goto unlock_and_exit;
            }
            num = tcp_connection_send(socket->connection, src, len);
        }
#endif
        if (num == 0) {
            if (no_block)
                errcode2 = MICROTCP_ERRCODE_WOULDBLOCK;
            else
                errcode2 = MICROTCP_ERRCODE_CANTBLOCK;
        }
    }
unlock_and_exit:
    UNLOCK_WHEN_THREADED(mtcp);

    if (errcode)
        *errcode = errcode2;
    return num;
}

#ifdef MICROTCP_USING_MUX
microtcp_mux_t *microtcp_mux_create(microtcp_t *mtcp)
{
    microtcp_mux_t *mux = malloc(sizeof(microtcp_mux_t));
    if (!mux)
        return NULL;

    mux->mtcp = mtcp;

    // Build the free list
    static_assert(MICROTCP_MAX_MUX_ENTRIES > 1);
    const int max = MICROTCP_MAX_MUX_ENTRIES;
    for (int i = 1; i < max-1; i++) {
        mux->entries[i].mux = mux; // This will be never changed
        mux->entries[i].mux_prev = &mux->entries[i-1].mux_next;
        mux->entries[i].mux_next = &mux->entries[i+1];
    }
    mux->entries[0].mux = mux; // Never changed
    mux->entries[0].mux_prev = &mux->free_list;
    mux->entries[0].mux_next = &mux->entries[1];
    mux->entries[max-1].mux = mux; // Never changed
    mux->entries[max-1].mux_prev = &mux->entries[max-2].mux_next;
    mux->entries[max-1].mux_next = NULL;

    mux->idle_list = NULL;
    mux->free_list = mux->entries;
    mux->ready_queue_head = NULL;
    mux->ready_queue_tail = NULL;

#ifdef MICROTCP_BACKGROUND_THREAD
    if (cnd_init(&mux->queue_not_empty) != thrd_success) {
        free(mux);
        return NULL;
    }
#endif

    return mux;
}

static bool mux_poll(microtcp_mux_t *mux, microtcp_muxevent_t *ev);

void microtcp_mux_destroy(microtcp_mux_t *mux)
{
    // Unregister all idle sockets
    // Idle entries don't have pending events
    // to deliver so by unregistering them the
    // entry is unlinked.
    while (mux->idle_list)
        microtcp_mux_unregister(mux, mux->idle_list->sock, ~0);

    // Consume all previously reported events
    // to make sure that when unregistering
    // the entries are actually removed
    while (mux_poll(mux, NULL));

    // Unreagister all sockets that have events
    while (mux->ready_queue_head) {
        mux_entry_t *entry = mux->ready_queue_head;
        microtcp_mux_unregister(mux, entry->sock, ~0);

        // Since all events were consumed beforehand
        // we're sure the entry was removed.
        assert(entry != mux->ready_queue_head);
    }

#ifdef MICROTCP_BACKGROUND_THREAD
    cnd_destroy(&mux->queue_not_empty);
#endif

    free(mux);
}

static mux_entry_t*
find_socket_and_mux_entry(microtcp_mux_t *mux, microtcp_socket_t *sock)
{
    mux_entry_t *entry = sock->mux_list;
    while (entry) {
        if (entry->mux == mux)
            break;
        entry = entry->sock_next;
    }
    return entry;
}

static void 
move_mux_entry_to_free_list(mux_entry_t *entry)
{
    microtcp_mux_t *mux = entry->mux;
    
    // If the entry is in a list, unlink it
    if (mux->ready_queue_tail == entry)
        mux->ready_queue_tail = entry->mux_next;
    if (entry->mux_prev) 
        *entry->mux_prev = entry->mux_next;
    if (entry->sock_prev)
        *entry->sock_prev = entry->sock_next;

    // Put the structure into the free list
    entry->mux_prev = &mux->free_list;
    entry->mux_next = mux->free_list;
    if (mux->free_list)
        mux->free_list->mux_prev = &entry->mux_next;
    mux->free_list = entry;
}

static void 
move_mux_entry_to_idle_list(mux_entry_t *entry)
{
    microtcp_mux_t *mux = entry->mux;

    // To be moved to the idle list the entry
    // must be associated to a socket so it
    // must be in a socket mux list, therefore
    // it must be true that
    assert(entry->sock_prev); // not null iff the entry is in a mux list

    // Make sure the entry is unlinked relative
    // to the lists in the mux
    if (mux->ready_queue_tail == entry)
        mux->ready_queue_tail = entry->mux_next;
    if (entry->mux_prev)
        *entry->mux_prev = entry->mux_next;

    // Now actually insert it into the idle list
    entry->mux_prev = &mux->idle_list;
    entry->mux_next = mux->idle_list;
    if (mux->idle_list)
        mux->idle_list->mux_prev = &entry->mux_next;
    mux->idle_list = entry;
}

bool microtcp_mux_unregister(microtcp_mux_t *mux, microtcp_socket_t *sock, int events)
{
    LOCK_WHEN_THREADED(mux->mtcp);

    // There's no need to check that mux
    // and socket have the same mtcp because
    // if it's different it will result that
    // the socket isn't registered into the
    // mux. 

    mux_entry_t *entry = find_socket_and_mux_entry(mux, sock);
    if (!entry) {
        // This socket wasn't registered into the mux
        UNLOCK_WHEN_THREADED(mux->mtcp);
        return false;
    }

    // Unset the events of interest
    entry->events_of_interest &= ~events;
    
    if (entry->triggered_events) {
        // NOTE: Since we modified "events_of_interest"
        //       but not "triggered_events", any previously
        //       triggered events that were now unregistered
        //       will still be delivered to the user.
        //
        //       Though when events are delivered, if all 
        //       events registered were all unregistered, 
        //       the socket is removed from the mux.
    } else
        // No events were previously reported so we can
        // move the entry to the free list.
        move_mux_entry_to_free_list(entry);

    UNLOCK_WHEN_THREADED(mux->mtcp);
    return true;
}

bool microtcp_mux_register(microtcp_mux_t *mux, microtcp_socket_t *sock, int events, void *userp)
{
    LOCK_WHEN_THREADED(mux->mtcp);

    if (mux->mtcp != sock->mtcp) {
        UNLOCK_WHEN_THREADED(mux->mtcp);
        return false; // mux and socket are associated to different microtcp stacks
    }

    if (events == 0) {
        UNLOCK_WHEN_THREADED(mux->mtcp);
        return true; // Nothing to be done
    }

    mux_entry_t *entry = find_socket_and_mux_entry(mux, sock);
    if (!entry) {
        // This is the first time that the socket is registered.
        // Create an entry for it
        if (mux->free_list == NULL) {
            // The entry limit was reached. 
            // It's impossible to register the socket at this time
            UNLOCK_WHEN_THREADED(mux->mtcp);
            return false;
        }
        
        // Pop from the free list
        entry = mux->free_list;
        *entry->mux_prev = entry->mux_next;

        // Push it into the idle list of the mux
        entry->mux_prev = &mux->idle_list;
        entry->mux_next =  mux->idle_list;
        if (mux->idle_list)
            mux->idle_list->mux_prev = &entry->mux_next;
        mux->idle_list = entry;

        // Push it into the socket mux list
        entry->sock_prev = &sock->mux_list;
        entry->sock_next =  sock->mux_list;
        if (sock->mux_list)
            sock->mux_list->sock_prev = &entry->sock_next;
        sock->mux_list = entry;

        // Initialize the entry
        entry->sock = sock;
        entry->userp = userp;
        entry->triggered_events = 0;
        entry->events_of_interest = 0;
        // entry->mux = mux; This isn't necessary because the mux field
        //                   is initialized once with the mux and never
        //                   changed.
    }

    entry->events_of_interest |= events;

    UNLOCK_WHEN_THREADED(mux->mtcp);
    return true;
}

static bool mux_poll(microtcp_mux_t *mux, microtcp_muxevent_t *ev)
{
    
    if (!mux->ready_queue_head)
        return false; // No events occurred

    // Get the tail of the queue (without popping it)
    mux_entry_t *entry = mux->ready_queue_head;

    // If this socket was in the ready queue
    // it must have triggered events
    assert(entry->triggered_events);

    if (ev) {
        ev->userp  = entry->userp;
        ev->events = entry->triggered_events;
        ev->socket = entry->sock;
    }

    // Unmark events as triggered
    entry->triggered_events = 0;

    if (entry->events_of_interest == 0)
        // All events were unregistered. 
        // We can remove the socket from the mux.
        move_mux_entry_to_free_list(entry);
    else
        // The socket wasn't unregistered or
        // wasn't unregistered completely so
        // we put the entry into the idle list
        move_mux_entry_to_idle_list(entry);

    return true;
}

bool microtcp_mux_wait(microtcp_mux_t *mux, microtcp_muxevent_t *ev)
{
#ifdef MICROTCP_BACKGROUND_THREAD
    LOCK_WHEN_THREADED(mux->mtcp);
    while (!mux_poll(mux, ev)) {
        MICROTCP_DEBUG_LOG("Multiplexer waiting for an event");
        if (cnd_wait(&mux->queue_not_empty, &mux->mtcp->lock) != thrd_success)
            abort();
        MICROTCP_DEBUG_LOG("Multiplexer woke up for an event");
    }
    UNLOCK_WHEN_THREADED(mux->mtcp);
    return true;
#else
    return mux_poll(mux, ev);
#endif
}

static void
signal_events_to_muxes_associated_to_socket(microtcp_socket_t *socket, int events)
{
    // (This function is called by the socket and not the mux)

    assert(events); // If no events need to be signaled then
                    // this function has no reason to be called.
    
    MICROTCP_DEBUG_LOG("Socket about to signal to multiplexers");

    mux_entry_t *entry = socket->mux_list;
    while (entry) {
        
        microtcp_mux_t *mux = entry->mux;

        // Mask the bitmask of triggered events [events] with
        // the bitmask of events that this multiplexer is
        // interested in.
        int newly_triggered_events = events & entry->events_of_interest;
        
        if (!newly_triggered_events)
            MICROTCP_DEBUG_LOG("MUX not interested in these events");

        // If there are no previously triggered events by this
        // socket and the socket just generated some events the
        // mux is interested in, then we need to move the socket-mux
        // structure from the idle list to the ready queue of the mux.        
        bool first_event_of_socket_in_mux = (entry->triggered_events == 0) && newly_triggered_events;
        entry->triggered_events |= newly_triggered_events;

        if (first_event_of_socket_in_mux) {

            // Is this the first socket structure of the muxes
            // ready queue? If it is, we'll need to wake it up
            bool queue_was_empty = (mux->ready_queue_head == NULL);

            // Unlink it from the idle list
            *entry->mux_prev = entry->mux_next;
            if (entry->mux_next)
                entry->mux_next->mux_prev = entry->mux_prev;

            // Add it to the queue
            if (mux->ready_queue_tail)
                entry->mux_prev = &mux->ready_queue_tail->mux_next;
            else {
                entry->mux_prev = &mux->ready_queue_head;
                mux->ready_queue_head = entry;
            }
            entry->mux_next = NULL;
            mux->ready_queue_tail = entry;

#ifdef MICROTCP_BACKGROUND_THREAD
            MICROTCP_DEBUG_LOG("Signaling event to multiplexer");
            if (queue_was_empty)
                cnd_signal(&mux->queue_not_empty);
            MICROTCP_DEBUG_LOG("Signaled event to multiplexer");
#else
            (void) queue_was_empty;
#endif
        }
        entry = entry->sock_next;
    }
    MICROTCP_DEBUG_LOG("Socket signaled to multiplexers");
}
#endif