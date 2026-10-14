/* IPv4 sockets over lwIP's raw API. One mutex owns the NIC and protocol stack;
 * blocking syscalls release it before sleeping. No network work runs in an IRQ.
 * File descriptions/refcounts are owned by the existing VFS (VFS -> network).
 */
#include <net/network.h>
#include <net/e1000.h>
#include <net/igc.h>
#include <sched.h>
#include <fs/unix_socket.h>
#include <mm/pmm.h>
#include <random.h>
#include <string.h>
#include <tsc.h>
#include <printk.h>
#include <panic.h>
#include <lwip/init.h>
#include <lwip/netif.h>
#include <lwip/dhcp.h>
#include <lwip/dns.h>
#include <lwip/tcp.h>
#include <lwip/udp.h>
#include <lwip/timeouts.h>
#include <lwip/etharp.h>
#include <netif/ethernet.h>

static bool ethernet_present(void) {
    return igc_present() || e1000_present();
}

static bool ethernet_link(void) {
    return igc_present() ? igc_link() : e1000_link();
}

static void ethernet_mac(uint8_t out[6]) {
    if (igc_present())
        igc_mac(out);
    else
        e1000_mac(out);
}

static long ethernet_send(const void *frame, size_t length) {
    return igc_present() ? igc_send(frame, length) : e1000_send(frame, length);
}

static long ethernet_receive(void *frame, size_t length) {
    return igc_present() ? igc_receive(frame, length) : e1000_receive(frame, length);
}

#define RX_SIZE 65536u
#define UDP_QUEUE 32u
#define ACCEPT_QUEUE 16u
#define IO_LIMIT (16u * 1024u * 1024u)
#define MSG_PEEK 2u
#define MSG_TRUNC 0x20u
#define MSG_DONTWAIT 0x40u
#define MSG_WAITALL 0x100u
#define MSG_NOSIGNAL 0x4000u
#define SOCK_NONBLOCK 0x800u
#define SOCK_CLOEXEC 0x80000u

struct inet_address {
    uint16_t family, port;
    uint32_t ip;
    uint8_t padding[8];
};

struct udp_packet {
    struct pbuf *data;
    ip_addr_t address;
    uint16_t port;
};

struct net_socket {
    int type, error;
    bool connected, connecting, listening, eof, write_shutdown, read_shutdown;
    struct tcp_pcb *tcp;
    struct udp_pcb *udp;
    uint8_t *bytes;
    unsigned head, tail, count;
    struct udp_packet datagrams[UDP_QUEUE];
    unsigned first, queued, queued_bytes;
    struct net_socket *accepted[ACCEPT_QUEUE];
    unsigned accept_first, accept_count, backlog;
    uint64_t receive_timeout, send_timeout;
};

static mutex_t network_mutex;
static struct netif ethernet;
static bool ready, have_ethernet;
static uint64_t received_packets, transmitted_packets, dropped_packets;
static uint32_t written_dns[2];

uint32_t sys_now(void) {
    return tsc_uptime_us() / 1000;
}

uint32_t network_random(void) {
    uint32_t result;
    /* Predictable timer/address fallbacks are deliberately excluded. */
    if (random_bytes(&result, sizeof(result)) != sizeof(result))
        panic("network secure random source failed");
    return result;
}

static int error_number(err_t error) {
    switch (error) {
    case ERR_OK:
        return 0;
    case ERR_MEM:
        return E_NOMEM;
    case ERR_BUF:
        return E_NOBUFS;
    case ERR_TIMEOUT:
        return E_TIMEDOUT;
    case ERR_RTE:
        return E_NETUNREACH;
    case ERR_INPROGRESS:
        return E_INPROGRESS;
    case ERR_VAL:
        return E_INVAL;
    case ERR_WOULDBLOCK:
        return E_AGAIN;
    case ERR_USE:
        return E_ADDRINUSE;
    case ERR_ALREADY:
        return E_ALREADY;
    case ERR_ISCONN:
        return E_ISCONN;
    case ERR_CONN:
        return E_NOTCONN;
    case ERR_IF:
        return E_NETDOWN;
    case ERR_ABRT:
        return E_CONNABORTED;
    case ERR_RST:
        return E_CONNRESET;
    case ERR_CLSD:
        return E_NOTCONN;
    default:
        return E_IO;
    }
}

static err_t link_output(struct netif *netif, struct pbuf *packet) {
    (void)netif;
    uint8_t frame[1514];
    if (packet->tot_len > sizeof(frame))
        return ERR_BUF;
    if (pbuf_copy_partial(packet, frame, packet->tot_len, 0) != packet->tot_len)
        return ERR_BUF;
    long count = ethernet_send(frame, packet->tot_len);
    if (count >= 0) {
        transmitted_packets++;
        return ERR_OK;
    }
    return count == -E_AGAIN ? ERR_MEM : ERR_IF;
}

static err_t interface_init(struct netif *netif) {
    netif->name[0] = 'e';
    netif->name[1] = 'n';
    netif->hostname = "modernos";
    netif->mtu = 1500;
    netif->hwaddr_len = 6;
    ethernet_mac(netif->hwaddr);
    netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET;
    netif->output = etharp_output;
    netif->linkoutput = link_output;
    return ERR_OK;
}

