/* Copyright Joyent, Inc. and other Node contributors. All rights reserved.
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

#include "runner-unix.h"
#include "runner.h"

#include <stdint.h> /* uintptr_t */

#include <errno.h>
#include <unistd.h> /* usleep */
#include <string.h> /* strdup */
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <assert.h>

#include <sys/un.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <pthread.h>

static int lowest_fd = -1;


/* Do platform-specific initialization. */
void platform_init(int argc, char **argv) {
  const char* tap;

  tap = getenv("UV_TAP_OUTPUT");
  tap_output = (tap != NULL && atoi(tap) > 0);

  /* Disable stdio output buffering. */
  setvbuf(stdout, NULL, _IONBF, 0);
  setvbuf(stderr, NULL, _IONBF, 0);
  strncpy(executable_path, argv[0], sizeof(executable_path) - 1);
  signal(SIGPIPE, SIG_IGN);
}


/* Invoke "argv[0] test-name [test-part]". Store process info in *p. */
/* Make sure that all stdio output of the processes is buffered up. */
int process_start(char* name, char* part, process_info_t* p, int is_helper) {
  FILE* stdout_file;
  const char* arg;
  char* args[16];
  int n;

  stdout_file = tmpfile();
  if (!stdout_file) {
    perror("tmpfile");
    return -1;
  }

  p->terminated = 0;
  p->status = 0;

  pid_t pid = fork();

  if (pid < 0) {
    perror("fork");
    return -1;
  }

  if (pid == 0) {
    /* child */
    arg = getenv("UV_USE_VALGRIND");
    n = 0;

    /* Disable valgrind for helpers, it complains about helpers leaking memory.
     * They're killed after the test and as such never get a chance to clean up.
     */
    if (is_helper == 0 && arg != NULL && atoi(arg) != 0) {
      args[n++] = "valgrind";
      args[n++] = "--quiet";
      args[n++] = "--leak-check=full";
      args[n++] = "--show-reachable=yes";
      args[n++] = "--error-exitcode=125";
    }

    args[n++] = executable_path;
    args[n++] = name;
    args[n++] = part;
    args[n++] = NULL;

    dup2(fileno(stdout_file), STDOUT_FILENO);
    dup2(fileno(stdout_file), STDERR_FILENO);
    execvp(args[0], args);
    perror("execvp()");
    _exit(127);
  }

  /* parent */
  p->pid = pid;
  p->name = strdup(name);
  p->stdout_file = stdout_file;

  return 0;
}


typedef struct {
  int pipe[2];
  process_info_t* vec;
  int n;
} dowait_args;


/* This function is run inside a pthread. We do this so that we can possibly
 * timeout.
 */
static void* dowait(void* data) {
  dowait_args* args = data;

  int i, r;
  process_info_t* p;

  for (i = 0; i < args->n; i++) {
    p = (process_info_t*)(args->vec + i * sizeof(process_info_t));
    if (p->terminated) continue;
    r = waitpid(p->pid, &p->status, 0);
    if (r < 0) {
      perror("waitpid");
      return NULL;
    }
    p->terminated = 1;
  }

  if (args->pipe[1] >= 0) {
    /* Write a character to the main thread to notify it about this. */
    ssize_t r;

    do
      r = write(args->pipe[1], "", 1);
    while (r == -1 && errno == EINTR);
  }

  return NULL;
}


