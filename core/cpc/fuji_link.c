#define _POSIX_C_SOURCE 200809L

#include "fuji_link.h"

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

enum {
    SLIP_END = 0xc0,
    SLIP_ESCAPE = 0xdb,
    SLIP_ESC_END = 0xdc,
    SLIP_ESC_ESCAPE = 0xdd,
    FUJIBUS_HEADER = 6,
    FUJIBUS_ACK = 0x06,
    FUJI_DEVICE = 0x70,
    FUJI_READ_HOST_SLOTS = 0xf4,
    HOST_SLOTS = 8,
    HOST_SLOT_BYTES = 32,
    NETWORK_DEVICE = 0x71,
    NETWORK_INPUT = 4,
    NETWORK_DIRECTORY = 6,
    NETWORK_OPEN = 'O',
    NETWORK_STATUS = 'S',
    NETWORK_READ = 'R',
    NETWORK_CLOSE = 'C',
    NETWORK_OK = 1,
    NETWORK_EOF = 0x88,
    // A directory newline can expand to two characters.
    DIRECTORY_READ_LIMIT = (CPC_FUJI_CAPACITY - 1) / 2,
    FILE_READ_LIMIT = CPC_FUJI_CAPACITY - 1,
    PACKET_CAPACITY = FUJIBUS_HEADER + HOST_SLOTS * HOST_SLOT_BYTES + 1,
    REQUEST_MS = 3800
};

typedef enum {
    STREAM_CLOSED,
    STREAM_DIRECTORY,
    STREAM_FILE
} stream_kind;

typedef struct {
    int fd;
    uint16_t port;
    stream_kind stream;
    size_t file_bytes_read;
    bool after_cr;
} connection;

typedef struct {
    uint64_t ticket;
    uint8_t command;
    bool timed_out;
    size_t size;
    uint8_t data[CPC_FUJI_CAPACITY];
} link_reply;

struct cpc_fuji_link {
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t wake;
    connection conn;
    bool stopping;
    bool reset;
    bool pending;
    bool complete;
    cpc_fuji_request request;
    link_reply reply;
    cpc_fuji_listing listing;
    uint64_t traced_ticket;
    cpc_fuji_trace_fn trace;
    void *trace_data;
};

static int64_t milliseconds(void)
{
    struct timespec time;
    clock_gettime(CLOCK_MONOTONIC, &time);
    return (int64_t)time.tv_sec * 1000 + time.tv_nsec / 1000000;
}

static bool socket_ready(int fd, short events, int64_t deadline)
{
    for (;;) {
        int64_t remaining = deadline - milliseconds();
        if (remaining <= 0) {
            return false;
        }
        struct pollfd poll_fd = { .fd = fd, .events = events };
        int result = poll(&poll_fd, 1, (int)remaining);
        if (result >= 0 || errno != EINTR) {
            return result > 0 && (poll_fd.revents & events) != 0;
        }
    }
}

static void disconnect(connection *conn)
{
    if (conn->fd >= 0) {
        close(conn->fd);
        conn->fd = -1;
    }
    conn->stream = STREAM_CLOSED;
    conn->after_cr = false;
}

static bool connect_loopback(connection *conn, int64_t deadline)
{
    if (conn->fd >= 0) {
        return true;
    }
    if (!conn->port) {
        return false;
    }
    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons(conn->port),
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK)
    };
    do {
        int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd < 0) {
            return false;
        }
        int error = 0;
        if (connect(fd, (const struct sockaddr *)&address, sizeof(address)) < 0) {
            error = errno;
            if (error == EINPROGRESS && socket_ready(fd, POLLOUT, deadline)) {
                socklen_t size = sizeof(error);
                if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) < 0) {
                    error = errno;
                }
            }
        }
        if (!error) {
            conn->fd = fd;
            return true;
        }
        close(fd);
        if (error != ECONNREFUSED) {
            return false;
        }
        // FujiNet-PC starts its BoIP listener after its Wi-Fi layer initializes.
        struct timespec delay = { .tv_nsec = 50000000 };
        nanosleep(&delay, NULL);
    } while (milliseconds() < deadline);
    return false;
}

