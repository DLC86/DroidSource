#include "scrcpy-camera-control.h"

#include "scrcpy-adb.h"

#include <obs-module.h>

#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET camera_socket_t;
#define CAMERA_INVALID_SOCKET INVALID_SOCKET
#define camera_close_socket closesocket
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int camera_socket_t;
#define CAMERA_INVALID_SOCKET (-1)
#define camera_close_socket close
#endif

enum {
	CAMERA_CTL_ZOOM = 1,
	CAMERA_CTL_TORCH = 2,
	CAMERA_CTL_EXPOSURE = 3,
	CAMERA_CTL_FOCUS = 4,
	CAMERA_CTL_WHITE_BALANCE = 5,
};

struct scrcpy_camera_control {
	char *serial;
	uint16_t port;
	camera_socket_t socket;
};

static void close_control_socket(struct scrcpy_camera_control *control)
{
	if (control->socket != CAMERA_INVALID_SOCKET) {
#ifdef _WIN32
		shutdown(control->socket, SD_BOTH);
#else
		shutdown(control->socket, SHUT_RDWR);
#endif
		camera_close_socket(control->socket);
		control->socket = CAMERA_INVALID_SOCKET;
	}
}

static bool send_all(camera_socket_t socket, const uint8_t *data, size_t size)
{
	size_t offset = 0;
	while (offset < size) {
		int n = send(socket, (const char *)data + offset, (int)(size - offset), 0);
		if (n <= 0)
			return false;
		offset += (size_t)n;
	}
	return true;
}

static void write_u32be(uint8_t *dst, uint32_t value)
{
	dst[0] = (uint8_t)(value >> 24);
	dst[1] = (uint8_t)(value >> 16);
	dst[2] = (uint8_t)(value >> 8);
	dst[3] = (uint8_t)value;
}

static bool connect_control(struct scrcpy_camera_control *control)
{
	if (control->socket != CAMERA_INVALID_SOCKET)
		return true;

	if (!adb_forward_tcp(control->serial, control->port))
		return false;

	for (int attempt = 0; attempt < 20; ++attempt) {
		camera_socket_t socket_fd = socket(AF_INET, SOCK_STREAM, 0);
		if (socket_fd == CAMERA_INVALID_SOCKET)
			return false;

		struct sockaddr_in addr = {0};
		addr.sin_family = AF_INET;
		addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		addr.sin_port = htons(control->port);

		if (connect(socket_fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
			int one = 1;
			setsockopt(socket_fd, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));
			control->socket = socket_fd;
			return true;
		}

		camera_close_socket(socket_fd);
#ifdef _WIN32
		Sleep(100);
#else
		usleep(100000);
#endif
	}
	return false;
}

static bool send_packet(struct scrcpy_camera_control *control, uint8_t type,
				 const uint8_t *payload, uint8_t payload_size)
{
	if (!connect_control(control))
		return false;

	uint8_t packet[32];
	packet[0] = type;
	packet[1] = payload_size;
	if (payload_size)
		memcpy(packet + 2, payload, payload_size);

	if (send_all(control->socket, packet, (size_t)payload_size + 2))
		return true;

	close_control_socket(control);
	return false;
}

static bool send_float_packet(struct scrcpy_camera_control *control, uint8_t type, float value)
{
	uint32_t bits = 0;
	memcpy(&bits, &value, sizeof(bits));
	uint8_t payload[4];
	write_u32be(payload, bits);
	return send_packet(control, type, payload, sizeof(payload));
}

static bool send_u32_packet(struct scrcpy_camera_control *control, uint8_t type, uint32_t value)
{
	uint8_t payload[4];
	write_u32be(payload, value);
	return send_packet(control, type, payload, sizeof(payload));
}

scrcpy_camera_control_t *scrcpy_camera_control_create(const char *serial, uint16_t port)
{
	if (!serial || !*serial || port == 0)
		return NULL;

	struct scrcpy_camera_control *control = bzalloc(sizeof(*control));
	control->serial = bstrdup(serial);
	control->port = port;
	control->socket = CAMERA_INVALID_SOCKET;
	return control;
}

void scrcpy_camera_control_destroy(scrcpy_camera_control_t *control)
{
	if (!control)
		return;

	close_control_socket(control);
	adb_remove_forward(control->serial, control->port);
	bfree(control->serial);
	bfree(control);
}

bool scrcpy_camera_control_apply(scrcpy_camera_control_t *control,
					 float zoom, bool torch, int iso, int shutter_us,
					 float focus_distance, int wb_kelvin)
{
	if (!control)
		return false;

	bool ok = true;
	if (!send_float_packet(control, CAMERA_CTL_ZOOM, zoom))
		ok = false;

	uint8_t torch_payload = torch ? 1 : 0;
	if (!send_packet(control, CAMERA_CTL_TORCH, &torch_payload, 1))
		ok = false;

	uint8_t exposure[8];
	write_u32be(exposure, (uint32_t)(iso > 0 ? iso : 0));
	write_u32be(exposure + 4, (uint32_t)(shutter_us > 0 ? shutter_us : 0));
	if (!send_packet(control, CAMERA_CTL_EXPOSURE, exposure, sizeof(exposure)))
		ok = false;

	if (!send_float_packet(control, CAMERA_CTL_FOCUS, focus_distance))
		ok = false;

	if (!send_u32_packet(control, CAMERA_CTL_WHITE_BALANCE,
				     (uint32_t)(wb_kelvin > 0 ? wb_kelvin : 0)))
		ok = false;

	return ok;
}
