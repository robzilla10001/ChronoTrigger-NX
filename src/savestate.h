// savestate.h - the SAVE-STATE FRAMEWORK (a host feature, not the ROM's): quick save / load of the running game into slots.
//
// The engine's machine is spread over many places (the scene VM, the host's 128 KB WRAM image, the SRAM, the sound processor, the
// host loop's own variables). A state is therefore a CONTAINER of CHUNKS, one per part, each written and read by a PROVIDER that
// owns its part:
//
//   - A provider registers a 4-letter tag, its chunk VERSION, how to save, how to CHECK a chunk (no side effects) and how to LOAD it.
//   - Saving asks every provider whether it can save NOW (`unavailable` returns the reason, e.g. "not in a battle yet"): if any says no,
//     nothing is written. A state is never half a game.
//   - Loading is TWO-PHASE: the whole file (header, every chunk's CRC, the trailer CRC) is verified and every provider's check() passes
//     before any provider's load() touches the live game. A bad file leaves the game as it was.
//   - Unknown chunks are skipped; a chunk newer than the build or older than the provider's minVersion fails the load, loudly.
//
// Requests (from the mods menu, a button chord, a test) are queued and run at ONE safe point of the frame, CtSsService(), so a state
// is always taken between two whole frames. Slots are files, written to a temporary name and renamed into place.
//
// File (little endian):  "CTNX" u16 fmt u16 nChunks | u32 unixTime u32 frame u16 loc u8 mode u8 0 | u8 thumbW u8 thumbH u32 thumbBytes
//   | thumbnail RGB24 | nChunks x { tag[4] u16 version u16 flags u32 length u32 crc32 data } | u32 crc32 of everything before it.
#ifndef CT_SAVESTATE_H
#define CT_SAVESTATE_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define CT_SS_FMT 1
#define CT_SS_SLOTS 10
#define CT_SS_THUMB_W 64
#define CT_SS_THUMB_H 56

// A growing output buffer for providers.
typedef struct { uint8_t* p; size_t len, cap; int err; } CtSsBuf;
void CtSsPut(CtSsBuf* b, const void* d, size_t n);
void CtSsPutU8(CtSsBuf* b, uint32_t v);
void CtSsPutU16(CtSsBuf* b, uint32_t v);
void CtSsPutU32(CtSsBuf* b, uint32_t v);
void CtSsPutF64(CtSsBuf* b, double v);
// A bounds-checked reader for providers: any read past the end sets err and returns 0.
typedef struct { const uint8_t* p; size_t len, pos; int err; } CtSsReader;
CtSsReader CtSsReaderOf(const uint8_t* d, size_t n);
uint32_t CtSsGetU8(CtSsReader* r);
uint32_t CtSsGetU16(CtSsReader* r);
uint32_t CtSsGetU32(CtSsReader* r);
double   CtSsGetF64(CtSsReader* r);
const uint8_t* CtSsGet(CtSsReader* r, size_t n);        // NULL (and err) past the end

typedef struct CtSsProvider {
  char tag[5];                                          // 4 characters
  uint16_t version;                                     // what save() writes
  uint16_t minVersion;                                  // the oldest chunk check()/load() still read
  int required;                                         // 1: a state without this chunk does not load
  const char* (*unavailable)(void* ctx);                // NULL = can be saved now; else why not (shown to the player)
  int (*save)(void* ctx, CtSsBuf* out);                 // 0 = ok
  int (*check)(void* ctx, const uint8_t* d, size_t n, uint16_t ver);   // 0 = this chunk would load; NO side effects
  int (*load)(void* ctx, const uint8_t* d, size_t n, uint16_t ver);    // 0 = applied; runs only after every check passed
} CtSsProvider;

typedef struct CtSsMeta {
  uint32_t unixTime, frame; uint16_t loc; uint8_t mode;  // when, which engine frame, which location, what it was doing (CT_SS_MODE_*)
  uint8_t thumbW, thumbH; const uint8_t* thumb;          // RGB24, thumbW x thumbH (CT_SS_THUMB_*); NULL = none
} CtSsMeta;
enum { CT_SS_MODE_FIELD = 0, CT_SS_MODE_WORLD = 1, CT_SS_MODE_BATTLE = 2, CT_SS_MODE_MENU = 3 };

