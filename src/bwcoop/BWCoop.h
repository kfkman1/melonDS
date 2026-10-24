/*
    bw-coop: co-op presence bridge for Pokemon Black (US, rev 0, IRBO) on melonDS.
    Part of the Co-op Anything project. GPL-3.0-or-later, same as melonDS.

    Usage from a frontend:
        bwcoop::Bridge bridge(nds);            // after the ROM is loaded
        bridge.Host(port) / bridge.Join(host, port);
        ...each emulated frame, after nds.RunFrame():  bridge.OnFrame();
    The bridge does nothing unless the loaded cart is IRBO rev 0.
*/
#ifndef BWCOOP_H
#define BWCOOP_H

#include <string>
#include <deque>
#include <functional>
#include "types.h"

namespace melonDS { class NDS; }

namespace bwcoop
{
using melonDS::u8; using melonDS::u16; using melonDS::u32; using melonDS::s32; using melonDS::u64;

// Wire format (little endian, 36 bytes). Version bump on any change.
struct PlayerState
{
    u32 magic;      // 'BWC1'
    u32 frame;      // sender frame counter
    s32 map;        // -1 = no player (warp, battle, menu, title)
    u16 x, y, z;    // tile
    u16 facing;     // 0 up, 1 down, 2 left, 3 right
    s32 fx, fy, fz; // fx32 world position
    u16 sprite;
    u8  fpt;        // frames per tile of the last step (speed class)
    u8  pad;
};
static_assert(sizeof(PlayerState) == 36, "wire size");

struct Stats
{
    u32 sent = 0, received = 0, spawns = 0, deletes = 0, commands = 0, teleports = 0, callsBusy = 0;
    u32 irSent = 0, irRecv = 0, irEvents = 0, invitesSent = 0, invitesRecv = 0;
};

class Bridge
{
public:
    explicit Bridge(melonDS::NDS& nds);
    ~Bridge();

    bool Attach();                 // true if the loaded game is supported
    bool Host(int port);
    bool Join(const std::string& host, int port);
    void Disconnect();

    void OnFrame();                // call once after every RunFrame()
    bool StartIR();                // open the game's infrared event (Battle / Trade menu) now
    // Give this console its own MAC address (firmware + the OS copy). The game rejects a link partner with the
    // same MAC, and melonDS's generated firmware gives every player 00:09:BF:11:22:33.
    void SetMac(const u8 mac[6]);
    void SetIRDelay(int frames) { irDelay = frames; }   // test knob: hold received IR packets this many frames

    bool Supported() const { return supported; }
    bool Connected() const;
    const Stats& GetStats() const { return stats; }
    // Last state we read from our own game, and what we are showing for the remote (for tests/OSD).
    const PlayerState& Own() const { return own; }
    bool RemoteShown(PlayerState& out) const;

    std::function<void(const std::string&)> Log;   // optional status sink (OSD, stderr)

private:
    melonDS::NDS& nds;
    bool supported = false;
    u32 frame = 0;
    PlayerState own {}, remote {};
    bool haveRemote = false;
    Stats stats;

    // remote trainer
    u32 slot = 0;
    std::deque<PlayerState> queue;
    u16 lastTX = 0xFFFF, lastTY = 0xFFFF;

    // own speed estimate
    u16 ownLX = 0xFFFF, ownLY = 0xFFFF; u32 ownLFrame = 0; u8 ownFpt = 16;

    // injected call state
    enum class CallKind { None, Spawn, Delete, IRC };
    CallKind pending = CallKind::None;
    u32 pendingFrames = 0;

    // infrared link
    struct IRPacket { u32 due; int len; u8 data[0xB8]; };
    std::deque<IRPacket> irIn;
    int irDelay = 0;
    bool irWanted = false;
    u32 irWantedFrames = 0;
    bool TryStartIR();
    // press A facing the partner's trainer -> both players get the game's IR menu
    bool prevA = false;
    u32 inviteCooldown = 0;
    void CheckInteract();
    void SendInvite(u32 kind);
    // Exit ordering for the IR menu's wireless session (see ExitGate in BWCoop.cpp)
    u32 partnerIrcSeq = 0, partnerIrcMax = 0, lastIrcSeqSent = ~0u, exitHold = 0;
    void ExitGate();
    void IRSend(const u8* data, int len);
    int IRRecv(u8* buf, int max);

    // network (ENet), opaque
    struct Net;
    Net* net = nullptr;

    u32 R32(u32 a); u16 R16(u32 a);
    void W32(u32 a, u32 v); void W16(u32 a, u16 v);
    static bool Valid(u32 a) { return a >= 0x02000000 && a < 0x02400000; }

    u32 MmdlSys();
    u32 PlayerSlot();
    void ReadOwn();
    bool SlotAlive();
    bool CpuIdle();
    bool BeginCall(u32 fn, const u32* regs, const u32* stackArgs, int nStack, CallKind kind);
    void PollCall();
    void Apply();
    void Teleport(const PlayerState& r);
    void Pump();
    void NetPump();
    void Say(const std::string& s) { if (Log) Log(s); }
};

}

#endif
