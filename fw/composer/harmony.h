/* harmony.h - keys, diatonic chord graph, voice-leading */
#ifndef AP_HARMONY_H
#define AP_HARMONY_H

#include <stdint.h>
#include "prng.h"

/* A key is a major-scale pitch collection on `tonic` with a modal centre:
 * centre 0 = Ionian, 1 Dorian, 3 Lydian, 4 Mixolydian, 5 Aeolian. */
typedef struct { int8_t tonic; int8_t centre; } hkey_t;

enum { CH_TRIAD, CH_SEV, CH_ADD9, CH_SUS2, CH_SUS4 };
typedef struct { int8_t deg; uint8_t type; } chord_t;   /* deg relative to the centre */

#define CHORD_VOCAB_N 14
extern const chord_t CHORD_VOCAB[CHORD_VOCAB_N];

int    key_pc_of_degree(hkey_t k, int deg);              /* absolute degree 0..6 */
int    key_has_pc(hkey_t k, int pc);
int    key_centre_pc(hkey_t k);                          /* pitch class of the modal centre */
hkey_t key_neighbor(hkey_t k, prng_t *r);
int    key_is_neighbor(hkey_t a, hkey_t b);

int    chord_pcs(hkey_t k, chord_t c, int8_t out[4]);     /* returns tone count, out[0] = root */
int    chord_valid(hkey_t k, chord_t c);                  /* no dim/aug/tritone sus/b9 */
int    chord_common_tones(hkey_t k, chord_t a, chord_t b);
/* weighted walk; tension_q15 in [-32768, 32767]: high tension leaves home more */
int    chord_next(hkey_t k, int cur, int32_t tension_q15, prng_t *r);
int    chord_home(hkey_t k, int cur, prng_t *r);          /* best tonic chord after a key change */

/* Pentatonic subset of the key (major pent from the tonic, or minor pent
 * from the centre when centre is Aeolian/Dorian). Returns count (5). */
int    key_pentatonic(hkey_t k, int8_t out[5]);

/* Voice-leading. Pitches in semitones (MIDI). `prev` holds nprev upper voices
 * (may be 0). Writes bass to out[0] and n_upper upper voices to out[1..],
 * sorted ascending. Upper voices move at most VL_MAX_JUMP from their
 * predecessors except when the window is crowded (measured: 0.03% of moves),
 * and never more than VL_HARD_MAX. To avoid unisons the window [lo, hi] is
 * soft by VL_HEADROOM. */
#define VL_MAX_JUMP 7
#define VL_HARD_MAX 11
#define VL_HEADROOM 4   /* a crowded window may be exceeded by this much to avoid unisons */
void voice_lead(const int8_t *pcs, int npc, int bass_prev,
                const int32_t *prev, int nprev, int n_upper,
                int lo, int hi, int bass_lo, int bass_hi,
                int32_t *out, prng_t *r);

#endif
