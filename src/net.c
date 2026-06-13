/*
 * net.c — Network C module for TinyActor VM
 *
 * Non-blocking TCP sockets: listen, accept, read, write, close.
 * Returns symbol 'would-block on EAGAIN/EWOULDBLOCK.
 */

#include "ta.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

static void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0)
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static Val sym_would_block(VM *vm) {
    int idx = vm_intern_symbol(vm, "would-block");
    return val_symbol((uint32_t)idx);
}

static Val sym_eof(VM *vm) {
    int idx = vm_intern_symbol(vm, "eof");
    return val_symbol((uint32_t)idx);
}

static Val net_listen(VM *vm, Val *args, int nargs) {
    (void)nargs;
    int port = (int)val_get_int(args[0]);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return val_int(-1);

    int optval = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = htons((uint16_t)port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        listen(fd, 16) < 0) {
        close(fd);
        return val_int(-1);
    }

    set_nonblocking(fd);
    return val_int(fd);
}

static Val net_accept(VM *vm, Val *args, int nargs) {
    (void)nargs;
    int server_fd = (int)val_get_int(args[0]);

    int client_fd = accept(server_fd, NULL, NULL);
    if (client_fd < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            vm->last_wait_fd = server_fd;
            return sym_would_block(vm);
        }
        return val_int(-1);
    }

    set_nonblocking(client_fd);
    return val_int(client_fd);
}

static Val net_read(VM *vm, Val *args, int nargs) {
    Proc *p = vm->current_proc;
    int fd = (int)val_get_int(args[0]);
    int max_len = 4096;
    if (nargs >= 2)
        max_len = (int)val_get_int(args[1]);
    if (max_len <= 0) max_len = 4096;
    if (max_len > 65536) max_len = 65536;

    char buf[65536];
    ssize_t n = read(fd, buf, (size_t)max_len);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            vm->last_wait_fd = fd;
            return sym_would_block(vm);
        }
        return val_int(-1);
    }
    if (n == 0) return sym_eof(vm);

    return val_string(p, buf, (int)n);
}

static Val net_write(VM *vm, Val *args, int nargs) {
    (void)nargs;
    int fd = (int)val_get_int(args[0]);
    if (!val_is_string(args[1])) return val_int(0);

    HeapString *hs = val_get_string(args[1]);
    ssize_t n = write(fd, hs->data, (size_t)hs->len);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            vm->last_wait_fd = fd;
            return sym_would_block(vm);
        }
        return val_int(-1);
    }
    return val_int((int64_t)n);
}

static Val net_connect(VM *vm, Val *args, int nargs) {
    (void)nargs;
    if (!val_is_string(args[0])) return val_int(-1);
    HeapString *host = val_get_string(args[0]);
    int port = (int)val_get_int(args[1]);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return val_int(-1);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons((uint16_t)port);
    addr.sin_addr.s_addr = inet_addr(host->data);

    if (addr.sin_addr.s_addr == INADDR_NONE) {
        close(fd);
        return val_int(-1);
    }

    /* Blocking connect for simplicity; localhost is instant.
       After connect, set non-blocking for read/write. */
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return val_int(-1);
    }

    set_nonblocking(fd);
    return val_int(fd);
}

static Val net_close(VM *vm, Val *args, int nargs) {
    (void)vm; (void)nargs;
    int fd = (int)val_get_int(args[0]);
    close(fd);
    return val_nil();
}

TaFunc net_funcs[] = {
    {"listen",  net_listen,  1},
    {"accept",  net_accept,  1},
    {"connect", net_connect, 2},
    {"read",    net_read,   -1},  /* -1 = variable args */
    {"write",   net_write,   2},
    {"close",   net_close,   1},
    {NULL, NULL, 0}
};

void vm_register_net_module(VM *vm) {
    vm_register_module(vm, "net", net_funcs, 6);
}