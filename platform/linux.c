/* Linux transport and process boundary for the Ulysses development profile. */
#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MULIB_IMPL_HTTP
#define MULIB_IMPL_BUFFER
#define MULIB_IMPL_STRBUF
#define MULIB_IMPL_URI
#define MULIB_IMPL_INET6
#define MULIB_IMPL_INET4
#define MULIB_IMPL_PARSEC
#include "mulib-http.h"

enum { HTTP_CAP = 8192, WIRE_CAP = 4123, IO_TIMEOUT_MS = 2000 };

static int listener = -1;
static int client = -1;
static int worker = -1;
static pid_t worker_pid = -1;
static mulib_http_message_t request;

int64_t uly_now_ms(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        abort();
    return INT64_C(1000) * now.tv_sec + now.tv_nsec / 1000000;
}

static int option(const char *name, int fallback, int min, int max)
{
    const unsigned char *s = (const unsigned char *)getenv(name);
    int value = 0;
    if (!s)
        return fallback;
    if (!*s)
        return -1;
    for (; *s; ++s) {
        int digit = *s - '0';
        if (digit < 0 || digit > 9 || value > (max - digit) / 10)
            return -1;
        value = value * 10 + digit;
    }
    return value >= min && value <= max ? value : -1;
}

static int wait_fd(int fd, short events, int64_t deadline)
{
    struct pollfd pfd = {.fd = fd, .events = events};
    for (;;) {
        int64_t remaining = deadline - uly_now_ms();
        if (remaining <= 0)
            return 0;
        int result = poll(&pfd, 1, remaining > INT_MAX ? INT_MAX : (int)remaining);
        if (result >= 0)
            return result;
        if (errno != EINTR)
            return -1;
    }
}

static int transfer(int fd, void *read_into, const void *write_from,
                    size_t size, int64_t deadline)
{
    unsigned char *out = read_into;
    const unsigned char *in = write_from;
    size_t at = 0;
    while (at < size) {
        if (wait_fd(fd, out ? POLLIN : POLLOUT, deadline) != 1)
            return 0;
        ssize_t n = out ? recv(fd, out + at, size - at, MSG_DONTWAIT)
                        : send(fd, in + at, size - at, MSG_DONTWAIT | MSG_NOSIGNAL);
        if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
            continue;
        if (n <= 0)
            return 0;
        at += (size_t)n;
    }
    return 1;
}

static int read_record(int fd, char *out, int64_t deadline)
{
    uint32_t network_size;
    if (!transfer(fd, &network_size, NULL, sizeof network_size, deadline))
        return 0;
    uint32_t size = ntohl(network_size);
    if (!size || size > WIRE_CAP || !transfer(fd, out, NULL, size, deadline)
        || memchr(out, 0, size))
        return 0;
    out[size] = '\0';
    return 1;
}

static int write_record(int fd, const char *wire, int64_t deadline)
{
    size_t size = strlen(wire);
    if (!size || size > WIRE_CAP)
        return 0;
    uint32_t network_size = htonl((uint32_t)size);
    return transfer(fd, NULL, &network_size, sizeof network_size, deadline)
           && transfer(fd, NULL, wire, size, deadline);
}

int64_t uly_connection_limit(void)
{
    return option("ULYSSES_MAX_CONNECTIONS", 256, 1, 1024);
}

int64_t uly_http_open(void)
{
    int port = option("ULYSSES_PORT", 8080, 0, 65535);
    struct sigaction child_action = {.sa_handler = SIG_DFL};
    socklen_t address_size = sizeof(struct sockaddr_in);
    if (port < 0 || sigemptyset(&child_action.sa_mask) < 0
        || sigaction(SIGCHLD, &child_action, NULL) < 0)
        return -1;
    struct sockaddr_in address = {.sin_family = AF_INET,
                                  .sin_port = htons((uint16_t)port),
                                  .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    int enabled = 1;
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof enabled) < 0
        || bind(fd, (const struct sockaddr *)&address, sizeof address) < 0
        || listen(fd, 16) < 0
        || getsockname(fd, (struct sockaddr *)&address, &address_size) < 0) {
        close(fd);
        return -1;
    }
    listener = fd;
    return ntohs(address.sin_port);
}

