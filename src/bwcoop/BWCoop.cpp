/*
    bw-coop: co-op presence bridge for Pokemon Black (US, rev 0, IRBO) on melonDS.
    GPL-3.0-or-later. Addresses and routines are documented, with evidence, in the
    Co-op Anything write-up games/pokemon-black-white/NOTES.md.
*/
#include "BWCoop.h"

#include <cstring>
#include <enet/enet.h>
#include "NDS.h"
#include "NDSCart.h"

namespace bwcoop
{

// ---- game constants (Pokemon Black US rev 0) ----
static constexpr u32 FIELD_PTR  = 0x02146180;   // -> field system
static constexpr u32 MMDL_OFF   = 0x1738;       // field + this -> movement-model system
static constexpr u32 SLOT_SIZE  = 0x100;
static constexpr u32 MAPID      = 0x0224F90C;   // u16 current map id
static constexpr u32 FN_ADD     = 0x0216CE6C;   // slot* AddCharacter(sys, x, y, dir, [sp] id, sprite, move, map)
static constexpr u32 FN_DELETE  = 0x0216CF2C;   // void DeleteCharacter(slot*)
static constexpr u32 IDLE_LO = 0x02085B20, IDLE_HI = 0x02085B30, HALT_LO = 0x020882D0, HALT_HI = 0x020882E0;
static constexpr u32 IDLE_RESUME = 0x02085B28;  // ARM: bl halt; b loop
static constexpr u16 REMOTE_ID  = 0x80;
static constexpr u32 MAGIC      = 0x31435742;   // "BWC1"

// Scratch RAM, zero in every observed state: stack top, return stub, call data block.
static constexpr u32 SCRATCH_SP   = 0x023FE000;
static constexpr u32 STUB_ADDR    = 0x023FE800;
static constexpr u32 DATA_ADDR    = 0x023FE900;  // [0] result, [4] saved sp, [8] saved cpsr, [C] resume pc
static constexpr u32 SENTINEL     = 0xFFFFFFFF;
// Thumb "bx pc; mov r8,r8" then ARM: ldr r1,lit; str r0,[r1]; ldr sp,[r1,#4]; ldr r2,[r1,#8];
// msr cpsr_fc,r2; ldr pc,[r1,#12]; lit: .word DATA_ADDR
static const u8 STUB[32] = {
    0x78, 0x47, 0xC0, 0x46, 0x10, 0x10, 0x9F, 0xE5, 0x00, 0x00, 0x81, 0xE5, 0x04, 0xD0, 0x91, 0xE5,
    0x08, 0x20, 0x91, 0xE5, 0x02, 0xF0, 0x29, 0xE1, 0x0C, 0xF0, 0x91, 0xE5, 0x00, 0xE9, 0x3F, 0x02 };

static constexpr int SEND_EVERY = 2;            // frames
static constexpr u32 REMOTE_TIMEOUT = 180;      // frames without packets -> remote treated as gone
static constexpr size_t MAX_QUEUE = 3;

struct Bridge::Net
{
    ENetHost* host = nullptr;
    ENetPeer* peer = nullptr;
    bool server = false;
    u32 lastRecvFrame = 0;
};

static bool g_EnetInit = false;

Bridge::Bridge(melonDS::NDS& n) : nds(n) {}
Bridge::~Bridge() { Disconnect(); }

u32 Bridge::R32(u32 a) { return nds.ARM9Read32(a); }
u16 Bridge::R16(u32 a) { return nds.ARM9Read16(a); }
void Bridge::W32(u32 a, u32 v) { nds.ARM9Write32(a, v); }
void Bridge::W16(u32 a, u16 v) { nds.ARM9Write16(a, v); }

bool Bridge::Attach()
{
    supported = false;
    auto* cart = nds.GetNDSCart();
    if (!cart) return false;
    const auto& h = cart->GetHeader();
    supported = memcmp(h.GameCode, "IRBO", 4) == 0 && h.ROMVersion == 0;
    Say(supported ? "bw-coop: Pokemon Black (US) detected" : "bw-coop: unsupported game, co-op off");
    return supported;
}

// ---------------------------------------------------------------- network

bool Bridge::Host(int port)
{
    Disconnect();
    if (!g_EnetInit) { if (enet_initialize() != 0) return false; g_EnetInit = true; }
    net = new Net;
    ENetAddress addr; addr.host = ENET_HOST_ANY; addr.port = (enet_uint16)port;
    net->host = enet_host_create(&addr, 1, 1, 0, 0);
    net->server = true;
    if (!net->host) { Say("bw-coop: could not open port " + std::to_string(port)); Disconnect(); return false; }
    Say("bw-coop: hosting on port " + std::to_string(port));
    return true;
}

bool Bridge::Join(const std::string& hostname, int port)
{
    Disconnect();
    if (!g_EnetInit) { if (enet_initialize() != 0) return false; g_EnetInit = true; }
    net = new Net;
    net->host = enet_host_create(nullptr, 1, 1, 0, 0);
    if (!net->host) { Disconnect(); return false; }
    ENetAddress addr;
    if (enet_address_set_host(&addr, hostname.c_str()) != 0) { Say("bw-coop: bad host " + hostname); Disconnect(); return false; }
    addr.port = (enet_uint16)port;
    net->peer = enet_host_connect(net->host, &addr, 1, 0);
    Say("bw-coop: connecting to " + hostname + ":" + std::to_string(port));
    return net->peer != nullptr;
}

void Bridge::Disconnect()
{
    if (!net) return;
    if (net->peer) enet_peer_disconnect_now(net->peer, 0);
    if (net->host) enet_host_destroy(net->host);
    delete net; net = nullptr;
    haveRemote = false;
}

bool Bridge::Connected() const
{
    return net && net->peer && net->peer->state == ENET_PEER_STATE_CONNECTED;
}

void Bridge::NetPump()
{
    if (!net || !net->host) return;
    ENetEvent ev;
    while (enet_host_service(net->host, &ev, 0) > 0)
    {
        switch (ev.type)
        {
        case ENET_EVENT_TYPE_CONNECT:
            net->peer = ev.peer;
            Say("bw-coop: connected");
            break;
        case ENET_EVENT_TYPE_DISCONNECT:
            if (ev.peer == net->peer) net->peer = nullptr;
            haveRemote = false;
            Say("bw-coop: partner disconnected");
            break;
        case ENET_EVENT_TYPE_RECEIVE:
            if (ev.packet->dataLength == sizeof(PlayerState))
            {
                PlayerState p; memcpy(&p, ev.packet->data, sizeof p);
                if (p.magic == MAGIC && (!haveRemote || (s32)(p.frame - remote.frame) > 0 || p.frame < 60))
                {
                    remote = p; haveRemote = true; net->lastRecvFrame = frame; stats.received++;
                }
            }
            enet_packet_destroy(ev.packet);
            break;
        default: break;
        }
    }
    if (haveRemote && frame - net->lastRecvFrame > REMOTE_TIMEOUT) haveRemote = false;

    if (Connected() && frame % SEND_EVERY == 0)
    {
        ENetPacket* pk = enet_packet_create(&own, sizeof own, ENET_PACKET_FLAG_UNSEQUENCED);
        enet_peer_send(net->peer, 0, pk);
        enet_host_flush(net->host);
        stats.sent++;
    }
}

// ---------------------------------------------------------------- game state

u32 Bridge::MmdlSys()
{
    u32 f = R32(FIELD_PTR);
    if (!Valid(f)) return 0;
    u32 m = R32(f + MMDL_OFF);
    return Valid(m) ? m : 0;
}

u32 Bridge::PlayerSlot()
{
    u32 m = MmdlSys();
    if (!m) return 0;
    u32 slots = R32(m + 0x18); u16 mx = R16(m + 4);
    if (!Valid(slots) || mx == 0 || mx > 0x80) return 0;
    for (u32 k = 0; k < mx; k++)
    {
        u32 s = slots + k * SLOT_SIZE;
        if (R16(s + 8) == 0xFF && (R16(s) & 0x21) == 0x21) return s;
    }
    return 0;
}

void Bridge::ReadOwn()
{
    PlayerState p {}; p.magic = MAGIC; p.frame = frame; p.map = -1;
    u32 s = PlayerSlot();
    if (s)
    {
        p.map = R16(MAPID);
        p.facing = R16(s + 0x18);
        p.x = R16(s + 0x36); p.z = R16(s + 0x38); p.y = R16(s + 0x3A);
        p.fx = (s32)R32(s + 0x44); p.fz = (s32)R32(s + 0x48); p.fy = (s32)R32(s + 0x4C);
        p.sprite = R16(s + 0x0C);
        if (p.x != ownLX || p.y != ownLY)
        {
            if (ownLX != 0xFFFF) { u32 d = frame - ownLFrame; ownFpt = (u8)(d < 2 ? 2 : d > 32 ? 32 : d); }
            ownLX = p.x; ownLY = p.y; ownLFrame = frame;
        }
        p.fpt = ownFpt;
    }
    own = p;
}

bool Bridge::SlotAlive()
{
    return slot && Valid(slot) && (R16(slot) & 1) && R16(slot + 8) == REMOTE_ID;
}

bool Bridge::RemoteShown(PlayerState& out) const
{
    auto* self = const_cast<Bridge*>(this);
    if (!self->SlotAlive()) return false;
    out = {}; out.magic = MAGIC; out.map = own.map;
    out.x = self->R16(slot + 0x36); out.z = self->R16(slot + 0x38); out.y = self->R16(slot + 0x3A);
    out.fx = (s32)self->R32(slot + 0x44); out.fy = (s32)self->R32(slot + 0x4C);
    out.facing = self->R16(slot + 0x18); out.sprite = self->R16(slot + 0x0C);
    return true;
}

// ---------------------------------------------------------------- injected calls

bool Bridge::CpuIdle()
{
    u32 pc = nds.ARM9.R[15];
    return (pc >= IDLE_LO && pc < IDLE_HI) || (pc >= HALT_LO && pc < HALT_HI);
}

// Start a game function on the OS idle thread. It runs during the next emulated frame with IRQs
// masked and returns through STUB, which restores sp/cpsr and resumes the idle loop by itself.
bool Bridge::BeginCall(u32 fn, const u32* regs, const u32* stackArgs, int nStack, CallKind kind)
{
    if (pending != CallKind::None) { stats.callsBusy++; return false; }
    if (!CpuIdle()) return false;
    auto& c = nds.ARM9;
    for (int i = 0; i < 32; i += 4)
        W32(STUB_ADDR + i, STUB[i] | STUB[i+1] << 8 | STUB[i+2] << 16 | (u32)STUB[i+3] << 24);
    W32(DATA_ADDR + 0x0, SENTINEL);
    W32(DATA_ADDR + 0x4, c.R[13]);
    W32(DATA_ADDR + 0x8, c.CPSR & ~0x20u);
    W32(DATA_ADDR + 0xC, IDLE_RESUME);
    for (int i = 0; i < nStack; i++) W32(SCRATCH_SP + 4 * i, stackArgs[i]);
    for (int i = 0; i < 4; i++) c.R[i] = regs[i];
    c.R[13] = SCRATCH_SP;
    c.R[14] = STUB_ADDR | 1;
    c.CPSR |= 0x80;
    c.Halted = 0;
    c.JumpTo(fn | 1);
    pending = kind; pendingFrames = 0;
    return true;
}

void Bridge::PollCall()
{
    if (pending == CallKind::None) return;
    u32 r = R32(DATA_ADDR);
    if (r == SENTINEL)
    {
        if (++pendingFrames > 30) { Say("bw-coop: injected call timed out"); pending = CallKind::None; }
        return;
    }
    if (pending == CallKind::Spawn) { slot = Valid(r) ? r : 0; if (slot) stats.spawns++; }
    else if (pending == CallKind::Delete) { slot = 0; stats.deletes++; }
    pending = CallKind::None;
}

// ---------------------------------------------------------------- presence

static void SetCommand(Bridge* b, u32 s, u16 code, std::function<void(u32,u16)> w16,
                       std::function<u32(u32)> r32, std::function<void(u32,u32)> w32)
{
    w16(s + 0x26, code);
    w16(s + 0x28, 0);
    w32(s + 0x04, (r32(s + 0x04) | 0x200) & ~0x400u);
}

void Bridge::Teleport(const PlayerState& r)
{
    stats.teleports++;
    for (u32 o : {0x30u, 0x36u, 0x3Cu}) { W16(slot + o, r.x); W16(slot + o + 2, r.z); W16(slot + o + 4, r.y); }
    W32(slot + 0x44, r.fx); W32(slot + 0x48, r.fz); W32(slot + 0x4C, r.fy);
    W16(slot + 0x18, r.facing);
}

void Bridge::Pump()
{
    auto w16 = [this](u32 a, u16 v){ W16(a, v); };
    auto r32 = [this](u32 a){ return R32(a); };
    auto w32 = [this](u32 a, u32 v){ W32(a, v); };
    u32 fl = R32(slot + 4);
    bool idle = !(fl & 0x200) || (fl & 0x400);
    if (!idle) return;
    if (queue.empty())
    {
        if (R16(slot + 0x18) != remote.facing)
        { SetCommand(this, slot, 0x24 + (remote.facing & 3), w16, r32, w32); stats.commands++; }
        return;
    }
    const PlayerState& t = queue.front();
    int dx = (int)t.x - (int)R16(slot + 0x36), dy = (int)t.y - (int)R16(slot + 0x3A);
    int d = dx == 0 && dy == -1 ? 0 : dx == 0 && dy == 1 ? 1 : dx == -1 && dy == 0 ? 2 : dx == 1 && dy == 0 ? 3 : -1;
    if (d < 0) { queue.clear(); Teleport(remote); return; }
    u8 fpt = t.fpt;
    queue.pop_front();
    u16 base = fpt >= 12 ? 0x0C : fpt >= 6 ? 0x10 : 0x14;
    SetCommand(this, slot, base + d, w16, r32, w32);
    stats.commands++;
}

void Bridge::Apply()
{
    if (pending == CallKind::None && slot && !SlotAlive()) { slot = 0; queue.clear(); }  // field rebuilt
    bool sameMap = haveRemote && remote.map >= 0 && own.map >= 0 && remote.map == own.map;
    if (pending != CallKind::None) return;
    if (!sameMap)
    {
        queue.clear(); lastTX = lastTY = 0xFFFF;
        if (slot)
        {
            u32 regs[4] = {slot, 0, 0, 0};
            BeginCall(FN_DELETE, regs, nullptr, 0, CallKind::Delete);
        }
        return;
    }
    if (!slot)
    {
        u32 sys = MmdlSys();
        if (!sys) return;
        u32 regs[4] = {sys, remote.x, remote.y, (u32)(remote.facing & 3)};
        u32 st[4] = {REMOTE_ID, remote.sprite, 0, (u32)remote.map};
        if (BeginCall(FN_ADD, regs, st, 4, CallKind::Spawn)) { lastTX = remote.x; lastTY = remote.y; queue.clear(); }
        return;
    }
    if (remote.x != lastTX || remote.y != lastTY)
    {
        queue.push_back(remote); lastTX = remote.x; lastTY = remote.y;
    }
    if (queue.size() > MAX_QUEUE) { queue.clear(); Teleport(remote); return; }
    Pump();
}

void Bridge::OnFrame()
{
    frame++;
    if (!supported) return;
    PollCall();
    ReadOwn();
    NetPump();
    Apply();
}

}
