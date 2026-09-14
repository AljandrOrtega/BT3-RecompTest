// [netplay] Deterministic lockstep netplay transport for BT3-Recomp.
//
// MODEL: the two machines never exchange game state during play -- only BUTTONS. Both run the
// full simulation (both fighters); each renders only its own player's viewport (PS2X_NETVIEW,
// see the online-viewport work). A PS2 pad sample is 6 bytes, so this is ~500 bytes/sec/player,
// four orders of magnitude below video streaming.
//
// TIMING: input-delay lockstep. Input sampled on frame F is APPLIED on frame F + delay, which
// gives the network delay*16.7 ms to deliver it. Rollback can replace this later without
// touching the transport; it needs save/load-state, which is a separate build.
//
// Each packet repeats the last kRedundancy frames of input, so a dropped datagram heals on the
// next arrival with no retransmit -- that is why this is UDP and not TCP: a late input is
// useless, so we would rather drop it than wait for it.
//
//   PS2X_NET=<host>:<port>   connect to a peer        PS2X_NET_LISTEN=<port>   wait for one
//   PS2X_NET_PLAYER=1|2      which player is local    PS2X_NET_DELAY=<frames>  default 4
//   PS2X_NET_TIMEOUT=<ms>    stall limit, default 2000
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <unordered_map>

#if defined(_WIN32)
#  include <winsock2.h>
#  include <ws2tcpip.h>
   typedef int socklen_t;
#  define PS2X_CLOSESOCK closesocket
#else
#  include <arpa/inet.h>
#  include <fcntl.h>
#  include <netdb.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <unistd.h>
   typedef int SOCKET;
#  define INVALID_SOCKET (-1)
#  define PS2X_CLOSESOCK ::close
#endif

#include "runtime/ps2_netplay.h"

namespace {

constexpr uint32_t kMagic       = 0x4e335442u;   // 'BT3N'
constexpr uint16_t kVersion     = 1u;
constexpr uint32_t kRedundancy  = 8u;            // inputs repeated per packet
constexpr uint32_t kMaxInputs   = 16u;

#pragma pack(push, 1)
struct NetPkt
{
    uint32_t     magic;
    uint16_t     version;
    uint8_t      player;        // sender's player index, 1 or 2
    uint8_t      count;         // inputs carried, oldest first
    uint32_t     baseFrame;     // frame of inputs[0]
    uint32_t     checkFrame;    // frame the checksum belongs to (0 = none)
    uint64_t     checksum;      // gameplay-state hash, for desync detection
    Ps2xNetInput inputs[kMaxInputs];
};
#pragma pack(pop)

struct Net
{
    bool        active   = false;
    bool        listening = false;
    int         localPlayer = 1;      // 1 or 2
    // BT3 runs at 30 fps, so ONE frame of delay is 33 ms -- the old default of 4 was 133 ms,
    // chosen as if this were a 60 fps game. The delay only has to cover the network round trip:
    // 1 on loopback, 2 on a LAN, more only for a real internet link.
    uint32_t    delay    = 2;
    uint32_t    timeoutMs = 2000;
    SOCKET      sock     = INVALID_SOCKET;
    sockaddr_in peer{};
    bool        peerKnown = false;
    bool        connected = false;

    std::mutex  mtx;
    std::unordered_map<uint32_t, Ps2xNetInput> local;    // frame -> our input (already delayed)
    std::unordered_map<uint32_t, Ps2xNetInput> remote;   // frame -> peer input
    std::unordered_map<uint32_t, uint64_t>     peerHash; // frame -> peer's state hash
    std::unordered_map<uint32_t, uint64_t>     ourHash;
    // [relframe] Frame numbers are RELATIVE TO THE CONNECTION, not absolute. Connecting from the
    // overlay mid-session means the two machines are at completely different frame counts (2018 vs
    // 2333 when this was found), so keying inputs by absolute frame made every lookup miss and
    // both sides stalled the full timeout on every frame -- a hard freeze. Each side records its
    // own frame at connect and works in offsets from there; lockstep then keeps them in step.
    uint32_t    base = 0;
    bool        needBase = true;
    uint32_t    lastSent = 0;
    uint32_t    checkFrame = 0;
    uint64_t    checkValue = 0;
    std::atomic<uint64_t> stalls{0}, stallNs{0}, desyncs{0}, rx{0}, tx{0};
    std::chrono::steady_clock::time_point lastRx{};   // watchdog: peer silence
    uint32_t session = 0;                             // bumped per connect, so netjump can reset
};

Net g;

const Ps2xNetInput kNeutral{0xFFFFu, 0x80u, 0x80u, 0x80u, 0x80u};

void setNonBlocking(SOCKET s)
{
#if defined(_WIN32)
    u_long nb = 1; ioctlsocket(s, FIONBIO, &nb);
#else
    const int fl = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, fl | O_NONBLOCK);
#endif
}