static void write_resolver(uint32_t addresses[2]) {
    char text[160];
    unsigned used = 0;
    for (unsigned i = 0; i < 2; i++)
        if (addresses[i]) {
            ip4_addr_t address = {.addr = addresses[i]};
            char printed[16];
            ip4addr_ntoa_r(&address, printed, sizeof(printed));
            used += snprintf(text + used, sizeof(text) - used, "nameserver %s\n", printed);
        }
    if (!used)
        return; /* Never invent a DNS server. */
    mutex_lock(&vfs_mutex);
    struct file *file;
    int error = vfs_open("/etc/resolv.conf", O_WRONLY | O_CREAT | O_TRUNC, 0644, &file);
    if (!error) {
        long count = vfs_write(file, text, used);
        vfs_close(file);
        if (count != (long)used)
            error = E_IO;
    }
    mutex_unlock(&vfs_mutex);
    if (!error)
        memcpy(written_dns, addresses, sizeof(written_dns));
}

static void worker(void *argument) {
    (void)argument;
    for (;;) {
        uint32_t servers[2] = {0};
        mutex_lock(&network_mutex);
        if (have_ethernet) {
            bool linked = ethernet_link();
            if (linked != !!netif_is_link_up(&ethernet)) {
                if (linked)
                    netif_set_link_up(&ethernet);
                else
                    netif_set_link_down(&ethernet);
            }
            uint8_t bytes[1514];
            for (unsigned i = 0; i < 64; i++) {
                long length = ethernet_receive(bytes, sizeof(bytes));
                if (length < 0)
                    break;
                if (!length) {
                    dropped_packets++;
                    continue;
                }
                struct pbuf *packet = pbuf_alloc(PBUF_RAW, length, PBUF_POOL);
                if (!packet) {
                    dropped_packets++;
                    continue;
                }
                if (pbuf_take(packet, bytes, length) != ERR_OK ||
                    ethernet.input(packet, &ethernet) != ERR_OK) {
                    pbuf_free(packet);
                    dropped_packets++;
                } else
                    received_packets++;
            }
        }
        netif_poll_all();
        sys_check_timeouts();
        if (have_ethernet && !ip4_addr_isany_val(*netif_ip4_addr(&ethernet)))
            for (unsigned i = 0; i < 2; i++)
                servers[i] = ip4_addr_get_u32(ip_2_ip4(dns_getserver(i)));
        mutex_unlock(&network_mutex);
        /* VFS is acquired only after releasing network_mutex. */
        if (memcmp(servers, written_dns, sizeof(servers)))
            write_resolver(servers);
        sched_sleep(1000);
    }
}

void network_probe(void *pci_header) {
    if (!ethernet_present() && !igc_probe(pci_header))
        e1000_probe(pci_header);
}

void network_start(void) {
    uint32_t seed;
    if (!random_available() || random_bytes(&seed, sizeof(seed)) != sizeof(seed)) {
        pr_warn("Network: secure entropy unavailable; sockets disabled\n");
        return;
    }
    lwip_init();
    if (ethernet_present()) {
        ip4_addr_t zero = {0};
        if (netif_add(&ethernet, &zero, &zero, &zero, NULL, interface_init, ethernet_input)) {
            have_ethernet = true;
            netif_set_default(&ethernet);
            netif_set_up(&ethernet);
            if (ethernet_link())
                netif_set_link_up(&ethernet);
            err_t error = dhcp_start(&ethernet);
            if (error)
                pr_warn("Network: DHCP startup failed (%d)\n", error);
        }
    }
    if (task_kernel("network", worker, NULL, 0) < 0) {
        pr_warn("Network: cannot create protocol worker\n");
        return;
    }
    ready = true;
    pr_info("Network: IPv4 TCP/UDP and loopback%s\n",
            have_ethernet ? "; DHCP enabled" : "; no supported Ethernet device");
}

static void tcp_error(void *argument, err_t error) {
    struct net_socket *socket = argument;
    bool refused = socket->connecting && error == ERR_RST;
    socket->tcp = NULL;
    socket->connecting = false;
    socket->error = refused ? E_CONNREFUSED : error_number(error);
    socket->eof = true;
}

static err_t tcp_received(void *argument, struct tcp_pcb *pcb, struct pbuf *packet, err_t error) {
    struct net_socket *socket = argument;
    if (error != ERR_OK)
        return error;
    if (!packet) {
        socket->eof = true;
        return ERR_OK;
    }
    if (socket->read_shutdown) {
        tcp_recved(pcb, packet->tot_len);
        pbuf_free(packet);
        return ERR_OK;
    }
    if (packet->tot_len > RX_SIZE - socket->count)
        return ERR_MEM;
    unsigned first = min((unsigned)packet->tot_len, RX_SIZE - socket->head);
    pbuf_copy_partial(packet, socket->bytes + socket->head, first, 0);
    pbuf_copy_partial(packet, socket->bytes, packet->tot_len - first, first);
    socket->head = (socket->head + packet->tot_len) % RX_SIZE;
    socket->count += packet->tot_len;
    pbuf_free(packet);
    /* Receive credit is returned on read(), not on arrival. */
    return ERR_OK;
}

static err_t tcp_connected(void *argument, struct tcp_pcb *pcb, err_t error) {
    (void)pcb;
    struct net_socket *socket = argument;
    socket->connecting = false;
    socket->connected = error == ERR_OK;
    socket->error = error_number(error);
    return ERR_OK;
}

static void udp_received(void *argument, struct udp_pcb *pcb, struct pbuf *packet,
                         const ip_addr_t *address, uint16_t port) {
    (void)pcb;
    struct net_socket *socket = argument;
    if (socket->read_shutdown || socket->queued == UDP_QUEUE ||
        packet->tot_len > RX_SIZE - socket->queued_bytes) {
        pbuf_free(packet);
        return;
    }
    unsigned index = (socket->first + socket->queued) % UDP_QUEUE;
    socket->datagrams[index] = (struct udp_packet){packet, *address, port};
    socket->queued++;
    socket->queued_bytes += packet->tot_len;
}

