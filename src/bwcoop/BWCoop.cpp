/*
    bw-coop: co-op presence bridge for Pokemon Black (US, rev 0, IRBO) on melonDS.
    GPL-3.0-or-later. Addresses and routines are documented, with evidence, in the
    Co-op Anything write-up games/pokemon-black-white/NOTES.md.
*/
#include "BWCoop.h"
#include "../net/LAN.h"

#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <enet/enet.h>
#include "NDS.h"
#include "NDSCart.h"
#include "NDSCart/CartRetailIR.h"
#include "SPI_Firmware.h"
#include "Wifi.h"
#include <random>

namespace bwcoop
{

// ---- game constants (Pokemon Black US rev 0) ----
static constexpr u32 FIELD_PTR  = 0x02146180;   // -> field system
static constexpr u32 MMDL_OFF   = 0x1738;       // field + this -> movement-model system
static constexpr u32 SLOT_SIZE  = 0x100;
static constexpr u32 MAPID      = 0x0224F90C;   // u16 current map id
static constexpr u32 FN_ADD     = 0x0216CE6C;   // slot* AddCharacter(sys, x, y, dir, [sp] id, sprite, move, map)
static constexpr u32 FN_DELETE  = 0x0216CF2C;   // void DeleteCharacter(slot*)
static constexpr u32 GSYS_PTR   = 0x02146248;   // -> GameSystem (+0x18 current event, +0x20 Field)
static constexpr u32 FN_CALLIRC = 0x021559E4;
// IR menu (overlay loaded only while it is open): Trade's "at least two Pokemon" check, cmp r0,#2 -> cmp r0,#1
static constexpr u32 TRADE2_CMP = 0x021969C2;   // GameEvent* CallIRC(gsys, field, event, create): IR menu event
// Trade screen (overlay loaded only while it is open): "this is your last healthy Pokemon" check,
// if (countActive(party) == 1 && fromParty) return TRUE -> movs r0,#1 becomes movs r0,#0
static constexpr u32 TRADE_LAST = 0x021D9532;   // cmp r0,#1; bne; cmp r6,#0; beq; movs r0,#1
static constexpr u32 IDLE_LO = 0x02085B20, IDLE_HI = 0x02085B30, HALT_LO = 0x020882D0, HALT_HI = 0x020882E0;
static constexpr u32 IDLE_RESUME = 0x02085B28;  // ARM: bl halt; b loop
static constexpr u16 REMOTE_ID  = 0x80;
static constexpr u32 MAGIC      = 0x31435742;   // "BWC1"
static constexpr u32 EXIT_MAGIC = 0x58435742;    // "BWCX": {magic, IR menu event step} for exit ordering
static constexpr u32 IRC_EVENT_CB = 0x02155479;  // IR menu GameEvent callback (Thumb); work = event+0xC, step = event+8
static constexpr u32 INVITE_MAGIC = 0x49435742;  // "BWCI": {magic, kind} kind 1 = open IR menu, 2 = busy

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
Bridge::~Bridge()
{
    Disconnect();
    melonDS::NDSCart::g_IRSend = nullptr;
    melonDS::NDSCart::g_IRRecv = nullptr;
    melonDS::g_WifiAnyChannel = false;
}

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
    if (supported)
    {
        auto& fw = nds.GetFirmware();
        if (fw.GetHeader().MacAddr == melonDS::DEFAULT_MAC)
        {
            std::random_device rd;
            u8 mac[6] = {0x00, 0x09, 0xBF, (u8)rd(), (u8)rd(), (u8)rd()};
            SetMac(mac);
            char b[64]; snprintf(b, sizeof b, "bw-coop: using MAC 00:09:BF:%02X:%02X:%02X", mac[3], mac[4], mac[5]);
            Say(b);
        }
        melonDS::g_WifiAnyChannel = true;    // see Wifi.cpp: two linked consoles, any channel
        {
            // see net/LAN.cpp: frames that waited longer than this are dropped as stale (upstream 16 ms)
            const char* st = getenv("BWCOOP_STALEMS");
            melonDS::g_LANStaleMs = st ? (u32)atoi(st) : 100;
            melonDS::g_LANKeepDisconnect = true;
        }
        melonDS::NDSCart::g_IRSend = [this](const u8* d, int n) { IRSend(d, n); };
        melonDS::NDSCart::g_IRRecv = [this](u8* b, int m) { return IRRecv(b, m); };
    }
    return supported;
}

// ---------------------------------------------------------------- network