static uint8_t checksum(const uint8_t *raw, size_t size)
{
    unsigned sum = 0;
    for (size_t i = 0; i < size; ++i) {
        // The checksum field is zero when calculating the sum.
        if (i != 4) {
            sum += raw[i];
            sum = (sum >> 8) + (sum & 0xff);
        }
    }
    return (uint8_t)sum;
}

static bool write_frame(int fd, uint8_t *raw, size_t size, int64_t deadline)
{
    uint8_t encoded[2 * PACKET_CAPACITY + 2];
    raw[2] = (uint8_t)size;
    raw[3] = (uint8_t)(size >> 8);
    raw[4] = checksum(raw, size);
    size_t count = 0;
    encoded[count++] = SLIP_END;
    for (size_t i = 0; i < size; ++i) {
        if (raw[i] == SLIP_END || raw[i] == SLIP_ESCAPE) {
            encoded[count++] = SLIP_ESCAPE;
            encoded[count++] = raw[i] == SLIP_END ? SLIP_ESC_END : SLIP_ESC_ESCAPE;
        } else {
            encoded[count++] = raw[i];
        }
    }
    encoded[count++] = SLIP_END;

    size_t offset = 0;
    while (offset < count && socket_ready(fd, POLLOUT, deadline)) {
        ssize_t wrote = write(fd, encoded + offset, count - offset);
        if (wrote > 0) {
            offset += (size_t)wrote;
        } else if (wrote == 0 || (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)) {
            return false;
        }
    }
    return offset == count;
}

static size_t read_frame(int fd, uint8_t raw[PACKET_CAPACITY], int64_t deadline)
{
    bool started = false;
    bool escaped = false;
    size_t size = 0;
    while (socket_ready(fd, POLLIN, deadline)) {
        uint8_t byte;
        ssize_t count = read(fd, &byte, 1);
        if (count != 1) {
            if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
                continue;
            }
            break;
        }
        if (!started) {
            started = byte == SLIP_END;
        } else if (escaped) {
            if (byte != SLIP_ESC_END && byte != SLIP_ESC_ESCAPE) {
                break;
            }
            raw[size++] = byte == SLIP_ESC_END ? SLIP_END : SLIP_ESCAPE;
            escaped = false;
        } else if (byte == SLIP_END) {
            if (size != 0) {
                return size;
            }
        } else if (byte == SLIP_ESCAPE) {
            escaped = true;
        } else {
            raw[size++] = byte;
        }
        if (size == PACKET_CAPACITY) {
            break;
        }
    }
    return 0;
}

// RS232 sends descriptor-free ACK packets.
static bool exchange(connection *conn, uint8_t device, uint8_t raw[PACKET_CAPACITY], size_t *size,
                     int64_t deadline)
{
    raw[0] = device;
    if (!write_frame(conn->fd, raw, *size, deadline)) {
        return false;
    }
    *size = read_frame(conn->fd, raw, deadline);
    return *size >= FUJIBUS_HEADER && raw[0] == device &&
           raw[1] == FUJIBUS_ACK && (size_t)(raw[2] | (raw[3] << 8)) == *size &&
           raw[4] == checksum(raw, *size) && raw[5] == 0;
}

static bool close_stream(connection *conn, int64_t deadline)
{
    uint8_t raw[PACKET_CAPACITY] = { [1] = NETWORK_CLOSE };
    size_t size = FUJIBUS_HEADER;
    conn->stream = STREAM_CLOSED;
    return exchange(conn, NETWORK_DEVICE, raw, &size, deadline) && size == FUJIBUS_HEADER;
}

static bool valid_target(const uint8_t *target, size_t count, bool file)
{
    if (count == 0 || target[0] == '/') {
        return false;
    }
    bool in_path = false;
    for (size_t i = 0; i < count; ++i) {
        uint8_t ch = target[i];
        if (ch < 0x20 || ch > 0x7e || ch == '?' || ch == '#' || ch == '*' ||
            (!in_path && (ch == ' ' || ch == '@'))) {
            return false;
        }
        in_path |= ch == '/';
    }
    return !file || (in_path && target[count - 1] != '/');
}

