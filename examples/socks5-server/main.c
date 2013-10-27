/* Copyright StrongLoop, Inc. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "uv.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>  /* getopt() */

#ifndef INET6_ADDRSTRLEN
# define INET6_ADDRSTRLEN 63  /* Actually, it's more like 46. */
#endif

/* ASSERT() is for debug checks, CHECK() for run-time sanity checks.
 * We use CHECK() predominantly for checking return values where we know it's
 * not possible for an error to happen but don't want to quietly continue when
 * the impossible happens.
 */
#if defined(NDEBUG)
# define ASSERT(exp)
# define CHECK(exp)  do { if (!(exp)) abort(); } while (0)
#else
# define ASSERT(exp) assert(exp)
# define CHECK(exp)  assert(exp)
#endif

#define UNREACHABLE() CHECK(!"Unreachable code reached.")

#define CONTAINER_OF(ptr, type, field)                                          \
  ((type *) ((char *) (ptr) - ((char *) &((type *) 0)->field)))

#define USE(var) ((void) &(var))

typedef struct {
  enum { TCP4, TCP6, UDP4, UDP6 } type;
  uv_timer_t timer_handle;  /* For detecting timeouts. */
  union {
    uv_tcp_t tcp;
    uv_udp_t udp;
  } handle;
} conn;

typedef struct {
  conn incoming;
  conn outgoing;
  uv_timer_t timer_handle;
} client_ctx;

typedef struct {
  uv_tcp_t tcp_handle;
} server_ctx;

static void pr_do(FILE *stream,
                  const char *label,
                  const char *fmt,
                  va_list ap);
static void pr_info(const char *fmt, ...);
static void pr_warn(const char *fmt, ...);
static void pr_err(const char *fmt, ...);
static void *xmalloc(size_t size);
static void parse_opts(int argc, char **argv);
static void usage(void);
static void do_bind(uv_getaddrinfo_t *req, int status, struct addrinfo *ai);
static void on_connection(uv_stream_t *server, int status);
static void on_alloc(uv_handle_t *handle, size_t size, uv_buf_t *buf);
static void on_read(uv_stream_t *handle, ssize_t nread, const uv_buf_t *buf);

static const char *bindhost = "127.0.0.1";  /* Host/address to bind to. */
static unsigned short bindport = 1080;  /* Port number to bind to. */
static const char *progname = __FILE__;  /* Reset in parse_opts(). */
static server_ctx *servers = NULL;
static int exit_code = 1;

int main(int argc, char **argv) {
  struct addrinfo hints;
  uv_getaddrinfo_t ai;
  uv_loop_t *loop;
  int err;

  parse_opts(argc, argv);
  loop = uv_default_loop();

  /* Resolve the address of the interface that we should bind to.
   * The getaddrinfo callback starts the server and everything else.
   */
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;

  err = uv_getaddrinfo(loop, &ai, do_bind, bindhost, NULL, &hints);
  if (err != 0) {
    pr_err("getaddrinfo: %s", uv_strerror(err));
    exit(1);
  }

  /* Start the event loop.  Control continues in do_bind(). */
  if (uv_run(loop, UV_RUN_DEFAULT)) {
    abort();
  }

  /* Please Valgrind. */
  uv_loop_delete(loop);
  free(servers);

  return exit_code;
}

static void pr_do(FILE *stream,
                  const char *label,
                  const char *fmt,
                  va_list ap) {
  char fmtbuf[1024];
  vsnprintf(fmtbuf, sizeof(fmtbuf), fmt, ap);
  fprintf(stream, "%s:%s: %s\n", progname, label, fmtbuf);
}

static void pr_info(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  pr_do(stdout, "info", fmt, ap);
  va_end(ap);
}

static void pr_warn(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  pr_do(stderr, "warn", fmt, ap);
  va_end(ap);
}

static void pr_err(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  pr_do(stderr, "error", fmt, ap);
  va_end(ap);
}

static void *xmalloc(size_t size) {
  void *ptr;

  ptr = malloc(size);
  if (ptr == NULL) {
    pr_err("out of memory, need %lu bytes", (unsigned long) size);
    exit(1);
  }

  return ptr;
}

static void parse_opts(int argc, char **argv) {
  int opt;

  progname = argv[0];
  while (-1 != (opt = getopt(argc, argv, "H:hp:"))) {
    switch (opt) {
      case 'H':
        bindhost = optarg;
        break;

      case 'p':
        if (1 != sscanf(optarg, "%hu", &bindport)) {
          pr_err("bad port number: %s", optarg);
          usage();
        }
        break;

      default:
        usage();
    }
  }
}