static struct net_socket *allocate_socket(int type, struct tcp_pcb *accepted) {
    struct net_socket *socket = kmallocz(sizeof(*socket));
    if (!socket)
        return NULL;
    socket->type = type;
    if (type == 1) {
        socket->bytes = kmalloc(RX_SIZE);
        socket->tcp = accepted ? accepted : tcp_new_ip_type(IPADDR_TYPE_V4);
        if (!socket->bytes || !socket->tcp) {
            if (!accepted && socket->tcp)
                tcp_abort(socket->tcp);
            kfree(socket->bytes);
            kfree(socket);
            return NULL;
        }
        tcp_arg(socket->tcp, socket);
        tcp_recv(socket->tcp, tcp_received);
        tcp_err(socket->tcp, tcp_error);
        socket->connected = accepted != NULL;
    } else {
        socket->udp = udp_new_ip_type(IPADDR_TYPE_V4);
        if (!socket->udp) {
            kfree(socket);
            return NULL;
        }
        udp_recv(socket->udp, udp_received, socket);
    }
    return socket;
}

static void destroy_socket(struct net_socket *socket) {
    if (socket->tcp) {
        tcp_arg(socket->tcp, NULL);
        if (!socket->listening) {
            tcp_recv(socket->tcp, NULL);
            tcp_err(socket->tcp, NULL);
            tcp_sent(socket->tcp, NULL);
        } else
            tcp_accept(socket->tcp, NULL);
        if (tcp_close(socket->tcp) != ERR_OK)
            tcp_abort(socket->tcp);
    }
    if (socket->udp)
        udp_remove(socket->udp);
    for (unsigned i = 0; i < socket->queued; i++)
        pbuf_free(socket->datagrams[(socket->first + i) % UDP_QUEUE].data);
    for (unsigned i = 0; i < socket->accept_count; i++)
        destroy_socket(socket->accepted[(socket->accept_first + i) % ACCEPT_QUEUE]);
    kfree(socket->bytes);
    kfree(socket);
}

static err_t accept_connection(void *argument, struct tcp_pcb *pcb, err_t error) {
    struct net_socket *parent = argument;
    if (error != ERR_OK)
        return error;
    if (parent->accept_count >= parent->backlog)
        return ERR_MEM;
    struct net_socket *child = allocate_socket(1, pcb);
    if (!child)
        return ERR_MEM;
    tcp_backlog_delayed(pcb);
    parent->accepted[(parent->accept_first + parent->accept_count) % ACCEPT_QUEUE] = child;
    parent->accept_count++;
    return ERR_OK;
}

void network_close(struct file *file) {
    mutex_lock(&network_mutex);
    destroy_socket(file->socket);
    file->socket = NULL;
    mutex_unlock(&network_mutex);
}

unsigned network_poll(struct file *file, unsigned events) {
    mutex_lock(&network_mutex);
    struct net_socket *socket = file->socket;
    unsigned result = 0;
    if (socket->error)
        result |= POLLERR;
    if (socket->type == 2 ||
        (socket->connected && !socket->connecting && socket->tcp && !socket->write_shutdown &&
         tcp_sndbuf(socket->tcp)) ||
        (!socket->connecting && socket->error))
        result |= events & POLLOUT;
    if (socket->count || socket->queued || socket->accept_count || socket->eof ||
        socket->read_shutdown)
        result |= events & POLLIN;
    if (socket->eof && !socket->count)
        result |= POLLHUP;
    if (socket->type == 1 && socket->eof)
        result |= events & POLLRDHUP;
    mutex_unlock(&network_mutex);
    return result;
}

static int interruption(void) {
    struct task *task = this_cpu()->current;
    unsigned signal = task_pending_signal(task);
    if (signal)
        return -E_INTR;
    return signal_pending(task) ? -E_INTR : 0;
}

static int wait_socket(struct file *file, unsigned flags, uint64_t deadline) {
    if ((file->flags & O_NONBLOCK) || (flags & MSG_DONTWAIT))
        return -E_AGAIN;
    int error = interruption();
    if (error)
        return error;
    if (deadline && tsc_uptime_us() >= deadline)
        return -E_AGAIN;
    sched_sleep(1000);
    return 0;
}

