/* Copyright libuv project contributors. All rights reserved.
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
#include "task.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int accept_cb_calls;
static int cancel_cb_calls;
static int connect_cb_calls;
static int timer_cb_calls;

static void accept_cb(uv_accept_t* req, int status) {
  ASSERT_OK(status);
  uv_close((uv_handle_t*) req->client, NULL);
  accept_cb_calls++;
}

static void cancel_cb(uv_accept_t* req, int status) {
  ASSERT(uv_is_closing((uv_handle_t*) req->server));
  ASSERT(uv_is_closing((uv_handle_t*) req->client));
  ASSERT_EQ(status, UV_ECANCELED);
  cancel_cb_calls++;
}

static void connect_cb(uv_connect_t* req, int status) {
  ASSERT_OK(status);
  uv_close((uv_handle_t*) req->handle, NULL);
  connect_cb_calls++;
}

static void timer_cb(uv_timer_t* handle) {
  uv_accept_t* req;
  int err;

  req = handle->data;
  err = uv_cancel((uv_req_t*) req);

  /* TODO(bnoordhuis) remove branch when uv_cancel() works on Windows */
  if (err == UV_EBUSY) {
    uv_close((uv_handle_t*) req->server, NULL);
    uv_close((uv_handle_t*) req->client, NULL);
  } else {
    ASSERT_OK(err);
  }

  uv_close((uv_handle_t*) handle, NULL);
  timer_cb_calls++;
}

TEST_IMPL(pipe_stream_accept) {
  uv_connect_t connect_req;
  uv_accept_t accept_req;
  uv_loop_t* loop;
  uv_timer_t timer;
  uv_pipe_t server;
  uv_pipe_t client;
  uv_pipe_t peer;

  loop = uv_default_loop();

  ASSERT_OK(uv_pipe_init(loop, &server, 0));
  ASSERT_OK(uv_pipe_bind(&server, TEST_PIPENAME));
  ASSERT_OK(uv_listen((uv_stream_t*) &server, 128, NULL));

  /* Event loop should not block, no inflight accept requests. */
  ASSERT_OK(uv_run(loop, UV_RUN_DEFAULT));

  ASSERT_OK(uv_pipe_init(loop, &peer, 0));
  ASSERT_EQ(UV_EINVAL, uv_accept((uv_stream_t*) &server, (uv_stream_t*) &peer));
  ASSERT_OK(uv_stream_accept(&accept_req,
                             (uv_stream_t*) &server,
                             (uv_stream_t*) &peer,
                             0,
                             accept_cb));

  ASSERT_OK(uv_pipe_init(loop, &client, 0));
  uv_pipe_connect(&connect_req, &client, TEST_PIPENAME, connect_cb);

  ASSERT_EQ(0, accept_cb_calls);
  ASSERT_EQ(0, connect_cb_calls);
  ASSERT_OK(uv_run(loop, UV_RUN_DEFAULT));
  ASSERT(uv_is_closing((uv_handle_t*) &client));
  ASSERT(uv_is_closing((uv_handle_t*) &peer));
  ASSERT_EQ(1, connect_cb_calls);
  ASSERT_EQ(1, accept_cb_calls);

  /* TODO(bnoordhuis) remove cancel_cb when uv_cancel() works on Windows */
  ASSERT_OK(uv_pipe_init(loop, &peer, 0));
  ASSERT_OK(uv_stream_accept(&accept_req,
                             (uv_stream_t*) &server,
                             (uv_stream_t*) &peer,
                             0,
                             cancel_cb));

  timer.data = &accept_req;
  ASSERT_OK(uv_timer_init(loop, &timer));
  ASSERT_OK(uv_timer_start(&timer, timer_cb, 42, 0));

  ASSERT_EQ(0, cancel_cb_calls);
  ASSERT_EQ(0, timer_cb_calls);
  ASSERT_OK(uv_run(loop, UV_RUN_DEFAULT));
  ASSERT_EQ(1, timer_cb_calls);

  /* TODO(bnoordhuis) remove branch when uv_cancel() works on Windows */
  if (cancel_cb_calls == 0) {
    ASSERT(!uv_is_closing((uv_handle_t*) &peer));
    ASSERT(!uv_is_closing((uv_handle_t*) &server));
    uv_close((uv_handle_t*) &peer, NULL);
    uv_close((uv_handle_t*) &server, NULL);
  }

  ASSERT_OK(uv_run(loop, UV_RUN_DEFAULT));

  MAKE_VALGRIND_HAPPY(loop);
  return 0;
}
