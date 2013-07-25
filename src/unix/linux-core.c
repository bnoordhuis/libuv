/* Copyright Joyent, Inc. and other Node contributors. All rights reserved.
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
#include "internal.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>

#include <net/if.h>
#include <sys/param.h>
#include <sys/prctl.h>
#include <sys/sysinfo.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>

#ifndef __ANDROID__
#define HAVE_IFADDRS_H 1
#endif

#ifdef __UCLIBC__
# if __UCLIBC_MAJOR__ < 0 || __UCLIBC_MINOR__ < 9 || __UCLIBC_SUBLEVEL__ < 32
#  undef HAVE_IFADDRS_H
# endif
#endif
#ifdef HAVE_IFADDRS_H
# include <ifaddrs.h>
# include <sys/socket.h>
# include <net/ethernet.h>
# include <linux/if_packet.h>
#endif

#undef NANOSEC
#define NANOSEC ((uint64_t) 1e9)

/* This is rather annoying: CLOCK_BOOTTIME lives in <linux/time.h> but we can't
 * include that file because it conflicts with <time.h>. We'll just have to
 * define it ourselves.
 */
#ifndef CLOCK_BOOTTIME
# define CLOCK_BOOTTIME 7
#endif

static int read_models(unsigned int numcpus, uv_cpu_info_t* ci);
static int read_times(unsigned int numcpus, uv_cpu_info_t* ci);
static void read_speeds(unsigned int numcpus, uv_cpu_info_t* ci);
static unsigned long read_cpufreq(unsigned int cpunum);


int uv__platform_loop_init(uv_loop_t* loop, int default_loop) {
  int fd;

  fd = uv__epoll_create1(UV__EPOLL_CLOEXEC);

  /* epoll_create1() can fail either because it's not implemented (old kernel)
   * or because it doesn't understand the EPOLL_CLOEXEC flag.
   */
  if (fd == -1 && (errno == ENOSYS || errno == EINVAL)) {
    fd = uv__epoll_create(256);

    if (fd != -1)
      uv__cloexec(fd, 1);
  }

  loop->backend_fd = fd;
  loop->inotify_fd = -1;
  loop->inotify_watchers = NULL;

  if (fd == -1)
    return -errno;

  return 0;
}


void uv__platform_loop_delete(uv_loop_t* loop) {
  if (loop->inotify_fd == -1) return;
  uv__io_stop(loop, &loop->inotify_read_watcher, UV__POLLIN);
  close(loop->inotify_fd);
  loop->inotify_fd = -1;
}


