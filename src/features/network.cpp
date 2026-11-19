// Network providers: larger UDP socket buffers and per-session datagram statistics.
//
// Storm loads the network providers (standard.snp for LAN UDP, battle.snp for Battle.net) with LoadLibraryA.
// Neither sets SO_RCVBUF/SO_SNDBUF, so a turn's burst of datagrams (one per peer per turn, all broadcast; see
// re/notes/network.md) can overflow the default receive buffer and the kernel drops them, which costs a resend
// and stalls the lockstep. Each provider's socket calls are wrapped: every UDP socket gets larger buffers once it
// is bound, and when the last one closes the session's datagram counts are logged. Those counts include
// truncated receives (WSAEMSGSIZE) and the largest datagram, which tests whether full turns exceed the
// provider's receive buffers.
//
// standard.snp resolves wsock32 by ordinal through GetProcAddress at init; battle.snp imports it statically.
// Local only: nothing on the wire changes.

#include <winsock2.h>
#include <windows.h>
#include <iphlpapi.h>

#include <algorithm>
#include <atomic>
#include <cstring>

#include "core/config.h"
#include "core/log.h"
#include "core/patch.h"
#include "features/features.h"

namespace scpatch::features {
namespace {

// wsock32.dll export ordinals the providers resolve.
constexpr WORD kOrdBind = 2;
constexpr WORD kOrdCloseSocket = 3;
constexpr WORD kOrdRecvFrom = 17;
constexpr WORD kOrdSendTo = 20;

using BindFn = int(WINAPI*)(SOCKET, const sockaddr*, int);
using CloseSocketFn = int(WINAPI*)(SOCKET);
using RecvFromFn = int(WINAPI*)(SOCKET, char*, int, int, sockaddr*, int*);
using SendToFn = int(WINAPI*)(SOCKET, const char*, int, int, const sockaddr*, int);
using LoadLibraryAFn = HMODULE(WINAPI*)(LPCSTR);

struct Real {
  HMODULE wsock32;
  BindFn bind;
  CloseSocketFn closeSocket;
  RecvFromFn recvFrom;
  SendToFn sendTo;
};
Real g_real;
LoadLibraryAFn g_prevLoadLibraryA;  // the hook installed before ours (renderer) or kernel32's
int g_bufferBytes;                  // 0 = keep the Windows defaults

// Sockets tuned this session. Bind and close are rare, so a lock is fine; the hot paths only touch atomics.
SRWLOCK g_socketsLock = SRWLOCK_INIT;
SOCKET g_sockets[8];
int g_socketCount;
MIB_UDPSTATS g_udpAtStart;
bool g_haveUdpAtStart;

struct Stats {
  std::atomic<uint32_t> sent, sendErrors, received, truncated, recvErrors;
  std::atomic<uint64_t> bytesSent, bytesReceived;
  std::atomic<int> maxSent, maxReceived;
};
Stats g_stats;

void AtomicMax(std::atomic<int>& target, int value) {
  int current = target.load(std::memory_order_relaxed);
  while (value > current && !target.compare_exchange_weak(current, value, std::memory_order_relaxed)) {
  }
}

void ResetStats() {
  g_stats.sent = g_stats.sendErrors = g_stats.received = g_stats.truncated = g_stats.recvErrors = 0;
  g_stats.bytesSent = g_stats.bytesReceived = 0;
  g_stats.maxSent = g_stats.maxReceived = 0;
}

int GetIntOption(SOCKET s, int name) {
  int value = 0;
  int len = sizeof(value);
  return getsockopt(s, SOL_SOCKET, name, reinterpret_cast<char*>(&value), &len) == 0 ? value : -1;
}

void Tune(SOCKET s) {
  if (GetIntOption(s, SO_TYPE) != SOCK_DGRAM) return;

  sockaddr_in local{};
  int localLen = sizeof(local);
  unsigned port = getsockname(s, reinterpret_cast<sockaddr*>(&local), &localLen) == 0 ? ntohs(local.sin_port) : 0;
  int oldRecv = GetIntOption(s, SO_RCVBUF);
  int oldSend = GetIntOption(s, SO_SNDBUF);
  if (g_bufferBytes > 0) {
    // Never shrink a buffer Windows already made larger.
    int recvBytes = std::max(g_bufferBytes, oldRecv);
    int sendBytes = std::max(g_bufferBytes, oldSend);
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&recvBytes), sizeof(recvBytes));
    setsockopt(s, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&sendBytes), sizeof(sendBytes));
  }
  log::Info("network: UDP socket on port %u: receive buffer %d -> %d bytes, send buffer %d -> %d bytes", port,
            oldRecv, GetIntOption(s, SO_RCVBUF), oldSend, GetIntOption(s, SO_SNDBUF));

  AcquireSRWLockExclusive(&g_socketsLock);
  if (g_socketCount == 0) {
    ResetStats();
    g_haveUdpAtStart = GetUdpStatistics(&g_udpAtStart) == NO_ERROR;
  }
  if (g_socketCount < static_cast<int>(ARRAYSIZE(g_sockets))) g_sockets[g_socketCount++] = s;
  ReleaseSRWLockExclusive(&g_socketsLock);
}

