/*
 * lamassu-server — the server's transport, and nothing else.
 *
 * Everything this file does is move bytes: read an HTTP request, hand the body
 * to a precompiled function, write the rendered response back. It does not
 * parse JavaScript, and it cannot: like tools/run_bc.c it includes <lamassu.h>
 * and not <lamassu_compile.h>, and it links liblamassu_runtime.a ALONE. The
 * lexer, parser and compiler are not in the binary. Templates are compiled
 * ahead of time by `lamassu --emit-bytecode`; this process only ever runs the
 * result, so a request body reaches a bytecode verifier's output and never a
 * compiler. `make server-check` is that claim checked against the linked object.
 *
 * Shaped after the sibling nisaba-db repo's server/main.c, which solved this
 * first: one main() over three targets, a poll loop with non-blocking sockets,
 * a bounded client table, and a --stdio transport that works where sockets do
 * not.
 *
 * THREE TARGETS, ONE MAIN
 *
 *   --stdio     one HTTP request on stdin, one response on stdout. Works on
 *               every target, including wasm32-wasip1 and Node's WASI host,
 *               which have no sockets at all -- so the same binary is testable
 *               everywhere the engine builds.
 *   (default)   a TCP listener on loopback. Needs sockets, which means
 *               wasm32-wasip2 or native.
 *
 * ONE PROCESS PER BYTECODE DIRECTORY. The directory is the preopen (".") and
 * the process owns it for its lifetime. Every *.jsbc in it is loaded and
 * verified ONCE at startup and becomes a route: page.jsbc answers /page. A
 * request pays for execution and nothing else -- which is the entire point of
 * having split the compiler out.
 *
 * MANY CONNECTIONS, ONE AT A TIME THROUGH THE ENGINE. poll() over the listener
 * and every accepted socket; whichever is ready is served, and a client that
 * holds a connection open without asking anything costs a table slot and
 * nothing else. There are no threads and there is no second VM: a render runs
 * to completion before the next request is looked at. That means the sockets
 * are non-blocking and a connection carries state -- a request that has only
 * partly arrived, and a response that has only partly gone out. A blocking read
 * would hand one client the whole server.
 *
 * ONE REALM, ONE TENANT. All routes share a VM, so a route that writes a global
 * is visible to the next request. That is a deliberate trade for a process that
 * serves one directory of one tenant's templates; it is NOT multi-tenant
 * isolation. For that, run one process per tenant -- or an instance per
 * request, which is what src/reactor.c is for.
 *
 * print() IS A LOG, NOT OUTPUT. It goes to stderr. The response body is the
 * module's completion value, so a stray print cannot corrupt a rendered page.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h> /* strncasecmp */

#include <dirent.h>
#include <unistd.h>

#if defined(LAMASSU_SOCKETS)
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#endif

#include "lamassu.h"
#include "utf8.h"

#define MAX_CLIENTS 64
#define MAX_ROUTES 64
#define NAME_MAX_LEN 64
/* A request larger than this is refused rather than allocated. Bounded for the
 * reason every other table here is: a server that allocates whatever a client
 * claims has a failure mode nobody tests. */
#define REQ_MAX (4u * 1024u * 1024u)
#define DEFAULT_PORT 8080
#define DEFAULT_FUEL 200000000ull
#define DEFAULT_HEAP (64u * 1024u * 1024u)

/* ---- growable byte buffer ---- */

typedef struct {
    char *p;
    size_t len, cap;
} Buf;

static bool buf_reserve(Buf *b, size_t extra) {
    if (b->len + extra + 1 <= b->cap)
        return true;
    size_t ncap = b->cap ? b->cap * 2 : 256;
    while (ncap < b->len + extra + 1)
        ncap *= 2;
    char *np = realloc(b->p, ncap);
    if (!np)
        return false;
    b->p = np;
    b->cap = ncap;
    return true;
}

