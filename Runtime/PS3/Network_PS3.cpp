/**
 * @file Network_PS3.cpp
 * @brief PS3 networking layer — Phase-1 stub.
 *
 * The engine's Network interface must be satisfied for the build to link, but
 * the PS3 net stack (PSL1GHT's `net` / `netctl`) bring-up is deferred past the
 * minimal-bootable milestone. These inert stubs report "no network": HTTP
 * requests fall through as unavailable and raw sockets return -1. Fill in real
 * `netInitialize` / `socket` wiring in a later phase (Lua TCP/UDP, replication,
 * an HTTP backend) — the surface below is exactly the one the engine expects.
 */

#if defined(POLYPHASE_PLATFORM_ADDON)

#include "Network/Network.h"
#include "Log.h"
#include <string.h>
#include <stdio.h>

void NET_Initialize() {}
void NET_Shutdown()   {}
void NET_Update()     {}
bool NET_IsActive()   { return false; }

SocketHandle NET_SocketCreate() { return -1; }
void NET_SocketBind(SocketHandle, uint32_t, uint16_t) {}
int32_t NET_SocketRecvFrom(SocketHandle, char*, uint32_t, uint32_t&, uint16_t&) { return -1; }
int32_t NET_SocketSendTo(SocketHandle, const char*, uint32_t, uint32_t, uint16_t) { return -1; }
void NET_SocketSetBlocking(SocketHandle, bool) {}
void NET_SocketSetBroadcast(SocketHandle, bool) {}
void NET_SocketGetIpAndPort(SocketHandle, uint32_t& outIp, uint16_t& outPort) { outIp = 0; outPort = 0; }
int32_t NET_SocketRecv(SocketHandle, char*, uint32_t) { return -1; }
void NET_SocketClose(SocketHandle) {}
SocketHandle NET_SocketCreateStream() { return -1; }
bool NET_SocketConnect(SocketHandle, uint32_t, uint16_t, int32_t) { return false; }
int32_t NET_SocketSend(SocketHandle, const char*, uint32_t) { return -1; }
uint32_t NET_ResolveHost(const char*) { return 0; }
uint32_t NET_IpStringToUint32(const char*) { return 0; }
void NET_IpUint32ToString(uint32_t ip, char* outIpString)
{
    if (outIpString == nullptr) return;
    snprintf(outIpString, 16, "%u.%u.%u.%u",
             (unsigned)((ip >> 24) & 0xFF),
             (unsigned)((ip >> 16) & 0xFF),
             (unsigned)((ip >>  8) & 0xFF),
             (unsigned)( ip        & 0xFF));
}
uint32_t NET_GetIpAddress()  { return 0; }
uint32_t NET_GetSubnetMask() { return 0; }

#endif // POLYPHASE_PLATFORM_ADDON
