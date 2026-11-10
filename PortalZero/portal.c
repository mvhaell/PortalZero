// Emulation of a Skylanders Portal of Power (Wii)
//
// Behaves like the real portal / the portal emulation of the Wii emulator Dolphin.
// The Wii sends commands as reports; the first byte is the command:
//   'A' activate/deactivate portal       -> reply 'A'
//   'R' reset / query portal ID          -> reply 'R'
//   'S' request status                   -> no own reply (status is sent continuously)
//   'Q' read a figure block              -> reply 'Q' + 16 bytes
//   'W' write a figure block             -> reply 'W'
//   'C' portal LED color                 -> no reply
//   'J' LED with fade                    -> reply 'J'
//   'L' trap LED                         -> no reply
//   'M' query audio firmware             -> reply 'M'
// Without a pending reply the portal continuously sends status reports ('S').
#include <string.h>
#include "portal.h"

#define RESP_QUEUE   32
#define STATUS_QUEUE 8

// Status per figure (2 bits): 0 = absent, 1 = present, 2 = being removed, 3 = just added
enum { ST_NONE = 0, ST_READY = 1, ST_REMOVING = 2, ST_ADDED = 3 };

typedef struct {
    bool     used;
    uint8_t  status;
    uint8_t  queue[STATUS_QUEUE];
    uint8_t  qlen;
    bool     dirty;
    uint32_t seq;                    // number of write commands
    uint32_t last_write_ms;
    uint32_t last_id;                // serial of the figure that last used this slot
    uint8_t  data[PORTAL_FIGURE_SIZE];
} slot_t;

static slot_t   slots[PORTAL_SLOTS];
static uint8_t  resp[RESP_QUEUE][PORTAL_REPORT_SIZE];
static int      resp_head, resp_count;
static bool     active;
static uint8_t  counter;

static void push_status(slot_t *s, uint8_t st) {
    if (s->qlen < STATUS_QUEUE) s->queue[s->qlen++] = st;
}

static void queue_response(const uint8_t *data, int len) {
    if (resp_count >= RESP_QUEUE) return;
    uint8_t *r = resp[(resp_head + resp_count) % RESP_QUEUE];
    memset(r, 0, PORTAL_REPORT_SIZE);
    memcpy(r, data, len);
    resp_count++;
}

static uint32_t serial_of(const uint8_t *data) {
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8) | ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

static void build_status(uint8_t out[PORTAL_REPORT_SIZE]) {
    uint32_t bits = 0;
    for (int i = PORTAL_SLOTS - 1; i >= 0; i--) {
        slot_t *s = &slots[i];
        if (s->qlen > 0) {              // apply the next queued status change
            s->status = s->queue[0];
            memmove(s->queue, s->queue + 1, --s->qlen);
        }
        bits = (bits << 2) | (s->status & 3);
    }
    memset(out, 0, PORTAL_REPORT_SIZE);
    out[0] = 'S';
    out[1] = bits & 0xFF;
    out[2] = (bits >> 8) & 0xFF;
    out[3] = (bits >> 16) & 0xFF;
    out[4] = (bits >> 24) & 0xFF;
    out[5] = counter++;
    out[6] = active ? 0x01 : 0x00;
}

// The game activates the portal: re-announce all figures already on it
// (this is what the real portal does - otherwise the game would not "see" them).
// Once active, the portal stays active - exactly like Dolphin.
static void activate(void) {
    if (active) return;
    for (int i = 0; i < PORTAL_SLOTS; i++) {
        slot_t *s = &slots[i];
        if (s->status & 1) {
            push_status(s, ST_ADDED);
            push_status(s, ST_READY);
        }
    }
    active = true;
}

void portal_init(void) {
    memset(slots, 0, sizeof(slots));
    resp_head = resp_count = 0;
    active = false;
    counter = 0;
}