bool parseHostPort(const char *v, sockaddr_in &out)
{
    std::string s(v);
    const size_t c = s.rfind(':');
    if (c == std::string::npos) return false;
    const std::string host = s.substr(0, c);
    const int port = std::atoi(s.c_str() + c + 1);
    if (port <= 0 || port > 65535) return false;
    std::memset(&out, 0, sizeof out);
    out.sin_family = AF_INET;
    out.sin_port = htons(static_cast<uint16_t>(port));
    if (inet_pton(AF_INET, host.c_str(), &out.sin_addr) == 1) return true;
    addrinfo hints{}; hints.ai_family = AF_INET; hints.ai_socktype = SOCK_DGRAM;
    addrinfo *res = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res) return false;
    out.sin_addr = reinterpret_cast<sockaddr_in *>(res->ai_addr)->sin_addr;
    freeaddrinfo(res);
    return true;
}

void sendOurs(uint32_t frame)
{
    if (!g.peerKnown) return;
    NetPkt p{};
    p.magic = kMagic; p.version = kVersion;
    p.player = static_cast<uint8_t>(g.localPlayer);
    p.checkFrame = g.checkFrame; p.checksum = g.checkValue;
    const uint32_t base = frame > (kRedundancy - 1u) ? frame - (kRedundancy - 1u) : 0u;
    p.baseFrame = base;
    uint32_t n = 0;
    for (uint32_t f = base; f <= frame && n < kMaxInputs; ++f, ++n)
    {
        const auto it = g.local.find(f);
        p.inputs[n] = (it != g.local.end()) ? it->second : kNeutral;
    }
    p.count = static_cast<uint8_t>(n);
    const size_t bytes = sizeof(NetPkt) - sizeof(Ps2xNetInput) * (kMaxInputs - n);
    ::sendto(g.sock, reinterpret_cast<const char *>(&p), static_cast<int>(bytes), 0,
             reinterpret_cast<sockaddr *>(&g.peer), sizeof g.peer);
    g.tx.fetch_add(1, std::memory_order_relaxed);
}

void pump()
{
    for (;;)
    {
        NetPkt p{};
        sockaddr_in from{}; socklen_t fl = sizeof from;
        const int n = ::recvfrom(g.sock, reinterpret_cast<char *>(&p), sizeof p, 0,
                                 reinterpret_cast<sockaddr *>(&from), &fl);
        if (n <= 0) break;
        if (static_cast<size_t>(n) < sizeof(NetPkt) - sizeof(Ps2xNetInput) * kMaxInputs) continue;
        if (p.magic != kMagic || p.version != kVersion) continue;
        if (p.player == 0xFFu)
        {   // peer said goodbye
            g.peerKnown = false;
            std::fprintf(stderr, "[netplay] peer disconnected\n");
            ps2NetDisconnect("peer left");
            return;
        }
        g.lastRx = std::chrono::steady_clock::now();
        if (!g.peerKnown)
        {   // listener learns the peer address from the first valid packet
            g.peer = from; g.peerKnown = true;
            char ip[64] = {0}; inet_ntop(AF_INET, &from.sin_addr, ip, sizeof ip);
            std::fprintf(stderr, "[netplay] peer connected from %s:%u (they are player %u)\n",
                         ip, (unsigned)ntohs(from.sin_port), (unsigned)p.player);
        }
        if (!g.connected)
        {
            g.connected = true; ++g.session;
            // host drives; the joiner just follows the inputs it receives
            if (g.listening) { if (const char *a = std::getenv("PS2X_NET_AUTOSTART")) ps2NetBeginAutoStart(a); }
        }
        g.rx.fetch_add(1, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lk(g.mtx);
        for (uint32_t i = 0; i < p.count && i < kMaxInputs; ++i)
            g.remote[p.baseFrame + i] = p.inputs[i];
        if (p.checkFrame) g.peerHash[p.checkFrame] = p.checksum;
    }
}

}  // namespace

bool ps2NetActive() { return g.active; }
int  ps2NetLocalPlayer() { return g.localPlayer; }
uint32_t ps2NetDelay() { return g.delay; }

