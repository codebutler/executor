// mactcp_bridge — Executor's MacTCP `.IPP` networking veneer over the pc
// host-socket seam (issue #711, the second consumer of #703's `hostConnect`).
//
// Executor reimplements the Mac Toolbox from scratch and ships NO Apple drivers,
// so — unlike Basilisk II / SheepShaver / Mini vMac, which shim networking at
// the Ethernet-frame layer and let the guest's REAL Apple MacTCP/OT do TCP —
// there is no MacTCP inside Executor to hand frames to. This file IS a MacTCP
// reimplementation: a Device Manager native driver named ".IPP" that answers the
// classic MacTCP client csCodes, plus a `'dnrp'` DNR code resource, translating
// each call into the pc host-socket seam over a futex ring in the shared wasm
// heap. A browser-main servicer (js/apps/executor/mactcp-servicer.ts) drains the
// ring and drives js/vnet `hostConnect` — so a 1991 Mac program's socket comes
// out of pc's own IP (the gateway, 10.0.2.1), exactly like Wine's winsock and a
// pure-JS app. No per-runtime DHCP lease or virtual NIC.
//
// The model (recap, issue #711): apps in Executor are pc apps, not a guest host.
// LAN-local destinations reach other guests over the bridge; everything else
// egresses via pc's single shared Wisp uplink. "What's my IP" (ipctlGetAddr)
// answers with pc's gateway address.
//
// Structure:
//   • a futex ring (`_pc_mactcp_ring()`), SAME layout + transport as the /mac
//     sync-VFS ring (pc_vfs_bridge.cpp), but carrying the net SOCK_* / RESOLVE /
//     LOCAL_ADDR ops (js/kernel/wasi/futex.ts). A SEPARATE ring so a blocking
//     TCPRcv parked on the net futex never stalls a /mac file call.
//   • a `.IPP` Device Manager native driver (RegisterDriver, like serial.cpp):
//     open/prime/ctl/status/close. TCP + UDP client csCodes are dispatched in
//     ctl/status.
//   • a `'dnrp'` code resource so the standard dnr.c glue (OpenResolver /
//     StrToAddr / AddrToStr / AddrToName) resolves names through the gateway.
//   • deferred completion for a blocking async TCPRcv: a native VBL task polls
//     pending receives non-blocking and fires the guest completion routine when
//     data arrives — the one genuinely new piece of Device-Manager plumbing the
//     ticket calls out (dispatch is synchronous otherwise).
//
// The ring layout + op numbers MUST stay in sync with js/kernel/wasi/futex.ts.
// The MacTCP/DNR ABI (csCodes, param-block offsets, dnr calling convention) is
// sourced from Apple's MacTCP.h / AddressXlation.h / dnr.c (see #711 notes).

#include <base/common.h>
#include <DeviceMgr.h>
#include <FileMgr.h>
#include <MemoryMgr.h>
#include <OSUtil.h>
#include <ResourceMgr.h>
#include <VRetraceMgr.h>

#include <rsys/device.h>
#include <base/cpu.h>
#include <base/byteswap.h>
#include <base/functions.impl.h>
#include <base/traps.impl.h>

#include <cstring>
#include <cstdio>
#include <cstdlib>

#include <emscripten.h>
#include <emscripten/threading.h>

using namespace Executor;

// ── ring layout — MUST stay in sync with js/kernel/wasi/futex.ts ─────────────
namespace
{
enum {
  I_TURN = 0,
  I_OP = 1,
  I_STATUS = 2,
  I_REQ_LEN = 3,
  I_RES_LEN = 4,
  I_ARG_OFF = 5,
  I_ARG_LEN = 6,
  I_WROTE = 7,
  I_SIGNAL = 8,
  HEADER_SLOTS = 9,
};
const int DATA_OFFSET = HEADER_SLOTS * 4;
// Net payloads are small (one TCP segment at a time); 64 KiB is ample and the
// host servicer maps exactly DATA_OFFSET + this. MUST match the byteLength the
// host passes (js/apps/executor/executor-window.ts).
const int PC_MACTCP_DATA_BYTES = 64 * 1024;
const int RING_BYTES = DATA_OFFSET + PC_MACTCP_DATA_BYTES;

enum { TURN_WORKER = 0, TURN_SERVICER = 1 };

// OP.* — MUST stay in sync with js/kernel/wasi/futex.ts
enum {
  OP_SOCK_CONNECT = 21,
  OP_SOCK_SEND = 22,
  OP_SOCK_RECV = 23,
  OP_SOCK_CLOSE = 24,
  OP_SOCK_POLL = 25,
  OP_RESOLVE = 26,
  OP_LOCAL_ADDR = 27,
};

// POLL_* bits (SOCK_POLL response `wrote`) — js/kernel/wasi/futex.ts.
const int POLL_READABLE = 1;
const int POLL_CLOSED = 4;

// NET_ERR.* — the NEGATIVE of a SOCK_* `wrote` result (js/kernel/wasi/futex.ts).
enum {
  NET_EAGAIN = 1,
  NET_ECONNRESET = 2,
  NET_ECONNREFUSED = 3,
  NET_ENOTCONN = 4,
  NET_ENOSYS = 5,
  NET_EINVAL = 6,
};

// The shared ring, in the wasm data segment (shared under -pthread). The host
// maps it read/write at _pc_mactcp_ring(). 16-aligned so the host's Int32Array
// view is 4-aligned. The __atomic_* builtins carry the ordering.
alignas(16) int32_t g_ring[RING_BYTES / 4];

inline int32_t* rctrl() { return g_ring; }
inline unsigned char* rdata() {
  return reinterpret_cast<unsigned char*>(g_ring) + DATA_OFFSET;
}
} // namespace

extern "C" EMSCRIPTEN_KEEPALIVE uintptr_t pc_mactcp_ring(void) {
  return reinterpret_cast<uintptr_t>(g_ring);
}

namespace
{
// ── debug logging (instrument-first, per CLAUDE.md) ──────────────────────────
// PC_MACTCP_DEBUG=1 → every MacTCP call the driver services narrates to stderr
// with an [mactcp] prefix (localStorage["cb.executor.netDebug"]="1", wired in
// executor-window.ts). Off by default.
int dbg_on() {
  static int v = -1;
  if (v < 0) {
    const char* e = getenv("PC_MACTCP_DEBUG");
    v = (e && e[0] == '1') ? 1 : 0;
  }
  return v;
}
#define DBG(...) do { if (dbg_on()) { fprintf(stderr, "[mactcp] " __VA_ARGS__); fputc('\n', stderr); fflush(stderr); } } while (0)

// ── one request/response round-trip to the browser-main net servicer ─────────
// Fills the ring header, hands off (release store + notify), blocks this pthread
// in memory.atomic.wait32 until the servicer flips the turn back (acquire load),
// then reads the response header out. Data in/out ride rdata(). Returns I_WROTE
// (the servicer's per-op result: a count, a bitmask, or a negative NET_ERR).
// A /mac-style call from the MAIN thread would deadlock (the servicer lives
// there); the driver only ever runs on the emulator pthread, but guard anyway.
int net_round(int op, int fd, int arg,
              const void* data_in, int data_in_len,
              int wrote_in,
              void* out, int out_cap, int* out_res_len) {
  if (emscripten_is_main_runtime_thread()) {
    static int warned = 0;
    if (!warned) { warned = 1; fprintf(stderr, "[mactcp] WARN op=%d on MAIN thread — failing\n", op); fflush(stderr); }
    return -NET_EINVAL;
  }
  if (data_in_len > PC_MACTCP_DATA_BYTES) return -NET_EINVAL;
  unsigned char* d = rdata();
  if (data_in && data_in_len) memcpy(d, data_in, data_in_len);

  int32_t* c = rctrl();
  c[I_OP] = op;
  c[I_ARG_OFF] = fd;
  c[I_ARG_LEN] = arg;
  c[I_REQ_LEN] = data_in_len;
  c[I_WROTE] = wrote_in; // recv nonblock flag rides I_WROTE on the request

  __atomic_store_n(&c[I_TURN], TURN_SERVICER, __ATOMIC_SEQ_CST);
  __builtin_wasm_memory_atomic_notify((int*)&c[I_TURN], 1);
  while (__atomic_load_n(&c[I_TURN], __ATOMIC_SEQ_CST) == TURN_SERVICER)
    __builtin_wasm_memory_atomic_wait32((int*)&c[I_TURN], TURN_SERVICER, -1);

  int res_len = c[I_RES_LEN];
  if (out && res_len > 0) {
    int n = res_len < out_cap ? res_len : out_cap;
    memcpy(out, d, n);
  }
  if (out_res_len) *out_res_len = res_len;
  return c[I_WROTE];
}
} // namespace

