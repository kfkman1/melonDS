/*
    Copyright 2016-2026 melonDS team

    This file is part of melonDS.

    melonDS is free software: you can redistribute it and/or modify it under
    the terms of the GNU General Public License as published by the Free
    Software Foundation, either version 3 of the License, or (at your option)
    any later version.

    melonDS is distributed in the hope that it will be useful, but WITHOUT ANY
    WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
    FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with melonDS. If not, see http://www.gnu.org/licenses/.
*/

#include "CartRetailIR.h"
#include "../NDS.h"
#include "../Utils.h"

// CartRetailIR: NDS cartridge with IR transceiver (Pokémon games)
// the IR transceiver is connected to the SPI interface, with a passthrough command for SRAM access

namespace melonDS
{
using Platform::Log;
using Platform::LogLevel;

namespace NDSCart
{

CartRetailIR::CartRetailIR(const u8* rom, u32 len, u32 chipid, u32 irversion, bool badDSiDump, ROMListEntry romparams, std::unique_ptr<u8[]>&& sram, u32 sramlen, void* userdata) :
    CartRetailIR(CopyToUnique(rom, len), len, chipid, irversion, badDSiDump, romparams, std::move(sram), sramlen, userdata)
{
}

CartRetailIR::CartRetailIR(
    std::unique_ptr<u8[]>&& rom,
    u32 len,
    u32 chipid,
    u32 irversion,
    bool badDSiDump,
    ROMListEntry romparams,
    std::unique_ptr<u8[]>&& sram,
    u32 sramlen,
    void* userdata
) :
    CartRetail(std::move(rom), len, chipid, badDSiDump, romparams, std::move(sram), sramlen, userdata, CartType::RetailIR),
    IRVersion(irversion)
{
}

CartRetailIR::~CartRetailIR() = default;

void CartRetailIR::Reset()
{
    CartRetail::Reset();

    IRCmd = 0;
}

void CartRetailIR::DoSavestate(Savestate* file)
{
    CartRetail::DoSavestate(file);

    file->Var8(&IRCmd);
}

std::function<void(const u8*, int)> g_IRSend;
std::function<int(u8*, int)> g_IRRecv;

void CartRetailIR::SPISelect()
{
    CartRetail::SPISelect();
    IRPos = 0;
    IRLen = 0;
}

void CartRetailIR::SPIRelease()
{
    // bw-coop: a send command transmits its bytes as one IR packet when the transfer ends
    if (IRCmd == 0x02 && IRPos > 1 && g_IRSend) g_IRSend(IRBuf, IRLen);
    IRLen = 0;
    CartRetail::SPIRelease();
}

u8 CartRetailIR::SPITransmitReceive(u8 val)
{
    if (IRPos == 0)
    {
        IRCmd = val;
        IRPos++;
        return 0;
    }

    u8 ret = 0;
    switch (IRCmd)
    {
    case 0x00: // pass-through
        ret = CartRetail::SPITransmitReceive(val);
        break;

    case 0x01: // receive: count byte, then the received bytes
        if (IRPos == 1)
        {
            IRLen = g_IRRecv ? g_IRRecv(IRBuf, sizeof(IRBuf)) : 0;
            if (IRLen < 0 || IRLen > 0xB5) IRLen = 0;
            ret = (u8)IRLen;
        }
        else
            ret = (IRPos - 2 < (u32)IRLen) ? IRBuf[IRPos - 2] : 0;
        break;

    case 0x02: // send: collect the bytes of one IR packet
        if (IRLen < (int)sizeof(IRBuf)) IRBuf[IRLen++] = val;
        break;

    case 0x08: // ID
        ret = 0xAA;
        break;
    }

    IRPos++;
    return ret;
}

}

}
