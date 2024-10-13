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

#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef SA_RESTART
# define SA_RESTART 0
#endif

static int uv__signal_start(uv_signal_t* handle,
                            uv_signal_cb signal_cb,
                            int signum,
                            int oneshot);
static void uv__signal_event(uv_loop_t* loop, uv__io_t* w, unsigned int events);
static int uv__signal_stop(uv_signal_t* handle);

static struct uv__queue event_loops[NSIG];
static atomic_uintptr_t owners[NSIG];

/* Value is unused but address is used as thread id for |owners| array. */
static _Thread_local char thread_id;

static int try_enter_critical_section(int signum) {
  uintptr_t owner;
  uintptr_t tid;

  assert(signum > 0);
  assert(signum <= NSIG);

  tid = (uintptr_t) &thread_id;
  for (;;) {
    owner = 0;
    if (atomic_compare_exchange_strong(&owners[signum-1], &owner, tid))
      return 1;
    if (owner == tid)
      return 0;  /* Called on thread that's inside critical section. */
  }
}


static void enter_critical_section(int signum) {
  if (!try_enter_critical_section(signum))
    abort();
}


static void exit_critical_section(int signum) {
  atomic_store(&owners[signum-1], 0);
}


static void handle(int signum) {
  uv__loop_internal_fields_t* lfields;
  struct sigaction act;
  struct uv__queue* h;
  struct uv__queue* q;
  uv_loop_t* loop;
  int del;
  int idx;
  int bit;
  int* fd;

  if (signum < 1 || signum > NSIG)
    return;
  if (!try_enter_critical_section(signum))
    return;  /* Called on thread that's inside critical section. */
  del = 1;  /* Delete handler if no handles, or all handles are oneshots. */
  idx = signum >> 6;
  bit = 1 << (signum & 63);
  h = &event_loops[signum-1];
  q = h->next;
  while (q != h) {
    lfields =
        container_of(q, uv__loop_internal_fields_t, signal_queues[signum-1]);
    loop = lfields->loop;
    fd = loop->signal_pipefd;
    atomic_fetch_or(&lfields->signal_pending[idx], bit);
    /* Once |del| turns false, it stays false. */
    del &= !!(bit & atomic_load(&lfields->signal_oneshot[idx]));
    while (write(fd[1], "", 1) < 0) {
      if (errno == EINTR)
        continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK)
        break;
      abort();
    }
    q = q->next;
  }
  act = (struct sigaction){.sa_handler = SIG_DFL};
  if (del)
    if (sigaction(signum, &act, 0))
      abort();
  exit_critical_section(signum);
}


static int register_event_loop(uv_loop_t* loop, int signum) {
  struct sigaction act;
  struct uv__queue* h;
  struct uv__queue* q;
  int err;

  if (signum < 1 || signum > NSIG)
    return UV_EINVAL;
  q = &uv__get_internal_fields(loop)->signal_queues[signum-1];
  if (q->next)
    return 0;
  err = 0;
  act = (struct sigaction){.sa_handler = handle, .sa_flags = SA_RESTART};
  enter_critical_section(signum);
  h = &event_loops[signum-1];
  if (h->next) {
    uv__queue_insert_tail(h, q);
  } else if (sigaction(signum, &act, NULL)) {
    err = UV__ERR(errno);
  } else {
    h->prev = h->next = q;
    q->prev = q->next = h;
  }
  exit_critical_section(signum);
  return err;
}


static int unregister_event_loop(uv_loop_t* loop, int signum) {
  struct sigaction act;
  struct uv__queue* q;
  int err;

  if (signum < 1 || signum > NSIG)
    return UV_EINVAL;
  q = &uv__get_internal_fields(loop)->signal_queues[signum-1];
  if (q->next == 0 || q->next == q)
    return 0;
  err = 0;
  act = (struct sigaction){.sa_handler = SIG_DFL};
  enter_critical_section(signum);
  uv__queue_remove(q);
  uv__queue_init(q);
  if (uv__queue_empty(&event_loops[signum-1]))
    if (sigaction(signum, &act, 0))
      err = UV__ERR(errno);
  exit_critical_section(signum);
  return err;
}


static void uv__signal_event(uv_loop_t* loop,
                             uv__io_t* w,
                             unsigned int events) {
  uv__loop_internal_fields_t* lfields;
  uint64_t pending[ARRAY_SIZE(lfields->signal_pending)];
  ssize_t r;
  char c;

  lfields = uv__get_internal_fields(loop);
  memcpy(pending, lfields->signal_pending, sizeof(pending));

  for (;;) {
    r = read(loop->signal_pipefd[0], &c, 1);

    if (r == -1 && r == EINTR)
      continue;

    if (r == -1 && (errno == EAGAIN || errno == EWOULDBLOCK))
      return;
  }
}

