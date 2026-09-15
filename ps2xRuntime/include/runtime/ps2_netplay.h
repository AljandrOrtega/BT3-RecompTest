// [netplay] see src/lib/ps2_netplay.cpp
#pragma once
#include <cstdint>

#pragma pack(push, 1)
struct Ps2xNetInput { uint16_t buttons; uint8_t rx, ry, lx, ly; };   // 6 bytes on the wire
#pragma pack(pop)

void     ps2NetInit();
bool     ps2NetHost(int port, int player);            // start hosting at runtime (overlay)
bool     ps2NetJoin(const char *hostPort, int player); // "1.2.3.4:7777"
bool     ps2NetPeerConnected();                        // a peer's packets have arrived
void     ps2NetSetAutoJump(bool on);                   // overlay: jump to char select on connect
bool     ps2NetAutoJump();
void     ps2NetSetDelay(int frames);                   // 1 loopback, 2 LAN; 1 frame = 33 ms
void     ps2NetDisconnect(const char *why);            // tear down, restore local pads
uint32_t ps2NetSession();                              // bumped per connect
void     ps2NetSetBattleType(int t);                   // 0 Single, 1 Team, 2 DP (host decides)
int      ps2NetBattleType();
void     ps2NetSetDpLimit(int t);                      // DP Battle budget: 0 = 10, 1 = 15, 2 = 20
int      ps2NetDpLimit();
void     ps2NetSetTimeLimit(int t);                    // Battle Settings index, 3 = default
int      ps2NetTimeLimit();
void     ps2NetBeginAutoStart(const char *path);       // host: replay a canned menu sequence
bool     ps2NetAutoInput(Ps2xNetInput &out);           // next canned input, if any
bool     ps2NetAutoStartActive();
bool     ps2NetActive();
int      ps2NetLocalPlayer();                 // 1 or 2
uint32_t ps2NetDelay();
void     ps2NetSubmitLocal(uint32_t frame, const Ps2xNetInput &in);
bool     ps2NetGetInput(uint32_t frame, int player, Ps2xNetInput &out);
void     ps2NetSetChecksum(uint32_t frame, uint64_t hash);
void     ps2NetFrame(uint32_t frame);
// [rollback] Rollback netplay (PS2X_NET_ROLLBACK=<window frames>; needs PS2X_FIBERS + frame stepping).
// A missing remote input is PREDICTED (last known repeated) instead of stalling; when the real one
// arrives and differs, the frame-boundary controller restores that frame's snapshot and re-simulates.
uint32_t ps2NetRollbackWindow();                       // 0 = lockstep as before
// Called by the controller at every frame boundary (frameAbs = the frame about to run). Pumps the
// network. Returns the absolute frame to roll back to (0 = none), and sets *mustStall when a remote
// input older than the window is still missing (the controller then waits as lockstep did).
uint32_t ps2NetRollbackPoll(uint32_t frameAbs, bool *mustStall);
// [statesync] State sync at connect (PS2X_NET_SYNC=1; needs the rollback controller). The host publishes
// its frame-boundary state (a file for now: PS2X_NET_SYNCFILE, visible to both), the joiner adopts it at
// a structurally comparable boundary and acknowledges; inputs are exchanged from that frame on.
bool     ps2NetSyncPending();                                   // connected, sync on, state not yet adopted
bool     ps2NetSyncIsHost();
void     ps2NetSyncOffer(uint32_t frameAbs, uint64_t bytes, const char *path);   // host: announce the blob
bool     ps2NetSyncWaitDone(uint32_t timeoutMs);                 // host: pump until the joiner's DONE; sets the frame base
bool     ps2NetSyncOffered(uint32_t *frameAbs, uint64_t *bytes, char *path, size_t pathCap);   // joiner: an offer arrived
void     ps2NetSyncApplied(uint32_t frameAbs);                   // joiner: state adopted; sets the base, sends DONE
