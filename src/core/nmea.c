/*
 * Vigil - NMEA 0183 framing and own-ship sentences.
 * SPDX-License-Identifier: MIT
 */
#include "nmea.h"

#include <stdlib.h>
#include <string.h>

uint8_t vg_nmea_xor(const char *s, size_t len)
{
    uint8_t x = 0;
    for (size_t i = 0; i < len; i++) {
        x ^= (uint8_t)s[i];
    }
    return x;
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

vg_nmea_status_t vg_nmea_parse(const char *line, size_t len, vg_nmea_t *out)
{
    if (line == NULL || out == NULL) {
        return VG_NMEA_ERR_FORMAT;
    }
    memset(out, 0, sizeof(*out));

    /* Trim trailing CR/LF/space/NUL. */
    while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n' ||
                       line[len - 1] == ' ' || line[len - 1] == '\0')) {
        len--;
    }
    /* Skip an NMEA 4.x tag block: \...\ */
    if (len > 0 && line[0] == '\\') {
        const char *end = memchr(line + 1, '\\', len - 1);
        if (end == NULL) {
            return VG_NMEA_ERR_FORMAT;
        }
        size_t skip = (size_t)(end - line) + 1;
        line += skip;
        len -= skip;
    }
    if (len < 2 || (line[0] != '$' && line[0] != '!')) {
        return VG_NMEA_ERR_FORMAT;
    }
    out->start = line[0];

    const char *star = memchr(line, '*', len);
    size_t body_len;
    if (star != NULL) {
        body_len = (size_t)(star - line) - 1;
        if ((size_t)(star - line) + 3 > len) {
            return VG_NMEA_ERR_FORMAT; /* need two hex digits after '*' */
        }
        int hi = hexval(star[1]);
        int lo = hexval(star[2]);
        if (hi < 0 || lo < 0) {
            return VG_NMEA_ERR_FORMAT;
        }
        if (vg_nmea_xor(line + 1, body_len) != (uint8_t)((hi << 4) | lo)) {
            return VG_NMEA_ERR_CHECKSUM;
        }
    } else {
        if (out->start == '!') {
            return VG_NMEA_ERR_FORMAT; /* AIS must carry a checksum */
        }
        body_len = len - 1;
    }
    if (body_len == 0) {
        return VG_NMEA_ERR_FORMAT;
    }
    if (body_len > VG_NMEA_MAX_LEN) {
        return VG_NMEA_ERR_LENGTH;
    }

    memcpy(out->buf, line + 1, body_len);
    out->buf[body_len] = '\0';

    /* Split on commas, in place. */
    char *p = out->buf;
    out->field[out->nfields++] = p;
    for (; *p != '\0'; p++) {
        if (*p == ',') {
            *p = '\0';
            if (out->nfields >= VG_NMEA_MAX_FIELDS) {
                return VG_NMEA_ERR_LENGTH;
            }
            out->field[out->nfields++] = p + 1;
        }
    }

    /* Address field: "GPRMC" -> talker "GP", type "RMC"; "PGRME" -> "P" + "GRME". */
    const char *addr = out->field[0];
    size_t alen = strlen(addr);
    if (alen < 2) {
        return VG_NMEA_ERR_FORMAT;
    }
    if (addr[0] == 'P') {
        out->talker[0] = 'P';
        strncpy(out->type, addr + 1, sizeof(out->type) - 1);
    } else {
        if (alen < 5) {
            return VG_NMEA_ERR_FORMAT;
        }
        out->talker[0] = addr[0];
        out->talker[1] = addr[1];
        strncpy(out->type, addr + 2, sizeof(out->type) - 1);
    }
    return VG_NMEA_OK;
}

int vg_nmea_is(const vg_nmea_t *s, const char *type)
{
    return s != NULL && strcmp(s->type, type) == 0;
}

static const char *field_or_null(const vg_nmea_t *s, int idx)
{
    if (s == NULL || idx < 0 || idx >= s->nfields || s->field[idx][0] == '\0') {
        return NULL;
    }
    return s->field[idx];
}