void uv__io_poll(uv_loop_t* loop, int timeout) {
  struct uv__epoll_event events[1024];
  struct uv__epoll_event* pe;
  struct uv__epoll_event e;
  QUEUE* q;
  uv__io_t* w;
  uint64_t base;
  uint64_t diff;
  unsigned int pevents;
  int nevents;
  int nfds;
  int fd;
  int op;
  int i;

  if (loop->nfds == 0) {
    assert(QUEUE_EMPTY(&loop->watcher_queue));
    return;
  }

  while (!QUEUE_EMPTY(&loop->watcher_queue)) {
    q = QUEUE_HEAD(&loop->watcher_queue);
    QUEUE_REMOVE(q);
    QUEUE_INIT(q);

    w = QUEUE_DATA(q, uv__io_t, watcher_queue);
    assert(w->levents != 0);
    assert(w->fd >= 0);
    assert(w->fd < (int) loop->nwatchers);

    op = UV__EPOLL_CTL_ADD;
    if (w->events & ~UV__POLLET)
      op = UV__EPOLL_CTL_MOD;

    w->events = w->levents;

    /* We don't have to update edge-triggered file descriptors, they're already
     * being watched for both read and write readiness. Either one of two
     * things happens here:
     *
     * 1. We're being asked to watch for read/write readiness and the kernel
     *    has already reported that. When that happens, the watcher is added
     *    to the ready list for dispatch in the near future.
     *
     * 2. We're being asked to stop watching for read/write readiness. Just
     *    update the event mask and continue.
     */
    if (op == UV__EPOLL_CTL_MOD && (w->levents & UV__POLLET)) {
      pevents = w->levents & w->revents;
      if (pevents != 0) {
        w->revents &= ~pevents;
        uv__io_feed(loop, w, pevents);
      }
      continue;
    }

    /* Observation: most file descriptors will be watched for both reading and
     * writing during their lifetime. That's why we register the fd for both
     * right away because it saves a call to epoll_ctl(EPOLL_CTL_MOD) later on.
     *
     * The worst case with read-only or write-only file descriptors is  that
     * epoll_wait() wakes up unnecessarily once. In the grand scheme of things,
     * that's something we can live with because it won't happen often.
     */
    if (w->levents & UV__EPOLLET)
      e.events = UV__EPOLLIN | UV__EPOLLOUT | UV__EPOLLET;
    else
      e.events = w->levents;

    e.data = w->fd;

    if (uv__epoll_ctl(loop->backend_fd, op, w->fd, &e) == 0)
      continue;

    if (errno != EEXIST)
      abort();

    /* We're reactivating a file descriptor that's been watched before.
     * Level-triggered file descriptors we can just modify but edge-triggered
     * file descriptors have to be removed and re-added because we don't know
     * what their current state is.
     */
    assert(op == UV__EPOLL_CTL_ADD);
    if ((w->events & UV__POLLET) == 0)
      op = UV__EPOLL_CTL_MOD;
    else if (uv__epoll_ctl(loop->backend_fd, UV__EPOLL_CTL_DEL, w->fd, &e))
      abort();

    if (uv__epoll_ctl(loop->backend_fd, op, w->fd, &e))
      abort();
  }

  assert(timeout >= -1);
  base = loop->time;

  for (;;) {
    nfds = uv__epoll_wait(loop->backend_fd,
                          events,
                          ARRAY_SIZE(events),
                          timeout);

    /* Update loop->time unconditionally. It's tempting to skip the update when
     * timeout == 0 (i.e. non-blocking poll) but there is no guarantee that the
     * operating system didn't reschedule our process while in the syscall.
     */
    SAVE_ERRNO(uv__update_time(loop));

    if (nfds == 0) {
      assert(timeout != -1);
      return;
    }

    if (nfds == -1) {
      if (errno != EINTR)
        abort();

      if (timeout == -1)
        continue;

      if (timeout == 0)
        return;

      /* Interrupted by a signal. Update timeout and poll again. */
      goto update_timeout;
    }

    nevents = 0;

    for (i = 0; i < nfds; i++) {
      pe = events + i;
      fd = pe->data;

      assert(fd >= 0);
      assert((unsigned) fd < loop->nwatchers);

      w = loop->watchers[fd];
      if (w == NULL) {
        /* File descriptor that we've stopped watching, disarm it.
         *
         * Ignore all errors because we may be racing with another thread
         * when the file descriptor is closed.
         */
        uv__epoll_ctl(loop->backend_fd, UV__EPOLL_CTL_DEL, fd, pe);
        continue;
      }

      w->revents |= pe->events;

      /* In edge-triggered mode, we're always watching for read and write
       * readiness. Mask off the events that the watcher is not interested in.
       */
      pevents = w->revents & (w->events | UV__EPOLLERR | UV__EPOLLHUP);
      if (pevents != 0) {
        uv__io_feed(loop, w, pevents);
        nevents++;
      }
    }

    if (nevents != 0)
      return;

    if (timeout == 0)
      return;

    if (timeout == -1)
      continue;

update_timeout:
    assert(timeout > 0);

    diff = loop->time - base;
    if (diff >= (uint64_t) timeout)
      return;

    timeout -= diff;
  }
}


uint64_t uv__hrtime(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (((uint64_t) ts.tv_sec) * NANOSEC + ts.tv_nsec);
}


void uv_loadavg(double avg[3]) {
  struct sysinfo info;

  if (sysinfo(&info) < 0) return;

  avg[0] = (double) info.loads[0] / 65536.0;
  avg[1] = (double) info.loads[1] / 65536.0;
  avg[2] = (double) info.loads[2] / 65536.0;
}


int uv_exepath(char* buffer, size_t* size) {
  ssize_t n;

  if (buffer == NULL || size == NULL)
    return -EINVAL;

  n = readlink("/proc/self/exe", buffer, *size - 1);
  if (n == -1)
    return -errno;

  buffer[n] = '\0';
  *size = n;

  return 0;
}