bool Bridge::Host(int port)
{
    Disconnect();
    if (!g_EnetInit) { if (enet_initialize() != 0) return false; g_EnetInit = true; }
    net = new Net;
    ENetAddress addr; addr.host = ENET_HOST_ANY; addr.port = (enet_uint16)port;
    net->host = enet_host_create(&addr, 1, 2, 0, 0);
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
    net->host = enet_host_create(nullptr, 1, 2, 0, 0);
    if (!net->host) { Disconnect(); return false; }
    ENetAddress addr;
    if (enet_address_set_host(&addr, hostname.c_str()) != 0) { Say("bw-coop: bad host " + hostname); Disconnect(); return false; }
    addr.port = (enet_uint16)port;
    net->peer = enet_host_connect(net->host, &addr, 2, 0);
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
            if (ev.channelID == 1)
            {
                IRPacket p; p.due = frame + irDelay; p.len = (int)std::min<size_t>(ev.packet->dataLength, sizeof p.data);
                memcpy(p.data, ev.packet->data, p.len);
                irIn.push_back(p); stats.irRecv++;
            }
            else if (ev.packet->dataLength == 8 && ((u32*)ev.packet->data)[0] == EXIT_MAGIC)
            {
                partnerIrcSeq = ((u32*)ev.packet->data)[1];
                if (partnerIrcSeq > partnerIrcMax) partnerIrcMax = partnerIrcSeq;
            }
            else if (ev.packet->dataLength == 8 && ((u32*)ev.packet->data)[0] == INVITE_MAGIC)
            {
                u32 kind = ((u32*)ev.packet->data)[1];
                stats.invitesRecv++;
                if (kind == 1)
                {
                    u32 gs = R32(GSYS_PTR);
                    bool free = own.map >= 0 && Valid(gs) && R32(gs + 0x18) == 0 && pending == CallKind::None;
                    if (free) { Say("bw-coop: your partner wants to battle or trade"); StartIR(); }
                    else SendInvite(2);
                }
                else if (kind == 2) Say("bw-coop: your partner is busy right now");
            }
            else if (ev.packet->dataLength == sizeof(PlayerState))
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

// ---------------------------------------------------------------- infrared link

static bool IRLog() { static int on = -1; if (on < 0) on = getenv("BWCOOP_IRLOG") != nullptr; return on; }
static void IRDump(const char* dir, u32 frame, u64 cyc, const u8* d, int n)
{
    fprintf(stderr, "IR %s f=%u c=%llu n=%d:", dir, frame, (unsigned long long)cyc, n);
    for (int i = 0; i < n && i < 48; i++) fprintf(stderr, " %02x", d[i] ^ 0xAA);
    fprintf(stderr, "\n");
}

void Bridge::IRSend(const u8* data, int len)
{
    if (IRLog()) IRDump("tx", frame, nds.ARM9Timestamp, data, len);
    if (!Connected() || len <= 0) return;
    ENetPacket* pk = enet_packet_create(data, len, ENET_PACKET_FLAG_RELIABLE);
    enet_peer_send(net->peer, 1, pk);
    enet_host_flush(net->host);
    stats.irSent++;
}

int Bridge::IRRecv(u8* buf, int max)
{
    // Called from inside the emulated frame; deliver one packet per receive transfer.
    if (irIn.empty() || (s32)(frame - irIn.front().due) < 0) return 0;
    IRPacket& p = irIn.front();
    int n = std::min(p.len, max);
    memcpy(buf, p.data, n);
    if (IRLog()) IRDump("rx", frame, nds.ARM9Timestamp, p.data, n);
    irIn.pop_front();
    return n;
}

void Bridge::SetMac(const u8 mac[6])
{
    auto& fw = nds.GetFirmware();
    for (int i = 0; i < 6; i++) fw.GetHeader().MacAddr[i] = mac[i];
    fw.UpdateChecksums();
    for (int i = 0; i < 6; i++) nds.ARM9Write8(0x027FFCF4 + i, mac[i]);   // OS_GetMacAddress copy
}

void Bridge::SendInvite(u32 kind)
{
    if (!Connected()) return;
    u32 msg[2] = {INVITE_MAGIC, kind};
    enet_peer_send(net->peer, 0, enet_packet_create(msg, sizeof msg, ENET_PACKET_FLAG_RELIABLE));   // channel 1 is the IR stream
    enet_host_flush(net->host);
    if (kind == 1) stats.invitesSent++;
}

// Leaving a link session (battle or trade) in the right order. Both games count 60 frames after their final sync
// and then shut wireless down. On real hardware both do that within the same frame. Two emulators are not in lockstep:
// the guest console runs its count a little slower (it waits on the host's frames), so the host closes the session
// first and the guest, still mid-shutdown, waits forever on a black screen. Real hardware tolerates a guest leaving
// first, so: hold each game's count at 58 until the partner has reached the same step, and hold the wireless host
// until the guest has fully shut down (its step has moved past 20). Never longer than 10 seconds.
void Bridge::ExitGate()
{
    u32 seq = 0, work = 0;
    u32 gs = R32(GSYS_PTR);
    if (Valid(gs))
    {
        u32 ev = R32(gs + 0x18);
        if (Valid(ev) && R32(ev + 4) == IRC_EVENT_CB) { seq = R32(ev + 8); work = R32(ev + 0xC); }
    }
    if (Connected() && (seq != lastIrcSeqSent || (seq && frame % 30 == 0)))
    {
        u32 msg[2] = {EXIT_MAGIC, seq};
        enet_peer_send(net->peer, 0, enet_packet_create(msg, sizeof msg, ENET_PACKET_FLAG_RELIABLE));
        lastIrcSeqSent = seq;
    }
    if (seq == 0) { exitHold = 0; if (partnerIrcSeq == 0) partnerIrcMax = 0; return; }
    if (seq != 19 || !Valid(work)) return;
    u32 count = R32(work + 0xE4);
    if (count < 58) return;
    bool partnerDone = partnerIrcSeq > 20 || (partnerIrcSeq == 0 && partnerIrcMax >= 19);
    bool wait = partnerIrcSeq < 19 && !partnerDone;
    if (nds.Wifi.IsMPHost() && !partnerDone) wait = true;
    if (wait && Connected() && exitHold < 600)
    {
        if (exitHold == 0) Say(nds.Wifi.IsMPHost() ? "bw-coop: waiting for partner to leave the link first"
                                                   : "bw-coop: waiting for partner to finish");
        exitHold++;
        W32(work + 0xE4, 58);
    }
}

// A pressed this frame (edge), player free on the field, facing the tile where the partner's trainer stands.
void Bridge::CheckInteract()
{
    bool a = !(nds.KeyInput & 1);           // KeyInput bit 0 = A, 0 when pressed
    bool edge = a && !prevA;
    prevA = a;
    if (inviteCooldown) inviteCooldown--;
    if (!edge || inviteCooldown || !Connected() || !SlotAlive() || pending != CallKind::None || irWanted) return;
    u32 gs = R32(GSYS_PTR);
    if (!Valid(gs) || R32(gs + 0x18) != 0) return;          // a game event (talk, menu, warp) is running
    if (own.map < 0 || !haveRemote || remote.map != own.map) return;
    static const int DX[4] = {0, 0, -1, 1}, DY[4] = {-1, 1, 0, 0};
    int f = own.facing & 3;
    int tx = (int)own.x + DX[f], ty = (int)own.y + DY[f];
    if (R16(slot + 0x36) != tx || R16(slot + 0x3A) != ty) return;
    Say("bw-coop: opening battle / trade with your partner");
    SendInvite(1);
    StartIR();
    inviteCooldown = 120;
}

bool Bridge::StartIR()
{
    if (!supported) return false;
    irWanted = true; irWantedFrames = 0;      // started from OnFrame as soon as the game allows
    return true;
}

bool Bridge::TryStartIR()
{
    if (pending != CallKind::None) return false;
    u32 gs = R32(GSYS_PTR);
    if (!Valid(gs) || R32(gs + 0x18) != 0) return false;   // another event is running
    u32 field = R32(gs + 0x20);
    if (!Valid(field)) return false;
    u32 regs[4] = {gs, field, 0, 1};
    return BeginCall(FN_CALLIRC, regs, nullptr, 0, CallKind::IRC);
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
    // The field's memory survives under link/battle screens; only trust it while the field is open.
    u32 gs = R32(GSYS_PTR);
    u32 s = (Valid(gs) && Valid(R32(gs + 0x20))) ? PlayerSlot() : 0;
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
    {   // never inject while a game event runs (talks, menus, warps, link screens)
        u32 gs = R32(GSYS_PTR);
        if (!Valid(gs) || R32(gs + 0x18) != 0 || !Valid(R32(gs + 0x20))) return false;
    }
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
    else if (pending == CallKind::IRC)
    {
        u32 gs = R32(GSYS_PTR);
        if (Valid(r) && Valid(gs) && R32(gs + 0x18) == 0) { W32(gs + 0x18, r); stats.irEvents++; }
    }
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

// Owner's rule change: allow IR trades with a single Pokemon. Patched only when the IR menu code is present,
// recognised by the exact instructions around the check (cmp r0,#2; bge; ... movs r1,#0x2C message id). The trade
// screen itself has a second check that refuses to send away your last healthy Pokemon; that one is lifted too.
static void PatchTradeRule(Bridge* b, std::function<u16(u32)> r16, std::function<void(u32,u16)> w16)
{
    if (r16(TRADE2_CMP) == 0x2802 && r16(TRADE2_CMP + 2) == 0xDA0D && r16(TRADE2_CMP + 8) == 0x212C)
        w16(TRADE2_CMP, 0x2801);
    if (r16(TRADE_LAST) == 0x2801 && r16(TRADE_LAST + 2) == 0xD103 && r16(TRADE_LAST + 4) == 0x2E00 &&
        r16(TRADE_LAST + 6) == 0xD001 && r16(TRADE_LAST + 8) == 0x2001 && r16(TRADE_LAST - 6) == 0x5828)
        w16(TRADE_LAST + 8, 0x2000);
}

void Bridge::OnFrame()
{
    frame++;
    if (!supported) return;
    PatchTradeRule(this, [this](u32 a){ return R16(a); }, [this](u32 a, u16 v){ W16(a, v); });
    PollCall();
    ReadOwn();
    NetPump();
    ExitGate();
    CheckInteract();
    if (irWanted)
    {
        if (TryStartIR()) irWanted = false;
        else if (++irWantedFrames > 300) { irWanted = false; Say("bw-coop: could not open the IR menu"); }
    }
    Apply();
}

}
