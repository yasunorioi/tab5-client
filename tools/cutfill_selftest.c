// cutfill_selftest.c — host verification of main/cutfill.c with synthetic fields.
//
// The outdoor RTK fixtures don't exist yet (the captured ones are indoor/DNU),
// but cut/fill is pure geometry, so it's verified analytically against a known
// plane. NO ESP-IDF, NO receiver.
//
//   cc -Imain -o /tmp/cutfill_selftest tools/cutfill_selftest.c main/cutfill.c -lm
//   /tmp/cutfill_selftest
//
// Exit status is non-zero if any check fails.

#include "cutfill.h"

#include <math.h>
#include <stdio.h>

#define DEG2RAD (M_PI / 180.0)

static int g_fail = 0;
static void check(int cond, const char *msg)
{
    if (!cond) { printf("FAIL: %s\n", msg); g_fail = 1; }
}
static int approx(double a, double b, double tol) { return fabs(a - b) < tol; }

int main(void)
{
    // Origin somewhere in Eniwa, Hokkaido (matches the project site roughly).
    const double lat0 = 42.90 * DEG2RAD;
    const double lon0 = 141.60 * DEG2RAD;
    const double h0   = 40.0;   // ellipsoidal height datum (m)

    cutfill_geo_t g;
    cutfill_geo_init(&g, lat0, lon0, h0);

    // Ground truth plane over the field: gentle grade + offset.
    //   true height(E,N) = h0 + A*E + B*N + C
    const double A = 0.003;    // 0.3% east grade
    const double B = -0.001;   // -0.1% north grade
    const double C = 0.25;     // 25 cm above datum at the origin

    // ── 1. Balance-plane fit recovers the ground-truth coefficients ──────────
    // Sample a 200 m × 200 m grid. Add a deterministic zero-mean "roughness" so
    // the fit is a real least-squares problem, not an exact interpolation.
    cutfill_survey_t s;
    cutfill_survey_reset(&s);
    int idx = 0;
    for (int ie = -10; ie <= 10; ie++) {
        for (int in = -10; in <= 10; in++) {
            double E = ie * 10.0;   // metres east
            double N = in * 10.0;   // metres north
            // zero-mean bump pattern (sums to ~0 over the symmetric grid)
            double rough = 0.02 * sin(0.7 * ie) * cos(0.9 * in);
            double h = h0 + A * E + B * N + C + rough;
            cutfill_survey_add_en(&s, &g, E, N, h);
            idx++;
        }
    }
    check(s.n == 441, "survey point count");

    cutfill_plane_t bp;
    check(cutfill_fit_balance(&s, &bp) && bp.valid, "balance fit succeeded");
    printf("fit: a=%.6f b=%.6f c=%.4f  (truth a=%.6f b=%.6f c=%.4f)\n",
           bp.a, bp.b, bp.c, A, B, C);
    check(approx(bp.a, A, 1e-4), "recovered east grade");
    check(approx(bp.b, B, 1e-4), "recovered north grade");
    check(approx(bp.c, C, 1e-3), "recovered offset");

    // ── 2. Balance property: Σ(measured − target) ≈ 0 (cut volume ≈ fill) ────
    double sum = 0.0;
    for (int ie = -10; ie <= 10; ie++) {
        for (int in = -10; in <= 10; in++) {
            double E = ie * 10.0, N = in * 10.0;
            double rough = 0.02 * sin(0.7 * ie) * cos(0.9 * in);
            double h = h0 + A * E + B * N + C + rough;
            sum += h - cutfill_target_height(&g, &bp, E, N);
        }
    }
    printf("balance residual sum = %.6e m (should be ~0)\n", sum);
    check(fabs(sum) < 1e-6, "least-squares balance (Σ residual ≈ 0)");

    // ── 3. delta sign: a point above the plane is CUT, below is FILL ─────────
    // Take the origin lat/lon; target there is h0 + C. Measure C+0.10 → +10 cm CUT.
    double d_hi = cutfill_delta(&g, &bp, lat0, lon0, h0 + C + 0.10);
    double d_lo = cutfill_delta(&g, &bp, lat0, lon0, h0 + C - 0.07);
    printf("delta above=%.3f m (%s)  below=%.3f m (%s)\n",
           d_hi, cutfill_state_str(cutfill_classify(d_hi, 0.005)),
           d_lo, cutfill_state_str(cutfill_classify(d_lo, 0.005)));
    check(approx(d_hi,  0.10, 2e-3), "delta magnitude above plane");
    check(approx(d_lo, -0.07, 2e-3), "delta magnitude below plane");
    check(cutfill_classify(d_hi, 0.005) == +1, "above plane classifies as CUT");
    check(cutfill_classify(d_lo, 0.005) == -1, "below plane classifies as FILL");
    check(cutfill_classify(0.002, 0.005) == 0, "within deadband is ON GRADE");

    // ── 4. Flat mode: target is constant regardless of position ──────────────
    cutfill_plane_t fp;
    cutfill_plane_flat(&fp, &g, h0 + 0.50);   // level the whole field to +50 cm
    double t_a = cutfill_target_height(&g, &fp, 0.0, 0.0);
    double t_b = cutfill_target_height(&g, &fp, 123.0, -77.0);
    check(approx(t_a, h0 + 0.50, 1e-9) && approx(t_b, h0 + 0.50, 1e-9),
          "flat target is position-independent");

    // ── 5. Fixed-slope mode: uses given grade, balances the offset ───────────
    cutfill_plane_t sp;
    check(cutfill_fit_slope(&s, A, B, &sp) && sp.valid, "slope fit succeeded");
    check(approx(sp.a, A, 1e-12) && approx(sp.b, B, 1e-12), "slope uses given grade");
    check(approx(sp.c, C, 1e-3), "slope offset balances to truth");

    // ── 6. Projection sanity: 1° longitude east → ~cos(lat)·111 km ───────────
    double e, n;
    cutfill_project(&g, lat0, lon0 + 1.0 * DEG2RAD, &e, &n);
    double expect_e = 6378137.0 * (1.0 * DEG2RAD) * cos(lat0);
    check(approx(e, expect_e, 1.0) && approx(n, 0.0, 1e-6), "east projection scale");

    printf("\n%s\n", g_fail ? "FAIL" : "PASS");
    return g_fail;
}