static size_t read_saved_host(connection *conn, unsigned slot, uint8_t target[HOST_SLOT_BYTES],
                              int64_t deadline)
{
    if (!connect_loopback(conn, deadline)) {
        return 0;
    }
    uint8_t raw[PACKET_CAPACITY] = { [1] = FUJI_READ_HOST_SLOTS };
    size_t size = FUJIBUS_HEADER;
    if (!exchange(conn, FUJI_DEVICE, raw, &size, deadline) ||
        size != FUJIBUS_HEADER + HOST_SLOTS * HOST_SLOT_BYTES) {
        return 0;
    }
    unsigned first = slot ? slot - 1 : 0;
    unsigned end = slot ? slot : HOST_SLOTS;
    for (unsigned i = first; i < end; ++i) {
        char *host = (char *)raw + FUJIBUS_HEADER + i * HOST_SLOT_BYTES;
        size_t count = strnlen(host, HOST_SLOT_BYTES);
        if (!count || count == HOST_SLOT_BYTES || strcmp(host, "SD") == 0) {
            continue;
        }
        static const char scheme[] = "TNFS://";
        if (strncasecmp(host, scheme, sizeof(scheme) - 1) == 0) {
            host += sizeof(scheme) - 1;
            count -= sizeof(scheme) - 1;
        }
        if (!strstr(host, "://") && valid_target((uint8_t *)host, count, false)) {
            memcpy(target, host, count);
            return count;
        }
    }
    if (!slot) {
        static const char official_host[] = "tnfs.fujinet.online";
        memcpy(target, official_host, sizeof(official_host) - 1);
        return sizeof(official_host) - 1;
    }
    return 0;
}

static bool open_stream(connection *conn, const uint8_t *target, size_t count,
                        stream_kind stream, int64_t deadline)
{
    bool directory = stream == STREAM_DIRECTORY;
    if (!valid_target(target, count, !directory) ||
        !connect_loopback(conn, deadline) || !close_stream(conn, deadline)) {
        return false;
    }

    static const char prefix[] = "N1:TNFS://";
    uint8_t raw[PACKET_CAPACITY] = { [1] = NETWORK_OPEN, [5] = 2,
                                    [6] = directory ? NETWORK_DIRECTORY : NETWORK_INPUT };
    size_t size = FUJIBUS_HEADER + 2;
    memcpy(raw + size, prefix, sizeof(prefix) - 1);
    size += sizeof(prefix) - 1;
    memcpy(raw + size, target, count);
    size += count;
    // A trailing slash keeps directory mode from treating the last component as a glob.
    if (directory && target[count - 1] != '/') {
        raw[size++] = '/';
    }
    raw[size++] = 0;
    if (!exchange(conn, NETWORK_DEVICE, raw, &size, deadline) || size != FUJIBUS_HEADER) {
        return false;
    }
    conn->stream = stream;
    conn->file_bytes_read = 0;
    conn->after_cr = false;
    return true;
}

static bool open_directory(connection *conn, const cpc_fuji_request *request,
                           link_reply *reply, int64_t deadline)
{
    const uint8_t *target = request->data + 1;
    size_t count = request->size - 1;
    uint8_t saved[HOST_SLOT_BYTES];
    if (!count || (count == 2 && target[0] == '@' &&
                   target[1] >= '1' && target[1] <= '8')) {
        unsigned slot = count ? (unsigned)(target[1] - '0') : 0;
        count = read_saved_host(conn, slot, saved, deadline);
        target = saved;
    }
    if (!open_stream(conn, target, count, STREAM_DIRECTORY, deadline)) {
        return false;
    }
    memcpy(reply->data + 1, target, count);
    reply->size += count;
    return true;
}

static void append_listing(connection *conn, link_reply *reply,
                           const uint8_t *bytes, size_t size)
{
    for (size_t i = 0; i < size; ++i) {
        uint8_t byte = bytes[i];
        if (byte == '\r' || byte == '\n' || byte == 0x9b) {
            if (byte != '\n' || !conn->after_cr) {
                reply->data[reply->size++] = '\r';
                reply->data[reply->size++] = '\n';
            }
        } else {
            reply->data[reply->size++] = byte >= 0x20 && byte <= 0x7e ? byte : '?';
        }
        conn->after_cr = byte == '\r';
    }
}

