#include "core/Pif.h"

namespace reality64 {

namespace {

constexpr uint8_t CmdInfo = 0x00;
constexpr uint8_t CmdReadButtons = 0x01;
constexpr uint8_t CmdReset = 0xFF;

constexpr uint8_t ErrorNoDevice = 0x80;  // set in the rx-length byte when nothing answers

constexpr uint8_t TxSkipChannel = 0x00;
constexpr uint8_t TxResetChannel = 0xFD;
constexpr uint8_t TxEnd = 0xFE;
constexpr uint8_t TxPad = 0xFF;

}  // namespace

void processPifCommands(uint8_t* ram, const ControllerProvider& provider) {
    unsigned channel = 0;
    unsigned i = 0;

    // The last byte is the PIF control byte, never part of a command.
    while (i < PifRamSize - 1) {
        const uint8_t tx = ram[i];
        if (tx == TxEnd) break;
        if (tx == TxSkipChannel || tx == TxResetChannel) {
            ++channel;
            ++i;
            continue;
        }
        if (tx == TxPad) {
            ++i;
            continue;
        }

        const unsigned txLen = tx & 0x3F;
        if (i + 2 > PifRamSize - 1) break;
        const unsigned rxLen = ram[i + 1] & 0x3F;
        if (i + 2 + txLen + rxLen > PifRamSize - 1) break;

        const uint8_t cmd = ram[i + 2];
        uint8_t* response = ram + i + 2 + txLen;

        ControllerState pad;
        bool present = false;
        if (channel < static_cast<unsigned>(MaxControllerPorts)) {
            if (provider) {
                pad = provider(static_cast<int>(channel));
                present = pad.connected;
            } else {
                present = (channel == 0);  // headless: a lone idle controller
            }
        }

        if (!present || txLen == 0) {
            ram[i + 1] |= ErrorNoDevice;
        } else if (cmd == CmdInfo || cmd == CmdReset) {
            if (rxLen >= 3) {
                response[0] = 0x05;  // standard controller
                response[1] = 0x00;
                response[2] = 0x02;  // no Controller Pak inserted
            }
        } else if (cmd == CmdReadButtons) {
            if (rxLen >= 4) {
                response[0] = static_cast<uint8_t>(pad.buttons >> 8);
                response[1] = static_cast<uint8_t>(pad.buttons);
                response[2] = static_cast<uint8_t>(pad.stickX);
                response[3] = static_cast<uint8_t>(pad.stickY);
            }
        } else {
            // Pak read/write and anything else: nobody home.
            ram[i + 1] |= ErrorNoDevice;
        }

        i += 2 + txLen + rxLen;
        ++channel;
    }

    ram[PifRamSize - 1] = 0;  // command block consumed
}

}  // namespace reality64
