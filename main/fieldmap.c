// fieldmap.c — see fieldmap.h. Pure C, no ESP-IDF dependency.

#include "fieldmap.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

// Capacity caps. A distance-gated recorder (leveler.c) adds a point roughly every
// metre driven, so these cover a large field with a long perimeter trace.
#define MAX_BOUNDARY 2000
#define MAX_POINTS   8000
// Grid cap: keeps fieldmap_compute() bounded (cells × points work) on the P4.
#define MAX_CELLS    40000

typedef struct { double e, n; }    vtx_t;
typedef struct { double e, n, z; } pt_t;

static vtx_t   *s_bound;
static uint32_t s_nbound;
static pt_t    *s_pts;
static uint32_t s_npts;

bool fieldmap_init(void)
{
    if (!s_bound) s_bound = malloc(sizeof(vtx_t) * MAX_BOUNDARY);
    if (!s_pts)   s_pts   = malloc(sizeof(pt_t) * MAX_POINTS);
    return s_bound && s_pts;
}

void fieldmap_reset(void)
{
    s_nbound = 0;
    s_npts = 0;
}

void fieldmap_boundary_add(double e, double n)
{
    if (s_bound && s_nbound < MAX_BOUNDARY) {
        s_bound[s_nbound].e = e;
        s_bound[s_nbound].n = n;
        s_nbound++;
    }
}
uint32_t fieldmap_boundary_count(void) { return s_nbound; }

void fieldmap_point_add(double e, double n, double z)
{
    if (s_pts && s_npts < MAX_POINTS) {
        s_pts[s_npts].e = e;
        s_pts[s_npts].n = n;
        s_pts[s_npts].z = z;
        s_npts++;
    }
}
uint32_t fieldmap_point_count(void) { return s_npts; }

// Shoelace area (absolute) of the boundary polygon.
double fieldmap_area(void)
{
    if (s_nbound < 3) return 0.0;
    double a2 = 0.0;
    for (uint32_t i = 0, j = s_nbound - 1; i < s_nbound; j = i++) {
        a2 += (s_bound[j].e + s_bound[i].e) * (s_bound[i].n - s_bound[j].n);
    }
    return fabs(a2) * 0.5;
}

bool fieldmap_bbox(double *emin, double *emax, double *nmin, double *nmax)
{
    if (s_nbound < 1) return false;
    double e0 = s_bound[0].e, e1 = e0, n0 = s_bound[0].n, n1 = n0;
    for (uint32_t i = 1; i < s_nbound; i++) {
        if (s_bound[i].e < e0) e0 = s_bound[i].e;
        if (s_bound[i].e > e1) e1 = s_bound[i].e;
        if (s_bound[i].n < n0) n0 = s_bound[i].n;
        if (s_bound[i].n > n1) n1 = s_bound[i].n;
    }
    if (emin) *emin = e0;
    if (emax) *emax = e1;
    if (nmin) *nmin = n0;
    if (nmax) *nmax = n1;
    return true;
}

bool fieldmap_boundary_get(uint32_t i, double *e, double *n)
{
    if (i >= s_nbound) return false;
    if (e) *e = s_bound[i].e;
    if (n) *n = s_bound[i].n;
    return true;
}

// Ray-casting point-in-polygon test on the boundary.
bool fieldmap_inside(double e, double n)
{
    bool in = false;
    for (uint32_t i = 0, j = s_nbound - 1; i < s_nbound; j = i++) {
        double ei = s_bound[i].e, ni = s_bound[i].n;
        double ej = s_bound[j].e, nj = s_bound[j].n;
        if (((ni > n) != (nj > n)) &&
            (e < (ej - ei) * (n - ni) / (nj - ni) + ei)) {
            in = !in;
        }
    }
    return in;
}

// Solve the symmetric 3×3 system by Cramer's rule (same as cutfill.c). Returns
// false if (near-)singular.
static bool solve3(double a11, double a12, double a13,
                   double a22, double a23, double a33,
                   double r1, double r2, double r3,
                   double *x, double *y, double *z)
{
    double m00 = a11, m01 = a12, m02 = a13;
    double m10 = a12, m11 = a22, m12 = a23;
    double m20 = a13, m21 = a23, m22 = a33;
    double det = m00 * (m11 * m22 - m12 * m21)
               - m01 * (m10 * m22 - m12 * m20)
               + m02 * (m10 * m21 - m11 * m20);
    double scale = fabs(m00) + fabs(m11) + fabs(m22) + 1.0;
    if (fabs(det) < 1e-9 * scale * scale * scale) return false;
    double inv = 1.0 / det;
    double dx = r1  * (m11 * m22 - m12 * m21)
              - m01 * (r2  * m22 - m12 * r3)
              + m02 * (r2  * m21 - m11 * r3);
    double dy = m00 * (r2  * m22 - m12 * r3)
              - r1  * (m10 * m22 - m12 * m20)
              + m02 * (m10 * r3  - r2  * m20);
    double dz = m00 * (m11 * r3  - r2  * m21)
              - m01 * (m10 * r3  - r2  * m20)
              + r1  * (m10 * m21 - m11 * m20);
    *x = dx * inv; *y = dy * inv; *z = dz * inv;
    return true;
}