int64_t uly_http_accept(void)
{
    /* The parent can fork only after this function returns. */
    do {
        client = accept(listener, NULL, NULL);
    } while (client < 0 && errno == EINTR);
    if (client < 0)
        return -1;
    if (fcntl(client, F_SETFD, FD_CLOEXEC) < 0) {
        close(client);
        client = -1;
        return -1;
    }
    char wire[HTTP_CAP];
    size_t size = 0, scan = 0;
    const int64_t deadline = uly_now_ms() + IO_TIMEOUT_MS;
    while (size < sizeof wire) {
        int ready = wait_fd(client, POLLIN, deadline);
        if (ready <= 0)
            return ready == 0 ? 408 : 400;
        ssize_t got = recv(client, wire + size, sizeof wire - size, MSG_DONTWAIT);
        if (got < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
            continue;
        if (got <= 0)
            return 400;
        size += (size_t)got;
        while (scan + 4 <= size) {
            if (memcmp(wire + scan, "\r\n\r\n", 4) == 0) {
                const mulib_http_options_t options = {HTTP_CAP, 32, 1, 0};
                size_t used = 0;
                int result = mulib_http_parse(wire, scan + 4, 0, &options, &request, &used);
                if (result == MULIB_HTTP_NOMEM)
                    return 503;
                if (result != MULIB_HTTP_OK || used != size || request.chunked
                    || request.body_length || mulib_http_header(&request, "expect"))
                    return 400;
                return 1;
            }
            ++scan;
        }
    }
    return 431;
}

const char *uly_http_method(void) { return request.method ? request.method : ""; }
const char *uly_http_target(void) { return request.target ? request.target : ""; }

void uly_http_reply(int64_t status, const char *body)
{
    const char *reason;
    switch (status) {
    case 200: reason = "OK"; break;
    case 400: reason = "Bad Request"; break;
    case 404: reason = "Not Found"; break;
    case 405: reason = "Method Not Allowed"; break;
    case 408: reason = "Request Timeout"; break;
    case 431: reason = "Request Header Fields Too Large"; break;
    case 502: reason = "Bad Gateway"; break;
    default: status = 503; reason = "Service Unavailable"; break;
    }
    size_t length = strlen(body);
    char header[256];
    int size = snprintf(header, sizeof header,
                        "HTTP/1.1 %d %s\r\nContent-Type: text/plain; charset=utf-8\r\n"
                        "Content-Length: %zu\r\nConnection: close\r\n%s\r\n",
                        (int)status, reason, length, status == 405 ? "Allow: GET\r\n" : "");
    int64_t deadline = uly_now_ms() + IO_TIMEOUT_MS;
    if (client >= 0) {
        if (size > 0 && (size_t)size < sizeof header
            && transfer(client, NULL, header, (size_t)size, deadline)
            && strcmp(uly_http_method(), "HEAD") != 0)
            (void)transfer(client, NULL, body, length, deadline);
        close(client);
        client = -1;
    }
    mulib_http_message_free(&request);
}

void uly_http_close(void)
{
    if (listener >= 0)
        close(listener);
    listener = -1;
}

bool uly_worker_start(void)
{
    int sockets[2];
    const char *path = getenv("ULYSSES_WORKER");
    if (!path)
        path = "./build/telemachaid-worker";
    if (worker >= 0 || socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) < 0)
        return false;
    const pid_t parent = getpid();
    pid_t pid = fork();
    if (pid == 0) {
        close(sockets[0]);
        if (prctl(PR_SET_PDEATHSIG, SIGKILL) < 0 || getppid() != parent
            || dup2(sockets[1], STDIN_FILENO) < 0 || dup2(sockets[1], STDOUT_FILENO) < 0
            || fcntl(STDIN_FILENO, F_SETFD, 0) < 0 || fcntl(STDOUT_FILENO, F_SETFD, 0) < 0)
            _exit(126);
        if (sockets[1] > STDOUT_FILENO)
            close(sockets[1]);
        execl(path, path, (char *)NULL);
        _exit(127);
    }
    close(sockets[1]);
    if (pid < 0) {
        close(sockets[0]);
        return false;
    }
    worker = sockets[0];
    worker_pid = pid;
    return true;
}

void uly_worker_stop(void)
{
    if (worker < 0)
        return;
    close(worker);
    worker = -1;
    /* This exact child remains waitable, so its PID cannot be recycled. */
    kill(worker_pid, SIGKILL);
    while (waitpid(worker_pid, NULL, 0) < 0 && errno == EINTR) {}
    worker_pid = -1;
}

const char *uly_worker_exchange(const char *wire)
{
    static char reply[WIRE_CAP + 1];
    int64_t deadline = uly_now_ms() + IO_TIMEOUT_MS;
    if (worker < 0 || !write_record(worker, wire, deadline)
        || !read_record(worker, reply, deadline))
        return "";
    return reply;
}

const char *uly_guest_read(void)
{
    static char wire[WIRE_CAP + 1];
    /* The host owns idle time and terminates the guest by closing its channel. */
    if (!read_record(STDIN_FILENO, wire, INT64_MAX))
        return "";
    return wire;
}

bool uly_guest_write(const char *wire)
{
    return write_record(STDOUT_FILENO, wire, uly_now_ms() + IO_TIMEOUT_MS);
}