static bool next_chunk(connection *conn, stream_kind stream, link_reply *reply,
                       int64_t deadline)
{
    bool directory = stream == STREAM_DIRECTORY;
    unsigned limit = directory ? DIRECTORY_READ_LIMIT : FILE_READ_LIMIT;
    while (conn->stream == stream && milliseconds() < deadline) {
        uint8_t raw[PACKET_CAPACITY] = { [1] = NETWORK_STATUS };
        size_t size = FUJIBUS_HEADER;
        if (!exchange(conn, NETWORK_DEVICE, raw, &size, deadline) || size != FUJIBUS_HEADER + 4) {
            return false;
        }
        unsigned waiting = raw[6] | (raw[7] << 8);
        if (raw[9] != NETWORK_OK && raw[9] != NETWORK_EOF) {
            return false;
        }
        if (waiting) {
            if (!directory && waiting > CPC_FUJI_PROGRAM_LIMIT - conn->file_bytes_read) {
                close_stream(conn, deadline);
                return false;
            }
            unsigned read_size = waiting < limit ? waiting : limit;
            raw[1] = NETWORK_READ;
            raw[5] = 5; // One 16-bit parameter.
            raw[6] = (uint8_t)read_size;
            raw[7] = 0;
            size = FUJIBUS_HEADER + 2;
            if (!exchange(conn, NETWORK_DEVICE, raw, &size, deadline) || size <= FUJIBUS_HEADER ||
                size > FUJIBUS_HEADER + read_size) {
                return false;
            }
            size_t received = size - FUJIBUS_HEADER;
            if (!directory) {
                if (received != read_size) {
                    return false;
                }
                memcpy(reply->data + 1, raw + FUJIBUS_HEADER, received);
                reply->size += received;
                conn->file_bytes_read += received;
                return true;
            }
            append_listing(conn, reply, raw + FUJIBUS_HEADER, received);
            if (reply->size > 1) {
                return true;
            }
        } else if (raw[9] == NETWORK_EOF) {
            reply->data[0] = CPC_FUJI_RESULT_END;
            return close_stream(conn, deadline);
        } else {
            struct timespec delay = { .tv_nsec = 10000000 };
            nanosleep(&delay, NULL);
        }
    }
    return false;
}

static link_reply handle_request(connection *conn, const cpc_fuji_request *request)
{
    link_reply reply = { .ticket = request->ticket, .command = request->data[0],
                         .size = 1, .data = { CPC_FUJI_RESULT_OK } };
    int64_t deadline = milliseconds() + REQUEST_MS;
    bool ok = false;
    switch (request->data[0]) {
    case CPC_FUJI_DIR_OPEN:
        ok = open_directory(conn, request, &reply, deadline);
        break;
    case CPC_FUJI_DIR_NEXT:
        ok = request->size == 1 && next_chunk(conn, STREAM_DIRECTORY, &reply, deadline);
        break;
    case CPC_FUJI_FILE_OPEN:
        ok = open_stream(conn, request->data + 1, request->size - 1, STREAM_FILE, deadline);
        break;
    case CPC_FUJI_FILE_NEXT:
        ok = request->size == 1 && next_chunk(conn, STREAM_FILE, &reply, deadline);
        break;
    }
    if (!ok) {
        reply.timed_out = milliseconds() >= deadline;
        disconnect(conn);
        reply.size = 1;
        reply.data[0] = CPC_FUJI_RESULT_ERROR;
    }
    return reply;
}

static void *worker(void *user_data)
{
    cpc_fuji_link *link = user_data;
    sigset_t blocked;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &blocked, NULL);
    pthread_mutex_lock(&link->mutex);
    while (!link->stopping) {
        if (link->reset) {
            disconnect(&link->conn);
            link->reset = false;
        }
        if (!link->pending) {
            pthread_cond_wait(&link->wake, &link->mutex);
            continue;
        }
        cpc_fuji_request request = link->request;
        link->pending = false;
        pthread_mutex_unlock(&link->mutex);

        link_reply reply = handle_request(&link->conn, &request);

        pthread_mutex_lock(&link->mutex);
        if (!link->reset && !link->stopping) {
            link->reply = reply;
            link->complete = true;
        }
    }
    pthread_mutex_unlock(&link->mutex);
    disconnect(&link->conn);
    return NULL;
}

