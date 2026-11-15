/*
 * Mikron/Philips MIFARE Core Module (MCM) with a virtual MIFARE Classic 1K card
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_MIFARE_MCM_H
#define HW_MISC_MIFARE_MCM_H

#include "hw/sysbus.h"
#include "qom/object.h"

#define TYPE_MIFARE_MCM "mifare-mcm"
OBJECT_DECLARE_SIMPLE_TYPE(MifareMCMState, MIFARE_MCM)

#define MCM_MMIO_SIZE     0x10
#define MCM_FIFO_SIZE     32
#define MCM_KEYSETS       4
#define MCM_SECTORS       16
#define MIFARE_1K_SIZE    1024
#define MIFARE_BLOCK_SIZE 16

struct MifareMCMState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;

    /* MCM registers */
    uint8_t stacon;         /* last value written to STACON */
    uint8_t status;         /* value read from STACON */
    uint8_t enable;
    uint8_t bcnts;
    uint8_t bcntr;
    uint8_t baudrate;
    uint8_t toc;
    uint8_t mode;
    uint8_t crcdata;
    uint8_t crcstacon;
    uint8_t keystacon;
    uint8_t keyadr;
    uint8_t rcode;

    /* Frame being sent to the card and the card's answer */
    uint8_t tx[MCM_FIFO_SIZE];
    uint32_t tx_len;
    bool tx_done;
    uint8_t rx[MCM_FIFO_SIZE];
    uint32_t rx_len;
    uint32_t rx_pos;

    /* Key RAM: keyset x (A, B) x sector, loaded through KEYDATA */
    uint8_t keys[MCM_KEYSETS][2][MCM_SECTORS][6];
    uint8_t keybuf[12];
    uint32_t keybuf_len;

    /* Virtual card */
    bool present;
    uint8_t card_state;
    int32_t auth_sector;
    uint8_t pending_cmd;
    uint8_t pending_block;
    uint8_t value_buf[MIFARE_BLOCK_SIZE];
    bool value_valid;
    uint8_t mem[MIFARE_1K_SIZE];

    /* Properties */
    char *card_file;
    bool writeback;
    bool check_keys;
    uint32_t uid;
};

#endif
