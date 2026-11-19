/*
 * Vigil - vital-sign frame decoding and NEWS2 scoring.
 * SPDX-License-Identifier: MIT
 */
#include "vitals.h"

#include <string.h>

static const char *const k_name[VG_PARAM_COUNT] = { "HR", "SpO2", "RR", "SBP", "TEMP" };
static const char *const k_unit[VG_PARAM_COUNT] = { "bpm", "%", "/min", "mmHg", "C" };
static const double k_min[VG_PARAM_COUNT] = { 0.0, 0.0, 0.0, 0.0, 20.0 };
static const double k_max[VG_PARAM_COUNT] = { 300.0, 100.0, 100.0, 400.0, 45.0 };

const char *vg_param_name(int p) { return (p >= 0 && p < VG_PARAM_COUNT) ? k_name[p] : "?"; }
const char *vg_param_unit(int p) { return (p >= 0 && p < VG_PARAM_COUNT) ? k_unit[p] : ""; }

int vg_sample_parse(const vg_nmea_t *s, vg_sample_t *out)
{
    if (s == NULL || out == NULL || s->start != '$' || !vg_nmea_is(s, "VIT") || s->nfields < 9) {
        return 0;
    }
    memset(out, 0, sizeof(*out));
    long bed;
    if (!vg_nmea_field_int(s, 1, &bed) || bed < 1 || bed > VG_MAX_BEDS) {
        return 0;
    }
    out->bed = (uint8_t)bed;
    for (int p = 0; p < VG_PARAM_COUNT; p++) {
        double v;
        /* A value outside the physically possible range is treated as a sensor fault. */
        if (vg_nmea_field_double(s, 2 + p, &v) && v >= k_min[p] && v <= k_max[p]) {
            out->v[p] = v;
            out->has[p] = 1;
        }
    }
    const char c = s->field[7][0];
    out->acvpu = ((c == 'A' || c == 'C' || c == 'V' || c == 'P' || c == 'U') && s->field[7][1] == '\0') ? c : 0;
    long o2;
    out->o2 = (vg_nmea_field_int(s, 8, &o2) && (o2 == 0 || o2 == 1)) ? (int8_t)o2 : -1;
    return 1;
}

static int band_rr(double v)   { return v <= 8 ? 3 : v <= 11 ? 1 : v <= 20 ? 0 : v <= 24 ? 2 : 3; }
static int band_spo2(double v) { return v <= 91 ? 3 : v <= 93 ? 2 : v <= 95 ? 1 : 0; }
static int band_sbp(double v)  { return v <= 90 ? 3 : v <= 100 ? 2 : v <= 110 ? 1 : v <= 219 ? 0 : 3; }
static int band_hr(double v)   { return v <= 40 ? 3 : v <= 50 ? 1 : v <= 90 ? 0 : v <= 110 ? 1 : v <= 130 ? 2 : 3; }
static int band_temp(double v) { return v <= 35.0 ? 3 : v <= 36.0 ? 1 : v <= 38.0 ? 0 : v <= 39.0 ? 1 : 2; }

void vg_news2_compute(const double v[VG_PARAM_COUNT], const uint8_t has[VG_PARAM_COUNT],
                      char acvpu, int o2, vg_news2_t *out)
{
    memset(out, 0, sizeof(*out));
    for (int p = 0; p < VG_PARAM_COUNT; p++) {
        if (!has[p]) {
            return;
        }
    }
    if (acvpu == 0 || o2 < 0) {
        return;
    }
    out->part[0] = band_rr(v[VG_RR]);
    out->part[1] = band_spo2(v[VG_SPO2]);
    out->part[2] = o2 ? 2 : 0;
    out->part[3] = band_sbp(v[VG_SBP]);
    out->part[4] = band_hr(v[VG_HR]);
    out->part[5] = acvpu == 'A' ? 0 : 3;
    out->part[6] = band_temp(v[VG_TEMP]);
    for (int i = 0; i < 7; i++) {
        out->total += out->part[i];
        out->any3 |= out->part[i] == 3;
    }
    out->valid = 1;
}

const char *vg_news2_band(const vg_news2_t *n)
{
    if (!n->valid) return "N/A";
    if (n->total >= 7) return "HIGH";
    if (n->total >= 5) return "MEDIUM";
    if (n->any3) return "LOW-MEDIUM";
    return "LOW";
}

const char *vg_news2_part_name(int i)
{
    static const char *const nm[7] = { "RR", "SpO2", "O2", "SBP", "HR", "ACVPU", "TEMP" };
    return (i >= 0 && i < 7) ? nm[i] : "?";
}
