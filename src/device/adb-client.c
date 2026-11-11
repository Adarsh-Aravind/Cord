#include "adb-client.h"

#include <obs-module.h>
#include <util/platform.h>
#include <util/dstr.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#endif

#define LOG(level, fmt, ...) blog(level, "[cord/adb] " fmt, ##__VA_ARGS__)

/* Upper bound on any one adb invocation (start-server, push). */
#define CORD_ADB_CMD_TIMEOUT_MS 30000

static char adb_path[1024] = {0};

bool cord_adb_global_init(void)
{
#ifdef _WIN32
	WSADATA wsa;
	int r = WSAStartup(MAKEWORD(2, 2), &wsa);
	if (r != 0) {
		LOG(LOG_ERROR, "WSAStartup failed: %d", r);
		return false;
	}
#endif
	return true;
}

void cord_adb_global_shutdown(void)
{
#ifdef _WIN32
	WSACleanup();
#endif
}

const char *cord_adb_executable(void)
{
	if (adb_path[0])
		return adb_path;

	/* Prefer the copy we ship, so behaviour does not depend on the host. */
	char *bundled = obs_module_file("bin/adb.exe");
	if (bundled && os_file_exists(bundled)) {
		/*
		 * obs_module_file() can return a path relative to the OBS
		 * binary directory. Resolve it now: it is handed to
		 * CreateProcessW later, and relying on the process CWD staying
		 * put is a trap.
		 */
		char *absolute = os_get_abs_path_ptr(bundled);
		snprintf(adb_path, sizeof(adb_path), "%s",
			 absolute ? absolute : bundled);
		bfree(absolute);
		bfree(bundled);
		LOG(LOG_INFO, "using bundled adb: %s", adb_path);
		return adb_path;
	}
	bfree(bundled);

	snprintf(adb_path, sizeof(adb_path), "adb");
	LOG(LOG_INFO, "no bundled adb, falling back to PATH");
	return adb_path;
}

/* ------------------------------------------------------------------ */
/* socket helpers                                                      */
/* ------------------------------------------------------------------ */

void cord_socket_close(cord_socket_t sock)
{
	if (sock == CORD_INVALID_SOCKET)
		return;
#ifdef _WIN32
	closesocket(sock);
#else
	close(sock);
#endif
}

bool cord_socket_recv_all(cord_socket_t sock, void *buf, size_t len)
{
	uint8_t *p = buf;
	size_t got = 0;
	while (got < len) {
		int r = recv(sock, (char *)p + got, (int)(len - got), 0);
		if (r <= 0)
			return false;
		got += (size_t)r;
	}
	return true;
}

bool cord_socket_send_all(cord_socket_t sock, const void *buf, size_t len)
{
	const uint8_t *p = buf;
	size_t sent = 0;
	while (sent < len) {
		int r = send(sock, (const char *)p + sent, (int)(len - sent), 0);
		if (r <= 0)
			return false;
		sent += (size_t)r;
	}
	return true;
}

void cord_socket_set_nodelay(cord_socket_t sock, bool enable)
{
	int flag = enable ? 1 : 0;
	setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (const char *)&flag,
		   sizeof(flag));
}

void cord_socket_set_recv_timeout(cord_socket_t sock, int timeout_ms)
{
#ifdef _WIN32
	DWORD tv = (DWORD)timeout_ms;
	setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv,
		   sizeof(tv));
#else
	struct timeval tv = {timeout_ms / 1000, (timeout_ms % 1000) * 1000};
	setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
}

cord_socket_t cord_socket_connect_localhost(uint16_t port)
{
	cord_socket_t sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (sock == CORD_INVALID_SOCKET)
		return CORD_INVALID_SOCKET;

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

	if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
		cord_socket_close(sock);
		return CORD_INVALID_SOCKET;
	}
	return sock;
}

cord_socket_t cord_socket_listen_local(uint16_t *out_port)
{
	cord_socket_t sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (sock == CORD_INVALID_SOCKET)
		return CORD_INVALID_SOCKET;

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = 0; /* let the OS choose */
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

	if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
	    listen(sock, 4) != 0) {
		cord_socket_close(sock);
		return CORD_INVALID_SOCKET;
	}

	int len = (int)sizeof(addr);
	if (getsockname(sock, (struct sockaddr *)&addr, &len) != 0) {
		cord_socket_close(sock);
		return CORD_INVALID_SOCKET;
	}
	*out_port = ntohs(addr.sin_port);
	return sock;
}

