// Append-only flash journal. See Journal.h for the design constraints.

#include "Journal.h"

#if JOURNAL == ON

#include "esp_partition.h"

#define P ((const esp_partition_t *)part)
#define SLOT_ADDR(sec, slot) ((size_t)(sec) * JOURNAL_SECTOR + (size_t)(slot) * JOURNAL_REC_SIZE)

static uint16_t crc16(const uint8_t *d, size_t n) {
  uint16_t c = 0xFFFF;
  for (size_t i = 0; i < n; i++) {
    c ^= (uint16_t)d[i] << 8;
    for (int b = 0; b < 8; b++) c = (c & 0x8000) ? (uint16_t)((c << 1) ^ 0x1021) : (uint16_t)(c << 1);
  }
  return c;
}

static bool recordValid(const JournalRecord &r) {
  // JR_HEADER must be listed here. Omitting it made headerValid() reject the
  // journal's own header, so every boot reformatted and erased the last session.
  if (r.type != JR_POSITION && r.type != JR_EVENT && r.type != JR_HEADER) return false;
  return crc16((const uint8_t *)&r, JOURNAL_REC_SIZE - 2) == r.crc;
}

static bool recordBlank(const JournalRecord &r) {
  const uint8_t *b = (const uint8_t *)&r;
  for (size_t i = 0; i < JOURNAL_REC_SIZE; i++) if (b[i] != 0xFF) return false;
  return true;
}

bool Journal::sectorIsBlank(uint32_t sector) {
  // Records are only ever appended forwards, so a sector in use always has a
  // non-blank first slot.
  JournalRecord r;
  if (esp_partition_read(P, SLOT_ADDR(sector, 0), &r, sizeof(r)) != ESP_OK) return false;
  return recordBlank(r);
}

bool Journal::headerValid() {
  JournalRecord r;
  if (esp_partition_read(P, 0, &r, sizeof(r)) != ESP_OK) return false;
  if (!recordValid(r) || r.type != JR_HEADER) return false;
  uint32_t magic; uint8_t fmt;
  memcpy(&magic, r.payload, 4);
  memcpy(&fmt, r.payload + 4, 1);
  return magic == JOURNAL_MAGIC && fmt == JOURNAL_FORMAT;
}

bool Journal::format() {
  VLF("MSG: Journal, formatting");
  // Erase the whole partition, not just the runway. Erasing only the front left
  // records from the previous format sitting in the tail sectors; their sequence
  // numbers outrank the fresh ones, so the next boot's frontier scan would pick a
  // stale sector and restore a position from a dead session. Costs ~3 s, and only
  // ever on a first boot or a JOURNAL_FORMAT bump.
  if (esp_partition_erase_range(P, 0, (size_t)sectorCount * JOURNAL_SECTOR) != ESP_OK) {
    DLF("WRN: Journal, format erase failed");
    return false;
  }

  JournalRecord r;
  memset(&r, 0, sizeof(r));
  r.seq = 0;
  r.type = JR_HEADER;
  uint32_t magic = JOURNAL_MAGIC; uint8_t fmt = JOURNAL_FORMAT;
  memcpy(r.payload, &magic, 4);
  memcpy(r.payload + 4, &fmt, 1);
  r.crc = crc16((const uint8_t *)&r, JOURNAL_REC_SIZE - 2);
  if (esp_partition_write(P, 0, &r, sizeof(r)) != ESP_OK) return false;

  nextSeq = 1;
  writeSector = 1;
  writeSlot = 0;
  blankAhead = sectorCount - 2;  // every data sector but the frontier is now blank
  exhausted = false;
  return true;
}