// ── MacTCP / DNR ABI constants (Apple MacTCP.h / AddressXlation.h) ───────────
namespace
{
// TCP client csCodes
enum {
  csTCPCreate = 30,
  csTCPPassiveOpen = 31,
  csTCPActiveOpen = 32,
  csTCPSend = 34,
  csTCPNoCopyRcv = 35,
  csTCPRcvBfrReturn = 36,
  csTCPRcv = 37,
  csTCPClose = 38,
  csTCPAbort = 39,
  csTCPStatus = 40,
  csTCPRelease = 42,
  csTCPGlobalInfo = 43,
};
// UDP client csCodes
enum {
  csUDPCreate = 20,
  csUDPRead = 21,
  csUDPBfrReturn = 22,
  csUDPWrite = 23,
  csUDPRelease = 24,
  csUDPMaxMTUSize = 25,
};
// IP control csCode (GetMyIPAddr → PBControl)
enum { csIpctlGetAddr = 15 };

// MacTCP OSErr return codes (range -23000..)
enum {
  meNoErr = 0,
  meInProgress = 1,
  meConnectionClosing = -23005,
  meConnectionExists = -23007,
  meConnectionDoesntExist = -23008,
  meInsufficientResources = -23009,
  meInvalidStreamPtr = -23010,
  meConnectionTerminated = -23012,
  meCommandTimeout = -23016,
  meOpenFailed = -23015,
};
enum { meParamErr = -50, meControlErr = -17, meOpenErr = -23 };

// TCPiopb / UDPiopb field byte offsets (classic mac68k alignment, big-endian).
// The header through csCode is IDENTICAL to a standard cntrlParam ParamBlockRec,
// so those fields read via the typed ParmBlkPtr; tcpStream + the csParam union
// are read by offset here.
enum {
  off_tcpStream = 28, // StreamPtr (unsigned long)
  off_csParam = 32,   // union base
};
// TCPCreatePB @csParam (MacTCP.h, mac68k)
enum {
  off_create_rcvBuff = 32,
  off_create_rcvBuffLen = 36,
  off_create_notifyProc = 40, // TCPNotifyUPP (ASR)
  off_create_userDataPtr = 44,
};
// TCPOpenPB @csParam — 4 leading SInt8s, then hosts/ports
enum {
  off_open_remoteHost = 36, // ip_addr
  off_open_remotePort = 40, // tcp_port (u16)
  off_open_localHost = 42,
  off_open_localPort = 46,
};
// TCPSendPB @csParam
enum { off_send_wdsPtr = 38, off_send_sendLength = 46 };
// TCPReceivePB @csParam
enum {
  off_recv_commandTimeout = 32, // SInt8
  off_recv_rcvBuff = 36,        // Ptr
  off_recv_rcvBuffLen = 40,     // u16 (in/out)
};
// TCPStatusPB @csParam — mac68k (longs 2-byte aligned). Earlier veneer had
// these off-by-2 and wrote amtUnreadData over securityLevelPtr — Flynn's
// TCPStatus after Connect then dereferenced garbage → wasm OOB.
enum {
  off_status_remoteHost = 38,       // ip_addr (after ulpTimeout* + unused long)
  off_status_remotePort = 42,       // tcp_port
  off_status_localHost = 44,        // ip_addr
  off_status_localPort = 48,        // tcp_port
  off_status_connectionState = 52,  // SInt8
  off_status_amtUnreadData = 60,    // u16
  off_status_securityLevelPtr = 62, // Ptr (OUT — must be nil if unused)
  off_status_connStatPtr = 94,      // TCPConnectionStats* (IN; leave nil)
};
// ASR event codes (MacTCP.h)
enum {
  evTCPClosing = 1,
  evTCPULPTimeout = 2,
  evTCPTerminate = 3,
  evTCPDataArrival = 4,
  evTCPUrgent = 5,
  evTCPICMPReceived = 6,
};
// GetAddrParamBlock: ourAddress @28 (where tcpStream sits for TCP), netMask @32.
enum { off_getaddr_ourAddress = 28, off_getaddr_ourNetMask = 32 };

// UDPCreatePB @csParam
enum { off_udpc_rcvBuff = 32, off_udpc_rcvBuffLen = 36, off_udpc_localPort = 42 };
// UDPSendPB @csParam
enum { off_udps_remoteHost = 34, off_udps_remotePort = 38, off_udps_wdsPtr = 40, off_udps_sendLength = 48 };
// UDPReceivePB @csParam
enum {
  off_udpr_remoteHost = 34,
  off_udpr_remotePort = 38,
  off_udpr_rcvBuff = 40,
  off_udpr_rcvBuffLen = 44, // u16 in/out
};

// wdsEntry / rdsEntry: { u16 length; Ptr ptr; } — a length-0 terminator ends the
// list. Used by TCPSend/UDPWrite (write data structure).
} // namespace