cord_socket_t cord_socket_accept_timeout(cord_socket_t server, int timeout_ms)
{
	fd_set set;
	FD_ZERO(&set);
	FD_SET(server, &set);

	struct timeval tv = {timeout_ms / 1000, (timeout_ms % 1000) * 1000};
	int r = select((int)server + 1, &set, NULL, NULL, &tv);
	if (r <= 0)
		return CORD_INVALID_SOCKET;

	return accept(server, NULL, NULL);
}

/* ------------------------------------------------------------------ */
/* adb wire protocol                                                   */
/* ------------------------------------------------------------------ */

/* Requests are "%04x" of the payload length, then the payload. */
static bool adb_send_request(cord_socket_t sock, const char *service)
{
	char hdr[8];
	size_t len = strlen(service);
	snprintf(hdr, sizeof(hdr), "%04x", (unsigned)len);
	return cord_socket_send_all(sock, hdr, 4) &&
	       cord_socket_send_all(sock, service, len);
}

/* Reads a 4-byte hex length then that many bytes. Caller frees. */
static char *adb_read_hex_payload(cord_socket_t sock)
{
	char lenbuf[5] = {0};
	if (!cord_socket_recv_all(sock, lenbuf, 4))
		return NULL;

	unsigned long len = strtoul(lenbuf, NULL, 16);
	char *data = bmalloc(len + 1);
	if (len && !cord_socket_recv_all(sock, data, len)) {
		bfree(data);
		return NULL;
	}
	data[len] = 0;
	return data;
}

/* Reads the OKAY/FAIL status. On FAIL, logs the reason. */
static bool adb_read_status(cord_socket_t sock, const char *what)
{
	char status[5] = {0};
	if (!cord_socket_recv_all(sock, status, 4)) {
		LOG(LOG_ERROR, "%s: no reply from adb server", what);
		return false;
	}

	if (memcmp(status, "OKAY", 4) == 0)
		return true;

	if (memcmp(status, "FAIL", 4) == 0) {
		char *reason = adb_read_hex_payload(sock);
		LOG(LOG_ERROR, "%s failed: %s", what,
		    reason ? reason : "(no reason given)");
		bfree(reason);
		return false;
	}

	LOG(LOG_ERROR, "%s: malformed reply", what);
	return false;
}

/*
 * Spawns adb with the given args and waits for it. Returns the exit code, or
 * -1 if it could not be run or did not finish in time.
 *
 * Deliberately NOT os_process_pipe_create():
 *
 *   That helper spawns with bInheritHandles=TRUE and gives you a stdout pipe.
 *   `adb start-server` forks a daemon that outlives the command, and that
 *   daemon inherits the pipe's write end - so the read side never reaches EOF
 *   and draining it blocks for as long as the adb server lives. On the UI
 *   thread that is an OBS window frozen until the server is killed.
 *
 * CreateProcessW with inheritance OFF avoids the whole problem: nothing is
 * shared with the daemon, so the foreground adb exits immediately and the wait
 * returns. We only need the exit code, never adb's chatter.
 *
 * The application name is NULL and the command line carries the exe, so the
 * executable path is quoted (it contains spaces in a normal OBS install) and
 * nothing else is.
 */
static int run_adb(const char *args)
{
	struct dstr cmd = {0};
	dstr_printf(&cmd, "\"%s\" %s", cord_adb_executable(), args);

	wchar_t *cmd_w = NULL;
	os_utf8_to_wcs_ptr(cmd.array, 0, &cmd_w);
	if (!cmd_w) {
		LOG(LOG_ERROR, "could not convert the adb command line");
		dstr_free(&cmd);
		return -1;
	}

	STARTUPINFOW si = {0};
	si.cb = sizeof(si);
	PROCESS_INFORMATION pi = {0};

	const BOOL started = CreateProcessW(NULL, cmd_w, NULL, NULL,
					    FALSE, /* no inherited handles */
					    CREATE_NO_WINDOW, NULL, NULL, &si,
					    &pi);
	bfree(cmd_w);

	if (!started) {
		LOG(LOG_ERROR, "could not run adb (%s): error %lu", cmd.array,
		    GetLastError());
		dstr_free(&cmd);
		return -1;
	}
	dstr_free(&cmd);

	CloseHandle(pi.hThread);

	int result = -1;
	if (WaitForSingleObject(pi.hProcess, CORD_ADB_CMD_TIMEOUT_MS) ==
	    WAIT_OBJECT_0) {
		DWORD code = (DWORD)-1;
		if (GetExitCodeProcess(pi.hProcess, &code))
			result = (int)code;
	} else {
		LOG(LOG_WARNING, "adb did not finish within %d ms",
		    CORD_ADB_CMD_TIMEOUT_MS);
		TerminateProcess(pi.hProcess, 1);
	}

	CloseHandle(pi.hProcess);
	return result;
}

