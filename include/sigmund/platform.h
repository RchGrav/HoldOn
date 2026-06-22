#pragma once
#ifndef SIGMUND_PLATFORM_H
#define SIGMUND_PLATFORM_H

#include "sigmund/config.h"
#include "sigmund/types.h"

enum group_liveness { GROUP_SCAN_ERROR = -1, GROUP_EMPTY = 0, GROUP_ZOMBIE_ONLY = 1, GROUP_LIVE = 2 };

bool sigmund_current_boot_id(char *buf, size_t n);
enum group_liveness sigmund_group_session_liveness(pid_t pgid, pid_t sid);
int sigmund_count_session_escapees(pid_t sid, pid_t expected_pgid);
int sigmund_read_proc_stat_tokens(pid_t pid, char *state_out, uint64_t *starttime_out);
int sigmund_read_proc_exe(pid_t pid, uint64_t *dev, uint64_t *ino);
bool sigmund_leader_present(pid_t pid);
int sigmund_group_exists(pid_t pgid);
int sigmund_resolve_binary_path(const char *argv0, char *out, size_t n);
bool sigmund_path_is_within_dir(const char *path, const char *dir);
int sigmund_resolve_self_executable_path(const char *argv0, char *out, size_t n);

#endif /* SIGMUND_PLATFORM_H */
