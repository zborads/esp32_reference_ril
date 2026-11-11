/*
 * ESP-IDF port of the Android "cutils/sockets.h" shim.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#define ANDROID_SOCKET_NAMESPACE_RESERVED   0
#define ANDROID_SOCKET_NAMESPACE_FILESYSTEM 1
#define ANDROID_SOCKET_NAMESPACE_ABSTRACT   2

/* Local (AF_UNIX / abstract) sockets are not available on lwIP. */
int socket_local_client(const char *name, int namespaceId, int type);

/* TCP client helper, used by reference-ril's "-p <port>" loopback mode. */
int socket_network_client(const char *host, int port, int type);
int socket_network_client_timeout(const char *host, int port, int type, int timeout_secs);

#ifdef __cplusplus
}
#endif
