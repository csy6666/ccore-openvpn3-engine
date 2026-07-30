/* SPDX-License-Identifier: MPL-2.0 */

#ifndef CCORE_OPENVPN3_H
#define CCORE_OPENVPN3_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#define CCORE_OVPN3_API __declspec(dllexport)
#else
#define CCORE_OVPN3_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define CCORE_OVPN3_ABI_VERSION 2

typedef struct ccore_ovpn3_client ccore_ovpn3_client;

/*
 * The host owns every bridge returned by open_bridge.  It MUST listen only on
 * IPv4 loopback and MUST use its supplied detour dialer for the remote side.
 * Returning an error aborts the OpenVPN attempt; the engine never opens a
 * direct socket to the profile's remote endpoint.
 */
typedef int (*ccore_ovpn3_open_bridge_fn)(
    uint64_t user_data,
    const char *protocol,
    const char *remote_host,
    uint16_t remote_port,
    uint64_t *bridge_id,
    uint16_t *loopback_port,
    char *error,
    size_t error_capacity);

typedef void (*ccore_ovpn3_close_bridge_fn)(
    uint64_t user_data,
    uint64_t bridge_id);

/* Return zero to accept the pushed tunnel configuration, non-zero to reject. */
typedef int (*ccore_ovpn3_tunnel_config_fn)(
    uint64_t user_data,
    const char *configuration_json);

typedef void (*ccore_ovpn3_event_fn)(
    uint64_t user_data,
    const char *name,
    const char *info,
    int failed);

typedef struct ccore_ovpn3_client_options {
    size_t struct_size;
    const char *profile;
    const char *username;
    const char *password;
    int enable_legacy_algorithms;
    uint64_t user_data;
    ccore_ovpn3_open_bridge_fn open_bridge;
    ccore_ovpn3_close_bridge_fn close_bridge;
    ccore_ovpn3_tunnel_config_fn tunnel_config;
    ccore_ovpn3_event_fn event;
} ccore_ovpn3_client_options;

/*
 * String functions return the required byte count including the trailing NUL.
 * If output is NULL or capacity is zero, no bytes are written.
 */
CCORE_OVPN3_API unsigned int ccore_ovpn3_abi_version(void);
CCORE_OVPN3_API size_t ccore_ovpn3_version(char *output, size_t capacity);
CCORE_OVPN3_API size_t ccore_ovpn3_license(char *output, size_t capacity);
CCORE_OVPN3_API size_t ccore_ovpn3_eval_profile(
    const char *profile,
    char *output,
    size_t capacity);

/* Runtime lifecycle. All functions are thread-safe unless noted otherwise. */
CCORE_OVPN3_API int ccore_ovpn3_client_create(
    const ccore_ovpn3_client_options *options,
    ccore_ovpn3_client **client);
CCORE_OVPN3_API int ccore_ovpn3_client_start(ccore_ovpn3_client *client);
CCORE_OVPN3_API int ccore_ovpn3_client_ready(const ccore_ovpn3_client *client);
CCORE_OVPN3_API int ccore_ovpn3_client_reconnect(ccore_ovpn3_client *client);
CCORE_OVPN3_API int ccore_ovpn3_client_stop(ccore_ovpn3_client *client);
CCORE_OVPN3_API void ccore_ovpn3_client_destroy(ccore_ovpn3_client *client);

/*
 * Packet reads return bytes read, zero on timeout, and -1 on error.
 * Packet writes return zero on success and -1 on error.
 */
CCORE_OVPN3_API long long ccore_ovpn3_client_read_packet(
    ccore_ovpn3_client *client,
    unsigned char *packet,
    size_t capacity,
    int timeout_ms);
CCORE_OVPN3_API int ccore_ovpn3_client_write_packet(
    ccore_ovpn3_client *client,
    const unsigned char *packet,
    size_t length);

CCORE_OVPN3_API size_t ccore_ovpn3_client_last_error(
    const ccore_ovpn3_client *client,
    char *output,
    size_t capacity);

#ifdef __cplusplus
}
#endif

#endif