static bool buf_add(Buf *b, const char *s, size_t n) {
    if (!buf_reserve(b, n))
        return false;
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = 0;
    return true;
}

static void buf_free(Buf *b) { free(b->p); b->p = NULL; b->len = b->cap = 0; }

static void buf_utf16(Buf *b, const uint16_t *u, size_t n) {
    for (size_t i = 0; i < n;) {
        char t[4];
        int len = js_utf8_encode_cp(js_utf16_next_cp(u, n, &i), t);
        buf_add(b, t, (size_t)len);
    }
}

/* ---- engine ---- */

typedef struct {
    char name[NAME_MAX_LEN];
    JsValue fn; /* GC-protected for the process's lifetime */
} Route;

static JsVm *g_vm;
static JsContext *g_ctx;
static Route g_routes[MAX_ROUTES];
static int g_route_count;
static uint64_t g_fuel = DEFAULT_FUEL;

/* the current request body, handed to the guest by __input() */
static uint16_t *g_input;
static size_t g_input_len;

static bool native_print(JsContext *ctx, JsValue this_val, const JsValue *args,
                         int argc, JsValue *result) {
    (void)this_val;
    Buf line = {0};
    for (int i = 0; i < argc; i++) {
        if (i)
            buf_add(&line, " ", 1);
        JsValue s = js_to_string(ctx, args[i]);
        size_t n;
        const uint16_t *u = js_string_units(s, &n);
        if (u)
            buf_utf16(&line, u, n);
    }
    fprintf(stderr, "[js] %s\n", line.p ? line.p : "");
    buf_free(&line);
    *result = js_undefined();
    return true;
}

static bool native_input(JsContext *ctx, JsValue this_val, const JsValue *args,
                         int argc, JsValue *result) {
    (void)ctx;
    (void)this_val;
    (void)args;
    (void)argc;
    static const uint16_t empty[1] = {0};
    *result = js_atom(g_vm, g_input ? g_input : empty, g_input ? g_input_len : 0);
    return true;
}

static uint8_t *read_file(const char *path, size_t *len_out) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) {
        fclose(f);
        return NULL;
    }
    uint8_t *buf = malloc((size_t)sz ? (size_t)sz : 1);
    if (!buf || fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *len_out = (size_t)sz;
    return buf;
}

/*
 * Loads every *.jsbc in the preopened directory as a route, verifying each
 * before it can ever serve a request. A file that fails to load is named and
 * skipped rather than taking the server down with it -- one bad artifact in a
 * deploy should cost that route, not every route.
 */
static int load_routes(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) {
        fprintf(stderr, "lamassu-server: cannot open directory %s\n", dir);
        return -1;
    }
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        size_t nl = strlen(e->d_name);
        if (nl < 6 || strcmp(e->d_name + nl - 5, ".jsbc") != 0)
            continue;
        if (g_route_count >= MAX_ROUTES) {
            fprintf(stderr, "lamassu-server: more than %d routes; ignoring %s\n",
                    MAX_ROUTES, e->d_name);
            break;
        }
        size_t base = nl - 5;
        if (base >= NAME_MAX_LEN) {
            fprintf(stderr, "lamassu-server: route name too long: %s\n", e->d_name);
            continue;
        }
        char path[512];
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        size_t blen;
        uint8_t *bytes = read_file(path, &blen);
        if (!bytes) {
            fprintf(stderr, "lamassu-server: cannot read %s\n", path);
            continue;
        }
        const char *err = NULL;
        JsValue fn = js_bytecode_load(g_ctx, bytes, blen, &err);
        free(bytes);
        if (!js_is_function(fn)) {
            fprintf(stderr, "lamassu-server: %s rejected: %s\n", e->d_name,
                    err ? err : "invalid bytecode");
            continue;
        }
        Route *r = &g_routes[g_route_count++];
        memcpy(r->name, e->d_name, base);
        r->name[base] = 0;
        r->fn = fn;
        js_gc_protect(g_vm, &r->fn); /* rooted for the process's lifetime */
        fprintf(stderr, "lamassu-server: route /%s (%zu bytes)\n", r->name, blen);
    }
    closedir(d);
    if (g_route_count == 0)
        fprintf(stderr, "lamassu-server: warning: no .jsbc files in %s\n", dir);
    return g_route_count;
}

