// fieldmap_selftest.c — host verification of main/fieldmap.c with an analytic field.
//
// A square L×L field whose surface tilts linearly in East, z = s·E, has closed-
// form earthwork volumes against a flat target plane, so the grid/IDW integrator
// is checked against exact numbers. NO ESP-IDF, NO receiver.
//
//   cc -Imain -o /tmp/fieldmap_selftest tools/fieldmap_selftest.c main/fieldmap.c -lm
//   /tmp/fieldmap_selftest
//
// Exit status is non-zero if any check fails.

#include "fieldmap.h"

#include <math.h>
#include <stdio.h>

static int g_fail = 0;
static void check(int cond, const char *msg)
{
    if (!cond) { printf("FAIL: %s\n", msg); g_fail = 1; }
}
static int within(double got, double want, double frac, double absfloor)
{
    double tol = fabs(want) * frac + absfloor;
    return fabs(got - want) <= tol;
}

int main(void)
{
    const double L = 100.0;   // 100 m square
    const double s = 0.01;    // 1% east tilt: z = s*E, ranges 0..1.0 m

    fieldmap_init();
    fieldmap_reset();

    // Boundary: the 4 corners (ordered).
    fieldmap_boundary_add(0, 0);
    fieldmap_boundary_add(L, 0);
    fieldmap_boundary_add(L, L);
    fieldmap_boundary_add(0, L);

    // Survey cloud: sample the surface every 2 m (offset from 1 m cell centres so
    // the integrator interpolates rather than sitting on samples).
    for (int ie = 0; ie <= (int)L; ie += 2)
        for (int in = 0; in <= (int)L; in += 2)
            fieldmap_point_add(ie, in, s * ie);

    printf("boundary=%u pts  cloud=%u pts\n",
           fieldmap_boundary_count(), fieldmap_point_count());

    // Area = L².
    double area = fieldmap_area();
    printf("area = %.1f m2 (want %.1f)\n", area, L * L);
    check(within(area, L * L, 0.0, 1.0), "shoelace area");

    fieldmap_result_t r;

    // 1. Flat target at the mid-height (mean = s*L/2 = 0.5). Analytic:
    //    cut = fill = s*L^3/8, net = 0.
    double want_bal = s * L * L * L / 8.0;   // 1250 m3
    check(fieldmap_compute(0.0, 0.0, s * L / 2.0, 1.0, &r) && r.valid, "compute(flat@mean)");
    printf("flat@mean:  cut=%.0f fill=%.0f net=%.0f m3  (want cut=fill=%.0f)\n",
           r.cut_m3, r.fill_m3, r.net_m3, want_bal);
    check(within(r.cut_m3,  want_bal, 0.08, 20), "cut volume (flat@mean)");
    check(within(r.fill_m3, want_bal, 0.08, 20), "fill volume (flat@mean)");
    check(within(r.net_m3,  0.0,      0.0,  40), "net ~0 (balanced)");

    // 2. Target plane == the surface (a=s, b=0, c=0): already level → ~0 volumes.
    check(fieldmap_compute(s, 0.0, 0.0, 1.0, &r) && r.valid, "compute(plane==surface)");
    printf("plane=surf: cut=%.1f fill=%.1f m3 (want ~0)\n", r.cut_m3, r.fill_m3);
    check(within(r.cut_m3,  0.0, 0.0, 30), "cut ~0 (plane matches surface)");
    check(within(r.fill_m3, 0.0, 0.0, 30), "fill ~0 (plane matches surface)");

    // 3. Flat target at datum (c=0), whole field is above it → all cut.
    //    cut = ∫∫ s*E dA = s*L^3/2, fill = 0.
    double want_allcut = s * L * L * L / 2.0;   // 5000 m3
    check(fieldmap_compute(0.0, 0.0, 0.0, 1.0, &r) && r.valid, "compute(flat@datum)");
    printf("flat@datum: cut=%.0f fill=%.1f m3  (want cut=%.0f fill=0)\n",
           r.cut_m3, r.fill_m3, want_allcut);
    check(within(r.cut_m3, want_allcut, 0.08, 20), "cut volume (flat@datum)");
    check(r.fill_m3 < 1.0, "fill ~0 (flat@datum)");

    printf("\n%s\n", g_fail ? "FAIL" : "PASS");
    return g_fail;
}