static cpc_fuji_link *create_link(connection conn)
{
    cpc_fuji_link *link = calloc(1, sizeof(*link));
    if (link) {
        link->conn = conn;
        if (pthread_mutex_init(&link->mutex, NULL) == 0) {
            if (pthread_cond_init(&link->wake, NULL) == 0) {
                if (pthread_create(&link->thread, NULL, worker, link) == 0) {
                    return link;
                }
                pthread_cond_destroy(&link->wake);
            }
            pthread_mutex_destroy(&link->mutex);
        }
        free(link);
    }
    disconnect(&conn);
    return NULL;
}

cpc_fuji_link *cpc_fuji_link_create(const char *port)
{
    char *end;
    long value = port ? strtol(port, &end, 10) : CPC_FUJI_BOIP_PORT;
    if ((port && (end == port || *end)) || value < 1 || value > UINT16_MAX) {
        return NULL;
    }
    return create_link((connection){ .fd = -1, .port = (uint16_t)value });
}

cpc_fuji_link *cpc_fuji_link_from_fd(int fd)
{
    int flags = fcntl(fd, F_GETFL);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        close(fd);
        return NULL;
    }
    return create_link((connection){ .fd = fd });
}

void cpc_fuji_link_service(cpc_fuji_link *link, cpc_session *session)
{
    pthread_mutex_lock(&link->mutex);
    if (link->complete) {
        const link_reply *reply = &link->reply;
        if (cpc_session_fuji_reply(session, reply->ticket, reply->data, reply->size)) {
            link->traced_ticket = 0;
            if (link->trace) {
                link->trace(reply->timed_out ? CPC_FUJI_TIMEOUT : CPC_FUJI_REPLY,
                            reply->ticket, reply->data, reply->size, link->trace_data);
            }
            if (reply->command == CPC_FUJI_DIR_OPEN || reply->command == CPC_FUJI_DIR_NEXT) {
                if (reply->data[0] == CPC_FUJI_RESULT_END) {
                    link->listing = CPC_FUJI_LISTING_DONE;
                } else if (reply->data[0] == CPC_FUJI_RESULT_ERROR) {
                    link->listing = CPC_FUJI_LISTING_FAILED;
                }
            }
        }
        link->complete = false;
    }
    if (link->traced_ticket && !cpc_session_fuji_pending(session, link->traced_ticket)) {
        link->trace(CPC_FUJI_TIMEOUT, link->traced_ticket, NULL, 0, link->trace_data);
        link->traced_ticket = 0;
    }
    if (!link->pending && cpc_session_fuji_take(session, &link->request)) {
        if (link->trace) {
            link->traced_ticket = link->request.ticket;
            link->trace(CPC_FUJI_REQUEST, link->request.ticket,
                        link->request.data, link->request.size, link->trace_data);
        }
        if (link->request.data[0] == CPC_FUJI_DIR_OPEN) {
            link->listing = CPC_FUJI_LISTING_PENDING;
        }
        link->pending = true;
        pthread_cond_signal(&link->wake);
    }
    pthread_mutex_unlock(&link->mutex);
}

void cpc_fuji_link_set_trace(cpc_fuji_link *link, cpc_fuji_trace_fn trace, void *user_data)
{
    link->traced_ticket = 0;
    link->trace = trace;
    link->trace_data = user_data;
}

void cpc_fuji_link_reset(cpc_fuji_link *link)
{
    pthread_mutex_lock(&link->mutex);
    link->reset = true;
    link->pending = false;
    link->complete = false;
    link->traced_ticket = 0;
    link->listing = CPC_FUJI_LISTING_PENDING;
    pthread_cond_signal(&link->wake);
    pthread_mutex_unlock(&link->mutex);
}

cpc_fuji_listing cpc_fuji_link_listing(const cpc_fuji_link *link)
{
    return link->listing;
}

void cpc_fuji_link_destroy(cpc_fuji_link *link)
{
    pthread_mutex_lock(&link->mutex);
    link->stopping = true;
    pthread_cond_signal(&link->wake);
    pthread_mutex_unlock(&link->mutex);
    pthread_join(link->thread, NULL);
    pthread_cond_destroy(&link->wake);
    pthread_mutex_destroy(&link->mutex);
    free(link);
}