static Route *find_route(const char *name, size_t n) {
    for (int i = 0; i < g_route_count; i++)
        if (strlen(g_routes[i].name) == n && memcmp(g_routes[i].name, name, n) == 0)
            return &g_routes[i];
    return NULL;
}

/* Sets the request body as __input()'s value. */
static void set_input(const char *body, size_t n) {
    free(g_input);
    g_input = NULL;
    g_input_len = 0;
    if (!n)
        return;
    uint16_t *u = malloc(n * sizeof(uint16_t));
    if (!u)
        return;
    g_input_len = js_utf8_to_utf16((const uint8_t *)body, n, u);
    g_input = u;
}

/*
 * Runs a route and appends its completion value to `out`. Returns the HTTP
 * status: 200 when the module completed, 500 when it threw (the body is then
 * the error, which is what a template author needs to see) and 503 when it is
 * still pending -- nothing here settles host promises, so a module that awaits
 * something external never completes rather than hanging the server.
 */
static int run_route(Route *r, Buf *out) {
    js_context_set_fuel(g_ctx, g_fuel);
    JsValue p = js_run_module(g_ctx, r->fn);
    js_gc_protect(g_vm, &p);
    int st = js_promise_state(p);
    JsValue result = js_promise_result(p);
    js_gc_protect(g_vm, &result);
    JsValue s = js_to_string(g_ctx, result);
    size_t n;
    const uint16_t *u = js_string_units(s, &n);
    if (u && !(st == 1 && js_is_undefined(result)))
        buf_utf16(out, u, n);
    js_gc_unprotect(g_vm, &result);
    js_gc_unprotect(g_vm, &p);
    return st == 1 ? 200 : (st == 2 ? 500 : 503);
}

/* ---- HTTP ---- */

/* memmem is a GNU extension wasi-libc does not carry; the needle here is four
 * bytes, so a plain scan is the whole implementation. */
static const char *find_bytes(const char *h, size_t hn, const char *n, size_t nn) {
    if (nn > hn)
        return NULL;
    for (size_t i = 0; i + nn <= hn; i++)
        if (memcmp(h + i, n, nn) == 0)
            return h + i;
    return NULL;
}

static const char *status_text(int code) {
    switch (code) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 413: return "Payload Too Large";
    case 500: return "Internal Server Error";
    case 503: return "Service Unavailable";
    default:  return "Error";
    }
}

static void write_response(Buf *out, int code, const char *ctype, const char *body,
                           size_t blen) {
    char head[256];
    int n = snprintf(head, sizeof head,
                     "HTTP/1.1 %d %s\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %zu\r\n"
                     "Connection: close\r\n"
                     "\r\n",
                     code, status_text(code), ctype, blen);
    buf_add(out, head, (size_t)n);
    buf_add(out, body, blen);
}

/*
 * Is there a complete request in `in`? Returns the total byte length if so, 0
 * if more is needed, and -1 if it is malformed beyond recovery. Only what this
 * server needs: a request line, headers, and a Content-Length body. No chunked
 * transfer-encoding -- a client sending one gets a 400 rather than a wrong
 * answer, which is the right failure for something a proxy will never emit
 * toward an origin it just spoke HTTP/1.1 to.
 */
