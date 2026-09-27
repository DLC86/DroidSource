#include "scrcpy-process.h"

#include <obs-module.h>
#include <plugin-support.h>
#include <util/dstr.h>
#include <util/platform.h>

#include <string.h>

#ifdef _WIN32

static void quote_arg(struct dstr *out, const char *arg)
{
	bool need_quote = (*arg == '\0') || strpbrk(arg, " \t\"") != NULL;
	if (!need_quote) {
		dstr_cat(out, arg);
		return;
	}
	dstr_cat(out, "\"");
	size_t backslashes = 0;
	for (const char *p = arg; *p; ++p) {
		if (*p == '\\') {
			backslashes++;
		} else if (*p == '"') {
			for (size_t i = 0; i < backslashes * 2 + 1; ++i)
				dstr_cat(out, "\\");
			dstr_cat_ch(out, '"');
			backslashes = 0;
		} else {
			for (size_t i = 0; i < backslashes; ++i)
				dstr_cat(out, "\\");
			dstr_cat_ch(out, *p);
			backslashes = 0;
		}
	}
	for (size_t i = 0; i < backslashes * 2; ++i)
		dstr_cat(out, "\\");
	dstr_cat(out, "\"");
}

static wchar_t *utf8_to_wide(const char *utf8)
{
	if (!utf8)
		return NULL;
	int need = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
	if (need <= 0)
		return NULL;
	wchar_t *w = bmalloc((size_t)need * sizeof(wchar_t));
	MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w, need);
	return w;
}

bool scrcpy_proc_spawn(scrcpy_proc_t *proc, const char *exe_path, const char *const *argv, const char *log_path)
{
	memset(proc, 0, sizeof(*proc));

	struct dstr cmdline = {0};
	quote_arg(&cmdline, exe_path);
	for (size_t i = 0; argv[i]; ++i) {
		dstr_cat_ch(&cmdline, ' ');
		quote_arg(&cmdline, argv[i]);
	}

	wchar_t *wexe = utf8_to_wide(exe_path);
	wchar_t *wcmd = utf8_to_wide(cmdline.array);

	HANDLE job = CreateJobObjectW(NULL, NULL);
	if (job) {
		JOBOBJECT_EXTENDED_LIMIT_INFORMATION info = {0};
		info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
		SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info, sizeof(info));
	}

	HANDLE log_h = INVALID_HANDLE_VALUE;
	if (log_path && *log_path) {
		SECURITY_ATTRIBUTES sa = {0};
		sa.nLength = sizeof(sa);
		sa.bInheritHandle = TRUE;
		wchar_t *wlog = utf8_to_wide(log_path);
		log_h = CreateFileW(wlog, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, CREATE_ALWAYS,
				    FILE_ATTRIBUTE_NORMAL, NULL);
		bfree(wlog);
		if (log_h == INVALID_HANDLE_VALUE) {
			obs_log(LOG_WARNING, "scrcpy-process: cannot open log %s (err=%lu)", log_path, GetLastError());
		}
	}

	STARTUPINFOW si = {0};
	si.cb = sizeof(si);
	si.dwFlags = STARTF_USESTDHANDLES;
	si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
	if (log_h != INVALID_HANDLE_VALUE) {
		si.hStdOutput = log_h;
		si.hStdError = log_h;
	} else {
		si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
		si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
	}

	PROCESS_INFORMATION pi = {0};
	DWORD flags = CREATE_NO_WINDOW | CREATE_SUSPENDED;

	BOOL ok = CreateProcessW(wexe, wcmd, NULL, NULL, TRUE, flags, NULL, NULL, &si, &pi);

	bfree(wexe);
	bfree(wcmd);
	dstr_free(&cmdline);
	if (log_h != INVALID_HANDLE_VALUE)
		CloseHandle(log_h);

	if (!ok) {
		obs_log(LOG_ERROR, "CreateProcessW failed: %lu", GetLastError());
		if (job)
			CloseHandle(job);
		return false;
	}

	if (job) {
		if (!AssignProcessToJobObject(job, pi.hProcess)) {
			obs_log(LOG_WARNING, "AssignProcessToJobObject failed: %lu", GetLastError());
		}
	}

	ResumeThread(pi.hThread);
	CloseHandle(pi.hThread);

	proc->process = pi.hProcess;
	proc->job = job;
	proc->pid = pi.dwProcessId;
	return true;
}