// ── guest-memory accessors (big-endian; GUEST<> auto-swaps on access) ────────
namespace
{
inline uint16_t rdW(void* base, int off) {
  return (uint16_t)*reinterpret_cast<GUEST<uint16_t>*>((uint8_t*)base + off);
}
inline uint32_t rdL(void* base, int off) {
  return (uint32_t)*reinterpret_cast<GUEST<uint32_t>*>((uint8_t*)base + off);
}
inline uint8_t rdB(void* base, int off) { return *((uint8_t*)base + off); }
inline void wrW(void* base, int off, uint16_t v) {
  *reinterpret_cast<GUEST<uint16_t>*>((uint8_t*)base + off) = v;
}
inline void wrL(void* base, int off, uint32_t v) {
  *reinterpret_cast<GUEST<uint32_t>*>((uint8_t*)base + off) = v;
}
inline void wrB(void* base, int off, uint8_t v) { *((uint8_t*)base + off) = v; }

// A guest 32-bit address → a host pointer into guest memory (nil-safe).
inline void* guestPtr(uint32_t addr) { return addr ? SYN68K_TO_US(addr) : nullptr; }

// Format a big-endian ip_addr as dotted quad into buf (needs >=16 bytes).
void ipToDotted(uint32_t ip, char* buf) {
  snprintf(buf, 16, "%u.%u.%u.%u", (ip >> 24) & 0xFF, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF);
}
// Parse dotted quad → big-endian ip_addr (0 on malformed).
uint32_t dottedToIp(const char* s) {
  unsigned a = 0, b = 0, c = 0, d = 0;
  if (sscanf(s, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return 0;
  return ((a & 0xFF) << 24) | ((b & 0xFF) << 16) | ((c & 0xFF) << 8) | (d & 0xFF);
}
} // namespace

// ── stream table (MacTCP StreamPtr ↔ servicer fd) ────────────────────────────
// A StreamPtr the guest holds is one of our fds. Net-bridge keys its socket map
// by whatever fd we pass; we choose a high base (like cb_net.c's SOCK_FD_BASE)
// so the numbers are recognizable and never zero (a nil StreamPtr is invalid).
namespace
{
const int MACTCP_FD_BASE = 600;
const int MAX_STREAMS = 64;
struct Stream {
  bool used;
  bool udp;
  bool connected; // TCP: an ActiveOpen succeeded
  uint32_t rcvBuff; // guest receive buffer (from Create) — unused by us, kept for Status
  uint32_t rcvBuffLen;
  uint32_t notifyProc;  // guest TCPNotifyUPP (ASR); 0 = none
  uint32_t userDataPtr; // ASR userDataPtr from TCPCreate
  bool wasReadable;     // edge-detect for TCPDataArrival ASR
  bool wasClosed;       // edge-detect for TCPTerminate ASR
  uint32_t localHost;  // gateway IP after ActiveOpen (Status)
  uint16_t localPort;  // UDP bound / TCP local (informational)
  uint32_t remoteHost; // dotted→BE, filled on ActiveOpen (Status)
  uint16_t remotePort;
};
Stream g_streams[MAX_STREAMS];

int allocStream(bool udp) {
  for (int i = 0; i < MAX_STREAMS; i++) {
    if (!g_streams[i].used) {
      memset(&g_streams[i], 0, sizeof(Stream));
      g_streams[i].used = true;
      g_streams[i].udp = udp;
      return MACTCP_FD_BASE + i;
    }
  }
  return -1;
}
Stream* getStream(uint32_t sp) {
  int i = (int)sp - MACTCP_FD_BASE;
  if (i < 0 || i >= MAX_STREAMS) return nullptr;
  return g_streams[i].used ? &g_streams[i] : nullptr;
}
void freeStream(uint32_t sp) {
  Stream* s = getStream(sp);
  if (s) s->used = false;
}

// Fire the guest ASR (TCPNotifyProc). Pascal stack: args left-to-right, callee
// cleans. Signature: pascal void (StreamPtr, uint16 event, Ptr userData,
// uint16 terminReason, ICMPReport* icmpMsg).
void callAsr(uint32_t streamPtr, Stream* s, uint16_t eventCode, uint16_t terminReason) {
  if (!s || !s->notifyProc) return;
  DBG("ASR stream=%u event=%u", streamPtr, eventCode);
  LONGINT saved0 = EM_D0, saved1 = EM_D1, saved2 = EM_D2;
  LONGINT savea0 = EM_A0, savea1 = EM_A1;
  PUSHUL(streamPtr);
  PUSHUW(eventCode);
  PUSHUL(s->userDataPtr);
  PUSHUW(terminReason);
  PUSHUL(0); // icmpMsg = nil
  execute68K((syn68k_addr_t)s->notifyProc);
  EM_D0 = saved0;
  EM_D1 = saved1;
  EM_D2 = saved2;
  EM_A0 = savea0;
  EM_A1 = savea1;
}
} // namespace

// ── deferred completion for a blocking async TCPRcv ──────────────────────────
// Dispatch is synchronous, so a synchronous TCPRcv simply blocks on the futex
// (correct — the whole Mac blocks, as it does under real MacTCP synchronous
// mode). But an ASYNC TCPRcv (asyncTrpBit + ioCompletion) must return control
// immediately and fire the completion routine later when data arrives. We first
// try a non-blocking recv; if data is buffered we complete inline, else the pb
// is parked here and a native VBL task polls it non-blocking each tick and
// completes it when readable — the one new bit of Device-Manager plumbing #711
// calls out (the Time Manager/VBL wasn't wired to the Device Manager before).
namespace
{
struct Pending {
  bool used;
  uint32_t pb;      // guest TCPiopb address
  uint32_t completion; // guest completion routine (0 = none, poll ioResult)
  int fd;
  uint32_t rcvBuff;
  uint16_t rcvBuffLen;
  bool async;
};
const int MAX_PENDING = 32;
Pending g_pending[MAX_PENDING];

// Fire the guest ioCompletion routine (C-convention `void comp(TCPiopb*)`, arg
// pushed by us) — same execute68K trampoline serial.cpp's DOCOMPLETION uses,
// but here for the async recv we complete from the VBL, not inline.
void callCompletion(uint32_t pb, uint32_t comp, int16_t err) {
  wrW(guestPtr(pb), 16, (uint16_t)err); // ioResult @16
  if (!comp) return;
  EM_A0 = pb;
  EM_A1 = comp;
  EM_D0 = (uint16_t)err;
  execute68K((syn68k_addr_t)comp);
}

// Complete a pending recv: pull up to rcvBuffLen bytes (non-blocking) into the
// guest rcvBuff, set rcvBuffLen to the actual count, set ioResult, fire the
// completion. Returns true if it finished (ready), false if still waiting.
bool tryCompletePending(Pending& p) {
  int mask = net_round(OP_SOCK_POLL, p.fd, 0, nullptr, 0, 0, nullptr, 0, nullptr);
  if (mask == 0) return false; // not readable yet, not closed
  if (mask & POLL_READABLE) {
    int want = p.rcvBuffLen;
    if (want > PC_MACTCP_DATA_BYTES) want = PC_MACTCP_DATA_BYTES;
    int res_len = 0;
    int n = net_round(OP_SOCK_RECV, p.fd, want, nullptr, 0, /*nonblock*/ 1,
                      guestPtr(p.rcvBuff), p.rcvBuffLen, &res_len);
    if (n == -NET_EAGAIN) return false; // raced; try next tick
    if (n <= 0) {
      // 0 = graceful EOF, negative = reset — MacTCP reports connectionClosing.
      wrW(guestPtr(p.pb), off_recv_rcvBuffLen, 0);
      callCompletion(p.pb, p.completion, meConnectionClosing);
      return true;
    }
    wrW(guestPtr(p.pb), off_recv_rcvBuffLen, (uint16_t)res_len);
    if (Stream* s = getStream((uint32_t)p.fd)) s->wasReadable = false;
    callCompletion(p.pb, p.completion, meNoErr);
    return true;
  }
  // closed/err with nothing readable → connection closing.
  wrW(guestPtr(p.pb), off_recv_rcvBuffLen, 0);
  callCompletion(p.pb, p.completion, meConnectionClosing);
  return true;
}

bool parkPending(uint32_t pb, uint32_t comp, int fd, uint32_t rcvBuff, uint16_t rcvBuffLen) {
  for (int i = 0; i < MAX_PENDING; i++) {
    if (!g_pending[i].used) {
      g_pending[i] = { true, pb, comp, fd, rcvBuff, rcvBuffLen, true };
      return true;
    }
  }
  return false;
}
bool anyPending() {
  for (int i = 0; i < MAX_PENDING; i++) if (g_pending[i].used) return true;
  return false;
}
} // namespace

// ── the VBL poll task (native, via a callback_install'd magic address) ───────
// Runs on the emulator pthread at VBL time (js VBL runs execute68K(vblAddr));
// a magic callback address makes that call land here. Polls every parked async
// recv non-blocking and re-arms itself. EM_A0 is the VBLTask* (Retrace.h),
// which we re-arm by resetting vblCount.
namespace
{
const int VBL_INTERVAL = 6; // ~10 Hz at 60 VBLs/sec — cheap; recv latency is fine
VBLTask g_vblTask;
bool g_vblInstalled = false;

syn68k_addr_t vblPoll(syn68k_addr_t /*addr*/, void* /*arg*/) {
  // Drain parked async TCPRcvs first (no ASR / no notifyProc required).
  for (int i = 0; i < MAX_PENDING; i++) {
    if (g_pending[i].used && tryCompletePending(g_pending[i]))
      g_pending[i].used = false;
  }
  // ASR edge-detect: apps that register TCPCreate.notifyProc wait for
  // TCPDataArrival before posting TCPRcv. Flynn does NOT (notify=0) — it
  // polls TCPStatus.amtUnreadData from its event loop instead.
  for (int i = 0; i < MAX_STREAMS; i++) {
    Stream* s = &g_streams[i];
    if (!s->used || s->udp || !s->connected || !s->notifyProc) continue;
    uint32_t sp = (uint32_t)(MACTCP_FD_BASE + i);
    // Validate the UPP before touching the net ring — a corrupt pointer here
    // used to take down the whole renderer (wasm OOB via execute68K).
    if (s->notifyProc < 0x1000 || s->notifyProc > 0x00FFFFFF) {
      DBG("ASR skip bad notifyProc=%u stream=%u", s->notifyProc, sp);
      s->notifyProc = 0;
      continue;
    }
    int mask = net_round(OP_SOCK_POLL, (int)sp, 0, nullptr, 0, 0, nullptr, 0, nullptr);
    bool readable = (mask & POLL_READABLE) != 0;
    bool closed = (mask & POLL_CLOSED) != 0;
    if (readable && !s->wasReadable)
      callAsr(sp, s, evTCPDataArrival, 0);
    if (closed && !s->wasClosed) {
      callAsr(sp, s, evTCPTerminate, 0);
      s->connected = false;
    }
    s->wasReadable = readable;
    s->wasClosed = closed;
  }
  // Re-arm: VInstall dequeues the task if vblCount is still 0 after return.
  g_vblTask.vblCount = VBL_INTERVAL;
  // VBL tasks are invoked via execute68K/CALL_EMULATOR, which pushes
  // MAGIC_EXIT_EMULATOR_ADDRESS — same as a JSR. Return with RTS semantics
  // (pop + continue), NOT MAGIC_RTE_ADDRESS. RTE expects an exception frame
  // that was never pushed; the resulting junk PC was the Flynn Connect OOB.
  return POPADDR();
}

void ensureVblInstalled() {
  if (g_vblInstalled) return;
  g_vblInstalled = true;
  memset(&g_vblTask, 0, sizeof(g_vblTask));
  g_vblTask.qType = vType;
  // vblAddr holds a GUEST pointer. callback_install returns a magic guest
  // address; store it as a host pointer (SYN68K_TO_US) so the GUEST<ProcPtr>
  // write translates back to exactly that guest address — the same idiom as
  // qIMV.cpp's default-search-proc install. The VBL handler then does
  // execute68K(vblAddr) and lands in vblPoll.
  syn68k_addr_t magic = callback_install(&vblPoll, nullptr);
  DBG("VBL install magic=%u", (unsigned)magic);
  g_vblTask.vblAddr = (ProcPtr)SYN68K_TO_US(magic);
  g_vblTask.vblCount = VBL_INTERVAL;
  g_vblTask.vblPhase = 0;
  OSErr verr = VInstall(&g_vblTask);
  DBG("VBL VInstall err=%d", (int)verr);
}
} // namespace

// ── csCode handlers ──────────────────────────────────────────────────────────
namespace
{
// TCPActiveOpen: connect fd → remoteHost:remotePort. Optimistic like the Wine
// winsock bridge — Wisp has no connect-ack, so a returned 0 means the stream
// opened; an unreachable peer surfaces as a reset on the first recv.
int16_t doActiveOpen(void* pb, Stream* s, int fd) {
  uint32_t host = rdL(pb, off_open_remoteHost);
  uint16_t port = rdW(pb, off_open_remotePort);
  char dotted[16];
  ipToDotted(host, dotted);
  DBG("ActiveOpen fd=%d %s:%u", fd, dotted, port);
  // Flynn has been observed to issue TCPActiveOpen twice on the same stream
  // after Connect; a second SOCK_CONNECT opened another TCP session and both
  // telnet IAC greetings coalesced in the recv buffer (got=18). Treat an
  // already-connected stream as success.
  if (s->connected && s->remoteHost == host && s->remotePort == port) {
    DBG("ActiveOpen already connected fd=%d — no-op", fd);
    return meNoErr;
  }
  int rc = net_round(OP_SOCK_CONNECT, fd, port, dotted, (int)strlen(dotted), 0, nullptr, 0, nullptr);
  if (rc == 0) {
    s->connected = true;
    s->remoteHost = host;
    s->remotePort = port;
    s->wasReadable = false;
    s->wasClosed = false;
    // Fill localHost with our gateway IP (best-effort; ignore failure).
    int res_len = 0;
    char local[32] = { 0 };
    if (net_round(OP_LOCAL_ADDR, 0, 0, nullptr, 0, 0, local, sizeof(local) - 1, &res_len) == 0 && res_len > 0) {
      local[res_len] = '\0';
      s->localHost = dottedToIp(local);
      wrL(pb, off_open_localHost, s->localHost);
    }
    // Ensure the VBL is ticking so ASRs fire when the greeting arrives.
    ensureVblInstalled();
    DBG("ActiveOpen OK fd=%d local=%u", fd, s->localHost);
    return meNoErr;
  }
  return meOpenFailed;
}

// TCPSend: gather the WDS fragments into one buffer and send. The WDS is an
// array of { u16 length; Ptr ptr } terminated by length==0.
int16_t doSend(void* pb, int fd) {
  uint32_t wdsAddr = rdL(pb, off_send_wdsPtr);
  if (!wdsAddr) return meParamErr;
  static unsigned char buf[PC_MACTCP_DATA_BYTES];
  int total = 0;
  void* wds = guestPtr(wdsAddr);
  for (int e = 0;; e++) {
    uint16_t len = rdW(wds, e * 6);
    if (len == 0) break;
    uint32_t ptr = rdL(wds, e * 6 + 2);
    void* src = guestPtr(ptr);
    if (!src || total + len > (int)sizeof(buf)) break;
    memcpy(buf + total, src, len);
    total += len;
  }
  DBG("Send fd=%d %d bytes", fd, total);
  int off = 0;
  while (off < total) {
    // SOCK_SEND reads the byte count from I_ARG_LEN (js/kernel/wasi/futex.ts),
    // so the count rides `arg` AND the data region carries the same bytes.
    int n = net_round(OP_SOCK_SEND, fd, total - off, buf + off, total - off, 0, nullptr, 0, nullptr);
    if (n <= 0) return meConnectionClosing;
    off += n;
  }
  return meNoErr;
}

// TCPRcv (synchronous): block until data or close, copy into rcvBuff, set the
// actual length. Called only for the synchronous path; async goes via doRcvAsync.
int16_t doRcvSync(void* pb, int fd) {
  uint16_t want = rdW(pb, off_recv_rcvBuffLen);
  uint32_t rcvBuff = rdL(pb, off_recv_rcvBuff);
  // Flynn has been observed to issue a follow-up TCPRcv with want==previous
  // got (e.g. 9) instead of amtUnreadData — after an IAC-sized read the PB's
  // rcvBuffLen stays 9 and the next blocking Rcv (banner still on hold, so
  // POLL_READABLE is false at entry) only pulls a `\r\n\r\nBusyB` crumb.
  // Flynn's read_buf is TCP_READ_BUFSIZ (4096); treat sub-64 wants as stale
  // and expand unconditionally so a later banner flush arrives in one chunk.
  if (want > 0 && want < 64) {
    DBG("Rcv(sync) fd=%d bump want %u → 4096 (stale-len workaround)", fd, want);
    want = 4096;
    wrW(pb, off_recv_rcvBuffLen, want);
  }
  int cap = want;
  if (cap > PC_MACTCP_DATA_BYTES) cap = PC_MACTCP_DATA_BYTES;
  int res_len = 0;
  int n = net_round(OP_SOCK_RECV, fd, cap, nullptr, 0, /*nonblock*/ 0,
                    guestPtr(rcvBuff), want, &res_len);
  DBG("Rcv(sync) fd=%d want=%u got=%d res_len=%d", fd, want, n, res_len);
  if (n <= 0) {
    wrW(pb, off_recv_rcvBuffLen, 0);
    return meConnectionClosing; // 0 EOF or negative reset
  }
  // Prefer the byte count we actually copied; I_WROTE and I_RES_LEN should
  // match, but if they diverge trust the memcpy length.
  if (res_len <= 0) res_len = n;
  wrW(pb, off_recv_rcvBuffLen, (uint16_t)res_len);
  // Re-arm ASR edge-detect so further arrivals notify again.
  if (Stream* s = getStream((uint32_t)fd)) s->wasReadable = false;
  return meNoErr;
}

int16_t doStatus(void* pb, Stream* s, int fd) {
  // Best-effort: report connection state and amount of unread data via a
  // non-blocking poll. 8 = ESTABLISHED, 0 = closed (MacTCP connectionState).
  // SOCK_POLL only returns a readability bit — not a byte count — so when
  // readable we advertise a generous amtUnreadData. Flynn (and typical
  // MacTCP clients) then TCPRcv up to that many bytes; the recv returns the
  // true count. Advertising 1 forced one-byte reads and starved telnet
  // negotiation / banner display.
  int mask = net_round(OP_SOCK_POLL, fd, 0, nullptr, 0, 0, nullptr, 0, nullptr);
  bool up = s->connected && !(mask & POLL_CLOSED);
  uint16_t unread = (mask & POLL_READABLE) ? 4096 : 0;
  wrL(pb, off_status_remoteHost, s->remoteHost);
  wrW(pb, off_status_remotePort, s->remotePort);
  wrL(pb, off_status_localHost, s->localHost);
  wrW(pb, off_status_localPort, s->localPort);
  wrB(pb, off_status_connectionState, up ? 8 : 0);
  wrW(pb, off_status_amtUnreadData, unread);
  // Outputs Flynn (and others) may copy wholesale — leave pointer fields nil so
  // a guest never dereferences leftover PB garbage (prior OOB vector).
  wrL(pb, off_status_securityLevelPtr, 0);
  wrL(pb, off_status_connStatPtr, 0);
  if (unread) DBG("TCPStatus fd=%d up=%d unread=%u mask=%d", fd, (int)up, unread, mask);
  return meNoErr;
}

// UDPWrite: send one datagram to remoteHost:remotePort (WDS-gathered payload).
int16_t doUdpWrite(void* pb, int fd) {
  uint32_t host = rdL(pb, off_udps_remoteHost);
  uint16_t port = rdW(pb, off_udps_remotePort);
  uint32_t wdsAddr = rdL(pb, off_udps_wdsPtr);
  if (!wdsAddr) return meParamErr;
  // A UDP "connect" per datagram (the seam is stream-oriented; for the initial
  // veneer a UDP send opens a fresh datagram stream to the destination).
  char dotted[16];
  ipToDotted(host, dotted);
  if (net_round(OP_SOCK_CONNECT, fd, port, dotted, (int)strlen(dotted), 0, nullptr, 0, nullptr) != 0)
    return meOpenFailed;
  static unsigned char buf[PC_MACTCP_DATA_BYTES];
  int total = 0;
  void* wds = guestPtr(wdsAddr);
  for (int e = 0;; e++) {
    uint16_t len = rdW(wds, e * 6);
    if (len == 0) break;
    void* src = guestPtr(rdL(wds, e * 6 + 2));
    if (!src || total + len > (int)sizeof(buf)) break;
    memcpy(buf + total, src, len);
    total += len;
  }
  int off = 0;
  while (off < total) {
    int n = net_round(OP_SOCK_SEND, fd, total - off, buf + off, total - off, 0, nullptr, 0, nullptr);
    if (n <= 0) return meConnectionClosing;
    off += n;
  }
  return meNoErr;
}

int16_t doUdpRead(void* pb, int fd) {
  uint16_t want = rdW(pb, off_udpr_rcvBuffLen);
  uint32_t rcvBuff = rdL(pb, off_udpr_rcvBuff);
  int cap = want;
  if (cap > PC_MACTCP_DATA_BYTES) cap = PC_MACTCP_DATA_BYTES;
  int res_len = 0;
  int n = net_round(OP_SOCK_RECV, fd, cap, nullptr, 0, 0, guestPtr(rcvBuff), want, &res_len);
  if (n <= 0) { wrW(pb, off_udpr_rcvBuffLen, 0); return meConnectionClosing; }
  wrW(pb, off_udpr_rcvBuffLen, (uint16_t)res_len);
  return meNoErr;
}

// ipctlGetAddr: "what's my IP" → pc's gateway address + a /24 netmask.
int16_t doGetAddr(void* pb) {
  int res_len = 0;
  char local[32] = { 0 };
  int rc = net_round(OP_LOCAL_ADDR, 0, 0, nullptr, 0, 0, local, sizeof(local) - 1, &res_len);
  if (rc != 0 || res_len <= 0) return meControlErr;
  local[res_len] = '\0';
  wrL(pb, off_getaddr_ourAddress, dottedToIp(local));
  wrL(pb, off_getaddr_ourNetMask, 0xFFFFFF00u); // /24 (10.0.2.0/24 gateway net)
  DBG("ipctlGetAddr -> %s", local);
  return meNoErr;
}
} // namespace

// ── the .IPP Device Manager native driver ────────────────────────────────────
// Forward-declared + REGISTER_FUNCTION_PTR'd at global scope (matching
// serial.cpp, and `using namespace Executor` at the top of the file), so the
// generated `mactcp_*` UPP symbols and the `C_mactcp_*` definitions below share
// one namespace.
static OSErr C_mactcp_open(ParmBlkPtr pbp, DCtlPtr dcp);
REGISTER_FUNCTION_PTR(mactcp_open, D0(A0, A1));
static OSErr C_mactcp_prime(ParmBlkPtr pbp, DCtlPtr dcp);
REGISTER_FUNCTION_PTR(mactcp_prime, D0(A0, A1));
static OSErr C_mactcp_ctl(ParmBlkPtr pbp, DCtlPtr dcp);
REGISTER_FUNCTION_PTR(mactcp_ctl, D0(A0, A1));
static OSErr C_mactcp_status(ParmBlkPtr pbp, DCtlPtr dcp);
REGISTER_FUNCTION_PTR(mactcp_status, D0(A0, A1));
static OSErr C_mactcp_close(ParmBlkPtr pbp, DCtlPtr dcp);
REGISTER_FUNCTION_PTR(mactcp_close, D0(A0, A1));

// Fire the completion routine inline (synchronous or fast async), exactly like
// serial.cpp's DOCOMPLETION: set ioResult; if async + ioCompletion, call it.
// Evaluate `err` exactly once — callers pass doSend()/doRcvSync()/… which
// perform side effects (SOCK_SEND, etc.). A naive `(pbp)->ioResult = (err);
// return (err);` would run those twice and double every TCP payload (Flynn
// keystrokes → `llss` on the wire).
#define MACTCP_COMPLETE(pbp, err)                                            \
  do {                                                                       \
    OSErr _mactcp_err = (err);                                               \
    (pbp)->ioParam.ioResult = _mactcp_err;                                   \
    if (((pbp)->ioParam.ioTrap & asyncTrpBit) && (pbp)->ioParam.ioCompletion) \
      callCompletion(US_TO_SYN68K(pbp), US_TO_SYN68K((pbp)->ioParam.ioCompletion), _mactcp_err); \
    return _mactcp_err;                                                      \
  } while (0)

static OSErr C_mactcp_open(ParmBlkPtr pbp, DCtlPtr dcp) {
  ensureVblInstalled();
  MACTCP_COMPLETE(pbp, meNoErr);
}
static OSErr C_mactcp_close(ParmBlkPtr pbp, DCtlPtr /*dcp*/) {
  MACTCP_COMPLETE(pbp, meNoErr);
}
static OSErr C_mactcp_prime(ParmBlkPtr pbp, DCtlPtr /*dcp*/) {
  // MacTCP has no Read/Write prime path — all work is Control/Status.
  MACTCP_COMPLETE(pbp, meControlErr);
}

static OSErr C_mactcp_ctl(ParmBlkPtr pbp, DCtlPtr /*dcp*/) {
  void* pb = (void*)pbp;
  int16_t cs = (int16_t)pbp->cntrlParam.csCode;
  bool async = (pbp->ioParam.ioTrap & asyncTrpBit) != 0;
  uint32_t sp = rdL(pb, off_tcpStream);

  switch (cs) {
    case csTCPCreate: {
      int fd = allocStream(false);
      if (fd < 0) MACTCP_COMPLETE(pbp, meInsufficientResources);
      Stream* s = getStream((uint32_t)fd);
      s->rcvBuff = rdL(pb, off_create_rcvBuff);
      s->rcvBuffLen = rdL(pb, off_create_rcvBuffLen);
      s->notifyProc = rdL(pb, off_create_notifyProc);
      s->userDataPtr = rdL(pb, off_create_userDataPtr);
      wrL(pb, off_tcpStream, (uint32_t)fd);
      DBG("TCPCreate -> stream %d notify=%u", fd, s->notifyProc);
      if (s->notifyProc) ensureVblInstalled();
      MACTCP_COMPLETE(pbp, meNoErr);
    }
    case csTCPActiveOpen: {
      Stream* s = getStream(sp);
      if (!s) MACTCP_COMPLETE(pbp, meInvalidStreamPtr);
      MACTCP_COMPLETE(pbp, doActiveOpen(pb, s, (int)sp));
    }
    case csTCPSend: {
      if (!getStream(sp)) MACTCP_COMPLETE(pbp, meInvalidStreamPtr);
      MACTCP_COMPLETE(pbp, doSend(pb, (int)sp));
    }
    case csTCPRcv: {
      Stream* s = getStream(sp);
      if (!s) MACTCP_COMPLETE(pbp, meInvalidStreamPtr);
      // Async: try non-blocking; if data is buffered complete inline, else park
      // for the VBL poll and return inProgress (control back to the guest now).
      if (async && pbp->ioParam.ioCompletion) {
        uint16_t want = rdW(pb, off_recv_rcvBuffLen);
        uint32_t rcvBuff = rdL(pb, off_recv_rcvBuff);
        int mask = net_round(OP_SOCK_POLL, (int)sp, 0, nullptr, 0, 0, nullptr, 0, nullptr);
        if (mask & POLL_READABLE) {
          MACTCP_COMPLETE(pbp, doRcvSync(pb, (int)sp)); // data ready → inline
        }
        if (mask & POLL_CLOSED) {
          wrW(pb, off_recv_rcvBuffLen, 0);
          MACTCP_COMPLETE(pbp, meConnectionClosing);
        }
        ensureVblInstalled();
        parkPending(US_TO_SYN68K(pbp), US_TO_SYN68K(pbp->ioParam.ioCompletion),
                    (int)sp, rcvBuff, want);
        pbp->ioParam.ioResult = meInProgress; // guest polls ioResult / awaits completion
        return meInProgress;
      }
      MACTCP_COMPLETE(pbp, doRcvSync(pb, (int)sp));
    }
    case csTCPClose:
    case csTCPAbort: {
      if (getStream(sp)) net_round(OP_SOCK_CLOSE, (int)sp, 0, nullptr, 0, 0, nullptr, 0, nullptr);
      DBG("TCPClose/Abort stream %u", sp);
      MACTCP_COMPLETE(pbp, meNoErr);
    }
    case csTCPRelease: {
      if (getStream(sp)) {
        net_round(OP_SOCK_CLOSE, (int)sp, 0, nullptr, 0, 0, nullptr, 0, nullptr);
        freeStream(sp);
      }
      MACTCP_COMPLETE(pbp, meNoErr);
    }
    case csTCPStatus: {
      Stream* s = getStream(sp);
      if (!s) MACTCP_COMPLETE(pbp, meInvalidStreamPtr);
      MACTCP_COMPLETE(pbp, doStatus(pb, s, (int)sp));
    }
    case csTCPPassiveOpen:
      // A listening app is reachable as a pc SERVICE at the gateway, not as a
      // distinct LAN host (#711 scope note). Not implemented in the client veneer.
      MACTCP_COMPLETE(pbp, meOpenFailed);

    case csUDPCreate: {
      int fd = allocStream(true);
      if (fd < 0) MACTCP_COMPLETE(pbp, meInsufficientResources);
      getStream((uint32_t)fd)->localPort = rdW(pb, off_udpc_localPort);
      wrL(pb, off_tcpStream, (uint32_t)fd);
      MACTCP_COMPLETE(pbp, meNoErr);
    }
    case csUDPWrite: {
      if (!getStream(sp)) MACTCP_COMPLETE(pbp, meInvalidStreamPtr);
      MACTCP_COMPLETE(pbp, doUdpWrite(pb, (int)sp));
    }
    case csUDPRead: {
      if (!getStream(sp)) MACTCP_COMPLETE(pbp, meInvalidStreamPtr);
      MACTCP_COMPLETE(pbp, doUdpRead(pb, (int)sp));
    }
    case csUDPRelease: {
      if (getStream(sp)) {
        net_round(OP_SOCK_CLOSE, (int)sp, 0, nullptr, 0, 0, nullptr, 0, nullptr);
        freeStream(sp);
      }
      MACTCP_COMPLETE(pbp, meNoErr);
    }
    case csUDPBfrReturn:
    case csTCPRcvBfrReturn:
    case csUDPMaxMTUSize:
      MACTCP_COMPLETE(pbp, meNoErr);

    case csIpctlGetAddr:
      MACTCP_COMPLETE(pbp, doGetAddr(pb));

    default:
      DBG("unhandled ctl csCode %d", cs);
      MACTCP_COMPLETE(pbp, meControlErr);
  }
}

static OSErr C_mactcp_status(ParmBlkPtr pbp, DCtlPtr /*dcp*/) {
  void* pb = (void*)pbp;
  int16_t cs = (int16_t)pbp->cntrlParam.csCode;
  // ipctlGetAddr is issued via PBControl in Apple's GetMyIPAddr, but tolerate a
  // Status issue too. TCPStatus routes here when apps use PBStatus.
  if (cs == csIpctlGetAddr) MACTCP_COMPLETE(pbp, doGetAddr(pb));
  if (cs == csTCPStatus) {
    uint32_t sp = rdL(pb, off_tcpStream);
    Stream* s = getStream(sp);
    if (!s) MACTCP_COMPLETE(pbp, meInvalidStreamPtr);
    MACTCP_COMPLETE(pbp, doStatus(pb, s, (int)sp));
  }
  MACTCP_COMPLETE(pbp, meControlErr);
}

// ── DNR: the 'dnrp' code resource ────────────────────────────────────────────
// Apps carry Apple's dnr.c, which loads 'dnrp' and calls it as
// `(*dnr)(selector, ...)` (C convention: args pushed R→L, caller cleans,
// selector — an int, 2 bytes on 68k — nearest the return address; OSErr
// returned in D0). We AddResource a 6-byte `JMP.L <magic>` into the System
// file (current at InitResources) so GetIndResource / GetResource find it.
// NOTE: some Apple dnr.c copies (Wallops) call Get1IndResource after
// OpenOurRF() — that only sees the CURRENT file, so without a MacTCP cdev
// in Control Panels it misses our System stub. Wallops.bin is patched
// A80E→A80D at the OpenResolver site; a future fix should also drop a
// fake cdev/ztcp with this same stub so unmodified Get1IndResource apps
// work. JSR-ing the stub lands in dnrCallback; it reads C args off the
// 68k stack.
namespace
{
syn68k_addr_t dnrCallback(syn68k_addr_t /*addr*/, void* /*arg*/) {
  syn68k_addr_t ret = POPADDR(); // JSR return address; leave args for the caller
  uint32_t sp = EM_A7;           // [selector][arg1(4)][arg2(4)]…
  // Selector width: Apple's dnr.c typedefs it UInt32 (Wallops pushes via PEA →
  // 4 bytes). Executor historically assumed a 2-byte int (Think C). Accept
  // both: a leading 0 word means 4-byte PEA/UInt32 form. (Wallops.bin is also
  // patched to push move.w so it works with wasm builds that predate this.)
  void* spp = SYN68K_TO_US(sp);
  uint16_t selHi = rdW(spp, 0);
  uint16_t selLo = rdW(spp, 2);
  int argBase;
  uint16_t sel;
  if (selHi == 0 && selLo >= 1 && selLo <= 8) {
    sel = selLo;
    argBase = 4;
  } else {
    sel = selHi;
    argBase = 2;
  }
  int16_t rc = meNoErr;
  switch (sel) {
    case 1: // OPENRESOLVER(fileName)
    case 2: // CLOSERESOLVER
      rc = meNoErr;
      break;
    case 3: { // STRTOADDR(hostName, rtnStruct, resultProc, userDataPtr)
      uint32_t hostAddr = rdL(spp, argBase);
      uint32_t rtnStruct = rdL(spp, argBase + 4);
      const char* host = (const char*)guestPtr(hostAddr);
      char resolved[32] = { 0 };
      int res_len = 0;
      uint32_t ip = 0;
      if (host) {
        int r = net_round(OP_RESOLVE, 0, 0, host, (int)strlen(host), 0, resolved, sizeof(resolved) - 1, &res_len);
        if (r == 0 && res_len > 0) { resolved[res_len] = '\0'; ip = dottedToIp(resolved); }
      }
      // Fill the hostInfo: rtnCode(long)@0, cname[255]@4, filler@259, addr[4]@260.
      // We resolve SYNCHRONOUSLY and return noErr, so the caller reads the result
      // straight from hostInfo — dnr.c only spins on the ResultProc when the
      // resource returns cacheFault. We DON'T invoke the (pascal, stack-arg)
      // ResultProc: with a noErr / rtnCode=0 return it isn't expected, and it's
      // the one 68k-stack manipulation not worth the crash risk for the edge
      // case of an app that ignores the return code.
      void* hi = guestPtr(rtnStruct);
      if (hi) {
        wrL(hi, 0, ip ? 0u : (uint32_t)(int32_t)-23042 /*cacheFault*/);
        int hl = host ? (int)strlen(host) : 0;
        if (hl > 254) hl = 254;
        for (int i = 0; i < hl; i++) wrB(hi, 4 + i, (uint8_t)host[i]);
        wrB(hi, 4 + hl, 0);
        wrL(hi, 260, ip);
        for (int i = 1; i < 4; i++) wrL(hi, 260 + i * 4, 0);
      }
      rc = ip ? meNoErr : (int16_t)-23042; // noErr (cached hit) | cacheFault
      break;
    }
    case 4: { // ADDRTOSTR(addr, addrStr)
      uint32_t addr = rdL(spp, argBase);
      uint32_t strAddr = rdL(spp, argBase + 4);
      char dotted[16];
      ipToDotted(addr, dotted);
      char* dst = (char*)guestPtr(strAddr);
      if (dst) strcpy(dst, dotted);
      rc = meNoErr;
      break;
    }
    case 6: { // ADDRTONAME(addr, rtnStruct, resultProc, userDataPtr) — reverse
      uint32_t addr = rdL(spp, argBase);
      uint32_t rtnStruct = rdL(spp, argBase + 4);
      char dotted[16];
      ipToDotted(addr, dotted);
      void* hi = guestPtr(rtnStruct);
      if (hi) {
        wrL(hi, 0, 0); // noErr
        int dl = (int)strlen(dotted);
        for (int i = 0; i < dl; i++) wrB(hi, 4 + i, (uint8_t)dotted[i]);
        wrB(hi, 4 + dl, 0);
        wrL(hi, 260, addr);
      }
      rc = meNoErr;
      break;
    }
    default:
      rc = (int16_t)-23048; // outOfMemory-ish; benign "unsupported selector"
      break;
  }
  EM_D0 = (uint16_t)rc; // OSErr in D0; caller cleans the pushed args
  return ret;
}

// Install a 6-byte 'dnrp' resource (id 1) = `JMP.L <dnrCallback magic addr>` so
// dnr.c's GetIndResource('dnrp',1) + `(*dnr)(…)` trampolines here.
void installDnrResource() {
  syn68k_addr_t magic = callback_install(&dnrCallback, nullptr);
  Handle h = NewHandle(6);
  if (!h) return;
  unsigned char stub[6];
  stub[0] = 0x4E; // JMP.L absolute-long
  stub[1] = 0xF9;
  stub[2] = (magic >> 24) & 0xFF; // big-endian magic address
  stub[3] = (magic >> 16) & 0xFF;
  stub[4] = (magic >> 8) & 0xFF;
  stub[5] = magic & 0xFF;
  memcpy(*h, stub, 6);
  // Empty Pascal name: "" is a single 0 byte → pascal length 0. (NOT "\p" — clang
  // has no MPW "\p" prefix; it would silently become a length-'p'=112 string.)
  AddResource(h, "dnrp"_4, 1, (StringPtr) "");
}
} // namespace

// ── in-engine self-test (env-gated) ──────────────────────────────────────────
// There is no classic MacTCP application in the sandbox to drive, and the WAN
// path needs a Wisp relay the sandbox can't reach — but the LAN path is fully
// exercisable in-process. When PC_MACTCP_SELFTEST_HOST is set (via
// localStorage["cb.executor.netSelfTest"], see executor-window.ts), this drives
// the WHOLE driver the way a guest app would — OpenDriver(".IPP") + PBControl on
// a real TCPiopb — through the real ring + servicer + vnet lwIP to a gateway
// echo service the harness registers, and logs a SELFTEST PASS/FAIL to stderr
// (the browser console picks it up). It exercises the csCode dispatch, the
// param-block offset marshaling, the ring protocol, and the sync TCPRcv
// end-to-end — VERIFIED working (#711): the guest connects and sends the exact
// bytes, confirmed by a gateway echo service. It NEVER runs in production
// (PC_MACTCP_SELFTEST_HOST is only set by a deliberate
// localStorage["cb.executor.netSelfTest"] — the test/deploy harness). Runs on
// the emulator pthread at boot (blocking futex calls are legal there); it bails
// after a failed connect so it can't park boot, but POINT IT AT A REACHABLE ECHO
// — a connect that succeeds to a peer that never replies would block the
// (blocking) TCPRcv. Kept in-tree so a future agent can re-verify on a deploy
// after any engine rebuild (CLAUDE.md's instrument-and-gate rule).
namespace
{
void mactcp_selftest() {
  const char* host = getenv("PC_MACTCP_SELFTEST_HOST");
  if (!host || !host[0]) return;
  int port = 7;
  if (const char* ps = getenv("PC_MACTCP_SELFTEST_PORT")) port = atoi(ps);
  const char* msg = getenv("PC_MACTCP_SELFTEST_MSG");
  if (!msg || !msg[0]) msg = "HELLO MACTCP";
  int msglen = (int)strlen(msg);
  fprintf(stderr, "[mactcp] SELFTEST start %s:%d msg='%s'\n", host, port, msg);
  fflush(stderr);

  GUEST<INTEGER> refnum = 0;
  OSErr err = OpenDriver((StringPtr) "\04.IPP", &refnum);
  INTEGER rn = refnum;
  fprintf(stderr, "[mactcp] SELFTEST OpenDriver err=%d refnum=%d\n", (int)err, (int)rn);
  fflush(stderr);
  if (err != noErr) { fprintf(stderr, "[mactcp] SELFTEST FAIL OpenDriver\n"); fflush(stderr); return; }

  Ptr pb = NewPtr(256);
  Ptr rcvArea = NewPtr(16384);
  Ptr sendBuf = NewPtr(256);
  Ptr wds = NewPtr(16);
  Ptr recvInto = NewPtr(1024);
  if (!pb || !rcvArea || !sendBuf || !wds || !recvInto) {
    fprintf(stderr, "[mactcp] SELFTEST FAIL alloc\n"); fflush(stderr); return;
  }

  auto header = [&](int cs, uint32_t stream) {
    memset(pb, 0, 256);
    wrW(pb, 24, (uint16_t)rn); // ioCRefNum → the .IPP driver
    wrW(pb, 26, (uint16_t)cs); // csCode
    wrL(pb, off_tcpStream, stream);
  };

  // TCPCreate → a StreamPtr.
  header(csTCPCreate, 0);
  wrL(pb, off_create_rcvBuff, US_TO_SYN68K(rcvArea));
  wrL(pb, off_create_rcvBuffLen, 16384);
  err = PBControl((ParmBlkPtr)pb, false);
  uint32_t stream = rdL(pb, off_tcpStream);
  fprintf(stderr, "[mactcp] SELFTEST TCPCreate err=%d stream=%u\n", (int)err, stream);
  fflush(stderr);

  // TCPActiveOpen → connect to host:port.
  header(csTCPActiveOpen, stream);
  wrL(pb, off_open_remoteHost, dottedToIp(host));
  wrW(pb, off_open_remotePort, (uint16_t)port);
  err = PBControl((ParmBlkPtr)pb, false);
  fprintf(stderr, "[mactcp] SELFTEST TCPActiveOpen err=%d\n", (int)err);
  fflush(stderr);
  if (err != meNoErr) {
    // Connect failed — do NOT proceed to a blocking TCPRcv (it would park the
    // boot thread forever waiting for data that never comes).
    fprintf(stderr, "[mactcp] SELFTEST FAIL connect\n");
    fflush(stderr);
    header(csTCPRelease, stream);
    PBControl((ParmBlkPtr)pb, false);
    return;
  }

  // TCPStatus (Flynn's idle poll) — exercises StatusPB offsets + pointer clears.
  header(csTCPStatus, stream);
  err = PBControl((ParmBlkPtr)pb, false);
  uint8_t st = rdB(pb, off_status_connectionState);
  uint16_t unread = rdW(pb, off_status_amtUnreadData);
  uint32_t sec = rdL(pb, off_status_securityLevelPtr);
  fprintf(stderr, "[mactcp] SELFTEST TCPStatus err=%d state=%u unread=%u secPtr=%u\n",
          (int)err, (unsigned)st, unread, sec);
  fflush(stderr);
  if (err != meNoErr || st != 8 || sec != 0) {
    fprintf(stderr, "[mactcp] SELFTEST FAIL status\n");
    fflush(stderr);
    header(csTCPRelease, stream);
    PBControl((ParmBlkPtr)pb, false);
    return;
  }

  // TCPSend msg via a one-entry WDS { len, ptr, 0-terminator }.
  memcpy(sendBuf, msg, msglen);
  wrW(wds, 0, (uint16_t)msglen);
  wrL(wds, 2, US_TO_SYN68K(sendBuf));
  wrW(wds, 6, 0);
  header(csTCPSend, stream);
  wrL(pb, off_send_wdsPtr, US_TO_SYN68K(wds));
  err = PBControl((ParmBlkPtr)pb, false);
  fprintf(stderr, "[mactcp] SELFTEST TCPSend err=%d\n", (int)err);
  fflush(stderr);

  // TCPRcv (synchronous): block until the echo comes back.
  header(csTCPRcv, stream);
  wrL(pb, off_recv_rcvBuff, US_TO_SYN68K(recvInto));
  wrW(pb, off_recv_rcvBuffLen, 1024);
  err = PBControl((ParmBlkPtr)pb, false);
  uint16_t got = rdW(pb, off_recv_rcvBuffLen);
  char rbuf[128];
  int n = got < 127 ? got : 127;
  memcpy(rbuf, recvInto, n);
  rbuf[n] = '\0';
  fprintf(stderr, "[mactcp] SELFTEST TCPRcv err=%d got=%u data='%s'\n", (int)err, got, rbuf);
  bool pass = (int)got == msglen && memcmp(recvInto, msg, msglen) == 0;
  fprintf(stderr, "[mactcp] SELFTEST %s\n", pass ? "PASS" : "FAIL rcv-mismatch");
  fflush(stderr);

  header(csTCPRelease, stream);
  PBControl((ParmBlkPtr)pb, false);
}
} // namespace

// ── init: register the .IPP driver + the 'dnrp' resource ─────────────────────
// Called once from main.cpp right after InitResources() (System file open =
// current res file, so AddResource lands where GetIndResource finds it). The
// driver's refnum -24 (devicen 23) is MacTCP-authentic and unused (serial owns
// -6..-9); it must be < NDEVICES.
extern "C" void pc_mactcp_init(void) {
  static bool done = false;
  if (done) return;
  done = true;
  RegisterDriver({
    &mactcp_open, &mactcp_prime, &mactcp_ctl, &mactcp_status, &mactcp_close,
    (StringPtr) "\04.IPP", -24,
  });
  installDnrResource();
  mactcp_selftest(); // no-op unless PC_MACTCP_SELFTEST_HOST is set (test harness)
}
