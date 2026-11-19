/*
 * vigild HTTP thread: the dashboard and a read-only JSON API.
 *
 *   GET /               the embedded dashboard (web/index.html)
 *   GET /api/picture    the whole picture as JSON
 *   GET /api/stats      counters and timing, plain text
 *   GET /api/alarms     active alarms, plain text
 *   GET /api/events     recent alarm transitions, plain text
 *   GET /healthz        "ok"
 *
 * Deliberately tiny and single-threaded: one short-lived connection at a time,
 * 2 s socket timeouts, GET only, no state, lowest priority of the daemon. It
 * only ever reads the published picture, so a slow or hostile web client can
 * delay other web clients but never the engine. There is no authentication:
 * bind it to 127.0.0.1 (-b) or keep it on a trusted network.
 *
 * SPDX-License-Identifier: MIT
 */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "vigild.h"
#include "web_index.h"

#define HTTP_REQ_MAX 4096
#define HTTP_BODY_MAX (256 * 1024)

static lkd_pub_t  g_snap;
static vg_event_t g_events[VG_EVENT_RING];
static char       g_body[HTTP_BODY_MAX];

static int send_all(int fd, const char *p, size_t n)
{
    while (n > 0) {
        ssize_t w = send(fd, p, n, 0);
        if (w < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

static void respond(int fd, int code, const char *reason, const char *ctype, const char *body,
                    size_t len)
{
    char head[256];
    int hn = snprintf(head, sizeof(head),
                      "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
                      "Cache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n"
                      "Connection: close\r\n\r\n",
                      code, reason, ctype, len);
    if (hn > 0 && send_all(fd, head, (size_t)hn) == 0 && len > 0) {
        send_all(fd, body, len);
    }
}

static void serve(int fd)
{
    char req[HTTP_REQ_MAX];
    size_t have = 0;
    while (have < sizeof(req) - 1) {
        ssize_t n = recv(fd, req + have, sizeof(req) - 1 - have, 0);
        if (n <= 0) {
            return; /* timeout, reset or EOF before a full request */
        }
        have += (size_t)n;
        req[have] = '\0';
        if (strstr(req, "\r\n\r\n") != NULL || strstr(req, "\n\n") != NULL) {
            break;
        }
    }
    char method[8], path[256];
    if (sscanf(req, "%7s %255s", method, path) != 2) {
        respond(fd, 400, "Bad Request", "text/plain", "bad request\n", 12);
        return;
    }
    if (strcmp(method, "GET") != 0) {
        respond(fd, 405, "Method Not Allowed", "text/plain", "GET only\n", 9);
        return;
    }
    char *q = strchr(path, '?');
    if (q != NULL) {
        *q = '\0';
    }

    if (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0) {
        respond(fd, 200, "OK", "text/html; charset=utf-8", (const char *)VG_WEB_INDEX,
                VG_WEB_INDEX_LEN);
        return;
    }
    if (strcmp(path, "/healthz") == 0) {
        respond(fd, 200, "OK", "text/plain", "ok\n", 3);
        return;
    }

    lkd_pub_snapshot(&g_snap);
    size_t n;
    if (strcmp(path, "/api/picture") == 0) {
        n = vg_render_json(&g_snap.pic, &g_snap.cfg, g_body, sizeof(g_body));
        respond(fd, 200, "OK", "application/json", g_body, n);
    } else if (strcmp(path, "/api/stats") == 0) {
        n = vg_render_stats(&g_snap.pic, &g_snap.cfg, g_body, sizeof(g_body));
        respond(fd, 200, "OK", "text/plain; charset=utf-8", g_body, n);
    } else if (strcmp(path, "/api/alarms") == 0) {
        n = vg_render_alarms(&g_snap.pic, g_body, sizeof(g_body));
        respond(fd, 200, "OK", "text/plain; charset=utf-8", g_body, n);
    } else if (strcmp(path, "/api/events") == 0) {
        uint32_t ne = vg_events_since(&g_snap.events, 0, g_events, VG_EVENT_RING, NULL);
        n = vg_render_events(g_events, ne, g_body, sizeof(g_body));
        respond(fd, 200, "OK", "text/plain; charset=utf-8", g_body, n);
    } else {
        respond(fd, 404, "Not Found", "text/plain", "not found\n", 10);
    }
}

void *lkd_http_thread(void *arg)
{
    (void)arg;
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls == -1) {
        lkd_log(SLOG2_ERROR, "http: socket(): %s", strerror(errno));
        return NULL;
    }
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)g_opt.http_port);
    if (inet_pton(AF_INET, g_opt.http_bind, &addr.sin_addr) != 1) {
        lkd_log(SLOG2_ERROR, "http: bad bind address '%s'", g_opt.http_bind);
        close(ls);
        return NULL;
    }
    if (bind(ls, (struct sockaddr *)&addr, sizeof(addr)) == -1 || listen(ls, 8) == -1) {
        lkd_log(SLOG2_ERROR, "http: cannot listen on %s:%d: %s", g_opt.http_bind,
                g_opt.http_port, strerror(errno));
        close(ls);
        return NULL;
    }
    lkd_log(SLOG2_INFO, "http: dashboard on http://%s:%d/", g_opt.http_bind, g_opt.http_port);

    for (;;) {
        int fd = accept(ls, NULL, NULL);
        if (fd == -1) {
            if (errno != EINTR) {
                usleep(50000);
            }
            continue;
        }
        struct timeval tv = { 2, 0 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        serve(fd);
        close(fd);
    }
    return NULL;
}
