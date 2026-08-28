/* present_shot_listener.c -- production-speed screenshot control seam.
 *
 * The full debug server is intentionally unsuitable for acceptance captures:
 * enabling it also emits observers into every generated block and initializes
 * large diagnostic rings.  This loopback-only server understands exactly two
 * newline-delimited JSON commands and is polled once per guest vblank.
 */
#include "present_shot_listener.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <winsock2.h>
#  include <ws2tcpip.h>
typedef SOCKET shot_socket_t;
#  define SHOT_INVALID INVALID_SOCKET
#  define shot_close closesocket
static int shot_would_block(void) { return WSAGetLastError() == WSAEWOULDBLOCK; }
#else
#  include <errno.h>
#  include <fcntl.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <unistd.h>
typedef int shot_socket_t;
#  define SHOT_INVALID (-1)
#  define shot_close close
static int shot_would_block(void) { return errno == EAGAIN || errno == EWOULDBLOCK; }
#endif

#ifndef DEFAULT_DEBUG_PORT
#  define DEFAULT_DEBUG_PORT 4370
#endif

extern int present_shot_request(const char *path);
extern int present_shot_seq(void);
extern int present_shot_ok(void);

enum { SHOT_REQUEST_CAP = 2048, SHOT_RESPONSE_CAP = 2048 };
static shot_socket_t s_shot_listen = SHOT_INVALID;
static shot_socket_t s_shot_client = SHOT_INVALID;
static char s_shot_request[SHOT_REQUEST_CAP];
static size_t s_shot_request_len;

static void shot_set_nonblocking(shot_socket_t socket_handle)
{
#ifdef _WIN32
    u_long mode = 1;
    ioctlsocket(socket_handle, FIONBIO, &mode);
#else
    int flags = fcntl(socket_handle, F_GETFL, 0);
    if (flags >= 0) fcntl(socket_handle, F_SETFL, flags | O_NONBLOCK);
#endif
}

static int json_string(const char *json, const char *key,
                       char *out, size_t out_size)
{
    char pattern[80];
    const char *p;
    size_t n = 0;
    if (!json || !key || !out || out_size == 0) return 0;
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    p = strstr(json, pattern);
    if (!p) return 0;
    p += strlen(pattern);
    while (*p == ' ' || *p == '\t') p++;
    if (*p++ != ':') return 0;
    while (*p == ' ' || *p == '\t') p++;
    if (*p++ != '"') return 0;
    while (*p && *p != '"') {
        unsigned char c = (unsigned char)*p++;
        if (c == '\\') {
            c = (unsigned char)*p++;
            switch (c) {
            case '"': case '\\': case '/': break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case 'n': c = '\n'; break;
            case 'r': c = '\r'; break;
            case 't': c = '\t'; break;
            default: return 0; /* includes unsupported \u escapes */
            }
        }
        if (n + 1 >= out_size) return 0;
        out[n++] = (char)c;
    }
    if (*p != '"') return 0;
    out[n] = '\0';
    return 1;
}

static int json_int(const char *json, const char *key, int fallback)
{
    char pattern[80];
    const char *p;
    char *end;
    long value;
    if (!json || !key) return fallback;
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    p = strstr(json, pattern);
    if (!p) return fallback;
    p += strlen(pattern);
    while (*p == ' ' || *p == '\t') p++;
    if (*p++ != ':') return fallback;
    while (*p == ' ' || *p == '\t') p++;
    value = strtol(p, &end, 10);
    return end == p ? fallback : (int)value;
}

static int json_escape(const char *input, char *out, size_t out_size)
{
    size_t n = 0;
    if (!input || !out || out_size == 0) return 0;
    while (*input) {
        unsigned char c = (unsigned char)*input++;
        const char *escape = NULL;
        switch (c) {
        case '"': escape = "\\\""; break;
        case '\\': escape = "\\\\"; break;
        case '\b': escape = "\\b"; break;
        case '\f': escape = "\\f"; break;
        case '\n': escape = "\\n"; break;
        case '\r': escape = "\\r"; break;
        case '\t': escape = "\\t"; break;
        default: break;
        }
        if (escape) {
            if (n + 2 >= out_size) return 0;
            out[n++] = escape[0]; out[n++] = escape[1];
        } else {
            if (c < 0x20 || n + 1 >= out_size) return 0;
            out[n++] = (char)c;
        }
    }
    out[n] = '\0';
    return 1;
}