// Moving least-squares surface height at (e0,n0): a distance-weighted plane
// z = A·Δe + B·Δn + C fitted to the whole cloud about (e0,n0); the height is C.
// Weighted LS of a plane's samples returns that plane EXACTLY, so a planar field
// interpolates with zero error (unlike IDW, which smooths). Far points carry tiny
// weight, so for a curved surface the fit is effectively local.
//
// The per-point accumulation is FLOAT: the esp32p4 has a single-precision FPU but
// software double, so float here is ~10× faster — the difference between a snappy
// map repaint and tripping the task watchdog. Δe/Δn/z are metre-scale so float's
// ~7 digits is sub-millimetre. The tiny 3×3 solve stays double.
static double mls_height(double e0, double n0)
{
    float sEE = 0, sEN = 0, sE = 0, sNN = 0, sN = 0, sW = 0, sEZ = 0, sNZ = 0, sZ = 0;
    for (uint32_t k = 0; k < s_npts; k++) {
        float de = (float)(s_pts[k].e - e0), dn = (float)(s_pts[k].n - n0);
        float z  = (float)s_pts[k].z;
        float w  = 1.0f / (de * de + dn * dn + 1e-6f);   // IDW^2 weights
        sEE += w * de * de; sEN += w * de * dn; sE += w * de;
        sNN += w * dn * dn; sN += w * dn;       sW += w;
        sEZ += w * de * z;  sNZ += w * dn * z;  sZ += w * z;
    }
    double A, B, C;
    // Normal equations for [A,B,C] with the constant column = the Σw row/col.
    if (solve3(sEE, sEN, sE, sNN, sN, sW, sEZ, sNZ, sZ, &A, &B, &C)) return C;
    return sW > 0 ? (double)(sZ / sW) : 0.0;   // degenerate → weighted mean
}

double fieldmap_surface_at(double e, double n)
{
    return s_npts ? mls_height(e, n) : 0.0;
}

bool fieldmap_compute(double a, double b, double c, double cell_m,
                      fieldmap_result_t *out)
{
    if (out) memset(out, 0, sizeof(*out));
    if (!out || s_nbound < 3 || s_npts == 0) return false;
    if (cell_m <= 0.0) cell_m = 1.0;

    // Bounding box of the boundary.
    double emin = s_bound[0].e, emax = emin, nmin = s_bound[0].n, nmax = nmin;
    for (uint32_t i = 1; i < s_nbound; i++) {
        if (s_bound[i].e < emin) emin = s_bound[i].e;
        if (s_bound[i].e > emax) emax = s_bound[i].e;
        if (s_bound[i].n < nmin) nmin = s_bound[i].n;
        if (s_bound[i].n > nmax) nmax = s_bound[i].n;
    }
    // Enlarge the cell so the grid stays under MAX_CELLS for very large fields.
    for (;;) {
        double nx = ceil((emax - emin) / cell_m);
        double ny = ceil((nmax - nmin) / cell_m);
        if (nx * ny <= (double)MAX_CELLS || cell_m > 1e6) break;
        cell_m *= 1.5;
    }

    double cell_area = cell_m * cell_m;
    double cut = 0.0, fill = 0.0, cut_area = 0.0, fill_area = 0.0;
    uint32_t cells = 0;

    // Sample cell CENTRES; a centre inside the polygon contributes one cell.
    for (double n = nmin + cell_m * 0.5; n < nmax; n += cell_m) {
        for (double e = emin + cell_m * 0.5; e < emax; e += cell_m) {
            if (!fieldmap_inside(e, n)) continue;
            double surface = mls_height(e, n);
            double target  = a * e + b * n + c;
            double dev = surface - target;   // >0 ground high (cut), <0 low (fill)
            if (dev > 0.0) { cut  += dev * cell_area;  cut_area  += cell_area; }
            else           { fill += -dev * cell_area; fill_area += cell_area; }
            cells++;
        }
    }

    out->area_m2     = fieldmap_area();
    out->cut_m3      = cut;
    out->fill_m3     = fill;
    out->net_m3      = cut - fill;
    out->cut_mean_m  = cut_area  > 0.0 ? cut  / cut_area  : 0.0;
    out->fill_mean_m = fill_area > 0.0 ? fill / fill_area : 0.0;
    out->cells       = cells;
    out->valid       = cells > 0;
    return out->valid;
}