static long request_length(const char *in, size_t len, size_t *body_off,
                           size_t *body_len) {
    const char *end = find_bytes(in, len, "\r\n\r\n", 4);
    if (!end)
        return 0;
    size_t head_len = (size_t)(end - in) + 4;
    /* Header lines run to end+2, not end: `end` points AT the CR that begins
     * the terminating CRLFCRLF, which is also the last header line's own CR.
     * Scanning only [in, end) loses that line -- and losing the last line
     * means losing Content-Length on a well-formed request, which reads as a
     * body that isn't there rather than as an error. */
    const char *hdr_end = end + 2;
    size_t clen = 0;
    for (const char *p = in; p < hdr_end;) {
        const char *eol = memchr(p, '\r', (size_t)(hdr_end - p));
        if (!eol)
            break;
        if ((size_t)(eol - p) > 15 && strncasecmp(p, "Content-Length:", 15) == 0) {
            const char *v = p + 15;
            while (v < eol && (*v == ' ' || *v == '\t'))
                v++;
            clen = 0;
            for (; v < eol && *v >= '0' && *v <= '9'; v++)
                clen = clen * 10 + (size_t)(*v - '0');
            if (clen > REQ_MAX)
                return -1;
        } else if ((size_t)(eol - p) > 18 &&
                   strncasecmp(p, "Transfer-Encoding:", 18) == 0) {
            return -1; /* chunked: refuse rather than mis-frame */
        }
        p = eol + 2 > hdr_end ? hdr_end : eol + 2;
    }
    if (head_len + clen > len)
        return 0;
    *body_off = head_len;
    *body_len = clen;
    return (long)(head_len + clen);
}

/* Extracts the path from the request line into `out`; false if malformed. */
static bool request_path(const char *in, size_t len, char *out, size_t cap) {
    const char *sp = memchr(in, ' ', len);
    if (!sp)
        return false;
    const char *start = sp + 1;
    const char *sp2 = memchr(start, ' ', len - (size_t)(start - in));
    if (!sp2)
        return false;
    size_t n = (size_t)(sp2 - start);
    const char *q = memchr(start, '?', n); /* ignore any query string */
    if (q)
        n = (size_t)(q - start);
    if (n == 0 || n >= cap)
        return false;
    memcpy(out, start, n);
    out[n] = 0;
    return true;
}

/* Serves one complete request into `out`. */
static void handle_request(const char *req, size_t len, size_t body_off,
                           size_t body_len, Buf *out) {
    char path[256];
    if (!request_path(req, len, path, sizeof path)) {
        write_response(out, 400, "text/plain", "bad request\n", 12);
        return;
    }
    const char *name = path[0] == '/' ? path + 1 : path;
    Route *r = find_route(name, strlen(name));
    if (!r) {
        write_response(out, 404, "text/plain", "no such route\n", 14);
        return;
    }
    set_input(req + body_off, body_len);
    Buf body = {0};
    int code = run_route(r, &body);
    write_response(out, code, code == 200 ? "text/html; charset=utf-8" : "text/plain",
                   body.p ? body.p : "", body.len);
    buf_free(&body);
}

/* ---- transports ---- */

/* One request on stdin, one response on stdout. The transport that works
 * everywhere, including targets with no sockets at all. */
static int serve_stdio(void) {
    Buf in = {0};
    char chunk[4096];
    size_t body_off = 0, body_len = 0;
    long total;
    for (;;) {
        total = in.len ? request_length(in.p, in.len, &body_off, &body_len) : 0;
        if (total > 0)
            break;
        if (total < 0) {
            buf_free(&in);
            fputs("HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n", stdout);
            return 1;
        }
        size_t got = fread(chunk, 1, sizeof chunk, stdin);
        if (got == 0) {
            buf_free(&in);
            return in.len ? 1 : 0; /* clean EOF before any request */
        }
        if (in.len + got > REQ_MAX || !buf_add(&in, chunk, got)) {
            buf_free(&in);
            fputs("HTTP/1.1 413 Payload Too Large\r\nContent-Length: 0\r\n\r\n", stdout);
            return 1;
        }
    }
    Buf out = {0};
    handle_request(in.p, (size_t)total, body_off, body_len, &out);
    fwrite(out.p, 1, out.len, stdout);
    fflush(stdout);
    buf_free(&out);
    buf_free(&in);
    return 0;
}

