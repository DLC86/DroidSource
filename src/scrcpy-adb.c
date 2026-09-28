#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include "scrcpy-adb.h"
#include "scrcpy-process.h"

#include <obs-module.h>
#include <util/dstr.h>
#include <util/platform.h>

#include <stdlib.h>
#include <string.h>

char *get_bin_dir(void)
{
	const char *env = getenv("SCRCPY_OBS_BIN_DIR");
	if (env && *env)
		return bstrdup(env);

	char *mod_data = obs_module_file("bin");
	if (mod_data)
		return mod_data;
	return NULL;
}

char *path_join(const char *dir, const char *leaf)
{
	struct dstr p = {0};
	dstr_copy(&p, dir);
	size_t len = strlen(dir);
	if (len && dir[len - 1] != '/' && dir[len - 1] != '\\')
		dstr_cat(&p, "/");
	dstr_cat(&p, leaf);
	return p.array;
}

/* ADB resolution mirrors scrcpy: $ADB env → bundled bin → system PATH */
static char *get_adb_exe(void)
{
#ifdef _WIN32
	const char *adb_name = "adb.exe";
#else
	const char *adb_name = "adb";
#endif
	const char *env = getenv("ADB");
	if (env && *env && os_file_exists(env))
		return bstrdup(env);
	char *bin = get_bin_dir();
	if (bin) {
		char *p = path_join(bin, adb_name);
		bfree(bin);
		if (os_file_exists(p))
			return p;
		bfree(p);
	}
	return bstrdup(adb_name);
}

