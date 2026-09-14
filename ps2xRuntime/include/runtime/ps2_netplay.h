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