#if defined(LAMASSU_SOCKETS)

typedef struct {
    int fd;
    Buf in;
    Buf out;
    size_t out_off;
    bool closing;
} Conn;

static int listen_on(int port) {
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) {
        perror("socket");
        return -1;
    }
    int one = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = htonl(0x7f000001); /* loopback only */
    if (bind(srv, (struct sockaddr *)&addr, sizeof addr) != 0) {
        perror("bind");
        close(srv);
        return -1;
    }
    if (listen(srv, 16) != 0) {
        perror("listen");
        close(srv);
        return -1;
    }
    return srv;
}

static void set_nonblock(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0)
        fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

static void conn_close(Conn *c) {
    close(c->fd);
    buf_free(&c->in);
    buf_free(&c->out);
    c->fd = -1;
    c->out_off = 0;
    c->closing = false;
}

/*
 * The server loop: poll the listener and every client, serve whichever is
 * ready, repeat. A client is polled for POLLOUT while it owes bytes and POLLIN
 * otherwise, which is also the backpressure: nothing new is read from a client
 * whose last answer has not gone out.
 */
static int serve_forever(int srv, int max_clients) {
    Conn *cs = calloc((size_t)max_clients, sizeof *cs);
    struct pollfd *pf = calloc((size_t)max_clients + 1, sizeof *pf);
    if (!cs || !pf) {
        free(cs);
        free(pf);
        return -1;
    }
    for (int i = 0; i < max_clients; i++)
        cs[i].fd = -1;
    int n = 0; /* live connections, packed at the front of cs */

    for (;;) {
        pf[0].fd = srv;
        pf[0].events = POLLIN;
        pf[0].revents = 0;
        for (int i = 0; i < n; i++) {
            pf[i + 1].fd = cs[i].fd;
            pf[i + 1].events = (cs[i].out.len > cs[i].out_off) ? POLLOUT : POLLIN;
            pf[i + 1].revents = 0;
        }
        if (poll(pf, (nfds_t)(n + 1), -1) < 0) {
            if (errno == EINTR)
                continue;
            perror("poll");
            break;
        }

        if ((pf[0].revents & POLLIN) && n < max_clients) {
            int fd = accept(srv, NULL, NULL);
            if (fd >= 0) {
                set_nonblock(fd);
                cs[n].fd = fd;
                cs[n].in = (Buf){0};
                cs[n].out = (Buf){0};
                cs[n].out_off = 0;
                cs[n].closing = false;
                n++;
            }
        }

        for (int i = 0; i < n;) {
            Conn *c = &cs[i];
            short re = pf[i + 1].revents;
            bool drop = false;

            if (re & POLLOUT) {
                size_t owed = c->out.len - c->out_off;
                ssize_t w = write(c->fd, c->out.p + c->out_off, owed);
                if (w > 0)
                    c->out_off += (size_t)w;
                else if (w < 0 && errno != EAGAIN && errno != EINTR)
                    drop = true;
                if (!drop && c->out_off >= c->out.len)
                    drop = c->closing; /* Connection: close */
            } else if (re & POLLIN) {
                char chunk[4096];
                ssize_t got = read(c->fd, chunk, sizeof chunk);
                if (got == 0)
                    drop = true;
                else if (got < 0)
                    drop = errno != EAGAIN && errno != EINTR;
                else if (c->in.len + (size_t)got > REQ_MAX ||
                         !buf_add(&c->in, chunk, (size_t)got)) {
                    write_response(&c->out, 413, "text/plain", "too large\n", 10);
                    c->closing = true;
                } else {
                    size_t body_off = 0, body_len = 0;
                    long total = request_length(c->in.p, c->in.len, &body_off, &body_len);
                    if (total < 0) {
                        write_response(&c->out, 400, "text/plain", "bad request\n", 12);
                        c->closing = true;
                    } else if (total > 0) {
                        handle_request(c->in.p, (size_t)total, body_off, body_len,
                                       &c->out);
                        c->closing = true;
                    }
                }
            } else if (re & (POLLERR | POLLHUP | POLLNVAL)) {
                drop = true;
            }

            if (drop) {
                conn_close(c);
                cs[i] = cs[n - 1]; /* keep the table packed */
                cs[n - 1].fd = -1;
                n--;
            } else {
                i++;
            }
        }
    }
    for (int i = 0; i < n; i++)
        conn_close(&cs[i]);
    free(cs);
    free(pf);
    return 0;
}
#endif /* LAMASSU_SOCKETS */

