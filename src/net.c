
/*
 * vim:ts=4:sw=4:expandtab
 */
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <unistd.h>
#include <ev.h>
#include <assert.h>

#include "queue.h"

#include "net.h"
#include "log.h"
#include "data.h"
#include "main.h"
#include "mqtt.h"
#include "util.h"

static int read_packet(struct Client *client);

/* Accepts a single incoming connection on the listen_fd. If the connection
 * could be accepted, a new Client is allocated and appended to the client
 * list. In addition, a new ev_io watcher is registered and started for READ
 * events.
 */
void accept_cb(EV_P_ struct ev_io *watcher, int revents) {
    static struct sockaddr_in peer_addr;
    static socklen_t peer_len = sizeof(peer_addr);
    int peer_sd;
    char host[NI_MAXHOST], service[NI_MAXSERV];
    int ret;
    struct Client *client;

    logmsg(LOG_DEBUG, "got libev event: 0x%x\n", revents);

    if (EV_ERROR & revents) {
        logmsg(LOG_ERR, "libev: invalid event\n");
        return;
    }

    peer_sd = accept(watcher->fd, (struct sockaddr *)&peer_addr, &peer_len);
    if (peer_sd == EAGAIN || peer_sd == EINTR || peer_sd == EWOULDBLOCK)
        return;

    if (peer_sd < 0) {
        logmsg(LOG_ERR, "could not accept connection\n");
        return;
    }

    ret = getnameinfo((struct sockaddr *) &peer_addr, peer_len, host,
                NI_MAXHOST, service, NI_MAXSERV, NI_NUMERICSERV);

    if (ret == 0)
        logmsg(LOG_INFO, "new connection by %s:%d\n", host, atoi(service));
    else
        logmsg(LOG_ERR, "getnameinfo: %s\n", gai_strerror(ret));

    client = smalloc(sizeof(struct Client));
    client->watcher = (struct ev_io*) smalloc(sizeof(struct ev_io));
    client->fd = peer_sd;
    client->state = S_CONNECTING;
    client->inbuf = malloc(sizeof(char) * BUF_LEN);
    client->inbuf_bytes = 0;

    LIST_INSERT_HEAD(&clients, client, entries);
    LIST_INIT(&(client->outgoing_msgs));

    num_clients++;

    ev_io_init(client->watcher, peer_cb, peer_sd, EV_READ | EV_WRITE);
    ev_io_start(EV_A_ client->watcher);
}