uint64_t uv_get_free_memory(void) {
  return (uint64_t) sysconf(_SC_PAGESIZE) * sysconf(_SC_AVPHYS_PAGES);
}


uint64_t uv_get_total_memory(void) {
  return (uint64_t) sysconf(_SC_PAGESIZE) * sysconf(_SC_PHYS_PAGES);
}


int uv_resident_set_memory(size_t* rss) {
  char buf[1024];
  const char* s;
  ssize_t n;
  long val;
  int fd;
  int i;

  do
    fd = open("/proc/self/stat", O_RDONLY);
  while (fd == -1 && errno == EINTR);

  if (fd == -1)
    return -errno;

  do
    n = read(fd, buf, sizeof(buf) - 1);
  while (n == -1 && errno == EINTR);

  SAVE_ERRNO(close(fd));
  if (n == -1)
    return -errno;
  buf[n] = '\0';

  s = strchr(buf, ' ');
  if (s == NULL)
    goto err;

  s += 1;
  if (*s != '(')
    goto err;

  s = strchr(s, ')');
  if (s == NULL)
    goto err;

  for (i = 1; i <= 22; i++) {
    s = strchr(s + 1, ' ');
    if (s == NULL)
      goto err;
  }

  errno = 0;
  val = strtol(s, NULL, 10);
  if (errno != 0)
    goto err;
  if (val < 0)
    goto err;

  *rss = val * getpagesize();
  return 0;

err:
  return -EINVAL;
}


int uv_uptime(double* uptime) {
  static volatile int no_clock_boottime;
  struct timespec now;
  int r;

  /* Try CLOCK_BOOTTIME first, fall back to CLOCK_MONOTONIC if not available
   * (pre-2.6.39 kernels). CLOCK_MONOTONIC doesn't increase when the system
   * is suspended.
   */
  if (no_clock_boottime) {
    retry: r = clock_gettime(CLOCK_MONOTONIC, &now);
  }
  else if ((r = clock_gettime(CLOCK_BOOTTIME, &now)) && errno == EINVAL) {
    no_clock_boottime = 1;
    goto retry;
  }

  if (r)
    return -errno;

  *uptime = now.tv_sec;
  *uptime += (double)now.tv_nsec / 1000000000.0;
  return 0;
}


int uv_cpu_info(uv_cpu_info_t** cpu_infos, int* count) {
  unsigned int numcpus;
  uv_cpu_info_t* ci;
  int err;

  *cpu_infos = NULL;
  *count = 0;

  numcpus = sysconf(_SC_NPROCESSORS_ONLN);
  assert(numcpus != (unsigned int) -1);
  assert(numcpus != 0);

  ci = calloc(numcpus, sizeof(*ci));
  if (ci == NULL)
    return -ENOMEM;

  err = read_models(numcpus, ci);
  if (err == 0)
    err = read_times(numcpus, ci);

  if (err) {
    uv_free_cpu_info(ci, numcpus);
    return err;
  }

  /* read_models() on x86 also reads the CPU speed from /proc/cpuinfo.
   * We don't check for errors here. Worst case, the field is left zero.
   */
  if (ci[0].speed == 0)
    read_speeds(numcpus, ci);

  *cpu_infos = ci;
  *count = numcpus;

  return 0;
}


static void read_speeds(unsigned int numcpus, uv_cpu_info_t* ci) {
  unsigned int num;

  for (num = 0; num < numcpus; num++)
    ci[num].speed = read_cpufreq(num) / 1000;
}


/* Also reads the CPU frequency on x86. The other architectures only have
 * a BogoMIPS field, which may not be very accurate.
 *
 * Note: Simply returns on error, uv_cpu_info() takes care of the cleanup.
 */