void LogSession() {
  log::Info("network: session sent %u datagram(s) (%llu bytes, largest %d, %u failed); received %u (%llu bytes, "
            "largest %d), %u truncated, %u other receive error(s)",
            g_stats.sent.load(), g_stats.bytesSent.load(), g_stats.maxSent.load(), g_stats.sendErrors.load(),
            g_stats.received.load(), g_stats.bytesReceived.load(), g_stats.maxReceived.load(),
            g_stats.truncated.load(), g_stats.recvErrors.load());
  MIB_UDPSTATS now;
  if (g_haveUdpAtStart && GetUdpStatistics(&now) == NO_ERROR) {
    // System-wide counters: other programs' UDP traffic is included, but receive errors are rare otherwise.
    log::Info("network: system UDP during the session: %lu datagram(s) received, %lu receive error(s) "
              "(buffer overflows are counted here)",
              now.dwInDatagrams - g_udpAtStart.dwInDatagrams, now.dwInErrors - g_udpAtStart.dwInErrors);
  }
}

int WINAPI Hook_bind(SOCKET s, const sockaddr* address, int length) {
  int result = g_real.bind(s, address, length);
  if (result == 0) Tune(s);
  return result;
}

int WINAPI Hook_closesocket(SOCKET s) {
  bool last = false;
  AcquireSRWLockExclusive(&g_socketsLock);
  for (int i = 0; i < g_socketCount; ++i) {
    if (g_sockets[i] != s) continue;
    g_sockets[i] = g_sockets[--g_socketCount];
    last = g_socketCount == 0;
    break;
  }
  ReleaseSRWLockExclusive(&g_socketsLock);
  if (last) LogSession();
  return g_real.closeSocket(s);
}

int WINAPI Hook_sendto(SOCKET s, const char* buffer, int length, int flags, const sockaddr* to, int toLength) {
  int result = g_real.sendTo(s, buffer, length, flags, to, toLength);
  if (result == SOCKET_ERROR) {
    int error = WSAGetLastError();
    if (g_stats.sendErrors++ < 5) log::Warn("network: sendto of %d bytes failed (error %d)", length, error);
    WSASetLastError(error);
  } else {
    g_stats.sent.fetch_add(1, std::memory_order_relaxed);
    g_stats.bytesSent.fetch_add(result, std::memory_order_relaxed);
    AtomicMax(g_stats.maxSent, length);
  }
  return result;
}

int WINAPI Hook_recvfrom(SOCKET s, char* buffer, int length, int flags, sockaddr* from, int* fromLength) {
  int result = g_real.recvFrom(s, buffer, length, flags, from, fromLength);
  if (result == SOCKET_ERROR) {
    int error = WSAGetLastError();
    if (error == WSAEMSGSIZE) {
      if (g_stats.truncated++ < 5) log::Warn("network: datagram larger than the %d-byte receive buffer", length);
    } else if (error != WSAEINTR && error != WSAENOTSOCK && error != WSAECONNRESET) {
      // Closing the socket ends the provider's blocking receive with one of the excluded errors.
      g_stats.recvErrors.fetch_add(1, std::memory_order_relaxed);
    }
    WSASetLastError(error);
  } else {
    g_stats.received.fetch_add(1, std::memory_order_relaxed);
    g_stats.bytesReceived.fetch_add(result, std::memory_order_relaxed);
    AtomicMax(g_stats.maxReceived, result);
  }
  return result;
}