void portal_handle_command(const uint8_t *buf, uint16_t len, uint32_t now_ms) {
    if (len < 1) return;
    uint8_t r[PORTAL_REPORT_SIZE] = {0};

    switch (buf[0]) {
    case 'A': {                                  // activate
        // The game also sends "A 00" from time to time. The real portal (and Dolphin)
        // only acknowledges it and stays active - otherwise the figures would be
        // re-announced on every A 00 / A 01 and the game would keep starting over.
        uint8_t on = (len > 1) ? buf[1] : 0;
        r[0] = 'A'; r[1] = on; r[2] = 0xFF; r[3] = 0x77;
        queue_response(r, 4);
        activate();
        break;
    }
    case 'R':                                    // reset / ID
        r[0] = 'R'; r[1] = 0x02; r[2] = 0x1B;
        queue_response(r, 3);
        break;
    case 'Q': {                                  // read block
        if (len < 3) break;
        uint8_t n = buf[1] & 0x0F, block = buf[2];
        r[0] = 'Q';
        r[2] = block;
        if (block < 64 && (slots[n].status & ST_READY)) {
            r[1] = 0x10 | n;
            memcpy(&r[3], &slots[n].data[block * 16], 16);
        } else {
            r[1] = 0x01;
        }
        queue_response(r, 19);
        break;
    }
    case 'W': {                                  // write block
        if (len < 19) break;
        uint8_t n = buf[1] & 0x0F, block = buf[2];
        r[0] = 'W';
        r[2] = block;
        if (block < 64 && slots[n].used && (slots[n].status & 1)) {
            r[1] = 0x10 | n;
            memcpy(&slots[n].data[block * 16], &buf[3], 16);
            slots[n].dirty = true;
            slots[n].seq++;
            slots[n].last_write_ms = now_ms;
        } else {
            r[1] = 0x01;
        }
        queue_response(r, 3);
        break;
    }
    case 'J':                                    // LED with fade
        r[0] = 'J';
        queue_response(r, 1);
        break;
    case 'M':                                    // audio firmware
        r[0] = 'M'; r[1] = (len > 1) ? buf[1] : 0; r[2] = 0x00; r[3] = 0x19;
        queue_response(r, 4);
        break;
    case 'S':                                    // status is sent continuously anyway
    case 'C':                                    // LED color: no reply
    case 'L':                                    // trap LED: no reply
    default:
        break;
    }
}

void portal_next_report(uint8_t out[PORTAL_REPORT_SIZE]) {
    if (resp_count > 0) {
        memcpy(out, resp[resp_head], PORTAL_REPORT_SIZE);
        resp_head = (resp_head + 1) % RESP_QUEUE;
        resp_count--;
    } else {
        build_status(out);
    }
}

// a slot is free when no figure is on it (also while one is being removed)
static bool slot_free(const slot_t *s) {
    return !s->used && (s->status & 1) == 0;
}

int portal_place(const uint8_t *data) {
    // like the real portal: the same figure prefers its previous slot
    uint32_t id = serial_of(data);
    int found = -1;
    for (int i = 0; i < PORTAL_SLOTS; i++) {
        if (!slot_free(&slots[i])) continue;
        if (slots[i].last_id == id) { found = i; break; }
        if (found < 0) found = i;
    }
    if (found < 0) return -1;

    slot_t *s = &slots[found];
    memcpy(s->data, data, PORTAL_FIGURE_SIZE);
    s->used = true;
    s->dirty = false;
    s->seq = 0;
    s->last_id = id;
    s->status = ST_ADDED;
    push_status(s, ST_ADDED);                    // after a possibly pending removal notice
    push_status(s, ST_READY);
    return found;
}

void portal_remove(int slot) {
    if (slot < 0 || slot >= PORTAL_SLOTS || !slots[slot].used) return;
    slot_t *s = &slots[slot];
    s->used = false;
    s->dirty = false;
    if (s->status & ST_READY) {                  // like Dolphin: only announce if it was present
        s->status = ST_REMOVING;
        push_status(s, ST_REMOVING);
        push_status(s, ST_NONE);
    }
}

bool portal_slot_used(int slot) {
    return slot >= 0 && slot < PORTAL_SLOTS && slots[slot].used;
}

int portal_count(void) {
    int n = 0;
    for (int i = 0; i < PORTAL_SLOTS; i++) if (slots[i].used) n++;
    return n;
}

bool portal_is_active(void) { return active; }

int portal_dirty_slot(uint32_t now_ms, uint32_t idle_ms) {
    for (int i = 0; i < PORTAL_SLOTS; i++)
        if (slots[i].used && slots[i].dirty && (uint32_t)(now_ms - slots[i].last_write_ms) >= idle_ms)
            return i;
    return -1;
}

bool portal_slot_dirty(int slot) {
    return slot >= 0 && slot < PORTAL_SLOTS && slots[slot].used && slots[slot].dirty;
}

const uint8_t *portal_slot_data(int slot) { return slots[slot].data; }

uint32_t portal_slot_seq(int slot) {
    return (slot >= 0 && slot < PORTAL_SLOTS) ? slots[slot].seq : 0;
}

void portal_clear_dirty_if(int slot, uint32_t seq) {
    if (slot >= 0 && slot < PORTAL_SLOTS && slots[slot].seq == seq) slots[slot].dirty = false;
}
