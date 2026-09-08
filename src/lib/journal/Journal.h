// Append-only flash journal for mount position and event logging.
//
// Lives in its own flash partition and does not touch the OnStepX NV volume, so a
// bug here can cost at most a re-home, never park position or settings.
//
// Design constraints, measured on UMi 17 hardware (see docs/DESIGN-enhancements.md):
//   - a 16-byte append costs ~257 us typical, 2.2 ms worst
//   - a 4 KB sector erase costs ~64 ms, which would be visible in a guide trace
// So erase NEVER happens on the write path. Instead a runway of pre-erased sectors
// is kept ahead of the frontier and topped up only while the mount is not tracking.
#pragma once

#include "../../Common.h"

#if JOURNAL == ON

#define JOURNAL_REC_SIZE     16
#define JOURNAL_SECTOR       4096
#define JOURNAL_PER_SECTOR   (JOURNAL_SECTOR / JOURNAL_REC_SIZE)

// record types
#define JR_HEADER            0x4A // 'J' - format header, always the first record
#define JR_POSITION          1    // mount position heartbeat or state change
#define JR_EVENT             2    // flight recorder entry

// A partition can contain foreign data - an old filesystem, or residue from a
// previous use of the same address range. Without a header check that data can
// pass a CRC by coincidence and be mistaken for journal records, which is
// exactly what happened with leftover test data at 0x291000. The header is
// written when the journal formats itself and verified on every boot.
#define JOURNAL_MAGIC        0x4A524E4CUL  // "JRNL"
// 2: sector 0 reserved for the header, data is a ring over sectors 1..N-1
// 3: same layout, but format erases the whole partition so no stale records survive
#define JOURNAL_FORMAT       3

// event codes for JR_EVENT
#define JE_BOOT              1
#define JE_TRACK_ON          2
#define JE_TRACK_OFF         3
#define JE_SLEW_START        4
#define JE_SLEW_END          5
#define JE_PARK              6
#define JE_UNPARK            7
#define JE_HOME              8
#define JE_LIMIT_TRIP        9
#define JE_APS_TRIP         10
#define JE_ERROR            11

#pragma pack(push, 1)
typedef struct {
  uint32_t seq;                 // monotonic; the highest valid record wins
  uint8_t  type;                // JR_*
  uint8_t  payload[9];
  uint16_t crc;                 // CRC16-CCITT over the preceding 14 bytes
} JournalRecord;

typedef struct {                // JR_POSITION payload
  float   a1;
  float   a2;
  uint8_t flags;                // bit0 trusted, bit1 tracking, bits4-7 mount type
} JournalPosition;

typedef struct {                // JR_EVENT payload
  uint8_t  code;
  uint32_t arg;
  uint32_t millis;
} JournalEvent;
#pragma pack(pop)

class Journal {
  public:
    // Locate the partition, find the write frontier and load the newest valid
    // record. Returns false if the partition is missing, in which case every
    // other call is a no-op and the mount behaves exactly as it does today.
    bool init();

    bool ready() { return active; }

    // Append a record. Never erases; fails rather than stalling if the runway
    // is exhausted. Cheap enough to call while tracking.
    bool writePosition(float a1, float a2, uint8_t flags);
    bool writeEvent(uint8_t code, uint32_t arg);

    // Newest valid position record found at init, if any.
    bool haveRestored() { return restoredValid; }
    JournalPosition restored;

    // Call periodically. Only does work when mountIdle is true; tops the runway
    // back up to JOURNAL_RUNWAY_SECTORS, four sectors at a time.
    void maintain(bool mountIdle);

    // Records that can still be written before an erase would be needed.
    uint32_t runwayRecords();

    uint32_t sequence() { return nextSeq; }
    bool     runwayExhausted() { return exhausted; }

  private:
    bool append(uint8_t type, const void *payload);
    bool sectorIsBlank(uint32_t sector);
    bool headerValid();
    bool format();

    uint32_t nextSector(uint32_t s) { return (s + 1 >= sectorCount) ? 1 : s + 1; }

    const void *part = nullptr;   // esp_partition_t, opaque here
    bool     active = false;
    bool     restoredValid = false;
    bool     exhausted = false;
    uint32_t nextSeq = 1;
    uint32_t sectorCount = 0;
    // Sector 0 holds the header and nothing else, so it is never erased and the
    // header cannot be lost to a wrap. Data occupies sectors 1..sectorCount-1 as
    // a ring.
    uint32_t writeSector = 1;
    uint32_t writeSlot = 0;
    uint32_t blankAhead = 0;      // consecutive erased sectors ahead of the frontier
};

extern Journal journal;

#endif
