#pragma once

#include <obs-module.h>

#ifdef __cplusplus
extern "C" {
#endif

char *get_bin_dir(void);
char *path_join(const char *dir, const char *leaf);
void fill_device_list(obs_property_t *list);
char *first_adb_serial(void);

#ifdef __cplusplus
}
#endif

bool adb_forward_tcp(const char *serial, uint16_t port);
void adb_remove_forward(const char *serial, uint16_t port);