/* ---- main ---- */

static void usage(void) {
    fprintf(stderr,
            "usage: lamassu-server [options]\n"
            "  --port N        TCP listener on loopback (default %d; needs sockets)\n"
            "  --stdio         one HTTP request on stdin, response on stdout\n"
            "  --dir PATH      directory of .jsbc routes (default \".\")\n"
            "  --fuel N        per-request interpreter budget, 0 = unlimited\n"
            "  --heap N        heap cap in bytes, 0 = unlimited\n"
            "  --max-clients N connections held at once (default %d)\n",
            DEFAULT_PORT, MAX_CLIENTS);
}

int main(int argc, char **argv) {
    int port = DEFAULT_PORT;
    bool stdio_mode = false;
    const char *dir = ".";
    int max_clients = MAX_CLIENTS;
    size_t heap_limit = DEFAULT_HEAP;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--stdio") == 0)
            stdio_mode = true;
        else if (strcmp(a, "--port") == 0 && i + 1 < argc)
            port = atoi(argv[++i]);
        else if (strcmp(a, "--dir") == 0 && i + 1 < argc)
            dir = argv[++i];
        else if (strcmp(a, "--fuel") == 0 && i + 1 < argc)
            g_fuel = strtoull(argv[++i], NULL, 10);
        else if (strcmp(a, "--heap") == 0 && i + 1 < argc)
            heap_limit = (size_t)strtoull(argv[++i], NULL, 10);
        else if (strcmp(a, "--max-clients") == 0 && i + 1 < argc) {
            max_clients = atoi(argv[++i]);
            if (max_clients < 1 || max_clients > MAX_CLIENTS)
                max_clients = MAX_CLIENTS;
        } else {
            usage();
            return 2;
        }
    }

    JsVmConfig cfg = {0};
    cfg.heap_limit = heap_limit;
    g_vm = js_vm_new(heap_limit ? &cfg : NULL);
    g_ctx = g_vm ? js_context_new(g_vm) : NULL;
    if (!g_ctx) {
        fprintf(stderr, "lamassu-server: out of memory\n");
        return 2;
    }
    static const uint16_t print_name[] = {'p', 'r', 'i', 'n', 't'};
    static const uint16_t input_name[] = {'_', '_', 'i', 'n', 'p', 'u', 't'};
    js_register_native(g_ctx, print_name, 5, native_print, NULL);
    js_register_native(g_ctx, input_name, 7, native_input, NULL);

    if (load_routes(dir) < 0)
        return 2;

    if (stdio_mode)
        return serve_stdio();

#if defined(LAMASSU_SOCKETS)
    int srv = listen_on(port);
    if (srv < 0)
        return 2;
    fprintf(stderr, "lamassu-server: listening on 127.0.0.1:%d (%d routes)\n", port,
            g_route_count);
    int rc = serve_forever(srv, max_clients);
    close(srv);
    return rc;
#else
    /* preview1 has no socket() at all -- not a missing right, no such
     * function -- so a port is not something this build can be asked for. */
    (void)port;
    (void)max_clients;
    fprintf(stderr, "lamassu-server: this build has no sockets; use --stdio\n");
    return 2;
#endif
}