static int uv__signal_loop_once_init(uv_loop_t* loop) {
  int err;

  /* Return if already initialized. */
  if (loop->signal_pipefd[0] != -1)
    return 0;

  err = uv__make_pipe(loop->signal_pipefd, UV_NONBLOCK_PIPE);
  if (err)
    return err;

  uv__io_init(&loop->signal_io_watcher,
              uv__signal_event,
              loop->signal_pipefd[0]);
  uv__io_start(loop, &loop->signal_io_watcher, POLLIN);

  return 0;
}


int uv__signal_loop_fork(uv_loop_t* loop) {
  uv__loop_internal_fields_t* lfields;
  struct uv__queue* q;

  if (loop->signal_pipefd[0] == -1)
    return 0;
  uv__io_stop(loop, &loop->signal_io_watcher, POLLIN);
  uv__close(loop->signal_pipefd[0]);
  uv__close(loop->signal_pipefd[1]);
  loop->signal_pipefd[0] = -1;
  loop->signal_pipefd[1] = -1;

  lfields = uv__get_internal_fields(loop);
  memset(&lfields->signal_pending, 0, sizeof(lfields->signal_pending));

  uv__queue_foreach(q, &loop->handle_queue) {
    uv_handle_t* handle = uv__queue_data(q, uv_handle_t, handle_queue);
    uv_signal_t* sh;

    if (handle->type != UV_SIGNAL)
      continue;

    sh = (uv_signal_t*) handle;
    sh->caught_signals = 0;
    sh->dispatched_signals = 0;
  }

  return uv__signal_loop_once_init(loop);
}


void uv__signal_loop_cleanup(uv_loop_t* loop) {
  struct sigaction act;
  struct uv__queue *h;
  struct uv__queue *q;
  int signum;

  act = (struct sigaction){.sa_handler = SIG_DFL};
  for (signum = 1; signum <= NSIG; signum++) {
    q = &uv__get_internal_fields(loop)->signal_queues[signum-1];
    if (q->next == 0 || q->next == q)
      continue;
    enter_critical_section(signum);
    uv__queue_remove(q);
    h = &event_loops[signum-1];
    if (uv__queue_empty(h))
      if (sigaction(signum, &act, 0))
        perror("libuv: sigaction");
    exit_critical_section(signum);
  }
}


int uv_signal_init(uv_loop_t* loop, uv_signal_t* handle) {
  int err;

  err = uv__signal_loop_once_init(loop);
  if (err)
    return err;

  uv__handle_init(loop, (uv_handle_t*) handle, UV_SIGNAL);
  handle->signum = 0;
  handle->caught_signals = 0;
  handle->dispatched_signals = 0;
  uv__queue_init(&handle->queue);

  return 0;
}


void uv__signal_close(uv_signal_t* handle) {
  uv__signal_stop(handle);
}


int uv_signal_start(uv_signal_t* handle, uv_signal_cb signal_cb, int signum) {
  return uv__signal_start(handle, signal_cb, signum, 0);
}


int uv_signal_start_oneshot(uv_signal_t* handle,
                            uv_signal_cb signal_cb,
                            int signum) {
  return uv__signal_start(handle, signal_cb, signum, 1);
}


static int uv__signal_start(uv_signal_t* handle,
                            uv_signal_cb signal_cb,
                            int signum,
                            int oneshot) {
  uv__loop_internal_fields_t* lfields;
  uv_loop_t* loop;
  int err;

  loop = handle->loop;
  lfields = uv__get_internal_fields(loop);
  if (uv__is_closing(handle))
    return UV_EBUSY;
  if (signum == 0)
    return UV_EINVAL;
  if (signum == handle->signum) {
    handle->signal_cb = signal_cb;
    return 0;
  }
  if (handle->signum != 0) {
    err = uv__signal_stop(handle);
    if (err)
      return err;
  }
  err = register_event_loop(loop, signum);
  if (err)
    return err;
  handle->signal_cb = signal_cb;
  if (oneshot)
    handle->flags |= UV_SIGNAL_ONE_SHOT;
  uv__handle_start(handle);
  uv__queue_insert_tail(&lfields->signal_handles, &handle->queue);
  return UV_ENOSYS;
}


int uv_signal_stop(uv_signal_t* handle) {
  assert(!uv__is_closing(handle));
  return uv__signal_stop(handle);
}


static int uv__signal_stop(uv_signal_t* handle) {
  int signum;

  signum = handle->signum;
  if (signum == 0)
    return 0;
  handle->signum = 0;
  uv__handle_stop(handle);
  uv__queue_remove(&handle->queue);
  return unregister_event_loop(handle->loop, signum);
}