#ifdef _WIN32
static char *adb_devices_l(const char *adb_exe)
{
	struct dstr cmdline = {0};
	if (strchr(adb_exe, ' ')) {
		dstr_cat_ch(&cmdline, '"');
		dstr_cat(&cmdline, adb_exe);
		dstr_cat_ch(&cmdline, '"');
	} else {
		dstr_cat(&cmdline, adb_exe);
	}
	dstr_cat(&cmdline, " devices -l");

	SECURITY_ATTRIBUTES sa = {sizeof(sa), NULL, TRUE};
	HANDLE rd, wr;
	if (!CreatePipe(&rd, &wr, &sa, 0)) {
		dstr_free(&cmdline);
		return NULL;
	}
	SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

	int wlen = MultiByteToWideChar(CP_UTF8, 0, cmdline.array, -1, NULL, 0);
	wchar_t *wcmd = bmalloc((size_t)wlen * sizeof(wchar_t));
	MultiByteToWideChar(CP_UTF8, 0, cmdline.array, -1, wcmd, wlen);
	dstr_free(&cmdline);

	STARTUPINFOW si = {0};
	si.cb = sizeof(si);
	si.dwFlags = STARTF_USESTDHANDLES;
	si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
	si.hStdOutput = wr;
	si.hStdError = GetStdHandle(STD_ERROR_HANDLE);

	PROCESS_INFORMATION pi = {0};
	BOOL ok = CreateProcessW(NULL, wcmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
	bfree(wcmd);
	CloseHandle(wr);

	if (!ok) {
		CloseHandle(rd);
		return NULL;
	}

	struct dstr out = {0};
	char buf[1024];
	DWORD n;
	while (ReadFile(rd, buf, sizeof(buf) - 1, &n, NULL) && n > 0) {
		buf[n] = 0;
		dstr_cat(&out, buf);
	}
	CloseHandle(rd);
	WaitForSingleObject(pi.hProcess, 5000);
	CloseHandle(pi.hProcess);
	CloseHandle(pi.hThread);
	return out.array;
}
#else
static char *adb_devices_l(const char *adb_exe)
{
	struct dstr cmd = {0};
	dstr_printf(&cmd, "\"%s\" devices -l 2>/dev/null", adb_exe);
	FILE *f = popen(cmd.array, "r");
	dstr_free(&cmd);
	if (!f)
		return NULL;
	struct dstr out = {0};
	char buf[1024];
	while (fgets(buf, sizeof(buf), f))
		dstr_cat(&out, buf);
	pclose(f);
	return out.array;
}
#endif

bool adb_forward_tcp(const char *serial, uint16_t port)
{
	if (!serial || !*serial || port == 0)
		return false;

	char port_arg[32];
	snprintf(port_arg, sizeof(port_arg), "tcp:%u", (unsigned)port);

	char *adb = get_adb_exe();
	if (!adb)
		return false;

	const char *argv[] = {"-s", serial, "forward", port_arg, port_arg, NULL};
	char *output = NULL;
	bool ok = scrcpy_proc_run_capture(adb, argv, &output);
	bfree(output);
	bfree(adb);
	return ok;
}

void adb_remove_forward(const char *serial, uint16_t port)
{
	if (!serial || !*serial || port == 0)
		return;

	char port_arg[32];
	snprintf(port_arg, sizeof(port_arg), "tcp:%u", (unsigned)port);

	char *adb = get_adb_exe();
	if (!adb)
		return;

	const char *argv[] = {"-s", serial, "forward", "--remove", port_arg, NULL};
	char *output = NULL;
	(void)scrcpy_proc_run_capture(adb, argv, &output);
	bfree(output);
	bfree(adb);
}

void fill_device_list(obs_property_t *list)
{
	obs_property_list_clear(list);

	char *adb = get_adb_exe();
	char *output = adb_devices_l(adb);
	bfree(adb);

	if (!output || !*output)
		goto done;

	const char *p = output;
	while (*p) {
		const char *eol = p;
		while (*eol && *eol != '\n' && *eol != '\r')
			eol++;

		size_t line_len = (size_t)(eol - p);
		if (line_len > 0 && strncmp(p, "List of", 7) != 0) {
			/* first whitespace-delimited token = serial */
			const char *tok_end = p;
			while (tok_end < eol && *tok_end != ' ' && *tok_end != '\t')
				tok_end++;
			size_t serial_len = (size_t)(tok_end - p);

			if (serial_len > 0 && serial_len < 128) {
				char serial[128];
				memcpy(serial, p, serial_len);
				serial[serial_len] = 0;

				/* skip unusable devices */
				const char *rest = tok_end;
				while (rest < eol && (*rest == ' ' || *rest == '\t'))
					rest++;
				if (strncmp(rest, "offline", 7) == 0 || strncmp(rest, "unauthorized", 12) == 0 ||
				    strncmp(rest, "no permissions", 14) == 0) {
					p = eol;
					while (*p == '\n' || *p == '\r')
						p++;
					continue;
				}

				/* extract model: token */
				char model[128] = {0};
				const char *m = strstr(p, "model:");
				if (m && m < eol) {
					m += 6;
					size_t mlen = 0;
					while (m + mlen < eol && m[mlen] && m[mlen] != ' ' && m[mlen] != '\t')
						mlen++;
					if (mlen > 0 && mlen < sizeof(model)) {
						memcpy(model, m, mlen);
						model[mlen] = 0;
						for (size_t i = 0; i < mlen; i++)
							if (model[i] == '_')
								model[i] = ' ';
					}
				}

				struct dstr label = {0};
				if (model[0])
					dstr_printf(&label, "%s (%s)", model, serial);
				else
					dstr_copy(&label, serial);
				obs_property_list_add_string(list, label.array, serial);
				dstr_free(&label);
			}
		}

		p = eol;
		while (*p == '\n' || *p == '\r')
			p++;
	}

done:
	bfree(output);
}

char *first_adb_serial(void)
{
	char *adb = get_adb_exe();
	char *output = adb_devices_l(adb);
	bfree(adb);
	if (!output)
		return NULL;

	char *result = NULL;
	const char *p = output;
	while (*p && !result) {
		const char *eol = p;
		while (*eol && *eol != '\n' && *eol != '\r')
			eol++;

		size_t line_len = (size_t)(eol - p);
		if (line_len > 0 && strncmp(p, "List of", 7) != 0) {
			const char *tok_end = p;
			while (tok_end < eol && *tok_end != ' ' && *tok_end != '\t')
				tok_end++;
			size_t slen = (size_t)(tok_end - p);

			if (slen > 0 && slen < 128) {
				const char *rest = tok_end;
				while (rest < eol && (*rest == ' ' || *rest == '\t'))
					rest++;
				if (strncmp(rest, "offline", 7) != 0 && strncmp(rest, "unauthorized", 12) != 0 &&
				    strncmp(rest, "no permissions", 14) != 0) {
					char serial[128];
					memcpy(serial, p, slen);
					serial[slen] = 0;
					result = bstrdup(serial);
				}
			}
		}

		p = eol;
		while (*p == '\n' || *p == '\r')
			p++;
	}

	bfree(output);
	return result;
}