void peer_cb(EV_P_ struct ev_io *peer_w, int revents) {
    Client *client;
    int bytes_read, ret;

    /* look up client context */
    /* XXX: use hashtable? */
    LIST_FOREACH(client, &clients, entries) {
        if (client->fd == client->fd)
            break;
    }

    assert(client != NULL);
    assert(client->watcher->fd == client->fd);

    if (revents & EV_READ) {
        if ((bytes_read = read(client->fd, client->inbuf+client->inbuf_bytes,
                        BUF_LEN-client->inbuf_bytes)) == 0) {
            logmsg(LOG_INFO, "client disconnected\n");
            num_clients--;

            if (bytes_read == -1 && errno == EAGAIN)
                return;

            if ((ret = close(client->fd)) != 0)
                logmsg(LOG_ERR, "could not close socket: %s\n", gai_strerror(ret));

            LIST_REMOVE(client, entries);
            free(client->identifier); /* XXX: this may not have been malloced() yet */
            free(client->will_topic); /* XXX: this may not have been malloced() yet */
            free(client->will_msg);   /* XXX: this may not have been malloced() yet */
            free(client->inbuf);

            ev_io_stop(EV_A_ client->watcher);
            free(client->watcher);

            free(client);


            return;
        } else if (bytes_read == -1) {
            logmsg(LOG_DEBUG, "read() failed: %s\n", gai_strerror(bytes_read));
            return;
        }

        logmsg(LOG_DEBUG, "read %d bytes from client\n", bytes_read);

        client->inbuf_bytes += bytes_read;
        if (client->inbuf_bytes > 2)
            ret = read_packet(client);

        /* XXX: free and malloc() again if msg was > 4096 */
        if (ret)
            client->inbuf_bytes = 0;
    } else if (revents & EV_WRITE) {
        int ret;

        /* Try to send out a single message. */
        Envelope *envelope;
        LIST_FOREACH(envelope, &client->outgoing_msgs, entries) {
            assert(envelope->bytes_total != envelope->bytes_sent);

            logmsg(LOG_DEBUG, "going to send %d bytes of %d total bytes\n",
                    envelope->bytes_total-envelope->bytes_sent, envelope->bytes_total);

            ret = write(client->fd, envelope->msg+envelope->bytes_sent,
                    envelope->bytes_total-envelope->bytes_sent);

            if (ret < 0) {
                if (ret == EAGAIN || ret == EWOULDBLOCK || ret == EINTR)
                    break;
                /* else {
                    disconnect_client();
                    return;
                */
            }

            envelope->bytes_sent += ret;

            /* If the entire message could be sent, remove it from the queue. */
            if (envelope->bytes_sent == envelope->bytes_total) {
                /* XXX: if the envelope is shared, don't free it */
                logmsg(LOG_DEBUG, "msg sent!\n");
                LIST_REMOVE(envelope, entries);
                free(envelope->msg);
                free(envelope);
            }

            break; /* maybe send more? */
        }
    }
}

static int read_packet(struct Client *client) {
    assert(client->inbuf_bytes > 2);

    int msg_length = client->inbuf[1];
    bool msg_complete = false;
    if (client->inbuf[1] < 128 && client->inbuf_bytes-2 /* header */ == client->inbuf[1]) {
        msg_complete = true;
    } else if (client->inbuf[1] >= 128) {
        /* variable length encoding: seven bits encode the remaining length,
         * the eigth bit is the continuation indicator. the maximum number of
         * bytes is limited to four.                       (source: mqtt ref)
         */
        int msg_index = 1;
        int multiplier = 1;
        int thisbyte;
        do {
            thisbyte = client->inbuf[msg_index++];
            msg_length = (thisbyte & 0x7F) * multiplier;
            multiplier *= 128;
        } while (msg_index < client->inbuf_bytes && (thisbyte & 0x7F) != 0);

        if (client->inbuf_bytes-2 /* header */ == msg_length)
            msg_complete = true;
    }

    if (msg_complete)
        logmsg(LOG_DEBUG, "message is complete (%d bytes)\n", client->inbuf_bytes-2);
    else {
        /* XXX: the buffer is pretty small (4096 bytes), so messages with
         * payload > 4096 bytes will never be handled at the moment. */
        logmsg(LOG_DEBUG, "expected %d bytes, but only got %d so far\n", msg_length,
                client->inbuf_bytes-2);
        return 0; /* try again later */
    }

    int ret = 0;
    int msg_type = (client->inbuf[0]) & 0xF0;
    if (client->state == S_CONNECTING && msg_type != T_CONNECT) {
        /* XXX: disconnect client */
        logmsg(LOG_ERR, "invalid client state: expected CONNECT message but got 0x%x\n", msg_type);
        return 0;
    }

    switch(msg_type) {
        case T_CONNECT:
            logmsg(LOG_DEBUG, "CONNECT from client\n");
            ret = handle_connect(client, msg_length);
            break;
        case T_PINGREQ:
            logmsg(LOG_DEBUG, "PINGREQ from client\n");
            /*ret = handle_pinreq(client, msg_length);*/
            break;
        default:
            logmsg(LOG_DEBUG, "invalid message type: 0x%x\n", msg_type);
            break;
    }

    /*if (ret)
        reset_keepalive(client);*/

    return ret;
}