// Shared by the env path and the overlay's Host/Join buttons. `listenPort != 0` hosts;
// otherwise `conn` is "host:port". Safe to call while the game is running: the socket is the
// only state, and the frame hook pumps it from the next frame on.
static bool netStart(const char *conn, int listenPort, int player)
{
    if (g.active) { std::fprintf(stderr, "[netplay] already connected\n"); return false; }
#if defined(_WIN32)
    static bool s_wsa = [](){ WSADATA w; WSAStartup(MAKEWORD(2, 2), &w); return true; }();
    (void)s_wsa;
#endif
    g.localPlayer = (player == 2) ? 2 : 1;
    if (const char *d = std::getenv("PS2X_NET_DELAY")) { const int v = std::atoi(d); if (v >= 0 && v <= 20) g.delay = (uint32_t)v; }
    if (const char *t = std::getenv("PS2X_NET_TIMEOUT")) { const int v = std::atoi(t); if (v > 0) g.timeoutMs = (uint32_t)v; }

    g.sock = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (g.sock == INVALID_SOCKET) { std::fprintf(stderr, "[netplay] socket() failed\n"); return false; }

    if (g.sock == INVALID_SOCKET) { std::fprintf(stderr, "[netplay] socket() failed\n"); return false; }
    if (listenPort)
    {
        sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = INADDR_ANY;
        a.sin_port = htons(static_cast<uint16_t>(listenPort));
        if (::bind(g.sock, reinterpret_cast<sockaddr *>(&a), sizeof a) != 0)
        { std::fprintf(stderr, "[netplay] bind(%d) failed\n", listenPort); PS2X_CLOSESOCK(g.sock); g.sock = INVALID_SOCKET; return false; }
        g.listening = true;
        std::fprintf(stderr, "[netplay] listening on UDP %d as player %d (delay %u frames)\n", listenPort, g.localPlayer, g.delay);
    }
    else
    {
        if (!conn || !parseHostPort(conn, g.peer))
        { std::fprintf(stderr, "[netplay] '%s' is not host:port\n", conn ? conn : "(null)"); PS2X_CLOSESOCK(g.sock); g.sock = INVALID_SOCKET; return false; }
        g.peerKnown = true;
        char ip[64] = {0}; inet_ntop(AF_INET, &g.peer.sin_addr, ip, sizeof ip);
        std::fprintf(stderr, "[netplay] peer %s:%u, local player %d (delay %u frames)\n",
                     ip, (unsigned)ntohs(g.peer.sin_port), g.localPlayer, g.delay);
    }
    setNonBlocking(g.sock);
    g.active = true;
    return true;
}

bool ps2NetHost(int port, int player)              { return netStart(nullptr, port, player); }
bool ps2NetJoin(const char *hostPort, int player)  { return netStart(hostPort, 0, player); }
bool ps2NetPeerConnected()                         { return g.connected; }
uint32_t ps2NetSession()                           { return g.session; }

// A "bye" is an ordinary packet with player = 0xFF. Sending one means the peer tears down at
// once instead of discovering us gone via the stall timeout, which would otherwise freeze their
// game for PS2X_NET_TIMEOUT on every frame.
static void sendBye()
{
    if (!g.peerKnown || g.sock == INVALID_SOCKET) return;
    NetPkt p{}; p.magic = kMagic; p.version = kVersion; p.player = 0xFFu; p.count = 0u;
    const size_t bytes = sizeof(NetPkt) - sizeof(Ps2xNetInput) * kMaxInputs;
    for (int i = 0; i < 3; ++i)   // UDP: send a few, they are tiny
        ::sendto(g.sock, reinterpret_cast<const char *>(&p), static_cast<int>(bytes), 0,
                 reinterpret_cast<sockaddr *>(&g.peer), sizeof g.peer);
}

void ps2NetDisconnect(const char *why)
{
    if (!g.active) return;
    sendBye();
    std::lock_guard<std::mutex> lk(g.mtx);
    if (g.sock != INVALID_SOCKET) { PS2X_CLOSESOCK(g.sock); g.sock = INVALID_SOCKET; }
    g.active = false; g.connected = false; g.peerKnown = false; g.listening = false;
    g.local.clear(); g.remote.clear(); g.peerHash.clear(); g.ourHash.clear();
    g.needBase = true; g.base = 0; g.checkFrame = 0; g.checkValue = 0;
    std::fprintf(stderr, "[netplay] disconnected (%s) -- local pads restored\n", why ? why : "requested");
}

