/*
 * Cord - minimal ADB client.
 *
 * Speaks the ADB wire protocol directly to the adb server on 127.0.0.1:5037
 * instead of scraping `adb.exe` stdout. Requests are "%04x" + payload; replies
 * are "OKAY" or "FAIL" + "%04x" + message.
 *
 * Why direct sockets: opening the `shell:` service on OUR socket means the
 * device-side server process is a child of that connection. Closing the socket
 * kills it. Spawning `adb.exe shell` instead leaves orphaned app_process
 * instances on the phone whenever we crash, which then hold the capture lock
 * and make the next connection fail.
 *
 * `adb.exe` is still used for the one-shot jar push, because reimplementing the
 * sync protocol buys nothing: that push is not latency-sensitive.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef _WIN32
#include <winsock2.h>
typedef SOCKET cord_socket_t;
#define CORD_INVALID_SOCKET INVALID_SOCKET
#define CORD_SHUT_RDWR SD_BOTH
#else
#include <sys/socket.h>
#include <unistd.h>
typedef int cord_socket_t;
#define CORD_INVALID_SOCKET (-1)
#define CORD_SHUT_RDWR SHUT_RDWR
#endif

#define CORD_ADB_SERVER_PORT 5037
#define CORD_SERIAL_MAX 128
#define CORD_MODEL_MAX 128

enum cord_device_state {
	CORD_DEV_OFFLINE = 0,
	CORD_DEV_DEVICE,        /* ready */
	CORD_DEV_UNAUTHORIZED,  /* user has not accepted the RSA prompt */
	CORD_DEV_UNKNOWN,
};

struct cord_device {
	char serial[CORD_SERIAL_MAX];
	char model[CORD_MODEL_MAX];
	enum cord_device_state state;
	bool is_tcpip; /* serial looks like host:port, i.e. Wi-Fi */
};

struct cord_device_list {
	struct cord_device *items;
	size_t count;
};

/* Process handle for a spawned adb.exe invocation. */
struct cord_proc;

/* --- lifecycle --- */
bool cord_adb_global_init(void);
void cord_adb_global_shutdown(void);

/*
 * Path to the adb executable: the bundled data/bin/adb.exe if present,
 * otherwise `adb` from PATH. Returned pointer is owned by the module.
 */
const char *cord_adb_executable(void);

/*
 * Make sure an adb server is listening. Returns false if it could not be
 * started (adb missing). Safe to call repeatedly.
 */
bool cord_adb_ensure_server(void);

/* --- host services --- */

/*
 * Run a host service (e.g. "host:version", "host:devices-l") and return its
 * payload as a NUL-terminated heap string, or NULL on failure. Caller frees.
 */
char *cord_adb_query(const char *service);

/* Enumerate devices. Free with cord_adb_free_device_list(). */
bool cord_adb_list_devices(struct cord_device_list *out);
void cord_adb_free_device_list(struct cord_device_list *list);

/* Read a device property, e.g. "ro.build.version.sdk". Caller frees. */
char *cord_adb_getprop(const char *serial, const char *prop);

/* Android API level, or 0 if it could not be determined. */
int cord_adb_sdk_version(const char *serial);

/* --- tunnels --- */

/*
 * reverse: the device connects out to us. Preferred, because it needs no
 * retry loop and no dummy byte to prove the far end is listening.
 * `remote` is the abstract socket name, e.g. "scrcpy_0a1b2c3d".
 */
bool cord_adb_reverse(const char *serial, const char *remote,
		      uint16_t local_port);
bool cord_adb_reverse_remove(const char *serial, const char *remote);

/* forward: we connect to the device. Fallback for devices that refuse reverse. */
bool cord_adb_forward(const char *serial, uint16_t local_port,
		      const char *remote);
bool cord_adb_forward_remove(const char *serial, uint16_t local_port);

/* --- device services --- */

/*
 * Switch a fresh connection to `serial` and open `service` on it
 * (e.g. "shell:echo hi"). Returns a connected socket on success. The caller
 * owns it and must cord_socket_close() it; for `shell:` that also terminates
 * the remote process.
 */
cord_socket_t cord_adb_open_service(const char *serial, const char *service);

/* Push a local file to the device via adb.exe. Blocks until done. */
bool cord_adb_push(const char *serial, const char *local, const char *remote);

/* --- socket helpers --- */
void cord_socket_close(cord_socket_t sock);
/* recv() until `len` bytes are read; returns false on EOF or error. */
bool cord_socket_recv_all(cord_socket_t sock, void *buf, size_t len);
bool cord_socket_send_all(cord_socket_t sock, const void *buf, size_t len);
/* Connect to a port on 127.0.0.1 (forward-tunnel mode). */
cord_socket_t cord_socket_connect_localhost(uint16_t port);
/* Listen on an ephemeral localhost port for the reverse tunnel. */
cord_socket_t cord_socket_listen_local(uint16_t *out_port);
cord_socket_t cord_socket_accept_timeout(cord_socket_t server, int timeout_ms);
void cord_socket_set_nodelay(cord_socket_t sock, bool enable);
void cord_socket_set_recv_timeout(cord_socket_t sock, int timeout_ms);
