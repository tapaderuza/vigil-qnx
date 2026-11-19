/*
 * Vigil - real-time collision vigil for QNX 8.0
 * NMEA 0183 sentence framing: checksum, tag blocks, field splitting,
 * and the handful of own-ship sentences we need (RMC, GGA, VTG, HDT).
 *
 * Portable C11, no allocation, no QNX dependency: this file builds on the
 * host for unit tests and on QNX with qcc for the target.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef VG_NMEA_H
#define VG_NMEA_H

#include <stddef.h>
#include <stdint.h>

#define VG_NMEA_MAX_LEN    160  /* NMEA says 82; real AIS feeds exceed it, so be lenient */
#define VG_NMEA_MAX_FIELDS 32

typedef enum {
    VG_NMEA_OK           = 0,
    VG_NMEA_ERR_FORMAT   = -1,
    VG_NMEA_ERR_CHECKSUM = -2,
    VG_NMEA_ERR_LENGTH   = -3
} vg_nmea_status_t;

typedef struct {
    char        buf[VG_NMEA_MAX_LEN + 1]; /* body copy; fields are NUL-terminated in place */
    const char *field[VG_NMEA_MAX_FIELDS];
    int         nfields;
    char        start;     /* '$' (data) or '!' (encapsulated, e.g. AIS) */
    char        talker[3]; /* "GP", "AI", ... ("P" for proprietary) */
    char        type[6];   /* "RMC", "VDM", ... */
} vg_nmea_t;

/* XOR of every byte in s[0..len). */
uint8_t vg_nmea_xor(const char *s, size_t len);

/*
 * Parse one sentence. Accepts an optional NMEA 4.x tag block prefix
 * ("\s:abc,c:123*hh\") and trailing CR/LF/whitespace. A checksum is
 * mandatory for '!' sentences and verified when present on '$' sentences.
 */
vg_nmea_status_t vg_nmea_parse(const char *line, size_t len, vg_nmea_t *out);

/* Is this sentence talker-agnostic type `type` (e.g. "RMC")? */
int vg_nmea_is(const vg_nmea_t *s, const char *type);

/* Field helpers. Return 1 when the field exists and is non-empty. */
int vg_nmea_field_double(const vg_nmea_t *s, int idx, double *out);
int vg_nmea_field_int(const vg_nmea_t *s, int idx, long *out);

/* "ddmm.mmmm" + 'N'/'S' (or "dddmm.mmmm" + 'E'/'W') to signed degrees. */
int vg_nmea_latlon(const char *value, const char *hemi, double *deg_out);

/* Own-ship fix extracted from RMC/GGA/VTG/HDT. Flags tell what was present. */
typedef struct {
    unsigned has_pos : 1;
    unsigned has_sog : 1;
    unsigned has_cog : 1;
    unsigned has_heading : 1;
    double   lat_deg, lon_deg;
    double   sog_kn, cog_deg, heading_deg;
} vg_nmea_fix_t;

/* Returns 1 if the sentence contributed own-ship data, 0 if not relevant. */
int vg_nmea_ownship(const vg_nmea_t *s, vg_nmea_fix_t *fix);

#endif /* VG_NMEA_H */