/* Wait for all `n` processes in `vec` to terminate. */
/* Time out after `timeout` msec, or never if timeout == -1 */
/* Return 0 if all processes are terminated, -1 on error, -2 on timeout. */
int process_wait(process_info_t* vec, int n, int timeout) {
  int i;
  process_info_t* p;
  dowait_args args;
  args.vec = vec;
  args.n = n;
  args.pipe[0] = -1;
  args.pipe[1] = -1;

  /* The simple case is where there is no timeout */
  if (timeout == -1) {
    dowait(&args);
    return 0;
  }

  /* Hard case. Do the wait with a timeout.
   *
   * Assumption: we are the only ones making this call right now. Otherwise
   * we'd need to lock vec.
   */

  pthread_t tid;
  int retval;

  int r = pipe((int*)&(args.pipe));
  if (r) {
    perror("pipe()");
    return -1;
  }

  r = pthread_create(&tid, NULL, dowait, &args);
  if (r) {
    perror("pthread_create()");
    retval = -1;
    goto terminate;
  }

  struct timeval tv;
  tv.tv_sec = timeout / 1000;
  tv.tv_usec = 0;

  fd_set fds;
  FD_ZERO(&fds);
  FD_SET(args.pipe[0], &fds);

  r = select(args.pipe[0] + 1, &fds, NULL, NULL, &tv);

  if (r == -1) {
    perror("select()");
    retval = -1;

  } else if (r) {
    /* The thread completed successfully. */
    retval = 0;

  } else {
    /* Timeout. Kill all the children. */
    for (i = 0; i < n; i++) {
      p = (process_info_t*)(vec + i * sizeof(process_info_t));
      kill(p->pid, SIGTERM);
    }
    retval = -2;

    /* Wait for thread to finish. */
    r = pthread_join(tid, NULL);
    if (r) {
      perror("pthread_join");
      retval = -1;
    }
  }

terminate:
  close(args.pipe[0]);
  close(args.pipe[1]);
  return retval;
}


/* Returns the number of bytes in the stdio output buffer for process `p`. */
long int process_output_size(process_info_t *p) {
  /* Size of the p->stdout_file */
  struct stat buf;

  int r = fstat(fileno(p->stdout_file), &buf);
  if (r < 0) {
    return -1;
  }

  return (long)buf.st_size;
}


/* Copy the contents of the stdio output buffer to `fd`. */
int process_copy_output(process_info_t *p, int fd) {
  int r = fseek(p->stdout_file, 0, SEEK_SET);
  if (r < 0) {
    perror("fseek");
    return -1;
  }

  ssize_t nwritten;
  char buf[1024];

  /* TODO: what if the line is longer than buf */
  while (fgets(buf, sizeof(buf), p->stdout_file) != NULL) {
   /* TODO: what if write doesn't write the whole buffer... */
    nwritten = 0;

    if (tap_output)
      nwritten += write(fd, "#", 1);

    nwritten += write(fd, buf, strlen(buf));

    if (nwritten < 0) {
      perror("write");
      return -1;
    }
  }

  if (ferror(p->stdout_file)) {
    perror("read");
    return -1;
  }

  return 0;
}


/* Copy the last line of the stdio output buffer to `buffer` */
int process_read_last_line(process_info_t *p,
                           char* buffer,
                           size_t buffer_len) {
  char* ptr;

  int r = fseek(p->stdout_file, 0, SEEK_SET);
  if (r < 0) {
    perror("fseek");
    return -1;
  }

  buffer[0] = '\0';

  while (fgets(buffer, buffer_len, p->stdout_file) != NULL) {
    for (ptr = buffer; *ptr && *ptr != '\r' && *ptr != '\n'; ptr++);
    *ptr = '\0';
  }

  if (ferror(p->stdout_file)) {
    perror("read");
    buffer[0] = '\0';
    return -1;
  }
  return 0;
}


/* Return the name that was specified when `p` was started by process_start */
char* process_get_name(process_info_t *p) {
  return p->name;
}


/* Terminate process `p`. */
int process_terminate(process_info_t *p) {
  return kill(p->pid, SIGTERM);
}


/* Return the exit code of process p. */
/* On error, return -1. */
int process_reap(process_info_t *p) {
  if (WIFEXITED(p->status)) {
    return WEXITSTATUS(p->status);
  } else  {
    return p->status; /* ? */
  }
}


/* Clean up after terminating process `p` (e.g. free the output buffer etc.). */
void process_cleanup(process_info_t *p) {
  fclose(p->stdout_file);
  free(p->name);
}


