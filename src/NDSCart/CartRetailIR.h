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

#ifndef NDSCART_CARTRETAILIR_H
#define NDSCART_CARTRETAILIR_H

#include "CartRetail.h"
#include <functional>

namespace melonDS::NDSCart
{

// bw-coop: optional infrared link. When set, the IR cart sends each transmitted transfer as one packet
// through g_IRSend and fills receive transfers from g_IRRecv (returns length, 0 = nothing received).
extern std::function<void(const u8* data, int len)> g_IRSend;
extern std::function<int(u8* buf, int max)> g_IRRecv;

// CartRetailIR -- SPI IR device and SRAM
class CartRetailIR : public CartRetail
{
public:
    CartRetailIR(const u8* rom, u32 len, u32 chipid, u32 irversion, bool badDSiDump, ROMListEntry romparams, std::unique_ptr<u8[]>&& sram, u32 sramlen, void* userdata);
    CartRetailIR(std::unique_ptr<u8[]>&& rom, u32 len, u32 chipid, u32 irversion, bool badDSiDump, ROMListEntry romparams, std::unique_ptr<u8[]>&& sram, u32 sramlen, void* userdata);
    ~CartRetailIR() override;

    void Reset() override;

    void DoSavestate(Savestate* file) override;

    void SPISelect() override;
    void SPIRelease() override;
    u8 SPITransmitReceive(u8 val) override;

private:
    u32 IRVersion = 0;
    u8 IRCmd = 0;
    u32 IRPos = 0;
    u8 IRBuf[0xB8] {};   // bw-coop: current IR transfer
    int IRLen = 0;
};

}

#endif