static long receive_data(struct file *file, void *data, size_t size, unsigned flags,
                         struct inet_address *from, size_t *full_size) {
    struct net_socket *socket = file->socket;
    uint64_t deadline = socket->receive_timeout ? tsc_uptime_us() + socket->receive_timeout : 0;
    for (;;) {
        mutex_lock(&network_mutex);
        long count = -E_AGAIN;
        if (socket->type == 2 && socket->queued) {
            struct udp_packet *packet = &socket->datagrams[socket->first];
            size_t length = packet->data->tot_len;
            *full_size = length;
            count = min(size, length);
            pbuf_copy_partial(packet->data, data, count, 0);
            if (from)
                *from = (struct inet_address){.family = 2,
                                              .port = lwip_htons(packet->port),
                                              .ip = ip4_addr_get_u32(ip_2_ip4(&packet->address))};
            if (!(flags & MSG_PEEK)) {
                pbuf_free(packet->data);
                socket->queued_bytes -= length;
                socket->first = (socket->first + 1) % UDP_QUEUE;
                socket->queued--;
            }
        } else if (socket->type == 1 && socket->count) {
            count = min(size, (size_t)socket->count);
            *full_size = count;
            size_t first = min((size_t)count, (size_t)(RX_SIZE - socket->tail));
            memcpy(data, socket->bytes + socket->tail, first);
            memcpy((uint8_t *)data + first, socket->bytes, count - first);
            if (!(flags & MSG_PEEK)) {
                socket->tail = (socket->tail + count) % RX_SIZE;
                socket->count -= count;
                if (socket->tcp)
                    tcp_recved(socket->tcp, count);
            }
            if (from && socket->tcp)
                *from = (struct inet_address){
                    .family = 2,
                    .port = lwip_htons(socket->tcp->remote_port),
                    .ip = ip4_addr_get_u32(ip_2_ip4(&socket->tcp->remote_ip))};
        } else if (socket->error)
            count = -socket->error;
        else if (socket->eof || socket->read_shutdown)
            count = 0;
        else if (socket->type == 1 && (!socket->connected || socket->listening))
            count = -E_NOTCONN;
        mutex_unlock(&network_mutex);
        if (count != -E_AGAIN)
            return count;
        int error = wait_socket(file, flags, deadline);
        if (error)
            return error;
    }
}

static long send_data(struct file *file, const void *data, size_t size, unsigned flags,
                      const struct inet_address *destination) {
    struct net_socket *socket = file->socket;
    uint64_t deadline = socket->send_timeout ? tsc_uptime_us() + socket->send_timeout : 0;
    for (;;) {
        mutex_lock(&network_mutex);
        long result;
        if (socket->write_shutdown)
            result = -E_PIPE;
        else if (socket->error)
            result = -socket->error;
        else if (socket->type == 2) {
            if (size > 65507)
                result = -E_MSGSIZE;
            else if (!destination && !socket->connected)
                result = -E_DESTADDRREQ;
            else {
                struct pbuf *packet = pbuf_alloc(PBUF_TRANSPORT, size, PBUF_RAM);
                if (!packet)
                    result = -E_NOMEM;
                else {
                    pbuf_take(packet, data, size);
                    ip_addr_t address;
                    if (destination) {
                        IP_SET_TYPE_VAL(address, IPADDR_TYPE_V4);
                        ip4_addr_set_u32(ip_2_ip4(&address), destination->ip);
                    }
                    err_t error = destination ? udp_sendto(socket->udp, packet, &address,
                                                           lwip_ntohs(destination->port))
                                              : udp_send(socket->udp, packet);
                    pbuf_free(packet);
                    result = error ? -error_number(error) : (long)size;
                }
            }
        } else if (!socket->tcp || !socket->connected || socket->listening)
            result = -E_NOTCONN;
        else if (!size)
            result = 0;
        else {
            size_t count = min(size, (size_t)tcp_sndbuf(socket->tcp));
            if (!count)
                result = -E_AGAIN;
            else {
                err_t error = tcp_write(socket->tcp, data, count, TCP_WRITE_FLAG_COPY);
                result = error == ERR_MEM ? -E_AGAIN : error ? -error_number(error) : (long)count;
                if (!error)
                    tcp_output(socket->tcp);
            }
        }
        mutex_unlock(&network_mutex);
        if (result != -E_AGAIN)
            return result;
        int error = wait_socket(file, flags, deadline);
        if (error)
            return error;
    }
}

long network_read(struct file *file, void *data, size_t size, unsigned flags) {
    if (!size && file->socket->type == 1)
        return 0;
    size_t full = 0;
    return receive_data(file, data, size, flags, NULL, &full);
}

long network_write(struct file *file, const void *data, size_t size, unsigned flags) {
    return send_data(file, data, size, flags, NULL);
}

long network_ioctl(struct file *file, unsigned long request, void *value) {
    if (request == 0x5421) { /* FIONBIO */
        if (*(int *)value)
            file->flags |= O_NONBLOCK;
        else
            file->flags &= ~O_NONBLOCK;
        return 0;
    }
    if (request != FIONREAD)
        return -E_NOTTY;
    mutex_lock(&network_mutex);
    struct net_socket *socket = file->socket;
    *(int *)value = socket->type == 2
                        ? (socket->queued ? socket->datagrams[socket->first].data->tot_len : 0)
                        : socket->count;
    mutex_unlock(&network_mutex);
    return 0;
}

/* Linux x86_64 socket syscall marshaling is below. */
static int read_address(struct task *task, uint64_t pointer, uint64_t length,
                        struct inet_address *address, bool disconnect) {
    if (length < sizeof(uint16_t))
        return -E_INVAL;
    uint16_t family;
    if (!user_copy_in(&task->space, &family, pointer, sizeof(family)))
        return -E_FAULT;
    if (disconnect && !family) {
        memset(address, 0, sizeof(*address));
        return 0;
    }
    if (family != 2)
        return -E_AFNOSUPPORT;
    if (length < sizeof(*address))
        return -E_INVAL;
    if (!user_copy_in(&task->space, address, pointer, sizeof(*address)))
        return -E_FAULT;
    return 0;
}

static int address_output(struct task *task, uint64_t pointer, uint64_t length_pointer,
                          uint32_t *length) {
    if (!pointer) {
        *length = 0;
        return 0;
    }
    if (!user_copy_in(&task->space, length, length_pointer, sizeof(*length)) ||
        !user_access(&task->space, length_pointer, sizeof(*length), true) ||
        !user_access(&task->space, pointer, min((size_t)*length, sizeof(struct inet_address)),
                     true))
        return -E_FAULT;
    return 0;
}