bool Journal::init() {
  part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, JOURNAL_PARTITION);
  if (!part) {
    DLF("WRN: Journal, partition '" JOURNAL_PARTITION "' not found - journalling disabled");
    active = false;
    return false;
  }
  sectorCount = P->size / JOURNAL_SECTOR;
  if (sectorCount < 4) { DLF("WRN: Journal, partition too small"); active = false; return false; }

  if (!headerValid()) {
    VLF("MSG: Journal, no valid header - partition holds foreign or unformatted data");
    restoredValid = false;
    active = format();
    return active;
  }

  // Find the frontier. "First blank sector" is wrong once writing has wrapped,
  // because blank sectors then sit in the middle of the ring. The most recently
  // started sector is the one whose first record has the highest sequence
  // number, and sequence numbers never reset.
  uint32_t frontier = 0; uint32_t bestFirstSeq = 0; bool any = false;
  for (uint32_t s = 1; s < sectorCount; s++) {
    JournalRecord r;
    if (esp_partition_read(P, SLOT_ADDR(s, 0), &r, sizeof(r)) != ESP_OK) continue;
    if (recordBlank(r)) continue;
    uint32_t sq;
    if (recordValid(r)) sq = r.seq;
    else {
      // torn write in the first slot: fall back to the first valid record here
      sq = 0; bool found = false;
      for (uint32_t i = 1; i < JOURNAL_PER_SECTOR && !found; i++) {
        JournalRecord q;
        if (esp_partition_read(P, SLOT_ADDR(s, i), &q, sizeof(q)) != ESP_OK) break;
        if (recordBlank(q)) break;
        if (recordValid(q)) { sq = q.seq; found = true; }
      }
      if (!found) continue;
    }
    if (!any || sq > bestFirstSeq) { bestFirstSeq = sq; frontier = s; any = true; }
  }
  if (!any) { frontier = 1; }

  // Scan the frontier sector for the newest record and the first free slot.
  uint32_t bestSeq = 0; uint32_t slot = 0;
  restoredValid = false;
  for (uint32_t i = 0; i < JOURNAL_PER_SECTOR; i++) {
    JournalRecord r;
    if (esp_partition_read(P, SLOT_ADDR(frontier, i), &r, sizeof(r)) != ESP_OK) break;
    if (recordBlank(r)) break;
    slot = i + 1;
    if (!recordValid(r)) continue;             // torn write, skip
    if (r.seq >= bestSeq) {
      bestSeq = r.seq;
      if (r.type == JR_POSITION) { memcpy(&restored, r.payload, sizeof(restored)); restoredValid = true; }
    }
  }

  // The frontier sector can hold no position at all - a boot event landing in a
  // freshly rolled sector, then the power cut - and the last position is then one
  // sector back. Without this the mount silently comes up at home, which is the
  // exact failure the journal exists to prevent. Only runs when needed.
  uint32_t back = frontier;
  for (uint32_t n = 1; !restoredValid && n + 1 < sectorCount; n++) {
    back = (back <= 1) ? sectorCount - 1 : back - 1;
    if (sectorIsBlank(back)) break;              // reached the erased runway
    for (uint32_t i = 0; i < JOURNAL_PER_SECTOR; i++) {
      JournalRecord r;
      if (esp_partition_read(P, SLOT_ADDR(back, i), &r, sizeof(r)) != ESP_OK) break;
      if (recordBlank(r)) break;
      if (recordValid(r) && r.type == JR_POSITION) {
        memcpy(&restored, r.payload, sizeof(restored));
        restoredValid = true;                    // keep going; the last one is newest
      }
    }
  }

  nextSeq = bestSeq + 1;
  writeSector = frontier;
  writeSlot = slot;

  // Count the sectors already erased ahead of us. Without this the runway looks
  // empty on every boot and up to 15 already-blank sectors get needlessly
  // re-erased, which is both slow and pure wear.
  blankAhead = 0;
  uint32_t s = nextSector(frontier);
  while (s != frontier && sectorIsBlank(s)) { blankAhead++; s = nextSector(s); }
  exhausted = false;
  active = true;

  VF("MSG: Journal, "); V(sectorCount); VF(" sectors, frontier "); V(frontier);
  VF(" slot "); V(slot); VF(", next seq "); V(nextSeq);
  VF(", blank ahead "); VL(blankAhead);
  if (restoredValid) { VLF("MSG: Journal, recovered a position record"); }
  return true;
}

bool Journal::append(uint8_t type, const void *payload) {
  if (!active) return false;

  if (writeSlot >= JOURNAL_PER_SECTOR) {
    // Move to the next sector, which must already be erased. Erasing here would
    // cost ~64ms and is banned on the write path.
    if (blankAhead == 0) {
      if (!exhausted) { DLF("WRN: Journal, runway exhausted - pausing until the mount is idle"); }
      exhausted = true;
      return false;
    }
    writeSector = nextSector(writeSector);
    writeSlot = 0;
    blankAhead--;
  }

  JournalRecord r;
  memset(&r, 0, sizeof(r));
  r.seq = nextSeq;
  r.type = type;
  memcpy(r.payload, payload, sizeof(r.payload));
  r.crc = crc16((const uint8_t *)&r, JOURNAL_REC_SIZE - 2);

  if (esp_partition_write(P, SLOT_ADDR(writeSector, writeSlot), &r, sizeof(r)) != ESP_OK) {
    DLF("WRN: Journal, write failed");
    return false;
  }
  nextSeq++;
  writeSlot++;
  return true;
}

bool Journal::writePosition(float a1, float a2, uint8_t flags) {
  JournalPosition p; p.a1 = a1; p.a2 = a2; p.flags = flags;
  return append(JR_POSITION, &p);
}

bool Journal::writeEvent(uint8_t code, uint32_t arg) {
  JournalEvent e; e.code = code; e.arg = arg; e.millis = millis();
  return append(JR_EVENT, &e);
}

uint32_t Journal::runwayRecords() {
  if (!active) return 0;
  return (JOURNAL_PER_SECTOR - writeSlot) + blankAhead * JOURNAL_PER_SECTOR;
}

void Journal::maintain(bool mountIdle) {
  if (!active || !mountIdle) return;
  if (blankAhead >= (uint32_t)JOURNAL_RUNWAY_SECTORS) return;
  if (sectorCount < 4) return;

  // First sector that is not already blank, walking forward from the frontier.
  uint32_t s = writeSector;
  for (uint32_t i = 0; i <= blankAhead; i++) s = nextSector(s);

  // Never erase the frontier itself, and never erase sector 0.
  if (s == writeSector || s == 0) return;

  // erase_range needs contiguous addresses, so stop at the end of the ring and
  // let the next call continue after the wrap.
  uint32_t batch = 4;
  if (s + batch > sectorCount) batch = sectorCount - s;
  for (uint32_t i = 0; i < batch; i++) {
    if (((s + i) % sectorCount) == writeSector) { batch = i; break; }
  }
  if (batch == 0) return;

  if (esp_partition_erase_range(P, (size_t)s * JOURNAL_SECTOR,
                                (size_t)batch * JOURNAL_SECTOR) == ESP_OK) {
    blankAhead += batch;
    exhausted = false;
    VF("MSG: Journal, erased "); V(batch); VF(" sectors at "); V(s);
    VF(", blank ahead now "); VL(blankAhead);
  } else {
    DLF("WRN: Journal, erase failed");
  }
}

Journal journal;

#endif