bool cord_adb_ensure_server(void)
{
	cord_socket_t sock = cord_socket_connect_localhost(CORD_ADB_SERVER_PORT);
	if (sock != CORD_INVALID_SOCKET) {
		cord_socket_close(sock);
		return true;
	}

	LOG(LOG_INFO, "no adb server on :%d, starting one",
	    CORD_ADB_SERVER_PORT);
	run_adb("start-server");

	/* The server needs a moment to bind. */
	for (int i = 0; i < 20; i++) {
		os_sleep_ms(100);
		sock = cord_socket_connect_localhost(CORD_ADB_SERVER_PORT);
		if (sock != CORD_INVALID_SOCKET) {
			cord_socket_close(sock);
			return true;
		}
	}

	LOG(LOG_ERROR, "could not start an adb server; is adb installed?");
	return false;
}

char *cord_adb_query(const char *service)
{
	if (!cord_adb_ensure_server())
		return NULL;

	cord_socket_t sock = cord_socket_connect_localhost(CORD_ADB_SERVER_PORT);
	if (sock == CORD_INVALID_SOCKET)
		return NULL;

	cord_socket_set_recv_timeout(sock, 5000);

	char *result = NULL;
	if (adb_send_request(sock, service) && adb_read_status(sock, service))
		result = adb_read_hex_payload(sock);

	cord_socket_close(sock);
	return result;
}

/* Host services that reply OKAY with no payload (forward, killforward). */
static bool adb_query_void(const char *service)
{
	if (!cord_adb_ensure_server())
		return false;

	cord_socket_t sock = cord_socket_connect_localhost(CORD_ADB_SERVER_PORT);
	if (sock == CORD_INVALID_SOCKET)
		return false;

	cord_socket_set_recv_timeout(sock, 5000);
	bool ok = adb_send_request(sock, service) &&
		  adb_read_status(sock, service);
	cord_socket_close(sock);
	return ok;
}

cord_socket_t cord_adb_open_service(const char *serial, const char *service)
{
	if (!cord_adb_ensure_server())
		return CORD_INVALID_SOCKET;

	cord_socket_t sock = cord_socket_connect_localhost(CORD_ADB_SERVER_PORT);
	if (sock == CORD_INVALID_SOCKET)
		return CORD_INVALID_SOCKET;

	struct dstr transport = {0};
	dstr_printf(&transport, "host:transport:%s", serial);

	bool ok = adb_send_request(sock, transport.array) &&
		  adb_read_status(sock, transport.array);
	dstr_free(&transport);

	if (!ok) {
		cord_socket_close(sock);
		return CORD_INVALID_SOCKET;
	}

	/* The connection now belongs to the device; open the service on it. */
	if (!adb_send_request(sock, service) ||
	    !adb_read_status(sock, service)) {
		cord_socket_close(sock);
		return CORD_INVALID_SOCKET;
	}

	return sock;
}

/* ------------------------------------------------------------------ */
/* device enumeration                                                  */
/* ------------------------------------------------------------------ */

static enum cord_device_state parse_state(const char *s)
{
	if (strcmp(s, "device") == 0)
		return CORD_DEV_DEVICE;
	if (strcmp(s, "offline") == 0)
		return CORD_DEV_OFFLINE;
	if (strcmp(s, "unauthorized") == 0)
		return CORD_DEV_UNAUTHORIZED;
	return CORD_DEV_UNKNOWN;
}

/*
 * host:devices-l lines look like:
 *   SERIAL <tab> device product:x model:Pixel_7 device:y transport_id:3
 * Fields after the state are space-separated key:value pairs.
 */
static void parse_device_line(const char *line, struct cord_device *dev)
{
	memset(dev, 0, sizeof(*dev));

	/* serial = up to the first whitespace */
	size_t i = 0;
	while (line[i] && line[i] != ' ' && line[i] != '\t')
		i++;
	size_t n = i < CORD_SERIAL_MAX - 1 ? i : CORD_SERIAL_MAX - 1;
	memcpy(dev->serial, line, n);
	dev->serial[n] = 0;

	/* A Wi-Fi serial is host:port, which never appears in a USB serial. */
	dev->is_tcpip = strchr(dev->serial, ':') != NULL;

	while (line[i] == ' ' || line[i] == '\t')
		i++;

	/* state = next token */
	char state[32] = {0};
	size_t j = 0;
	while (line[i] && line[i] != ' ' && line[i] != '\t' &&
	       j < sizeof(state) - 1)
		state[j++] = line[i++];
	dev->state = parse_state(state);

	/* model:<value>, shown in the UI instead of a bare serial */
	const char *model = strstr(line + i, "model:");
	if (model) {
		model += 6;
		j = 0;
		while (model[j] && model[j] != ' ' && model[j] != '\t' &&
		       j < CORD_MODEL_MAX - 1) {
			dev->model[j] = model[j] == '_' ? ' ' : model[j];
			j++;
		}
		dev->model[j] = 0;
	}

	if (!dev->model[0])
		snprintf(dev->model, sizeof(dev->model), "%s", dev->serial);
}