typedef struct CtSs CtSs;
CtSs* CtSsNew(void);
void  CtSsFree(CtSs* s);
CtSs* CtSsDefault(void);                                // the game's instance
int   CtSsRegister(CtSs* s, const CtSsProvider* p, void* ctx);          // 0 = ok (tags are unique, at most 32 providers)
// The host describes the running game for the header / the slot list; called when a state is saved.
typedef void (*CtSsMetaFn)(void* ctx, CtSsMeta* m);
void  CtSsSetMetaFn(CtSs* s, CtSsMetaFn fn, void* ctx);
// Where the slot files live (default "states"); created on first save.
void  CtSsSetDir(CtSs* s, const char* dir);

// In-memory image. Save: 0 = ok and *out holds the file (free with CtSsFreeImage), else *why says why nothing was written.
int   CtSsSaveImage(CtSs* s, uint8_t** out, size_t* len, const char** why);
// Load: verifies everything, then applies. 0 = loaded; else *why (the live game is untouched unless a load() itself failed: *partial = 1).
int   CtSsLoadImage(CtSs* s, const uint8_t* img, size_t len, const char** why, int* partial);
void  CtSsFreeImage(uint8_t* img);
// Header and thumbnail of an image (the slot list). 0 = a valid header; thumb receives up to thumbCap bytes.
int   CtSsReadInfo(const uint8_t* img, size_t len, CtSsMeta* m, uint8_t* thumb, size_t thumbCap);

// Slots (files, `<dir>/state<N>.state`): a FIRST LINE of text, the time it was saved ("MM/DD/YYYY HH:MM:SS" + AM/PM, only ever shown to the player), then the
// image. A save with the backup option on first moves the slot's current file to `state<N>.bak`, replacing any old .bak without asking. 0 = ok.
void  CtSsSetBackup(CtSs* s, int on);
int   CtSsSaveSlot(CtSs* s, int slot, const char** why);
int   CtSsLoadSlot(CtSs* s, int slot, const char** why, int* partial);
int   CtSsSlotInfo(CtSs* s, int slot, CtSsMeta* m, uint8_t* thumb, size_t thumbCap);   // 0 = the slot holds a valid state
// The first line of the slot's file, for the menu (it is never parsed): 0 = a state is there and `out` is its time stamp; 1 = missing or blank.
int   CtSsSlotStamp(CtSs* s, int slot, char* out, size_t n);

// Backups of every slot at once: a zip (stored, openable anywhere) of all the `state<N>.state` and `state<N>.bak` files, named after the date and time
// and put in `<dir>/backups/`. Restoring verifies the whole archive first, then extracts it over the current files.
typedef struct { char file[96]; char shown[40]; long mtime; } CtSsBackup;
int   CtSsBackupAll(CtSs* s, char* name, size_t n, const char** why);                 // 0 = ok; `name` = the archive's file name
int   CtSsListBackups(CtSs* s, CtSsBackup* out, int max);                             // newest first; returns how many
int   CtSsRestoreBackup(CtSs* s, const char* file, const char** why);                 // 0 = ok
void  CtSsStampNow(char* out, size_t n);                                              // "MM/DD/YYYY HH:MM:SS" + AM/PM, the menu's format

// Requests, run at the frame's safe point.
enum { CT_SS_NONE = 0, CT_SS_SAVE = 1, CT_SS_LOAD = 2 };
void  CtSsRequest(CtSs* s, int kind, int slot);
int   CtSsPending(CtSs* s);
// Run the queued request (if any). Returns CT_SS_SAVE / CT_SS_LOAD when one ran (the host re-syncs its picture after a load), else 0.
int   CtSsService(CtSs* s);
// The last request's outcome, for the player: text, whether it succeeded, and a counter that changes with each new message.
const char* CtSsMessage(CtSs* s, int* ok, unsigned* serial);

// The container's own checks (round trip, truncation, bit flips, version skew, an unavailable provider, an unknown chunk). 0 = all passed.
int   CtSsSelfTest(void);

uint32_t CtSsCrc32(const uint8_t* d, size_t n);
#ifdef __cplusplus
}
#endif
#endif
