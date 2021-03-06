/*
 * ipop-tap
 * Copyright 2013, University of Florida
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *  1. Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *  2. Redistributions in binary form must reproduce the above copyright notice,
 *     this list of conditions and the following disclaimer in the documentation
 *     and/or other materials provided with the distribution.
 *  3. The name of the author may not be used to endorse or promote products
 *     derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO
 * EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <pthread.h>

#include "peerlist.h"
#include "headers.h"
#include "translator.h"
#include "tap.h"
#include "ipop_tap.h"
#include "packetio.h"

/**
 * Reads packet data from the tap device that was locally written, and sends it
 * off through a socket to the relevant peer(s).
 */
void *
ipop_send_thread(void *data)
{
    thread_opts_t *opts = (thread_opts_t *) data;
    int sock4 = opts->sock4;
    int sock6 = opts->sock6;
    int tap = opts->tap;
    struct threadqueue *queue = opts->send_queue;

    int rcount, ncount;
    unsigned char buf[BUFLEN];
    unsigned char enc_buf[BUFLEN];
    struct in_addr local_ipv4_addr;
    struct in6_addr local_ipv6_addr;
    struct peer_state *peer = NULL;
    int result, is_ipv4;

    while (1) {
        if ((rcount = read(tap, buf, BUFLEN)) < 0) {
            fprintf(stderr, "tap read failed\n");
            break;
        }

        if ((buf[14] >> 4) == 0x04) { // ipv4 packet
            memcpy(&local_ipv4_addr.s_addr, buf + 30, 4);
            is_ipv4 = 1;
        } else if ((buf[14] >> 4) == 0x06) { // ipv6 packet
            memcpy(&local_ipv6_addr.s6_addr, buf + 38, 16);
            is_ipv4 = 0;
        } else {
            fprintf(stderr, "unknown IP packet type: 0x%x\n", buf[14] >> 4);
            continue;
        }

        ncount = rcount + BUF_OFFSET;
        peerlist_reset_iterators();
        while (1) {
            if (is_ipv4) {
                result = peerlist_get_by_local_ipv4_addr(&local_ipv4_addr,
                                                         &peer);
            }
            else {
                result = peerlist_get_by_local_ipv6_addr(&local_ipv6_addr,
                                                         &peer);
            }
            if (result == -1) break;
            set_headers(enc_buf, peerlist_local.id, peer->id);
            if (is_ipv4 && opts->translate) {
                translate_packet(buf, NULL, NULL, rcount);
            }
            // TODO - Remove this extra copy
            memcpy(enc_buf + BUF_OFFSET, buf, rcount);
            if (queue != NULL) {
                if (thread_queue_bput(queue, enc_buf, ncount) < 0) {
                    fprintf(stderr, "thread queue error\n");
                    pthread_exit(NULL);
                }
                if (opts->send_signal != NULL) {
                  opts->send_signal(queue);
                }
            }
            else {
                struct sockaddr_in dest_ipv4_addr_sock = {
                    .sin_family = AF_INET,
                    .sin_port = htons(peer->port),
                    .sin_addr = peer->dest_ipv4_addr,
                    .sin_zero = { 0 }
                };
                // send our processed packet off
                if (sendto(sock4, enc_buf, rcount, 0,
                           (struct sockaddr *)(&dest_ipv4_addr_sock),
                           sizeof(struct sockaddr_in)) < 0) {
                    fprintf(stderr, "sendto failed\n");
                   pthread_exit(NULL);
                }
            }
            if (result == 0) break;
        }
    }

    close(sock4);
    close(sock6);
    tap_close();
    pthread_exit(NULL);
}

/**
 * Reads packet data from the socket that we received from a remote peer, and
 * writes it to the local tap device, making the traffic show up locally.
 */
void *
ipop_recv_thread(void *data)
{
    thread_opts_t *opts = (thread_opts_t *) data;
    int sock4 = opts->sock4;
    int sock6 = opts->sock6;
    int tap = opts->tap;
    struct threadqueue *queue = opts->rcv_queue;

    int rcount;
    struct sockaddr_in addr;
    socklen_t addrlen = sizeof(addr);

    unsigned char buf[BUFLEN];
    unsigned char dec_buf[BUFLEN];
    char source_id[ID_SIZE] = { 0 };
    char dest_id[ID_SIZE] = { 0 };
    struct peer_state *peer = NULL;

    while (1) {
        if (queue != NULL) {
            if ((rcount = thread_queue_bget(queue, dec_buf, BUFLEN)) < 0) {
              fprintf(stderr, "threadqueue get failed\n");
              break;
            }
        }
        else if ((rcount = recvfrom(sock4, dec_buf, BUFLEN, 0,
                               (struct sockaddr*) &addr, &addrlen)) < 0) {
            fprintf(stderr, "upd recv failed\n");
            break;
        }

        rcount -= BUF_OFFSET;
        get_headers(dec_buf, source_id, dest_id);
        memcpy(buf, dec_buf + BUF_OFFSET, rcount);
        if ((buf[14] >> 4) == 0x04 && opts->translate) {
            int peer_found = peerlist_get_by_id(source_id, &peer);
            if (peer_found != -1) {
                translate_packet(buf, (char *)(&peer->local_ipv4_addr.s_addr),
                                (char *)(&peerlist_local.local_ipv4_addr.s_addr),
                                rcount);
                translate_headers(buf, (char *)(&peer->local_ipv4_addr.s_addr),
                                  (char *)(&peerlist_local.local_ipv4_addr.s_addr),
                                  rcount);
            }
        }
        translate_mac(buf, opts->mac);
        if (write(tap, buf, rcount) < 0) {
            fprintf(stderr, "write to tap error\n");
            break;
        }
    }

    close(sock4);
    close(sock6);
    tap_close();
    pthread_exit(NULL);
}

