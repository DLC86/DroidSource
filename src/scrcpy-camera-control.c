#include "scrcpy-camera-control.h"

#include "scrcpy-adb.h"

#include <obs-module.h>
#include <util/threading.h>
#include <util/bmem.h>
#include <util/platform.h>

#include <pthread.h>
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
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int camera_socket_t;
#define CAMERA_INVALID_SOCKET (-1)
#define camera_close_socket close
#endif

#define CAMERA_CTL_SETTINGS 6
#define CAMERA_CTL_SETTINGS_SIZE 21

struct scrcpy_camera_control {
	char *serial;
	uint16_t port;
	camera_socket_t socket;

	pthread_t thread;
	bool thread_started;
	pthread_mutex_t mutex;
	pthread_cond_t cond;
	bool stop;
	uint64_t generation;
	uint64_t applied_generation;

	float zoom;
	bool torch;
	int iso;
	int shutter_us;
	float focus_distance;
	int wb_kelvin;
};

static void shutdown_control_socket(camera_socket_t socket)
{
	if (socket == CAMERA_INVALID_SOCKET)
		return;
#ifdef _WIN32
	shutdown(socket, SD_BOTH);
#else
	shutdown(socket, SHUT_RDWR);
#endif
}

static void close_control_socket(struct scrcpy_camera_control *control)
{
	if (control->socket != CAMERA_INVALID_SOCKET) {
		shutdown_control_socket(control->socket);
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

static uint32_t float_bits(float value)
{
	uint32_t bits;
	memcpy(&bits, &value, sizeof(bits));
	return bits;
}

static bool connect_control(struct scrcpy_camera_control *control)
{
	if (control->socket != CAMERA_INVALID_SOCKET)
		return true;

	if (!adb_forward_tcp(control->serial, control->port))
		return false;

	camera_socket_t socket_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (socket_fd == CAMERA_INVALID_SOCKET)
		return false;

	struct sockaddr_in addr = {0};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = htons(control->port);

	if (connect(socket_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
		camera_close_socket(socket_fd);
		return false;
	}

	int one = 1;
	setsockopt(socket_fd, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));
	control->socket = socket_fd;
	return true;
}

static bool send_snapshot(struct scrcpy_camera_control *control,
			  float zoom, bool torch, int iso, int shutter_us,
			  float focus_distance, int wb_kelvin)
{
	if (!connect_control(control))
		return false;

	uint8_t packet[2 + CAMERA_CTL_SETTINGS_SIZE];
	packet[0] = CAMERA_CTL_SETTINGS;
	packet[1] = CAMERA_CTL_SETTINGS_SIZE;
	write_u32be(packet + 2, float_bits(zoom));
	packet[6] = torch ? 1 : 0;
	write_u32be(packet + 7, (uint32_t)(iso > 0 ? iso : 0));
	write_u32be(packet + 11, (uint32_t)(shutter_us > 0 ? shutter_us : 0));
	write_u32be(packet + 15, float_bits(focus_distance));
	write_u32be(packet + 19, (uint32_t)(wb_kelvin > 0 ? wb_kelvin : 0));

	if (send_all(control->socket, packet, sizeof(packet)))
		return true;

	close_control_socket(control);
	return false;
}

static void *camera_control_worker(void *data)
{
	struct scrcpy_camera_control *control = data;
	os_set_thread_name("scrcpy-camera-control");

	pthread_mutex_lock(&control->mutex);
	for (;;) {
		while (!control->stop && control->generation == control->applied_generation)
			pthread_cond_wait(&control->cond, &control->mutex);

		if (control->stop) {
			pthread_mutex_unlock(&control->mutex);
			return NULL;
		}

		float zoom = control->zoom;
		bool torch = control->torch;
		int iso = control->iso;
		int shutter_us = control->shutter_us;
		float focus_distance = control->focus_distance;
		int wb_kelvin = control->wb_kelvin;
		uint64_t generation = control->generation;
		pthread_mutex_unlock(&control->mutex);

		bool ok = send_snapshot(control, zoom, torch, iso, shutter_us, focus_distance, wb_kelvin);

		pthread_mutex_lock(&control->mutex);
		if (ok && generation == control->generation)
			control->applied_generation = generation;
		bool stopping = control->stop;
		pthread_mutex_unlock(&control->mutex);

		if (!ok && !stopping)
			os_sleep_ms(100);
	}

	return NULL;
}

scrcpy_camera_control_t *scrcpy_camera_control_create(const char *serial, uint16_t port)
{
	if (!serial || !*serial || port == 0)
		return NULL;

	struct scrcpy_camera_control *control = bzalloc(sizeof(*control));
	control->serial = bstrdup(serial);
	control->port = port;
	control->socket = CAMERA_INVALID_SOCKET;

	pthread_mutex_init(&control->mutex, NULL);
	pthread_cond_init(&control->cond, NULL);

	if (pthread_create(&control->thread, NULL, camera_control_worker, control) != 0) {
		pthread_cond_destroy(&control->cond);
		pthread_mutex_destroy(&control->mutex);
		bfree(control->serial);
		bfree(control);
		return NULL;
	}
	control->thread_started = true;
	return control;
}

void scrcpy_camera_control_destroy(scrcpy_camera_control_t *control)
{
	if (!control)
		return;

	pthread_mutex_lock(&control->mutex);
	control->stop = true;
	pthread_cond_signal(&control->cond);
	pthread_mutex_unlock(&control->mutex);

	shutdown_control_socket(control->socket);

	if (control->thread_started)
		pthread_join(control->thread, NULL);

	if (control->socket != CAMERA_INVALID_SOCKET)
		close_control_socket(control);

	adb_remove_forward(control->serial, control->port);

	pthread_cond_destroy(&control->cond);
	pthread_mutex_destroy(&control->mutex);
	bfree(control->serial);
	bfree(control);
}

bool scrcpy_camera_control_apply(scrcpy_camera_control_t *control,
					 float zoom, bool torch, int iso, int shutter_us,
					 float focus_distance, int wb_kelvin)
{
	if (!control)
		return false;

	pthread_mutex_lock(&control->mutex);
	control->zoom = zoom;
	control->torch = torch;
	control->iso = iso;
	control->shutter_us = shutter_us;
	control->focus_distance = focus_distance;
	control->wb_kelvin = wb_kelvin;
	control->generation++;
	pthread_cond_signal(&control->cond);
	pthread_mutex_unlock(&control->mutex);

	return true;
}