static int read_models(unsigned int numcpus, uv_cpu_info_t* ci) {
  static const char model_marker[] = "model name\t: ";
  static const char speed_marker[] = "cpu MHz\t\t: ";
  const char* inferred_model;
  unsigned int model_idx;
  unsigned int speed_idx;
  char buf[1024];
  char* model;
  FILE* fp;

  /* Most are unused on non-ARM, non-MIPS and non-x86 architectures. */
  (void) &model_marker;
  (void) &speed_marker;
  (void) &speed_idx;
  (void) &model;
  (void) &buf;
  (void) &fp;

  model_idx = 0;
  speed_idx = 0;

#if defined(__arm__) || \
    defined(__i386__) || \
    defined(__mips__) || \
    defined(__x86_64__)
  fp = fopen("/proc/cpuinfo", "r");
  if (fp == NULL)
    return -errno;

  while (fgets(buf, sizeof(buf), fp)) {
    if (model_idx < numcpus) {
      if (strncmp(buf, model_marker, sizeof(model_marker) - 1) == 0) {
        model = buf + sizeof(model_marker) - 1;
        model = strndup(model, strlen(model) - 1);  /* Strip newline. */
        if (model == NULL) {
          fclose(fp);
          return -ENOMEM;
        }
        ci[model_idx++].model = model;
        continue;
      }
    }
#if defined(__arm__) || defined(__mips__)
    if (model_idx < numcpus) {
#if defined(__arm__)
      /* Fallback for pre-3.8 kernels. */
      static const char model_marker[] = "Processor\t: ";
#else	/* defined(__mips__) */
      static const char model_marker[] = "cpu model\t\t: ";
#endif
      if (strncmp(buf, model_marker, sizeof(model_marker) - 1) == 0) {
        model = buf + sizeof(model_marker) - 1;
        model = strndup(model, strlen(model) - 1);  /* Strip newline. */
        if (model == NULL) {
          fclose(fp);
          return -ENOMEM;
        }
        ci[model_idx++].model = model;
        continue;
      }
    }
#else  /* !__arm__ && !__mips__ */
    if (speed_idx < numcpus) {
      if (strncmp(buf, speed_marker, sizeof(speed_marker) - 1) == 0) {
        ci[speed_idx++].speed = atoi(buf + sizeof(speed_marker) - 1);
        continue;
      }
    }
#endif  /* __arm__ || __mips__ */
  }

  fclose(fp);
#endif  /* __arm__ || __i386__ || __mips__ || __x86_64__ */

  /* Now we want to make sure that all the models contain *something* because
   * it's not safe to leave them as null. Copy the last entry unless there
   * isn't one, in that case we simply put "unknown" into everything.
   */
  inferred_model = "unknown";
  if (model_idx > 0)
    inferred_model = ci[model_idx - 1].model;

  while (model_idx < numcpus) {
    model = strndup(inferred_model, strlen(inferred_model));
    if (model == NULL)
      return -ENOMEM;
    ci[model_idx++].model = model;
  }

  return 0;
}


static int read_times(unsigned int numcpus, uv_cpu_info_t* ci) {
  unsigned long clock_ticks;
  struct uv_cpu_times_s ts;
  unsigned long user;
  unsigned long nice;
  unsigned long sys;
  unsigned long idle;
  unsigned long dummy;
  unsigned long irq;
  unsigned int num;
  unsigned int len;
  char buf[1024];
  FILE* fp;

  clock_ticks = sysconf(_SC_CLK_TCK);
  assert(clock_ticks != (unsigned long) -1);
  assert(clock_ticks != 0);

  fp = fopen("/proc/stat", "r");
  if (fp == NULL)
    return -errno;

  if (!fgets(buf, sizeof(buf), fp))
    abort();

  num = 0;

  while (fgets(buf, sizeof(buf), fp)) {
    if (num >= numcpus)
      break;

    if (strncmp(buf, "cpu", 3))
      break;

    /* skip "cpu<num> " marker */
    {
      unsigned int n = num;
      for (len = sizeof("cpu0"); n /= 10; len++);
      assert(sscanf(buf, "cpu%u ", &n) == 1 && n == num);
    }

    /* Line contains user, nice, system, idle, iowait, irq, softirq, steal,
     * guest, guest_nice but we're only interested in the first four + irq.
     *
     * Don't use %*s to skip fields or %ll to read straight into the uint64_t
     * fields, they're not allowed in C89 mode.
     */
    if (6 != sscanf(buf + len,
                    "%lu %lu %lu %lu %lu %lu",
                    &user,
                    &nice,
                    &sys,
                    &idle,
                    &dummy,
                    &irq))
      abort();

    ts.user = clock_ticks * user;
    ts.nice = clock_ticks * nice;
    ts.sys  = clock_ticks * sys;
    ts.idle = clock_ticks * idle;
    ts.irq  = clock_ticks * irq;
    ci[num++].cpu_times = ts;
  }
  fclose(fp);

  return 0;
}


