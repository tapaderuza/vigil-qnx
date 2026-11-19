/*
 * Vigil - vital-sign samples and the NEWS2 early warning score.
 *
 * Frames are NMEA-style sentences (so they reuse the framing and checksum of nmea.c):
 *
 *     $VGVIT,<bed>,<hr>,<spo2>,<rr>,<sbp>,<temp>,<acvpu>,<o2>*hh
 *
 *   bed    1..VG_MAX_BEDS
 *   hr     heart rate, beats/min            (empty = sensor not delivering)
 *   spo2   oxygen saturation, %             (empty = probe off)
 *   rr     respiration rate, breaths/min
 *   sbp    systolic blood pressure, mmHg
 *   temp   temperature, degrees C
 *   acvpu  consciousness: A alert, C confusion, V voice, P pain, U unresponsive
 *   o2     1 if the patient is on supplemental oxygen, else 0
 *
 * NEWS2 is the Royal College of Physicians' National Early Warning Score 2 (2017), published
 * and free to implement. It is used here as an illustration of a clinical scoring rule; Vigil
 * is a software exercise, not a medical device.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef VG_VITALS_H
#define VG_VITALS_H

#include <stdint.h>

#include "nmea.h"

#define VG_MAX_BEDS 8

typedef enum {
    VG_HR = 0, VG_SPO2, VG_RR, VG_SBP, VG_TEMP, VG_PARAM_COUNT
} vg_param_t;

typedef struct {
    uint8_t bed;
    uint8_t has[VG_PARAM_COUNT];
    double  v[VG_PARAM_COUNT];
    char    acvpu;       /* 0 = not delivered */
    int8_t  o2;          /* -1 = not delivered */
} vg_sample_t;

const char *vg_param_name(int p);
const char *vg_param_unit(int p);

/* Returns 1 and fills *out for a valid $xxVIT sentence, 0 otherwise (range-checked). */
int vg_sample_parse(const vg_nmea_t *s, vg_sample_t *out);

typedef struct {
    int valid;       /* all seven NEWS2 inputs were available */
    int part[7];     /* RR, SpO2, air/oxygen, SBP, pulse, consciousness, temperature */
    int total;       /* 0..20 */
    int any3;        /* some single parameter scored 3 */
} vg_news2_t;

void vg_news2_compute(const double v[VG_PARAM_COUNT], const uint8_t has[VG_PARAM_COUNT],
                      char acvpu, int o2, vg_news2_t *out);

/* Clinical risk band of an aggregate score: "LOW", "LOW-MEDIUM", "MEDIUM", "HIGH". */
const char *vg_news2_band(const vg_news2_t *n);
const char *vg_news2_part_name(int i);

#endif /* VG_VITALS_H */