static int put_address(struct task *task, uint64_t pointer, uint64_t length_pointer,
                       uint32_t length, const struct inet_address *address) {
    if (!pointer)
        return 0;
    uint32_t actual = sizeof(*address);
    if (!user_copy_out(&task->space, pointer, address, min((size_t)length, sizeof(*address))) ||
        !user_copy_out(&task->space, length_pointer, &actual, sizeof(actual)))
        return -E_FAULT;
    return 0;
}

static int install_socket(struct task *task, struct net_socket *socket, unsigned flags) {
    mutex_lock(&task->context->syscall_lock);
    mutex_lock(&vfs_mutex);

    unsigned fd = 0;
    while (fd < OS_FD_MAX && task->fds[fd])
        fd++;
    int error =
        fd == OS_FD_MAX ? -E_MFILE : vfs_socket(socket, flags & SOCK_NONBLOCK, &task->fds[fd]);
    if (!error)
        task->fd_flags[fd] = flags & SOCK_CLOEXEC ? FD_CLOEXEC : 0;

    mutex_unlock(&vfs_mutex);
    mutex_unlock(&task->context->syscall_lock);

    if (error) {
        mutex_lock(&network_mutex);
        destroy_socket(socket);
        mutex_unlock(&network_mutex);
    }

    return error ? error : (int)fd;
}

static int connect_socket(struct file *file, const struct inet_address *address) {
    struct net_socket *socket = file->socket;
    mutex_lock(&network_mutex);
    if (socket->type == 2 && !address->family) {
        udp_disconnect(socket->udp);
        socket->connected = false;
        mutex_unlock(&network_mutex);
        return 0;
    }
    if (socket->connected) {
        mutex_unlock(&network_mutex);
        return -E_ISCONN;
    }
    if (socket->connecting) {
        mutex_unlock(&network_mutex);
        return -E_ALREADY;
    }
    if (socket->listening || !address->port) {
        mutex_unlock(&network_mutex);
        return -E_INVAL;
    }

    ip_addr_t remote;
    IP_SET_TYPE_VAL(remote, IPADDR_TYPE_V4);
    ip4_addr_set_u32(ip_2_ip4(&remote), address->ip);
    socket->error = 0;
    socket->eof = false;
    err_t error = socket->type == 2 ? udp_connect(socket->udp, &remote, lwip_ntohs(address->port))
                  : socket->tcp
                      ? tcp_connect(socket->tcp, &remote, lwip_ntohs(address->port), tcp_connected)
                      : ERR_CONN;
    if (!error) {
        if (socket->type == 2)
            socket->connected = true;
        else
            socket->connecting = true;
    }
    mutex_unlock(&network_mutex);
    if (error)
        return -error_number(error);
    if (socket->type == 2)
        return 0;
    if (file->flags & O_NONBLOCK)
        return -E_INPROGRESS;
    uint64_t deadline = tsc_uptime_us() + (socket->send_timeout ? socket->send_timeout : 120000000);
    for (;;) {
        mutex_lock(&network_mutex);
        bool done = !socket->connecting;
        int result = socket->error ? -socket->error : 0;
        if (tsc_uptime_us() >= deadline && !done) {
            tcp_abort(socket->tcp); /* tcp_error owns the resulting state. */
            socket->error = E_TIMEDOUT;
            result = -E_TIMEDOUT;
            done = true;
        }
        mutex_unlock(&network_mutex);
        if (done)
            return result;
        int interrupted = interruption();
        if (interrupted)
            return interrupted;
        sched_sleep(1000);
    }
}

static bool local_address(uint32_t address) {
    ip4_addr_t ip = {.addr = address};
    return !address || ip4_addr_isloopback(&ip) ||
           (have_ethernet && address == ip4_addr_get_u32(netif_ip4_addr(&ethernet)));
}

static int bind_socket(struct net_socket *socket, const struct inet_address *address) {
    if (!local_address(address->ip))
        return -E_ADDRNOTAVAIL;
    ip_addr_t local;
    IP_SET_TYPE_VAL(local, IPADDR_TYPE_V4);
    ip4_addr_set_u32(ip_2_ip4(&local), address->ip);
    mutex_lock(&network_mutex);
    err_t error = socket->type == 2 ? udp_bind(socket->udp, &local, lwip_ntohs(address->port))
                  : socket->tcp && !socket->connected && !socket->connecting && !socket->listening
                      ? tcp_bind(socket->tcp, &local, lwip_ntohs(address->port))
                      : ERR_VAL;
    mutex_unlock(&network_mutex);
    return -error_number(error);
}

static int listen_socket(struct net_socket *socket, unsigned backlog) {
    if (socket->type != 1)
        return -E_NOTSUP;
    mutex_lock(&network_mutex);
    int result = 0;
    if (socket->connected || socket->connecting || !socket->tcp)
        result = -E_INVAL;
    else if (!socket->listening) {
        err_t error = ERR_OK;
        struct tcp_pcb *pcb = tcp_listen_with_backlog_and_err(
            socket->tcp, min(backlog ? backlog : 1u, ACCEPT_QUEUE), &error);
        if (!pcb)
            result = -error_number(error);
        else {
            socket->tcp = pcb;
            socket->listening = true;
            tcp_arg(pcb, socket);
            tcp_accept(pcb, accept_connection);
        }
    }
    socket->backlog = min(backlog ? backlog : 1u, ACCEPT_QUEUE);
    mutex_unlock(&network_mutex);
    return result;
}

