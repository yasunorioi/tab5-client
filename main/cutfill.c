// cutfill.c — see cutfill.h. Pure C, no ESP-IDF dependency.

#include "cutfill.h"

#include <math.h>
#include <string.h>

// WGS84 semi-major axis. Using a single radius for the equirectangular
// projection is fine here: any horizontal scale error only shifts WHERE on the
// target plane we sample, and (horizontal error)·(plane slope) is sub-millimetre
// for field-scale distances and realistic grades.
#define WGS84_A 6378137.0

void cutfill_geo_init(cutfill_geo_t *g, double lat_rad, double lon_rad, double h0)
{
    g->lat0_rad = lat_rad;
    g->lon0_rad = lon_rad;
    g->coslat0  = cos(lat_rad);
    g->h0       = h0;
    g->set      = true;
}

void cutfill_project(const cutfill_geo_t *g, double lat_rad, double lon_rad,
                     double *east_m, double *north_m)
{
    *east_m  = (lon_rad - g->lon0_rad) * g->coslat0 * WGS84_A;
    *north_m = (lat_rad - g->lat0_rad) * WGS84_A;
}

void cutfill_survey_reset(cutfill_survey_t *s)
{
    memset(s, 0, sizeof(*s));
}

void cutfill_survey_add_en(cutfill_survey_t *s, const cutfill_geo_t *g,
                           double east_m, double north_m, double h)
{
    double z = h - g->h0;    // relative height keeps the normal equations well-conditioned
    s->sE  += east_m;
    s->sN  += north_m;
    s->sZ  += z;
    s->sEE += east_m * east_m;
    s->sNN += north_m * north_m;
    s->sEN += east_m * north_m;
    s->sEZ += east_m * z;
    s->sNZ += north_m * z;
    s->n   += 1;
}

void cutfill_survey_add_ll(cutfill_survey_t *s, const cutfill_geo_t *g,
                           double lat_rad, double lon_rad, double h)
{
    double e, n;
    cutfill_project(g, lat_rad, lon_rad, &e, &n);
    cutfill_survey_add_en(s, g, e, n, h);
}

// Solve the 3×3 symmetric normal-equation system by Cramer's rule.
//   [sEE sEN sE ][a]   [sEZ]
//   [sEN sNN sN ][b] = [sNZ]
//   [sE  sN  n  ][c]   [sZ ]
// Returns false if (near-)singular — e.g. all points on one line, or < 3 points.
static bool solve3(double a11, double a12, double a13,
                   double a22, double a23, double a33,
                   double r1, double r2, double r3,
                   double *x, double *y, double *z)
{
    // Symmetric matrix M = [[a11,a12,a13],[a12,a22,a23],[a13,a23,a33]].
    double m00 = a11, m01 = a12, m02 = a13;
    double m10 = a12, m11 = a22, m12 = a23;
    double m20 = a13, m21 = a23, m22 = a33;

    double det = m00 * (m11 * m22 - m12 * m21)
               - m01 * (m10 * m22 - m12 * m20)
               + m02 * (m10 * m21 - m11 * m20);

    // Scale-aware singularity test: compare |det| against the matrix magnitude.
    double scale = fabs(m00) + fabs(m11) + fabs(m22) + 1.0;
    if (fabs(det) < 1e-9 * scale * scale * scale) {
        return false;
    }
    double inv = 1.0 / det;

    // Cramer: replace each column with the RHS in turn.
    double dx = r1  * (m11 * m22 - m12 * m21)
              - m01 * (r2  * m22 - m12 * r3)
              + m02 * (r2  * m21 - m11 * r3);
    double dy = m00 * (r2  * m22 - m12 * r3)
              - r1  * (m10 * m22 - m12 * m20)
              + m02 * (m10 * r3  - r2  * m20);
    double dz = m00 * (m11 * r3  - r2  * m21)
              - m01 * (m10 * r3  - r2  * m20)
              + r1  * (m10 * m21 - m11 * m20);

    *x = dx * inv;
    *y = dy * inv;
    *z = dz * inv;
    return true;
}

bool cutfill_fit_balance(const cutfill_survey_t *s, cutfill_plane_t *plane)
{
    plane->valid = false;
    if (s->n < 3) {
        return false;
    }
    // Normal-equation matrix (symmetric):
    //   [sEE sEN sE][a]   [sEZ]
    //   [sEN sNN sN][b] = [sNZ]
    //   [sE  sN  n ][c]   [sZ ]
    double a, b, c;
    if (!solve3(s->sEE, s->sEN, s->sE,
                s->sNN, s->sN, (double)s->n,
                s->sEZ, s->sNZ, s->sZ,
                &a, &b, &c)) {
        return false;
    }
    plane->a = a;
    plane->b = b;
    plane->c = c;
    plane->valid = true;
    return true;
}

void cutfill_plane_flat(cutfill_plane_t *plane, const cutfill_geo_t *g,
                        double target_height)
{
    plane->a = 0.0;
    plane->b = 0.0;
    plane->c = target_height - g->h0;   // store relative to the origin datum
    plane->valid = true;
}

bool cutfill_fit_slope(const cutfill_survey_t *s, double a, double b,
                       cutfill_plane_t *plane)
{
    plane->valid = false;
    if (s->n == 0) {
        return false;
    }
    // With a,b fixed, the balance offset is the mean residual: c = mean(z − aE − bN).
    double c = (s->sZ - a * s->sE - b * s->sN) / (double)s->n;
    plane->a = a;
    plane->b = b;
    plane->c = c;
    plane->valid = true;
    return true;
}

double cutfill_target_height(const cutfill_geo_t *g, const cutfill_plane_t *plane,
                             double east_m, double north_m)
{
    // plane->c is relative to h0; add h0 back for an absolute ellipsoidal height.
    return g->h0 + plane->a * east_m + plane->b * north_m + plane->c;
}

double cutfill_delta(const cutfill_geo_t *g, const cutfill_plane_t *plane,
                     double lat_rad, double lon_rad, double height)
{
    double e, n;
    cutfill_project(g, lat_rad, lon_rad, &e, &n);
    return height - cutfill_target_height(g, plane, e, n);
}

int cutfill_classify(double delta_m, double deadband_m)
{
    if (delta_m >  deadband_m) return +1;   // CUT
    if (delta_m < -deadband_m) return -1;   // FILL
    return 0;                                // ON GRADE
}

const char *cutfill_state_str(int state)
{
    return state > 0 ? "CUT" : state < 0 ? "FILL" : "ON GRADE";
}
