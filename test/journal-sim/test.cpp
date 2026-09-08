#include "Journal.h"
#include "esp_partition.h"
#include <setjmp.h>
#include <vector>

int g_verbose = 0;
jmp_buf g_pwr;
static unsigned long g_ms = 0;
unsigned long millis() { return g_ms += 7; }

static int fails = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("  FAIL: %s\n", msg); fails++; } } while(0)

static void blankFlash() { memset(g_flash, 0xFF, FLASH_BYTES); }

// read a record straight from the fake flash
static JournalRecord raw(uint32_t sec, uint32_t slot) {
  JournalRecord r; memcpy(&r, g_flash + sec*4096 + slot*16, 16); return r;
}
static uint16_t crc16t(const uint8_t *d, size_t n) {
  uint16_t c = 0xFFFF;
  for (size_t i = 0; i < n; i++) { c ^= (uint16_t)d[i] << 8;
    for (int b = 0; b < 8; b++) c = (c & 0x8000) ? (uint16_t)((c<<1)^0x1021) : (uint16_t)(c<<1); }
  return c;
}
static bool headerLooksRight() {
  JournalRecord r = raw(0, 0);
  if (r.type != JR_HEADER) return false;
  if (crc16t((const uint8_t*)&r, 14) != r.crc) return false;
  uint32_t magic; uint8_t fmt;
  memcpy(&magic, r.payload, 4); memcpy(&fmt, r.payload+4, 1);
  return magic == JOURNAL_MAGIC && fmt == JOURNAL_FORMAT;
}
// highest sequence number present anywhere in the ring
static uint32_t maxSeqOnFlash() {
  uint32_t m = 0;
  for (uint32_t s = 1; s < 64; s++) for (uint32_t i = 0; i < 256; i++) {
    JournalRecord r = raw(s, i);
    if (r.type != JR_POSITION && r.type != JR_EVENT) continue;
    if (crc16t((const uint8_t*)&r, 14) != r.crc) continue;
    if (r.seq > m) m = r.seq;
  }
  return m;
}

// ---- 1. fresh format -------------------------------------------------------
static void t_freshFormat() {
  printf("1. fresh blank partition\n");
  blankFlash(); g_eraseSectors = 0;
  CHECK(journal.init(), "init on blank flash");
  CHECK(headerLooksRight(), "header written and valid");
  CHECK(g_eraseSectors == 64, "format erased the whole partition");
  CHECK(journal.sequence() == 1, "sequence starts at 1");
  CHECK(journal.runwayRecords() == 256 + 62*256, "full runway after format");
  printf("   erased %lu sectors, runway %u records\n", g_eraseSectors, journal.runwayRecords());
}

// ---- 2. stale records from an older format --------------------------------
static void t_staleResidue() {
  printf("2. stale records from a previous format in the tail\n");
  blankFlash();
  // plant plausible high-sequence records in sectors 40..45, as a format-1
  // journal would have left behind
  for (uint32_t s = 40; s <= 45; s++) for (uint32_t i = 0; i < 256; i++) {
    JournalRecord r; memset(&r, 0, sizeof(r));
    r.seq = 900000 + s*256 + i; r.type = JR_POSITION;
    float a1 = 3.14159f, a2 = -1.0f;
    memcpy(r.payload, &a1, 4); memcpy(r.payload+4, &a2, 4); r.payload[8] = 0x03;
    r.crc = crc16t((const uint8_t*)&r, 14);
    memcpy(g_flash + s*4096 + i*16, &r, 16);
  }
  CHECK(journal.init(), "init with foreign data");
  CHECK(headerLooksRight(), "header valid after format");
  CHECK(maxSeqOnFlash() == 0, "no stale record survived the format");
  // reboot: frontier must not be a stale sector
  journal.writePosition(0.5f, 0.25f, 0x03);
  CHECK(journal.init(), "reboot");
  CHECK(journal.sequence() == 2, "sequence follows our own record, not the stale ones");
  CHECK(journal.haveRestored(), "restored our own position");
  CHECK(journal.restored.a1 == 0.5f, "restored the right position");
  printf("   next seq after reboot %u, restored a1=%.3f\n", journal.sequence(), journal.restored.a1);
}