static long accept_socket(struct task *task, struct file *file, uint64_t pointer,
                          uint64_t length_pointer, unsigned flags) {
    struct net_socket *socket = file->socket;
    uint32_t length;
    if (flags & ~(SOCK_CLOEXEC | SOCK_NONBLOCK))
        return -E_INVAL;
    int error = address_output(task, pointer, length_pointer, &length);
    if (error)
        return error;
    if (!socket->listening)
        return -E_INVAL;
    uint64_t deadline = socket->receive_timeout ? tsc_uptime_us() + socket->receive_timeout : 0;
    for (;;) {
        mutex_lock(&network_mutex);
        struct net_socket *child = NULL;
        struct inet_address address = {0};
        if (socket->accept_count) {
            child = socket->accepted[socket->accept_first];
            socket->accept_first = (socket->accept_first + 1) % ACCEPT_QUEUE;
            socket->accept_count--;
            if (child->tcp) {
                tcp_backlog_accepted(child->tcp);
                address =
                    (struct inet_address){.family = 2,
                                          .port = lwip_htons(child->tcp->remote_port),
                                          .ip = ip4_addr_get_u32(ip_2_ip4(&child->tcp->remote_ip))};
            }
        }
        mutex_unlock(&network_mutex);
        if (child) {
            error = put_address(task, pointer, length_pointer, length, &address);
            if (error) {
                mutex_lock(&network_mutex);
                destroy_socket(child);
                mutex_unlock(&network_mutex);
                return error;
            }
            return install_socket(task, child, flags);
        }
        error = wait_socket(file, 0, deadline);
        if (error)
            return error;
    }
}

static long name_socket(struct task *task, struct net_socket *socket, bool peer, uint64_t pointer,
                        uint64_t length_pointer) {
    if (!pointer)
        return -E_FAULT;
    uint32_t length;
    int error = address_output(task, pointer, length_pointer, &length);
    if (error)
        return error;
    mutex_lock(&network_mutex);
    struct inet_address address = {.family = 2};
    if (peer && !socket->connected)
        error = -E_NOTCONN;
    else if (socket->type == 2) {
        address.port = lwip_htons(peer ? socket->udp->remote_port : socket->udp->local_port);
        address.ip =
            ip4_addr_get_u32(ip_2_ip4(peer ? &socket->udp->remote_ip : &socket->udp->local_ip));
    } else if (socket->tcp) {
        /* Listening PCBs share only the common IP/local-port prefix. */
        address.port = lwip_htons(peer ? socket->tcp->remote_port : socket->tcp->local_port);
        address.ip =
            ip4_addr_get_u32(ip_2_ip4(peer ? &socket->tcp->remote_ip : &socket->tcp->local_ip));
    } else
        error = -E_NOTCONN;
    mutex_unlock(&network_mutex);
    return error ? error : put_address(task, pointer, length_pointer, length, &address);
}

static long socket_option(struct task *task, struct net_socket *socket, register_t *registers,
                          bool set) {
    uint32_t length;
    if (set)
        length = registers->r8;
    else if (!user_copy_in(&task->space, &length, registers->r8, sizeof(length)) ||
             !user_access(&task->space, registers->r8, sizeof(length), true))
        return -E_FAULT;
    int level = registers->rsi, option = registers->rdx, value = 0;
    bool timeout = level == 1 && (option == 20 || option == 21);
    struct os_timeval time = {0};
    size_t required = timeout ? sizeof(time) : sizeof(value);
    if (set) {
        if (length < required)
            return -E_INVAL;
        if (!user_copy_in(&task->space, timeout ? (void *)&time : (void *)&value, registers->r10,
                          required))
            return -E_FAULT;
    } else if (!user_access(&task->space, registers->r10, min((size_t)length, required), true))
        return -E_FAULT;
    mutex_lock(&network_mutex);
    int error = 0;
    if (timeout) {
        uint64_t *setting = option == 20 ? &socket->receive_timeout : &socket->send_timeout;
        if (set) {
            if (time.sec < 0 || time.sec > 86400 || time.usec < 0 || time.usec >= 1000000)
                error = -E_INVAL;
            else
                *setting = time.sec * 1000000 + time.usec;
        } else
            time = (struct os_timeval){*setting / 1000000, *setting % 1000000};
    } else if (level == 1 && (option == 3 || option == 4)) {
        if (set)
            error = -E_NOPROTOOPT;
        else if (option == 3)
            value = socket->type;
        else {
            value = socket->error;
            socket->error = 0;
        }
    } else if (level == 1 && (option == 7 || option == 8)) {
        if (set && value <= 0)
            error = -E_INVAL;
        /* Bounded fixed buffers: getsockopt always reports the actual size. */
        value = RX_SIZE;
    } else if (level == 1 && (option == 2 || option == 6 || option == 9)) {
        struct ip_pcb *pcb =
            socket->type == 2 ? (struct ip_pcb *)socket->udp : (struct ip_pcb *)socket->tcp;
        unsigned bit = option == 2 ? SOF_REUSEADDR : option == 6 ? SOF_BROADCAST : SOF_KEEPALIVE;
        if (!pcb)
            error = -E_NOTCONN;
        else if (set) {
            if (value)
                ip_set_option(pcb, bit);
            else
                ip_reset_option(pcb, bit);
        } else
            value = !!ip_get_option(pcb, bit);
    } else if (level == 6 && option >= 1 && option <= 6 && socket->type == 1 &&
               (option == 1 || option == 4 || option == 5 || option == 6)) {
        if (!socket->tcp || socket->listening)
            error = -E_INVAL;
        else if (option == 1) {
            if (set) {
                if (value)
                    tcp_nagle_disable(socket->tcp);
                else
                    tcp_nagle_enable(socket->tcp);
            } else
                value = !!tcp_nagle_disabled(socket->tcp);
        } else {
            uint32_t *setting = option == 4   ? &socket->tcp->keep_idle
                                : option == 5 ? &socket->tcp->keep_intvl
                                              : &socket->tcp->keep_cnt;
            if (set) {
                if (value <= 0 || value > (option == 6 ? 255 : 86400))
                    error = -E_INVAL;
                else
                    *setting = option == 6 ? value : value * 1000;
            } else
                value = option == 6 ? *setting : *setting / 1000;
        }
    } else
        error = -E_NOPROTOOPT;
    mutex_unlock(&network_mutex);
    if (!error && !set) {
        uint32_t actual = required;
        if (!user_copy_out(&task->space, registers->r10, timeout ? (void *)&time : (void *)&value,
                           min((size_t)length, required)) ||
            !user_copy_out(&task->space, registers->r8, &actual, sizeof(actual)))
            return -E_FAULT;
    }
    return error;
}

