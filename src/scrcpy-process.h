#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef _WIN32
#include <windows.h>
typedef struct {
	HANDLE process;
	HANDLE job;
	DWORD pid;
} scrcpy_proc_t;
#else
#include <sys/types.h>
typedef struct {
	pid_t pid;
} scrcpy_proc_t;
#endif

#ifdef __cplusplus
extern "C" {
#endif

bool scrcpy_proc_spawn(scrcpy_proc_t *proc, const char *exe_path, const char *const *argv, const char *log_path);

bool scrcpy_proc_run_capture(const char *exe_path, const char *const *argv, char **output);

bool scrcpy_proc_alive(const scrcpy_proc_t *proc);

void scrcpy_proc_kill(scrcpy_proc_t *proc);

void scrcpy_proc_close(scrcpy_proc_t *proc);

#ifdef __cplusplus
}
#endif
