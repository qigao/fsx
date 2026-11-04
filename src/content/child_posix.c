#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#else
#define _GNU_SOURCE
#endif
#include "child_posix.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

FsStatus fsx_child_spawn(FsContentQuery *q, const char *tool, char *const *argv,
                         bool discovery, FsChild *child) {
  *child = (FsChild){0, -1, -1, -1, discovery};
  int pipes[3][2] = {{-1, -1}, {-1, -1}, {-1, -1}};
  FsStatus status = FS_IO;
  for (unsigned i = 0; i < 3; i++) {
#ifdef __APPLE__
    if (pipe(pipes[i]) || fcntl(pipes[i][0], F_SETFD, FD_CLOEXEC) < 0 ||
        fcntl(pipes[i][1], F_SETFD, FD_CLOEXEC) < 0)
      goto cleanup;
#else
    if (pipe2(pipes[i], O_CLOEXEC))
      goto cleanup;
#endif
  }
  for (unsigned i = 0; i < 3; i++)
    for (unsigned j = 0; j < 2; j++)
      if (pipes[i][j] < 3) {
        int fd = fcntl(pipes[i][j], F_DUPFD_CLOEXEC, 3);
        if (fd < 0)
          goto cleanup;
        close(pipes[i][j]);
        pipes[i][j] = fd;
      }
  posix_spawn_file_actions_t actions;
  posix_spawnattr_t attr;
  if (posix_spawn_file_actions_init(&actions))
    goto cleanup;
  if (posix_spawnattr_init(&attr)) {
    posix_spawn_file_actions_destroy(&actions);
    goto cleanup;
  }
  int rc = 0;
  for (unsigned i = 0; i < 3; i++)
    rc |=
        posix_spawn_file_actions_adddup2(&actions, pipes[i][i ? 1 : 0], (int)i);
#ifndef __APPLE__
  rc |= posix_spawn_file_actions_addclosefrom_np(&actions, 3);
#endif
  sigset_t empty, defaults;
  sigemptyset(&empty);
  sigemptyset(&defaults);
  sigaddset(&defaults, SIGPIPE);
  rc |= posix_spawnattr_setsigmask(&attr, &empty);
  rc |= posix_spawnattr_setsigdefault(&attr, &defaults);
  rc |= posix_spawnattr_setpgroup(&attr, 0);
  short flags =
      POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF;
#ifdef __APPLE__
  flags |= POSIX_SPAWN_CLOEXEC_DEFAULT;
#endif
  rc |= posix_spawnattr_setflags(&attr, flags);
  char *env[] = {"LANG=C", "LC_ALL=C", NULL};
  if (!rc)
    rc = posix_spawn(&child->pid, tool, &actions, &attr, argv, env);
  posix_spawnattr_destroy(&attr);
  posix_spawn_file_actions_destroy(&actions);
  if (rc) {
    status = rc == ENOMEM ? FS_NOMEM : FS_IO;
    goto cleanup;
  }
  child->input = pipes[0][1];
  child->output = pipes[1][0];
  child->error = pipes[2][0];
  pipes[0][1] = pipes[1][0] = pipes[2][0] = -1;
  cmeta_mutex_lock(&q->mutex);
  q->info.processes_started++;
  q->info.running_processes++;
  q->info.discovery_processes_started += discovery;
  cmeta_mutex_unlock(&q->mutex);
  status = FS_OK;
cleanup:
  for (unsigned i = 0; i < 3; i++)
    for (unsigned j = 0; j < 2; j++)
      if (pipes[i][j] >= 0)
        close(pipes[i][j]);
  return status;
}
FsStatus fsx_child_finish(FsContentQuery *q, FsChild *child, FsStatus status,
                          int *exit_code) {
  if (child->input >= 0)
    close(child->input);
  if (child->output >= 0)
    close(child->output);
  if (child->error >= 0)
    close(child->error);
  if (status != FS_OK)
    kill(-child->pid, SIGKILL);
  int result = 0;
  for (;;) {
    pid_t waited = waitpid(child->pid, &result, WNOHANG);
    if (waited == child->pid)
      break;
    if (waited < 0) {
      if (errno == EINTR)
        continue;
      status = FS_IO;
      break;
    }
    FsStatus stopped = fsx_content_stop(q);
    if (stopped != FS_OK) {
      if (status == FS_OK)
        status = stopped;
      kill(-child->pid, SIGKILL);
    }
    cmeta_sleep_ms(1);
  }
  *exit_code = WIFEXITED(result) ? WEXITSTATUS(result) : -1;
  cmeta_mutex_lock(&q->mutex);
  q->info.running_processes--;
  if (child->discovery)
    q->info.discovery_exit_code = *exit_code;
  else
    q->info.tool_exit_code = *exit_code;
  cmeta_mutex_unlock(&q->mutex);
  return status;
}