/* Move the console cursor one line up and back to the first column. */
void rewind_cursor(void) {
  fprintf(stderr, "\033[2K\r");
}


/* Pause the calling thread for a number of milliseconds. */
void uv_sleep(int msec) {
  usleep(msec * 1000);
}


static const char* fd_type(int fd, char* buf, size_t buflen) {
  struct sockaddr sa;
  const char* family;
  const char* kind;
  socklen_t len;
  struct stat s;
  int type;

  if (fstat(fd, &s))
    return strncpy(buf, strerror(errno), buflen);

  if (isatty(fd))
    return strncpy(buf, "tty", buflen);

  if (S_ISREG(s.st_mode))
    return strncpy(buf, "file", buflen);

  if (S_ISCHR(s.st_mode))
    return strncpy(buf, "character device", buflen);

  if (S_ISFIFO(s.st_mode))
    return strncpy(buf, "fifo", buflen);

  if (!S_ISSOCK(s.st_mode))
    return strncpy(buf, "unknown fd type", buflen);

  len = sizeof(type);
  if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &len))
    return strncpy(buf, strerror(errno), buflen);

  len = sizeof(sa);
  if (getsockname(fd, &sa, &len))
    return strncpy(buf, strerror(errno), buflen);

  kind = "unknown";
  if (type == SOCK_RAW)
    kind = "raw";
  else if (type == SOCK_DGRAM)
    kind = "dgram";
  else if (type == SOCK_STREAM)
    kind = "stream";

  family = "unknown";
  if (sa.sa_family == AF_UNSPEC)
    family = "unspec";
  else if (sa.sa_family == AF_INET)
    family = "inet";
  else if (sa.sa_family == AF_INET6)
    family = "inet6";
  else if (sa.sa_family == AF_UNIX)
    family = "unix";

  snprintf(buf, buflen, "%s %s socket", family, kind);
  return buf;
}


static int fd_is_open(int fd) {
  int rc;

  do
    rc = dup2(fd, fd);
  while (rc == -1 && errno == EINTR);

  if (rc == -1) {
    if (errno == EBADF)
      return 0;
    perror("fd_is_open");
  }

  return 1;
}


static int check_fd_range(int start, int end) {
  char type[256];
  int nfds;
  int fd;

  for (fd = start, nfds = 0; fd <= end; fd += 1) {
    if (fd_is_open(fd)) {
      fd_type(fd, type, sizeof(type));
      fprintf(stderr, "Open file descriptor %d of type %s.\n", fd, type);
      fflush(stderr);
      nfds += 1;
    }
  }

  return nfds;
}


void before_main_hook(task_entry_t* task) {
  /* We need to figure out what the lowest free file descriptor is because
   * it's > 3 when running under gdb.
   */
  lowest_fd = socket(AF_UNIX, SOCK_DGRAM, 0);
  if (lowest_fd == -1)
    perror("before_main_hook:socket");
  else
    close(lowest_fd);
}


/* Check for leaked file descriptors. */
int after_main_hook(task_entry_t* task, int status) {
  int fd;

  /* Yes, this potentially writes to a file descriptor that's closed. */
  for (fd = STDIN_FILENO; fd <= STDERR_FILENO; fd += 1) {
    if (!fd_is_open(fd)) {
      fprintf(stderr, "Stdio file descriptor %d was closed.\n", fd);
      fflush(stderr);
      status = -1;
    }
  }

  if (lowest_fd == -1)
    lowest_fd = STDERR_FILENO + 1;

  fd = socket(AF_UNIX, SOCK_DGRAM, 0);
  if (fd == -1) {
    perror("after_main_hook:socket");
    return -1;
  }
  close(fd);

  if (fd != lowest_fd) {
    fprintf(stderr,
            "File descriptor leak detected: lowest fd is %d, expected %d.\n",
            fd,
            lowest_fd);
    fflush(stderr);
    status = -1;
  }

  if (lowest_fd < fd)
    fd = lowest_fd;

  if (check_fd_range(fd, fd + 256))
    return -1;

  return status;
}