// [netjump] Whether a successful connection should take both sides to character select.
// Owned by the overlay's Netplay tab (the env var stays as an override for headless runs), so
// the jump is a property of CONNECTING rather than of standing on the main menu.
static std::atomic<bool> g_autoJump{false};
void ps2NetSetAutoJump(bool on) { g_autoJump.store(on, std::memory_order_relaxed); }
void ps2NetSetDelay(int frames)  { if (frames >= 0 && frames <= 20) g.delay = (uint32_t)frames; }
bool ps2NetAutoJump()           { return g_autoJump.load(std::memory_order_relaxed); }

// ---- auto-start -------------------------------------------------------------------------
// When the peer connects, the HOST replays a canned menu sequence as its own player-1 input.
// That crosses the wire through the ordinary path, so the peer follows automatically -- no new
// synchronisation is needed, because lockstep already guarantees both sides see the same inputs
// on the same frames. Record the sequence once with PS2X_INREC, walking title -> Duel ->
// 1P VS 2P -> character select, and point PS2X_NET_AUTOSTART at the file.
// OPEN-LOOP by nature: if one side hits an extra loading frame the presses land on the wrong
// screen, the same failure the rig's drive_duel.sh avoids by verifying each screen. Treat it as
// a convenience, not a guarantee -- and it only runs on the host, so the peer cannot fight it.
namespace {
#pragma pack(push, 1)
struct AutoSample { uint32_t frame; uint8_t player, pad0; uint16_t buttons; uint8_t rx, ry, lx, ly; };
#pragma pack(pop)
std::vector<Ps2xNetInput> g_auto;
size_t g_autoPos = 0;
bool   g_autoRunning = false;
}

void ps2NetBeginAutoStart(const char *path)
{
    if (!path || !path[0] || g_autoRunning) return;
    std::FILE *f = std::fopen(path, "rb");
    if (!f) { std::fprintf(stderr, "[netplay] auto-start: cannot read %s\n", path); return; }
    AutoSample e{};
    g_auto.clear();
    while (std::fread(&e, sizeof e, 1, f) == 1)
        if (e.player == 0u) g_auto.push_back(Ps2xNetInput{e.buttons, e.rx, e.ry, e.lx, e.ly});
    std::fclose(f);
    if (g_auto.empty()) { std::fprintf(stderr, "[netplay] auto-start: %s has no player-1 samples\n", path); return; }
    g_autoPos = 0; g_autoRunning = true;
    std::fprintf(stderr, "[netplay] auto-start: replaying %zu menu inputs from %s\n", g_auto.size(), path);
}

bool ps2NetAutoInput(Ps2xNetInput &out)
{
    if (!g_autoRunning) return false;
    if (g_autoPos >= g_auto.size())
    { g_autoRunning = false; std::fprintf(stderr, "[netplay] auto-start: sequence finished\n"); return false; }
    out = g_auto[g_autoPos++];
    return true;
}
bool ps2NetAutoStartActive() { return g_autoRunning; }

void ps2NetInit()
{
    static bool done = false;
    if (done) return;
    done = true;
    const char *conn = std::getenv("PS2X_NET");
    const char *lis  = std::getenv("PS2X_NET_LISTEN");
    if ((!conn || !conn[0]) && (!lis || !lis[0])) return;
    int player = 1;
    if (const char *p = std::getenv("PS2X_NET_PLAYER")) player = (std::atoi(p) == 2) ? 2 : 1;
    if (lis && lis[0]) ps2NetHost(std::atoi(lis), player);
    else               ps2NetJoin(conn, player);
}

static uint32_t relFrame(uint32_t frame)
{
    if (g.needBase) { g.base = frame; g.needBase = false;
                      std::fprintf(stderr, "[netplay] frame base = %u (inputs are relative from here)\n", frame); }
    return frame - g.base;
}

static void sendHello()
{   // count = 0: no inputs, just "I am here". The connector must send before anyone is connected
    // -- the listener learns the peer address from this. Gating sends on g.connected (which is
    // only set BY a receive) deadlocked both sides at tx 0, waiting for each other forever.
    if (!g.peerKnown) return;
    NetPkt p{};
    p.magic = kMagic; p.version = kVersion;
    p.player = static_cast<uint8_t>(g.localPlayer);
    p.count = 0u; p.baseFrame = 0u;
    const size_t bytes = sizeof(NetPkt) - sizeof(Ps2xNetInput) * kMaxInputs;
    ::sendto(g.sock, reinterpret_cast<const char *>(&p), static_cast<int>(bytes), 0,
             reinterpret_cast<sockaddr *>(&g.peer), sizeof g.peer);
    g.tx.fetch_add(1, std::memory_order_relaxed);
}