static unsigned long read_cpufreq(unsigned int cpunum) {
  unsigned long val;
  char buf[1024];
  FILE* fp;

  snprintf(buf,
           sizeof(buf),
           "/sys/devices/system/cpu/cpu%u/cpufreq/scaling_cur_freq",
           cpunum);

  fp = fopen(buf, "r");
  if (fp == NULL)
    return 0;

  if (fscanf(fp, "%lu", &val) != 1)
    val = 0;

  fclose(fp);

  return val;
}


void uv_free_cpu_info(uv_cpu_info_t* cpu_infos, int count) {
  int i;

  for (i = 0; i < count; i++) {
    free(cpu_infos[i].model);
  }

  free(cpu_infos);
}


int uv_interface_addresses(uv_interface_address_t** addresses,
  int* count) {
#ifndef HAVE_IFADDRS_H
  return -ENOSYS;
#else
  struct ifaddrs *addrs, *ent;
  uv_interface_address_t* address;
  int i;
  struct sockaddr_ll *sll;

  if (getifaddrs(&addrs))
    return -errno;

  *count = 0;

  /* Count the number of interfaces */
  for (ent = addrs; ent != NULL; ent = ent->ifa_next) {
    if (!((ent->ifa_flags & IFF_UP) && (ent->ifa_flags & IFF_RUNNING)) ||
        (ent->ifa_addr == NULL) ||
        (ent->ifa_addr->sa_family == PF_PACKET)) {
      continue;
    }

    (*count)++;
  }

  *addresses = malloc(*count * sizeof(**addresses));
  if (!(*addresses))
    return -ENOMEM;

  address = *addresses;

  for (ent = addrs; ent != NULL; ent = ent->ifa_next) {
    if (!((ent->ifa_flags & IFF_UP) && (ent->ifa_flags & IFF_RUNNING)))
      continue;

    if (ent->ifa_addr == NULL)
      continue;

    /*
     * On Linux getifaddrs returns information related to the raw underlying
     * devices. We're not interested in this information yet.
     */
    if (ent->ifa_addr->sa_family == PF_PACKET)
      continue;

    address->name = strdup(ent->ifa_name);

    if (ent->ifa_addr->sa_family == AF_INET6) {
      address->address.address6 = *((struct sockaddr_in6*) ent->ifa_addr);
    } else {
      address->address.address4 = *((struct sockaddr_in*) ent->ifa_addr);
    }

    if (ent->ifa_netmask->sa_family == AF_INET6) {
      address->netmask.netmask6 = *((struct sockaddr_in6*) ent->ifa_netmask);
    } else {
      address->netmask.netmask4 = *((struct sockaddr_in*) ent->ifa_netmask);
    }

    address->is_internal = !!(ent->ifa_flags & IFF_LOOPBACK);

    address++;
  }

  /* Fill in physical addresses for each interface */
  for (ent = addrs; ent != NULL; ent = ent->ifa_next) {
    if (!((ent->ifa_flags & IFF_UP) && (ent->ifa_flags & IFF_RUNNING)) ||
        (ent->ifa_addr == NULL) ||
        (ent->ifa_addr->sa_family != PF_PACKET)) {
      continue;
    }

    address = *addresses;

    for (i = 0; i < (*count); i++) {
      if (strcmp(address->name, ent->ifa_name) == 0) {
        sll = (struct sockaddr_ll*)ent->ifa_addr;
        memcpy(address->phys_addr, sll->sll_addr, sizeof(address->phys_addr));
      }
      address++;
    }
  }

  freeifaddrs(addrs);

  return 0;
#endif
}


void uv_free_interface_addresses(uv_interface_address_t* addresses,
  int count) {
  int i;

  for (i = 0; i < count; i++) {
    free(addresses[i].name);
  }

  free(addresses);
}


void uv__set_process_title(const char* title) {
#if defined(PR_SET_NAME)
  prctl(PR_SET_NAME, title);  /* Only copies first 16 characters. */
#endif
}