int vg_nmea_field_double(const vg_nmea_t *s, int idx, double *out)
{
    const char *f = field_or_null(s, idx);
    if (f == NULL) {
        return 0;
    }
    char *end = NULL;
    double v = strtod(f, &end);
    if (end == f || *end != '\0') {
        return 0;
    }
    *out = v;
    return 1;
}

int vg_nmea_field_int(const vg_nmea_t *s, int idx, long *out)
{
    const char *f = field_or_null(s, idx);
    if (f == NULL) {
        return 0;
    }
    char *end = NULL;
    long v = strtol(f, &end, 10);
    if (end == f || *end != '\0') {
        return 0;
    }
    *out = v;
    return 1;
}

int vg_nmea_latlon(const char *value, const char *hemi, double *deg_out)
{
    if (value == NULL || hemi == NULL || value[0] == '\0' || hemi[0] == '\0') {
        return 0;
    }
    char *end = NULL;
    double raw = strtod(value, &end);
    if (end == value || *end != '\0' || raw < 0.0) {
        return 0;
    }
    double deg = (double)(long)(raw / 100.0);
    double min = raw - deg * 100.0;
    if (min >= 60.0) {
        return 0;
    }
    double v = deg + min / 60.0;
    int is_lat = (hemi[0] == 'N' || hemi[0] == 'S');
    switch (hemi[0]) {
    case 'N': case 'E': break;
    case 'S': case 'W': v = -v; break;
    default: return 0;
    }
    if (is_lat && (v > 90.0 || v < -90.0)) return 0;
    if (!is_lat && (v > 180.0 || v < -180.0)) return 0;
    *deg_out = v;
    return 1;
}

int vg_nmea_ownship(const vg_nmea_t *s, vg_nmea_fix_t *fix)
{
    if (s == NULL || fix == NULL || s->start != '$') {
        return 0;
    }
    memset(fix, 0, sizeof(*fix));
    double v;

    if (vg_nmea_is(s, "RMC")) {
        /* $xxRMC,time,status,lat,N,lon,E,sog,cog,date,magvar,E[,mode]*hh */
        if (s->nfields < 10 || strcmp(s->field[2], "A") != 0) {
            return 0; /* 'V' = navigation receiver warning: ignore the fix */
        }
        if (vg_nmea_latlon(s->field[3], s->field[4], &fix->lat_deg) &&
            vg_nmea_latlon(s->field[5], s->field[6], &fix->lon_deg)) {
            fix->has_pos = 1;
        }
        if (vg_nmea_field_double(s, 7, &v)) { fix->sog_kn = v; fix->has_sog = 1; }
        if (vg_nmea_field_double(s, 8, &v)) { fix->cog_deg = v; fix->has_cog = 1; }
        return fix->has_pos || fix->has_sog || fix->has_cog;
    }
    if (vg_nmea_is(s, "GGA")) {
        /* $xxGGA,time,lat,N,lon,E,quality,... ; quality 0 = invalid */
        long q = 0;
        if (s->nfields < 7 || !vg_nmea_field_int(s, 6, &q) || q == 0) {
            return 0;
        }
        if (vg_nmea_latlon(s->field[2], s->field[3], &fix->lat_deg) &&
            vg_nmea_latlon(s->field[4], s->field[5], &fix->lon_deg)) {
            fix->has_pos = 1;
        }
        return fix->has_pos;
    }
    if (vg_nmea_is(s, "VTG")) {
        /* $xxVTG,cogT,T,cogM,M,sogN,N,sogK,K[,mode]*hh */
        if (vg_nmea_field_double(s, 1, &v)) { fix->cog_deg = v; fix->has_cog = 1; }
        if (vg_nmea_field_double(s, 5, &v)) { fix->sog_kn = v; fix->has_sog = 1; }
        return fix->has_sog || fix->has_cog;
    }
    if (vg_nmea_is(s, "HDT")) {
        /* $xxHDT,heading,T*hh */
        if (vg_nmea_field_double(s, 1, &v) && v >= 0.0 && v < 360.0) {
            fix->heading_deg = v;
            fix->has_heading = 1;
        }
        return fix->has_heading;
    }
    return 0;
}
