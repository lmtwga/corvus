#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include "corvus.h"
#include "socket.h"
#include "command.h"
#include "slot.h"
#include "logging.h"
#include "event.h"
#include "server.h"
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>

#define EMPTY_CMD_QUEUE(queue, field)     \
do {                                      \
    struct command *c;                    \
    while (!STAILQ_EMPTY(queue)) {        \
        c = STAILQ_FIRST(queue);          \
        STAILQ_REMOVE_HEAD(queue, field); \
        cmd_free(c);                      \
    }                                     \
} while (0)

void conn_init(struct connection *conn, struct context *ctx)
{
    conn->ctx = ctx;
    conn->fd = -1;
    conn->status = DISCONNECTED;
    conn->ready = NULL;
    conn->registered = 0;
    memset(&conn->addr, 0, sizeof(conn->addr));
    STAILQ_INIT(&conn->cmd_queue);
    STAILQ_INIT(&conn->ready_queue);
    STAILQ_INIT(&conn->waiting_queue);
    STAILQ_INIT(&conn->retry_queue);
    STAILQ_INIT(&conn->data);
}

struct connection *conn_create(struct context *ctx)
{
    struct connection *conn;
    if (!STAILQ_EMPTY(&ctx->free_connq)) {
        LOG(DEBUG, "connection get cache");
        conn = STAILQ_FIRST(&ctx->free_connq);
        STAILQ_REMOVE_HEAD(&ctx->free_connq, next);
        ctx->nfree_connq--;
        STAILQ_NEXT(conn, next) = NULL;
    } else {
        conn = malloc(sizeof(struct connection));
    }
    conn_init(conn, ctx);
    return conn;
}

int conn_connect(struct connection *conn)
{
    int status = -1;
    status = socket_connect(conn->fd, conn->addr.host, conn->addr.port);
    switch (status) {
        case CORVUS_ERR: conn->status = DISCONNECTED; return -1;
        case CORVUS_INPROGRESS: conn->status = CONNECTING; break;
        case CORVUS_OK: conn->status = CONNECTED; break;
    }
    return 0;
}

void conn_free(struct connection *conn)
{
    if (conn->fd != -1) {
        close(conn->fd);
        conn->fd = -1;
    }
    conn->status = DISCONNECTED;
    conn->registered = 0;
    memset(&conn->addr, 0, sizeof(conn->addr));

    EMPTY_CMD_QUEUE(&conn->cmd_queue, cmd_next);
    EMPTY_CMD_QUEUE(&conn->ready_queue, ready_next);
    EMPTY_CMD_QUEUE(&conn->waiting_queue, waiting_next);
    EMPTY_CMD_QUEUE(&conn->retry_queue, retry_next);

    struct mbuf *buf;
    while (!STAILQ_EMPTY(&conn->data)) {
        buf = STAILQ_FIRST(&conn->data);
        STAILQ_REMOVE_HEAD(&conn->data, next);
        mbuf_recycle(conn->ctx, buf);
    }
}

void conn_recycle(struct context *ctx, struct connection *conn)
{
    STAILQ_NEXT(conn, next) = NULL;
    STAILQ_INSERT_HEAD(&ctx->free_connq, conn, next);
    ctx->nfree_connq++;
}

int conn_create_fd()
{
    int fd = socket_create_stream();
    if (fd == -1) return -1;
    if (socket_set_nonblocking(fd) == -1) {
        LOG(ERROR, "can't set nonblocking");
        return -1;
    }
    return fd;
}

struct connection *conn_get_server_from_pool(struct context *ctx, struct address *addr)
{
    int fd;
    struct connection *server;
    char *key;

    key = socket_get_key(addr);
    server = hash_get(ctx->server_table, key);
    if (server != NULL) {
        free(key);
        if (server->status == DISCONNECTED) {
            close(server->fd);
            server->fd = conn_create_fd();
            if (conn_connect(server) == -1) {
                conn_free(server);
                LOG(ERROR, "can't connect");
                return NULL;
            }
            server->registered = 0;
        }
        return server;
    }

    fd = conn_create_fd();
    server = server_create(ctx, fd);
    memcpy(&server->addr, addr, sizeof(struct address));

    hash_set(ctx->server_table, key, (void*)server);

    if (conn_connect(server) == -1) {
        conn_free(server);
        LOG(ERROR, "can't connect");
        return NULL;
    }
    return server;
}

struct connection *conn_get_raw_server(struct context *ctx)
{
    int i;
    int fd, port;
    char *addr, *key;
    struct connection *server = NULL;
    struct address a;

    for (i = 0; i < ctx->node_conf->len; i++) {
        addr = ctx->node_conf->nodes[i];
        port = socket_parse_addr(addr, &a);
        if (port == -1) continue;

        key = socket_get_key(&a);
        server = hash_get(ctx->server_table, key);
        if (server != NULL) {
            free(key);
            break;
        }

        fd = conn_create_fd();
        if (fd == -1) {
            free(key);
            continue;
        }
        server = server_create(ctx, fd);
        memcpy(&server->addr, &a, sizeof(a));
        if (conn_connect(server) == -1) {
            free(key);
            conn_free(server);
            continue;
        };
        hash_set(ctx->server_table, key, (void*)server);
        break;
    }
    if (i >= ctx->node_conf->len) {
        LOG(ERROR, "cannot connect to redis server.");
        return NULL;
    }
    return server;
}

struct connection *conn_get_server(struct context *ctx, uint16_t slot)
{
    struct node_info *node = slot_get_node_info(slot);
    struct connection *server;

    server = (node == NULL) ?
        conn_get_raw_server(ctx) :
        conn_get_server_from_pool(ctx, &node->master);
    return server;
}

struct mbuf *conn_get_buf(struct connection *conn)
{
    struct mbuf *buf;
    buf = mbuf_queue_top(conn->ctx, &conn->data);
    if (buf->pos >= buf->end) {
        buf = mbuf_get(conn->ctx);
        mbuf_queue_insert(&conn->data, buf);
    }
    return buf;
}
