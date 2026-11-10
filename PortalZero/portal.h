// Emulation of a Skylanders portal (Wii) - protocol logic without hardware dependencies
//
// Note: not thread-safe. If USB and the main program run on different CPU cores,
// the caller must protect every access with a lock.
#ifndef PORTAL_H
#define PORTAL_H

#include <stdbool.h>
#include <stdint.h>

#define PORTAL_SLOTS        16      // a real portal handles this many figures at once
#define PORTAL_FIGURE_SIZE  1024    // 64 blocks x 16 bytes (MIFARE Classic 1K)
#define PORTAL_REPORT_SIZE  32

void portal_init(void);

// command from the Wii (SET_REPORT or OUT endpoint)
void portal_handle_command(const uint8_t *buf, uint16_t len, uint32_t now_ms);

// get the next report for the IN endpoint: pending replies first, otherwise status
void portal_next_report(uint8_t out[PORTAL_REPORT_SIZE]);

// place a figure on the portal: returns the slot or -1 if full
int  portal_place(const uint8_t *data);
void portal_remove(int slot);
bool portal_slot_used(int slot);
int  portal_count(void);
bool portal_is_active(void);                // the game has activated the portal

// saving: slot with changes that have been idle for idle_ms (or -1)
int  portal_dirty_slot(uint32_t now_ms, uint32_t idle_ms);
bool portal_slot_dirty(int slot);
const uint8_t *portal_slot_data(int slot);
uint32_t portal_slot_seq(int slot);         // counts every write command from the Wii
// only mark as saved if nothing new was written since the copy was taken
void portal_clear_dirty_if(int slot, uint32_t seq);

#endif