bool cord_adb_list_devices(struct cord_device_list *out)
{
	memset(out, 0, sizeof(*out));

	char *payload = cord_adb_query("host:devices-l");
	if (!payload)
		return false;

	/* Count line breaks to size the array in one allocation. */
	size_t cap = 1;
	for (const char *p = payload; *p; p++)
		if (*p == '\n')
			cap++;

	out->items = bzalloc(sizeof(struct cord_device) * (cap + 1));

	char *saveptr = NULL;
	char *line = strtok_s(payload, "\r\n", &saveptr);
	while (line) {
		if (*line)
			parse_device_line(line, &out->items[out->count++]);
		line = strtok_s(NULL, "\r\n", &saveptr);
	}

	bfree(payload);
	return true;
}

void cord_adb_free_device_list(struct cord_device_list *list)
{
	bfree(list->items);
	list->items = NULL;
	list->count = 0;
}

char *cord_adb_getprop(const char *serial, const char *prop)
{
	struct dstr service = {0};
	dstr_printf(&service, "shell:getprop %s", prop);

	cord_socket_t sock = cord_adb_open_service(serial, service.array);
	dstr_free(&service);
	if (sock == CORD_INVALID_SOCKET)
		return NULL;

	cord_socket_set_recv_timeout(sock, 3000);

	/* shell: streams raw output until EOF. */
	struct dstr buf = {0};
	char chunk[256];
	for (;;) {
		int r = recv(sock, chunk, sizeof(chunk) - 1, 0);
		if (r <= 0)
			break;
		chunk[r] = 0;
		dstr_cat(&buf, chunk);
	}
	cord_socket_close(sock);

	dstr_depad(&buf); /* strip the newline getprop always appends */
	return buf.array;
}

int cord_adb_sdk_version(const char *serial)
{
	char *val = cord_adb_getprop(serial, "ro.build.version.sdk");
	if (!val)
		return 0;
	int sdk = atoi(val);
	bfree(val);
	return sdk;
}

/* ------------------------------------------------------------------ */
/* tunnels                                                             */
/* ------------------------------------------------------------------ */

bool cord_adb_reverse(const char *serial, const char *remote,
		      uint16_t local_port)
{
	/*
	 * reverse: runs on the device transport, not as a host-serial service,
	 * so the connection must be switched to the device first. The socket is
	 * closed right after; the tunnel outlives it.
	 */
	struct dstr service = {0};
	dstr_printf(&service, "reverse:forward:localabstract:%s;tcp:%u", remote,
		    (unsigned)local_port);

	cord_socket_t sock = cord_adb_open_service(serial, service.array);
	dstr_free(&service);

	if (sock == CORD_INVALID_SOCKET)
		return false;

	cord_socket_close(sock);
	return true;
}

bool cord_adb_reverse_remove(const char *serial, const char *remote)
{
	struct dstr service = {0};
	dstr_printf(&service, "reverse:killforward:localabstract:%s", remote);

	cord_socket_t sock = cord_adb_open_service(serial, service.array);
	dstr_free(&service);

	if (sock == CORD_INVALID_SOCKET)
		return false;

	cord_socket_close(sock);
	return true;
}

bool cord_adb_forward(const char *serial, uint16_t local_port,
		      const char *remote)
{
	struct dstr service = {0};
	dstr_printf(&service,
		    "host-serial:%s:forward:norebind:tcp:%u;localabstract:%s",
		    serial, (unsigned)local_port, remote);
	bool ok = adb_query_void(service.array);
	dstr_free(&service);
	return ok;
}

bool cord_adb_forward_remove(const char *serial, uint16_t local_port)
{
	struct dstr service = {0};
	dstr_printf(&service, "host-serial:%s:killforward:tcp:%u", serial,
		    (unsigned)local_port);
	bool ok = adb_query_void(service.array);
	dstr_free(&service);
	return ok;
}

bool cord_adb_push(const char *serial, const char *local, const char *remote)
{
	struct dstr args = {0};
	dstr_printf(&args, "-s %s push \"%s\" \"%s\"", serial, local, remote);
	int rc = run_adb(args.array);
	dstr_free(&args);

	if (rc != 0) {
		LOG(LOG_ERROR, "push of %s failed (exit %d)", local, rc);
		return false;
	}
	return true;
}
