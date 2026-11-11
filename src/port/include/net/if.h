/*
 * Minimal <net/if.h> for the reference-ril port.
 *
 * lwIP installs its own <net/if.h>-ish definitions through sys/socket.h and
 * already provides struct ifreq. This shim only fills in the interface-flag
 * bits used by reference-ril's setInterfaceState().
 */
#pragma once

#include <sys/socket.h>
#include <sys/ioctl.h>

#ifndef IFNAMSIZ
#define IFNAMSIZ 16
#endif

#ifndef IFF_UP
#define IFF_UP 0x1
#endif

#ifndef SIOCGIFFLAGS
#define SIOCGIFFLAGS 0x8913
#endif

#ifndef SIOCSIFFLAGS
#define SIOCSIFFLAGS 0x8914
#endif

#ifdef __cplusplus
extern "C" {
#endif

unsigned int if_nametoindex(const char *ifname);

#ifdef __cplusplus
}
#endif