// ---- 3. many laps of the ring ---------------------------------------------
static void t_wrap() {
  printf("3. sustained load, several full laps of the ring\n");
  blankFlash();
  journal.init();
  const int N = 120000;             // ~7.4 laps of 16128 records
  int refused = 0, wrote = 0;
  uint32_t lastSeq = 0; bool seqOk = true;
  for (int i = 0; i < N; i++) {
    if (journal.writePosition((float)i, (float)-i, 0x03)) wrote++; else refused++;
    if (journal.sequence() <= lastSeq) seqOk = false;
    lastSeq = journal.sequence();
    journal.maintain(true);         // idle: runway tops up
    CHECK(headerLooksRight() || (fails > 20), "header survives");
    if (fails > 20) break;
  }
  CHECK(seqOk, "sequence strictly increasing");
  CHECK(refused == 0, "no record refused while idle");
  CHECK(headerLooksRight(), "header intact after all laps");
  printf("   wrote %d, refused %d, laps ~%.1f, erases %lu sectors\n",
         wrote, refused, (double)N / (63*256), g_eraseSectors);
  // reboot mid-ring and confirm we land on the true frontier
  uint32_t before = journal.sequence();
  CHECK(journal.init(), "reboot after wrapping");
  CHECK(journal.sequence() == before, "sequence recovered exactly across reboot");
  CHECK(journal.haveRestored(), "position recovered after wrap");
  CHECK(journal.restored.a1 == (float)(N-1), "recovered the newest position");
  printf("   reboot: seq %u (was %u), restored a1=%.0f (expect %d)\n",
         journal.sequence(), before, journal.restored.a1, N-1);
}

// ---- 4. power cut mid-write ------------------------------------------------
static void t_tornWrite() {
  printf("4. power cut part-way through a record write\n");
  blankFlash();
  journal.init();
  for (int i = 0; i < 700; i++) { journal.writePosition((float)i, 0, 0x03); journal.maintain(true); }
  uint32_t good = journal.sequence();
  g_tornPartial = 1; g_powerCut = 0;
  if (setjmp(g_pwr) == 0) { journal.writePosition(999.0f, 0, 0x03); CHECK(false, "should have cut"); }
  g_powerCut = -1; g_tornPartial = 0;
  CHECK(journal.init(), "init after torn write");
  CHECK(headerLooksRight(), "header fine after torn write");
  CHECK(journal.haveRestored(), "still restored a position");
  CHECK(journal.restored.a1 == 699.0f, "torn record rejected, previous one used");
  CHECK(journal.sequence() >= good, "sequence did not go backwards");
  printf("   restored a1=%.0f (expect 699), seq %u\n", journal.restored.a1, journal.sequence());
}

// ---- 5. frontier sector holding only an event ------------------------------
static void t_eventOnlyFrontier() {
  printf("5. frontier sector contains only an event record\n");
  blankFlash();
  journal.init();
  // fill exactly to a sector boundary with positions
  for (int i = 0; i < 256; i++) { journal.writePosition((float)i, 0, 0x03); journal.maintain(true); }
  // one event rolls into the next sector and is the only thing in it
  journal.writeEvent(JE_BOOT, 0);
  CHECK(journal.init(), "reboot");
  CHECK(journal.haveRestored(), "position found by walking back a sector");
  CHECK(journal.restored.a1 == 255.0f, "found the newest position, one sector back");
  printf("   restored a1=%.0f (expect 255)\n", journal.restored.a1);
}

// ---- 6. runway exhaustion with no idle time --------------------------------
static void t_exhaustion() {
  printf("6. tracking forever, never idle\n");
  blankFlash();
  journal.init();
  int wrote = 0;
  while (journal.writePosition(1.0f, 2.0f, 0x03)) wrote++;   // never call maintain()
  CHECK(journal.runwayExhausted(), "reports exhausted rather than stalling");
  CHECK(headerLooksRight(), "header intact at exhaustion");
  printf("   %d records before exhaustion (~%.1f h at 10 s)\n", wrote, wrote*10.0/3600);
  journal.maintain(true);
  CHECK(!journal.runwayExhausted(), "recovers once idle");
  CHECK(journal.writePosition(3.0f, 4.0f, 0x03), "writes again after top-up");
  printf("   recovered after one idle maintain()\n");
}

int main() {
  t_freshFormat();
  t_staleResidue();
  t_wrap();
  t_tornWrite();
  t_eventOnlyFrontier();
  t_exhaustion();
  printf("\n%s (%d failures)\n", fails ? "FAILURES" : "all checks passed", fails);
  return fails != 0;
}