struct message_header {
    uint64_t name;
    uint32_t name_length, padding;
    uint64_t vectors, vector_count, control, control_length;
    uint32_t flags, padding2;
};

static long transfer_call(struct task *task, struct file *file, register_t *registers, bool send,
                          unsigned vector) {
    unsigned flags = vector == 2 ? 0 : vector ? registers->rdx : registers->r10;
    if (flags & ~(MSG_PEEK | MSG_TRUNC | MSG_DONTWAIT | MSG_WAITALL | MSG_NOSIGNAL))
        return -E_NOTSUP;
    if (send && (flags & (MSG_PEEK | MSG_TRUNC | MSG_WAITALL)))
        return -E_INVAL;
    if ((flags & (MSG_PEEK | MSG_WAITALL)) == (MSG_PEEK | MSG_WAITALL))
        return -E_NOTSUP;

    struct message_header message = {0};
    struct os_iovec vectors[16];
    size_t total = 0;
    uint64_t name_pointer = registers->r8, length_pointer = registers->r9;
    uint32_t name_length = registers->r9;
    unsigned count = 1;

    if (vector == 2) {
        if (registers->rdx > 16)
            return -E_INVAL;
        count = registers->rdx;
        if (!user_copy_in(&task->space, vectors, registers->rsi, count * sizeof(vectors[0])))
            return -E_FAULT;
        name_pointer = 0;
        name_length = 0;
    } else if (vector) {
        if (!user_copy_in(&task->space, &message, registers->rsi, sizeof(message)))
            return -E_FAULT;
        if (message.vector_count > 16)
            return -E_MSGSIZE;
        if (send && message.control_length)
            return -E_NOTSUP;
        if (!send && !user_access(&task->space, registers->rsi, sizeof(message), true))
            return -E_FAULT;
        count = message.vector_count;
        if (!user_copy_in(&task->space, vectors, message.vectors, count * sizeof(vectors[0])))
            return -E_FAULT;
        name_pointer = message.name;
        name_length = message.name_length;
    } else
        vectors[0] = (struct os_iovec){registers->rsi, registers->rdx};

    for (unsigned i = 0; i < count; i++) {
        if (vectors[i].len > IO_LIMIT - total)
            return -E_MSGSIZE;
        if (!user_access(&task->space, vectors[i].base, vectors[i].len, !send))
            return -E_FAULT;
        total += vectors[i].len;
    }

    struct inet_address address = {0};
    int error = 0;
    if (send && name_pointer)
        error = read_address(task, name_pointer, name_length, &address, false);
    else if (!send && name_pointer) {
        if (vector) {
            if (!user_access(&task->space, name_pointer, min((size_t)name_length, sizeof(address)),
                             true))
                error = -E_FAULT;
        } else
            error = address_output(task, name_pointer, length_pointer, &name_length);
    }
    if (error)
        return error;
    if (file->socket->type == 2 && send && total > 65507)
        return -E_MSGSIZE;

    size_t capacity = min(total, (size_t)65535);
    uint8_t *buffer = kmalloc(capacity ? capacity : 1);
    if (!buffer)
        return -E_NOMEM;

    if (send) {
        size_t offset = 0;
        for (unsigned i = 0; i < count && offset < capacity; i++) {
            size_t length = min((size_t)vectors[i].len, capacity - offset);
            if (!user_copy_in(&task->space, buffer + offset, vectors[i].base, length)) {
                kfree(buffer);
                return -E_FAULT;
            }
            offset += length;
        }
    }

    size_t full = 0, done = 0;
    long result;
    if (!total && file->socket->type == 1)
        result = 0;
    else if (send)
        result = send_data(file, buffer, capacity, flags, name_pointer ? &address : NULL);
    else {
        do {
            result = receive_data(file, buffer + done, capacity - done, flags, &address, &full);
            if (result <= 0)
                break;
            done += result;
        } while (file->socket->type == 1 && (flags & MSG_WAITALL) && done < capacity &&
                 !(flags & MSG_DONTWAIT));
        if (done)
            result = done;
        if (result >= 0) {
            size_t offset = 0;
            for (unsigned i = 0; i < count && offset < (size_t)result; i++) {
                size_t length = min((size_t)vectors[i].len, (size_t)result - offset);
                if (!user_copy_out(&task->space, vectors[i].base, buffer + offset, length)) {
                    result = -E_FAULT;
                    break;
                }
                offset += length;
            }
            if (result >= 0) {
                if (vector == 1) {
                    if (name_pointer && !user_copy_out(&task->space, name_pointer, &address,
                                                       min((size_t)name_length, sizeof(address))))
                        result = -E_FAULT;
                    message.name_length = name_pointer ? sizeof(address) : 0;
                    message.control_length = 0;
                    message.flags = full > capacity ? MSG_TRUNC : 0;
                    if (!user_copy_out(&task->space, registers->rsi, &message, sizeof(message)))
                        result = -E_FAULT;
                } else
                    error = put_address(task, name_pointer, length_pointer, name_length, &address);
                if (error)
                    result = error;
                else if (result >= 0 && (flags & MSG_TRUNC) && file->socket->type == 2)
                    result = full;
            }
        }
    }
    kfree(buffer);
    return result;
}