void ps2NetSubmitLocal(uint32_t frameAbs, const Ps2xNetInput &in)
{
    if (!g.active) return;
    if (!g.connected) { sendHello(); pump(); return; }   // handshake only, no frame numbering yet
    const uint32_t frame = relFrame(frameAbs);
    {
        std::lock_guard<std::mutex> lk(g.mtx);
        g.local[frame + g.delay] = in;      // sampled now, APPLIED delay frames later
    }
    sendOurs(frame + g.delay);
    pump();
}

bool ps2NetGetInput(uint32_t frameAbs, int player, Ps2xNetInput &out)
{
    if (!g.active || !g.connected) return false;   // pre-connection: leave the pads alone
    const uint32_t frame = relFrame(frameAbs);
    const bool wantLocal = (player == g.localPlayer);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(g.timeoutMs);
    const auto t0 = std::chrono::steady_clock::now();
    bool stalled = false;
    for (;;)
    {
        {
            std::lock_guard<std::mutex> lk(g.mtx);
            auto &m = wantLocal ? g.local : g.remote;
            const auto it = m.find(frame);
            if (it != m.end()) { out = it->second; break; }
            // the first `delay` frames have no sampled input yet: neutral, by definition
            if (frame < g.delay) { out = kNeutral; break; }
        }
        if (wantLocal) { out = kNeutral; break; }        // our own gap: never stall on ourselves
        pump();
        if (std::chrono::steady_clock::now() > deadline)
        {
            std::fprintf(stderr, "[netplay] TIMEOUT waiting for peer input at frame %u -- using neutral (THIS WILL DESYNC)\n", frame);
            out = kNeutral;
            break;
        }
        stalled = true;
        std::this_thread::sleep_for(std::chrono::microseconds(250));
    }
    if (stalled)
    {
        g.stalls.fetch_add(1, std::memory_order_relaxed);
        g.stallNs.fetch_add((uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now() - t0).count(), std::memory_order_relaxed);
    }
    return true;
}

void ps2NetSetChecksum(uint32_t frame, uint64_t hash)
{
    if (!g.active) return;
    std::lock_guard<std::mutex> lk(g.mtx);
    g.checkFrame = frame; g.checkValue = hash; g.ourHash[frame] = hash;
    const auto it = g.peerHash.find(frame);
    if (it != g.peerHash.end() && it->second != hash)
    {
        if (g.desyncs.fetch_add(1, std::memory_order_relaxed) == 0)
            std::fprintf(stderr, "[netplay] *** DESYNC at frame %u: ours %016llx peer %016llx ***\n",
                         frame, (unsigned long long)hash, (unsigned long long)it->second);
    }
    // keep the tables small
    if (g.ourHash.size() > 600)
    {
        for (auto i = g.ourHash.begin(); i != g.ourHash.end();) i = (i->first + 600 < frame) ? g.ourHash.erase(i) : ++i;
        for (auto i = g.peerHash.begin(); i != g.peerHash.end();) i = (i->first + 600 < frame) ? g.peerHash.erase(i) : ++i;
        std::lock_guard<std::mutex> lk2(g.mtx);
    }
}

void ps2NetFrame(uint32_t frame)
{
    if (!g.active) return;
    pump();
    if (g.connected)
    {   // Watchdog: a peer that vanishes (crash, closed window, cable out) would otherwise make
        // every frame burn the full stall timeout forever. Drop the session instead.
        static const int s_deadMs = [](){ const char *v = std::getenv("PS2X_NET_DEADMS");
                                          const int n = (v && v[0]) ? std::atoi(v) : 5000; return n > 0 ? n : 5000; }();
        if (g.lastRx.time_since_epoch().count() != 0 &&
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - g.lastRx).count() > s_deadMs)
        { ps2NetDisconnect("peer timed out"); return; }
    }
    static auto tStat = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::seconds>(now - tStat).count() >= 5)
    {
        tStat = now;
        const uint64_t st = g.stalls.exchange(0), ns = g.stallNs.exchange(0);
        std::fprintf(stderr, "[netplay] frame %u | tx %llu rx %llu | stalls %llu (%.1f ms total) | desyncs %llu | peer %s\n",
                     frame, (unsigned long long)g.tx.load(), (unsigned long long)g.rx.load(),
                     (unsigned long long)st, ns / 1e6, (unsigned long long)g.desyncs.load(),
                     g.connected ? "connected" : "WAITING");
    }
}