static void usage(void) {
  printf("Usage:\n"
         "\n"
         "  %s [-b <address> [-h] [-p <port>]\n"
         "\n"
         "Options:\n"
         "\n"
         "  -b <hostname|address>  Bind to this address or hostname.\n"
         "                         Default: \"127.0.0.1\"\n"
         "  -h                     Show this help message.\n"
         "  -p <port>              Bind to this port number.  Default: 1080\n"
         "",
         progname);
  exit(1);
}

/* Bind a server to each address that getaddrinfo() reported. */
static void do_bind(uv_getaddrinfo_t *req, int status, struct addrinfo *addrs) {
  char addrbuf[INET6_ADDRSTRLEN + 1];
  unsigned int ipv4_naddrs;
  unsigned int ipv6_naddrs;
  struct addrinfo *ai;
  const void *addrv;
  const char *what;
  uv_loop_t *loop;
  server_ctx *sx;
  unsigned int n;
  int err;
  union {
    struct sockaddr sa;
    struct sockaddr_in sin;
    struct sockaddr_in6 sin6;
  } s;

  loop = req->loop;

  if (status < 0) {
    pr_err("getaddrinfo(\"%s\"): %s", bindhost, uv_strerror(status));
    uv_freeaddrinfo(addrs);
    return;
  }

  ipv4_naddrs = 0;
  ipv6_naddrs = 0;
  for (ai = addrs; ai != NULL; ai = ai->ai_next) {
    if (ai->ai_family == AF_INET) {
      ipv4_naddrs += 1;
    } else if (ai->ai_family == AF_INET6) {
      ipv6_naddrs += 1;
    }
  }

  if (ipv4_naddrs == 0 && ipv6_naddrs == 0) {
    pr_err("%s has no IPv4/6 addresses", bindhost);
    uv_freeaddrinfo(addrs);
    return;
  }
  servers = xmalloc((ipv4_naddrs + ipv6_naddrs) * sizeof(servers[0]));

  n = 0;
  for (ai = addrs; ai != NULL; ai = ai->ai_next) {
    if (ai->ai_family != AF_INET && ai->ai_family != AF_INET6) {
      continue;
    }

    sx = servers + n;
    CHECK(0 == uv_tcp_init(loop, &sx->tcp_handle));

    if (ai->ai_family == AF_INET) {
      s.sin = *(const struct sockaddr_in *) ai->ai_addr;
      s.sin.sin_port = htons(bindport);
      addrv = &s.sin.sin_addr;
    } else if (ai->ai_family == AF_INET6) {
      s.sin6 = *(const struct sockaddr_in6 *) ai->ai_addr;
      s.sin6.sin6_port = htons(bindport);
      addrv = &s.sin6.sin6_addr;
    } else {
      UNREACHABLE();
    }

    if (uv_inet_ntop(s.sa.sa_family, addrv, addrbuf, sizeof(addrbuf))) {
      UNREACHABLE();
    }

    what = "uv_tcp_bind";
    err = uv_tcp_bind(&sx->tcp_handle, &s.sa);
    if (err == 0) {
      what = "uv_listen";
      err = uv_listen((uv_stream_t *) &sx->tcp_handle, 128, on_connection);
    }

    if (err != 0) {
      pr_err("%s(\"%s:%hu\"): %s", what, addrbuf, bindport, uv_strerror(err));
      while (n > 0) {
        n -= 1;
        uv_close((uv_handle_t *) (servers + n), NULL);
      }
      break;
    }

    pr_info("Listening on %s:%hu", addrbuf, bindport);
    n += 1;
  }

  uv_freeaddrinfo(addrs);
}

static void on_connection(uv_stream_t *server, int status) {
  client_ctx *cx;
  server_ctx *sx;

  CHECK(status == 0);
  sx = CONTAINER_OF(server, server_ctx, tcp_handle);
  cx = xmalloc(sizeof(*cx));
  CHECK(0 == uv_tcp_init(server->loop, &cx->incoming.handle.tcp));
  CHECK(0 == uv_accept(server, (uv_stream_t *) &cx->incoming.handle.tcp));
  CHECK(0 == uv_read_start((uv_stream_t *) &cx->incoming.handle.tcp,
                           on_alloc,
                           on_read));
}

static void on_alloc(uv_handle_t *handle, size_t size, uv_buf_t *buf) {
  buf->base = xmalloc(size);
  buf->len = size;
}

static void on_read(uv_stream_t *handle, ssize_t nread, const uv_buf_t *buf) {
  pr_info("received %ld bytes", (long) nread);
  free(buf->base);
}