long network_syscall(void *task_pointer, void *register_pointer) {
    struct task *task = task_pointer;
    register_t *registers = register_pointer;
    unsigned call = registers->rax;
    if (call == SYS_socketpair)
        return unix_socketpair(task, registers);
    if (call == SYS_socket) {
        if (!ready)
            return -E_NETDOWN;
        if (registers->rdi != 2)
            return -E_AFNOSUPPORT;
        unsigned type = registers->rsi & ~(SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (type != 1 && type != 2)
            return -E_PROTONOSUPPORT;
        if (registers->rdx && registers->rdx != (type == 1 ? 6u : 17u))
            return -E_PROTONOSUPPORT;
        mutex_lock(&network_mutex);
        struct net_socket *socket = allocate_socket(type, NULL);
        mutex_unlock(&network_mutex);
        if (!socket)
            return -E_NOMEM;
        return install_socket(task, socket, registers->rsi);
    }
    struct file *file = task_file_pin(task, registers->rdi);
    if (!file)
        return -E_BADF;
    return network_file_syscall(task, registers, file);
}

long network_file_syscall(void *task_pointer, void *register_pointer, struct file *file) {
    struct task *task = task_pointer;
    register_t *registers = register_pointer;
    unsigned call = registers->rax;
    if (file->unix_socket)
        return unix_file_syscall(task, registers, file);
    if (!ready)
        return -E_NETDOWN;
    if (!file->socket)
        return -E_NOTSOCK;
    struct net_socket *socket = file->socket;
    switch (call) {
    case SYS_connect:
    case SYS_bind: {
        struct inet_address address;
        int error = read_address(task, registers->rsi, registers->rdx, &address,
                                 call == SYS_connect && socket->type == 2);
        return error                 ? error
               : call == SYS_connect ? connect_socket(file, &address)
                                     : bind_socket(socket, &address);
    }

    case SYS_listen:
        return listen_socket(socket, (int)registers->rsi < 0 ? ACCEPT_QUEUE : registers->rsi);

    case SYS_accept:
    case SYS_accept4:
        return accept_socket(task, file, registers->rsi, registers->rdx,
                             call == SYS_accept4 ? registers->r10 : 0);

    case SYS_getsockname:
    case SYS_getpeername:
        return name_socket(task, socket, call == SYS_getpeername, registers->rsi, registers->rdx);

    case SYS_setsockopt:
    case SYS_getsockopt:
        return socket_option(task, socket, registers, call == SYS_setsockopt);

    case SYS_sendto:
    case SYS_recvfrom:
    case SYS_sendmsg:
    case SYS_recvmsg:
        return transfer_call(task, file, registers, call == SYS_sendto || call == SYS_sendmsg,
                             call == SYS_sendmsg || call == SYS_recvmsg);

    case SYS_readv:
    case SYS_writev:
        return transfer_call(task, file, registers, call == SYS_writev, 2);

    case SYS_shutdown: {
        if (registers->rsi > 2)
            return -E_INVAL;
        mutex_lock(&network_mutex);
        int error = 0;
        if (!socket->connected)
            error = -E_NOTCONN;
        else {
            bool read = registers->rsi != 1, write = registers->rsi != 0;
            if (socket->type == 1 && socket->tcp)
                error = -error_number(tcp_shutdown(socket->tcp, read, write));
            if (!error) {
                socket->read_shutdown |= read;
                socket->write_shutdown |= write;
            }
        }
        mutex_unlock(&network_mutex);
        return error;
    }

    default:
        return -E_NOSYS;
    }
}

size_t network_info(char *out, size_t capacity) {
    mutex_lock(&network_mutex);
    char address[16] = "unavailable", gateway[16] = "unavailable", mask[16] = "unavailable";
    if (have_ethernet) {
        ip4addr_ntoa_r(netif_ip4_addr(&ethernet), address, sizeof(address));
        ip4addr_ntoa_r(netif_ip4_gw(&ethernet), gateway, sizeof(gateway));
        ip4addr_ntoa_r(netif_ip4_netmask(&ethernet), mask, sizeof(mask));
    }
    int count = snprintf(
        out, capacity,
        "IPv4 sockets: %s\nEthernet: %s\nAddress: %s\nNetmask: %s\nGateway: %s\n"
        "Packets: rx=%llu tx=%llu dropped=%llu\nIPv6, Wi-Fi and unsupported NICs: unavailable\n",
        ready ? "TCP/UDP, loopback" : "unavailable",
        have_ethernet ? (netif_is_link_up(&ethernet) ? "link up; DHCP" : "link down")
                      : "no supported device",
        address, mask, gateway, (unsigned long long)received_packets,
        (unsigned long long)transmitted_packets, (unsigned long long)dropped_packets);
    mutex_unlock(&network_mutex);
    return count < 0 ? 0 : min((size_t)count, capacity - 1);
}