int present_shot_listener_process_request(const char *request,
                                          char *response,
                                          size_t response_size)
{
    char command[64];
    int id = json_int(request, "id", 0);
    int length;
    if (!response || response_size == 0) return 0;
    if (!json_string(request, "cmd", command, sizeof(command))) {
        length = snprintf(response, response_size,
                          "{\"id\":%d,\"ok\":false,\"error\":\"missing command\"}\n", id);
    } else if (strcmp(command, "present_shot_seq") == 0) {
        length = snprintf(response, response_size,
                          "{\"id\":%d,\"ok\":true,\"seq\":%d,\"wrote\":%d}\n",
                          id, present_shot_seq(), present_shot_ok());
    } else if (strcmp(command, "present_shot") == 0) {
        char path[512];
        char escaped[1024];
        if (!json_string(request, "path", path, sizeof(path)))
            snprintf(path, sizeof(path), "%s", "psx_present_shot.png");
        if (!present_shot_request(path)) {
            length = snprintf(response, response_size,
                              "{\"id\":%d,\"ok\":false,\"error\":\"present_shot unavailable\"}\n",
                              id);
        } else if (!json_escape(path, escaped, sizeof(escaped))) {
            length = snprintf(response, response_size,
                              "{\"id\":%d,\"ok\":false,\"error\":\"invalid path\"}\n",
                              id);
        } else {
            length = snprintf(response, response_size,
                              "{\"id\":%d,\"ok\":true,\"path\":\"%s\",\"staged\":true,\"seq\":%d}\n",
                              id, escaped, present_shot_seq());
        }
    } else {
        length = snprintf(response, response_size,
                          "{\"id\":%d,\"ok\":false,\"error\":\"unknown command\"}\n", id);
    }
    if (length < 0) return 0;
    if ((size_t)length >= response_size) {
        response[response_size - 1] = '\0';
        return (int)(response_size - 1);
    }
    return length;
}

void present_shot_listener_init(int port)
{
    struct sockaddr_in address;
    int reuse = 1;
    if (s_shot_listen != SHOT_INVALID) return;
#ifdef _WIN32
    {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return;
    }
#endif
    s_shot_listen = socket(AF_INET, SOCK_STREAM, 0);
    if (s_shot_listen == SHOT_INVALID) return;
    setsockopt(s_shot_listen, SOL_SOCKET, SO_REUSEADDR,
               (const char *)&reuse, sizeof(reuse));
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons((uint16_t)(port > 0 ? port : DEFAULT_DEBUG_PORT));
    if (bind(s_shot_listen, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        listen(s_shot_listen, 4) != 0) {
        shot_close(s_shot_listen);
        s_shot_listen = SHOT_INVALID;
        return;
    }
    shot_set_nonblocking(s_shot_listen);
    fprintf(stdout, "psxrecomp: present_shot listener LISTENING on 127.0.0.1:%d\n",
            port > 0 ? port : DEFAULT_DEBUG_PORT);
}

static void shot_drop_client(void)
{
    if (s_shot_client != SHOT_INVALID) shot_close(s_shot_client);
    s_shot_client = SHOT_INVALID;
    s_shot_request_len = 0;
}

void present_shot_listener_poll(void)
{
    if (s_shot_listen == SHOT_INVALID) return;
    if (s_shot_client == SHOT_INVALID) {
        s_shot_client = accept(s_shot_listen, NULL, NULL);
        if (s_shot_client == SHOT_INVALID) return;
        shot_set_nonblocking(s_shot_client);
        s_shot_request_len = 0;
    }
    for (;;) {
        int received = recv(s_shot_client,
                            s_shot_request + s_shot_request_len,
                            (int)(sizeof(s_shot_request) - 1 - s_shot_request_len), 0);
        if (received > 0) {
            char *newline;
            s_shot_request_len += (size_t)received;
            s_shot_request[s_shot_request_len] = '\0';
            newline = strchr(s_shot_request, '\n');
            if (newline) {
                char response[SHOT_RESPONSE_CAP];
                int response_len;
                *newline = '\0';
                response_len = present_shot_listener_process_request(
                    s_shot_request, response, sizeof(response));
                if (response_len > 0)
                    (void)send(s_shot_client, response, response_len, 0);
                shot_drop_client();
                return;
            }
            if (s_shot_request_len + 1 >= sizeof(s_shot_request)) {
                shot_drop_client();
                return;
            }
            continue;
        }
        if (received == 0) { shot_drop_client(); return; }
        if (shot_would_block()) return;
        shot_drop_client();
        return;
    }
}

void present_shot_listener_shutdown(void)
{
    shot_drop_client();
    if (s_shot_listen != SHOT_INVALID) shot_close(s_shot_listen);
    s_shot_listen = SHOT_INVALID;
#ifdef _WIN32
    WSACleanup();
#endif
}