// The wrapper for a wsock32 export, by ordinal or name; null if it is not one we wrap.
FARPROC WrapperFor(LPCSTR name) {
  struct Entry {
    WORD ordinal;
    const char* name;
    void* hook;
  };
  static const Entry entries[] = {
      {kOrdBind, "bind", reinterpret_cast<void*>(&Hook_bind)},
      {kOrdCloseSocket, "closesocket", reinterpret_cast<void*>(&Hook_closesocket)},
      {kOrdRecvFrom, "recvfrom", reinterpret_cast<void*>(&Hook_recvfrom)},
      {kOrdSendTo, "sendto", reinterpret_cast<void*>(&Hook_sendto)},
  };
  for (const Entry& e : entries) {
    if (IS_INTRESOURCE(name) ? LOWORD(reinterpret_cast<uintptr_t>(name)) == e.ordinal : std::strcmp(name, e.name) == 0) {
      return reinterpret_cast<FARPROC>(e.hook);
    }
  }
  return nullptr;
}

FARPROC WINAPI Hook_GetProcAddress(HMODULE module, LPCSTR name) {
  FARPROC proc = GetProcAddress(module, name);
  if (!proc || module != g_real.wsock32) return proc;
  FARPROC wrapper = WrapperFor(name);
  return wrapper ? wrapper : proc;
}

bool ResolveReal() {
  if (g_real.wsock32) return true;
  HMODULE ws = LoadLibraryW(L"wsock32.dll");
  if (!ws) return false;
  g_real.bind = reinterpret_cast<BindFn>(GetProcAddress(ws, MAKEINTRESOURCEA(kOrdBind)));
  g_real.closeSocket = reinterpret_cast<CloseSocketFn>(GetProcAddress(ws, MAKEINTRESOURCEA(kOrdCloseSocket)));
  g_real.recvFrom = reinterpret_cast<RecvFromFn>(GetProcAddress(ws, MAKEINTRESOURCEA(kOrdRecvFrom)));
  g_real.sendTo = reinterpret_cast<SendToFn>(GetProcAddress(ws, MAKEINTRESOURCEA(kOrdSendTo)));
  if (!g_real.bind || !g_real.closeSocket || !g_real.recvFrom || !g_real.sendTo) return false;
  g_real.wsock32 = ws;
  return true;
}

bool IsProviderName(const char* name) {
  size_t n = name ? std::strlen(name) : 0;
  return n > 4 && _stricmp(name + n - 4, ".snp") == 0;
}

// Storm frees providers after enumerating them and on leaving multiplayer, and a reload gets a fresh IAT, so
// this runs on every load. Rewriting an entry that already points at our wrapper is harmless.
void HookProvider(HMODULE module, const char* name) {
  if (!module || !ResolveReal()) return;
  int hooked = patch::HookImport(module, "KERNEL32.dll", "GetProcAddress", reinterpret_cast<void*>(&Hook_GetProcAddress),
                                 nullptr);
  const struct {
    const char* name;
    WORD ordinal;
  } imports[] = {{"bind", kOrdBind}, {"closesocket", kOrdCloseSocket}, {"recvfrom", kOrdRecvFrom}, {"sendto", kOrdSendTo}};
  for (const auto& i : imports) {
    void* hook = reinterpret_cast<void*>(WrapperFor(i.name));
    hooked += patch::HookImport(module, "WSOCK32.dll", i.name, hook, nullptr);
    hooked += patch::HookImport(module, "WSOCK32.dll", nullptr, hook, nullptr, i.ordinal);
  }
  log::Info("network: hooked %d socket import(s) in %s", hooked, name);
}

HMODULE WINAPI Hook_LoadLibraryA(LPCSTR name) {
  HMODULE module = g_prevLoadLibraryA(name);
  if (module && IsProviderName(name)) HookProvider(module, name);
  return module;
}

}  // namespace

void InstallNetwork() {
  if (!config::GetBool(L"Network", L"Enabled", true)) {
    log::Info("network: disabled in config");
    return;
  }
  int kb = std::clamp(config::GetInt(L"Network", L"SocketBufferKB", 256), 0, 8192);
  g_bufferBytes = kb * 1024;

  HMODULE storm = GetModuleHandleW(L"storm.dll");
  void* previous = nullptr;
  if (!storm || !patch::HookImport(storm, "KERNEL32.dll", "LoadLibraryA", reinterpret_cast<void*>(&Hook_LoadLibraryA),
                                   &previous)) {
    log::Warn("network: could not hook storm.dll's LoadLibraryA; not installed");
    return;
  }
  // Chains to the renderer's hook when it is installed, which also hooks the providers.
  g_prevLoadLibraryA = reinterpret_cast<LoadLibraryAFn>(previous);
  for (const char* name : {"battle.snp", "standard.snp"}) HookProvider(GetModuleHandleA(name), name);
  log::Info("network: installed (socket buffers %d KB%s)", kb, kb ? "" : ", Windows defaults");
}

}  // namespace scpatch::features