bool scrcpy_proc_run_capture(const char *exe_path, const char *const *argv, char **output)
{
	*output = NULL;

	struct dstr cmdline = {0};
	quote_arg(&cmdline, exe_path);
	for (size_t i = 0; argv[i]; ++i) {
		dstr_cat_ch(&cmdline, ' ');
		quote_arg(&cmdline, argv[i]);
	}

	SECURITY_ATTRIBUTES sa = {sizeof(sa), NULL, TRUE};
	HANDLE rd = NULL;
	HANDLE wr = NULL;
	if (!CreatePipe(&rd, &wr, &sa, 65536)) {
		dstr_free(&cmdline);
		return false;
	}
	SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

	wchar_t *wcmd = utf8_to_wide(cmdline.array);
	dstr_free(&cmdline);
	if (!wcmd) {
		CloseHandle(rd);
		CloseHandle(wr);
		return false;
	}

	STARTUPINFOW si = {0};
	si.cb = sizeof(si);
	si.dwFlags = STARTF_USESTDHANDLES;
	si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
	si.hStdOutput = wr;
	si.hStdError = wr;

	PROCESS_INFORMATION pi = {0};
	BOOL ok = CreateProcessW(NULL, wcmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
	bfree(wcmd);
	CloseHandle(wr);

	if (!ok) {
		CloseHandle(rd);
		return false;
	}

	struct dstr out = {0};
	char buf[4096];
	for (;;) {
		DWORD n = 0;
		if (!ReadFile(rd, buf, sizeof(buf), &n, NULL) || n == 0)
			break;
		dstr_ncat(&out, buf, n);
	}
	CloseHandle(rd);
	WaitForSingleObject(pi.hProcess, 5000);
	CloseHandle(pi.hProcess);
	CloseHandle(pi.hThread);

	*output = out.array;
	return true;
}

bool scrcpy_proc_alive(const scrcpy_proc_t *proc)
{
	if (!proc->process)
		return false;
	DWORD code = 0;
	if (!GetExitCodeProcess(proc->process, &code))
		return false;
	return code == STILL_ACTIVE;
}

void scrcpy_proc_kill(scrcpy_proc_t *proc)
{
	if (proc->job) {
		TerminateJobObject(proc->job, 1);
	} else if (proc->process) {
		TerminateProcess(proc->process, 1);
	}
	if (proc->process)
		WaitForSingleObject(proc->process, 2000);
}

void scrcpy_proc_close(scrcpy_proc_t *proc)
{
	if (proc->process) {
		CloseHandle(proc->process);
		proc->process = NULL;
	}
	if (proc->job) {
		CloseHandle(proc->job);
		proc->job = NULL;
	}
	proc->pid = 0;
}

#else /* POSIX */

#include <errno.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

bool scrcpy_proc_spawn(scrcpy_proc_t *proc, const char *exe_path, const char *const *argv, const char *log_path)
{
	memset(proc, 0, sizeof(*proc));
	(void)log_path;

	size_t argc = 0;
	while (argv[argc])
		argc++;

	char **spawn_argv = bmalloc(sizeof(char *) * (argc + 2));
	spawn_argv[0] = (char *)exe_path;
	for (size_t i = 0; i < argc; ++i)
		spawn_argv[i + 1] = (char *)argv[i];
	spawn_argv[argc + 1] = NULL;

	pid_t pid = 0;
	int rc = posix_spawn(&pid, exe_path, NULL, NULL, spawn_argv, environ);
	bfree(spawn_argv);
	if (rc != 0) {
		obs_log(LOG_ERROR, "posix_spawn failed: %d", rc);
		return false;
	}
	proc->pid = pid;
	return true;
}

bool scrcpy_proc_run_capture(const char *exe_path, const char *const *argv, char **output)
{
	*output = NULL;

	size_t argc = 0;
	while (argv[argc])
		argc++;

	char **spawn_argv = bmalloc(sizeof(char *) * (argc + 2));
	spawn_argv[0] = (char *)exe_path;
	for (size_t i = 0; i < argc; ++i)
		spawn_argv[i + 1] = (char *)argv[i];
	spawn_argv[argc + 1] = NULL;

	int pipes[2];
	if (pipe(pipes) != 0) {
		bfree(spawn_argv);
		return false;
	}

	posix_spawn_file_actions_t actions;
	posix_spawn_file_actions_init(&actions);
	posix_spawn_file_actions_adddup2(&actions, pipes[1], STDOUT_FILENO);
	posix_spawn_file_actions_adddup2(&actions, pipes[1], STDERR_FILENO);
	posix_spawn_file_actions_addclose(&actions, pipes[0]);
	posix_spawn_file_actions_addclose(&actions, pipes[1]);

	pid_t pid = 0;
	int rc = posix_spawn(&pid, exe_path, &actions, NULL, spawn_argv, environ);
	posix_spawn_file_actions_destroy(&actions);
	close(pipes[1]);
	bfree(spawn_argv);

	if (rc != 0) {
		close(pipes[0]);
		return false;
	}

	struct dstr out = {0};
	char buf[4096];
	for (;;) {
		ssize_t n = read(pipes[0], buf, sizeof(buf));
		if (n <= 0)
			break;
		dstr_ncat(&out, buf, (size_t)n);
	}
	close(pipes[0]);

	int status = 0;
	waitpid(pid, &status, 0);
	*output = out.array;
	return true;
}

bool scrcpy_proc_alive(const scrcpy_proc_t *proc)
{
	if (proc->pid <= 0)
		return false;
	return kill(proc->pid, 0) == 0;
}

void scrcpy_proc_kill(scrcpy_proc_t *proc)
{
	if (proc->pid > 0) {
		kill(proc->pid, SIGTERM);
		for (int i = 0; i < 20; ++i) {
			int status = 0;
			pid_t r = waitpid(proc->pid, &status, WNOHANG);
			if (r == proc->pid || r < 0)
				return;
			usleep(100000);
		}
		kill(proc->pid, SIGKILL);
		waitpid(proc->pid, NULL, 0);
	}
}

void scrcpy_proc_close(scrcpy_proc_t *proc)
{
	proc->pid = 0;
}

#endif
