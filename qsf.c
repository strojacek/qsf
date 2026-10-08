/* qsf — OGC Simple Features (SFA 1.2.1, ISO 19125) for q, on GEOS and GDAL/OGR.
 *
 * A geometry is a byte vector (4h) holding PostGIS-style extended WKB (EWKB): the geometry
 * type carries Z (0x80000000), M (0x40000000) and SRID (0x20000000) flags, and the SRID follows
 * the type in the outermost header. A column is a general list of them. An empty byte vector is
 * SQL NULL; an EMPTY geometry (POINT EMPTY, ...) is a real geometry.
 *
 * Who does what:
 *   - this file: WKB parsing/writing (ISO and extended), every structural accessor, measures on
 *     polyhedral surfaces, LocateAlong / LocateBetween, the polyhedral-surface edge logic;
 *   - GEOS: topology (DE-9IM predicates, Relate, overlay, buffer, hull, boundary, centroid,
 *     point on surface, simplicity, validity), distance and area, WKT for GEOS-supported types;
 *   - OGR: WKT for PolyhedralSurface, TIN and Triangle (GEOS has no such types) and coordinate
 *     transformation through OSR/PROJ.
 * GEOS works in the plane: polyhedral surfaces and TINs go to it as MultiPolygons, triangles as
 * Polygons, and measures are dropped from its results.
 *
 * MIT licensed. Links GEOS (LGPL) and GDAL (MIT) dynamically.
 */
#include <geos_c.h>
#include <ogr_api.h>
#include <ogr_srs_api.h>
#include <cpl_conv.h>
#include <cpl_error.h>
#include <math.h>
#include <stdarg.h>
#include <strings.h>
#include <gdal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define KXVER 3
#include "k.h"

/* ---------- errors ---------- */

static _Thread_local char errbuf[512];
static void seterr(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void seterr(const char *fmt, ...) {
    if (errbuf[0]) return;            /* keep the first, most specific message */
    va_list ap; va_start(ap, fmt);
    vsnprintf(errbuf, sizeof errbuf, fmt, ap);
    va_end(ap);
}
/* raise the pending message (krr() returns NULL, so this is the last thing a function does) */
static K qraise(const char *fallback) {
    S s = ss(errbuf[0] ? errbuf : (S)fallback);
    errbuf[0] = 0;
    return krr(s);
}

/* ---------- GEOS and OGR state ---------- */

static _Thread_local GEOSContextHandle_t ctx;
static _Thread_local GEOSWKBReader *wkbr;
static _Thread_local GEOSWKBWriter *wkbw;
static _Thread_local GEOSWKTReader *wktr;
static _Thread_local GEOSWKTWriter *wktw;
static _Thread_local char geosmsg[400];

static void on_geos_error(const char *msg, void *u) { (void)u; snprintf(geosmsg, sizeof geosmsg, "%s", msg); }
static void on_geos_notice(const char *msg, void *u) { (void)msg; (void)u; }
static void quiet_cpl(CPLErr e, CPLErrorNum n, const char *m) { (void)e; (void)n; (void)m; }

static void ensure(void) {
    if (ctx) return;
    ctx = GEOS_init_r();
    GEOSContext_setErrorMessageHandler_r(ctx, on_geos_error, NULL);
    GEOSContext_setNoticeMessageHandler_r(ctx, on_geos_notice, NULL);
    wkbr = GEOSWKBReader_create_r(ctx);
    wkbw = GEOSWKBWriter_create_r(ctx);
    GEOSWKBWriter_setOutputDimension_r(ctx, wkbw, 4);
    GEOSWKBWriter_setFlavor_r(ctx, wkbw, GEOS_WKB_EXTENDED);
    GEOSWKBWriter_setByteOrder_r(ctx, wkbw, GEOS_WKB_NDR);
    wktr = GEOSWKTReader_create_r(ctx);
    wktw = GEOSWKTWriter_create_r(ctx);
    GEOSWKTWriter_setTrim_r(ctx, wktw, 1);
    GEOSWKTWriter_setOutputDimension_r(ctx, wktw, 4);
    CPLSetErrorHandler(quiet_cpl);
}
static void geos_fail(const char *what) {
    if (geosmsg[0]) { seterr("%s: %s", what, geosmsg); geosmsg[0] = 0; }
    else seterr("%s failed", what);
}

/* ---------- geometry tree ---------- */

enum { T_POINT = 1, T_LINE = 2, T_POLY = 3, T_MPOINT = 4, T_MLINE = 5, T_MPOLY = 6, T_COLL = 7,
       T_PS = 15, T_TIN = 16, T_TRI = 17 };

/* Points and linestrings hold coordinates (np points, stride 2 + z + m); polygons and triangles
 * hold rings as linestring children; collections, polyhedral surfaces and TINs hold members. */
typedef struct sfg {
    int type, z, m, srid;
    int np; double *c;
    int n; struct sfg **s;
} sfg;

static int stride(const sfg *g) { return 2 + g->z + g->m; }

static void sfg_free(sfg *g) {
    if (!g) return;
    for (int i = 0; i < g->n; i++) sfg_free(g->s[i]);
    free(g->s); free(g->c); free(g);
}
static sfg *sfg_new(int type, int z, int m) {
    sfg *g = calloc(1, sizeof *g);
    g->type = type; g->z = z; g->m = m;
    return g;
}
static void sfg_add(sfg *g, sfg *child) {
    g->s = realloc(g->s, sizeof(sfg *) * (size_t)(g->n + 1));
    g->s[g->n++] = child;
}
static sfg *sfg_copy(const sfg *g) {
    sfg *r = sfg_new(g->type, g->z, g->m);
    r->srid = g->srid; r->np = g->np;
    if (g->np) { size_t sz = sizeof(double) * (size_t)g->np * stride(g); r->c = malloc(sz); memcpy(r->c, g->c, sz); }
    for (int i = 0; i < g->n; i++) sfg_add(r, sfg_copy(g->s[i]));
    return r;
}
static int is_coll(int t) { return t == T_MPOINT || t == T_MLINE || t == T_MPOLY || t == T_COLL; }
static int has_poly3d(const sfg *g) {
    if (g->type == T_PS || g->type == T_TIN || g->type == T_TRI) return 1;
    for (int i = 0; i < g->n && is_coll(g->type); i++) if (has_poly3d(g->s[i])) return 1;
    return 0;
}

static const char *type_name(int t) {
    switch (t) {
    case T_POINT: return "Point"; case T_LINE: return "LineString"; case T_POLY: return "Polygon";
    case T_MPOINT: return "MultiPoint"; case T_MLINE: return "MultiLineString"; case T_MPOLY: return "MultiPolygon";
    case T_COLL: return "GeometryCollection"; case T_PS: return "PolyhedralSurface"; case T_TIN: return "TIN";
    case T_TRI: return "Triangle"; default: return "Geometry";
    }
}

/* ---------- WKB reading (ISO, OGC 2.5D and PostGIS extended; either byte order) ---------- */

typedef struct { const unsigned char *p, *e; } rd;

static int rd_u32(rd *r, int swap, uint32_t *v) {
    if (r->e - r->p < 4) return 0;
    uint32_t x; memcpy(&x, r->p, 4); r->p += 4;
    if (swap) x = __builtin_bswap32(x);
    *v = x; return 1;
}
static int rd_f64(rd *r, int swap, double *v) {
    if (r->e - r->p < 8) return 0;
    uint64_t x; memcpy(&x, r->p, 8); r->p += 8;
    if (swap) x = __builtin_bswap64(x);
    memcpy(v, &x, 8); return 1;
}
static int host_le(void) { uint16_t one = 1; return *(unsigned char *)&one == 1; }

static int rd_coords(rd *r, int swap, int n, int st, double **out) {
    if (n < 0 || (r->e - r->p) / 8 < (long)n * st) { seterr("wkb: truncated coordinates"); return 0; }
    double *c = malloc(sizeof(double) * (size_t)(n ? n : 1) * st);
    for (long i = 0; i < (long)n * st; i++) rd_f64(r, swap, &c[i]);
    *out = c; return 1;
}

static sfg *parse_wkb(rd *r, int top, int depth) {
    if (depth > 64) { seterr("wkb: nesting too deep"); return NULL; }
    if (r->p >= r->e) { seterr("wkb: truncated"); return NULL; }
    int order = *r->p++;
    if (order > 1) { seterr("wkb: bad byte order %d", order); return NULL; }
    int swap = (order == 1) != host_le();
    uint32_t t;
    if (!rd_u32(r, swap, &t)) { seterr("wkb: truncated header"); return NULL; }
    int z = (t & 0x80000000u) != 0, m = (t & 0x40000000u) != 0, hassrid = (t & 0x20000000u) != 0;
    uint32_t code = t & 0x1FFFFFFFu;
    if (code >= 3000 && code < 4000) { z = m = 1; code -= 3000; }
    else if (code >= 2000) { m = 1; code -= 2000; }
    else if (code >= 1000) { z = 1; code -= 1000; }
    int srid = 0;
    if (hassrid) { uint32_t s; if (!rd_u32(r, swap, &s)) { seterr("wkb: truncated srid"); return NULL; } srid = (int)s; }
    int base = (int)code;
    if (!(base >= 1 && base <= 7) && base != T_PS && base != T_TIN && base != T_TRI) {
        seterr("wkb: geometry type %u is not a Simple Features type", code);
        return NULL;
    }
    sfg *g = sfg_new(base, z, m);
    g->srid = top ? srid : 0;
    int st = stride(g);
    uint32_t n;
    switch (base) {
    case T_POINT: {
        double *c;
        if (!rd_coords(r, swap, 1, st, &c)) goto bad;
        int allnan = 1;
        for (int i = 0; i < st; i++) if (!isnan(c[i])) allnan = 0;
        if (allnan) { free(c); g->np = 0; } else { g->c = c; g->np = 1; }
        break;
    }
    case T_LINE:
        if (!rd_u32(r, swap, &n) || !rd_coords(r, swap, (int)n, st, &g->c)) goto bad;
        g->np = (int)n;
        break;
    case T_POLY: case T_TRI:
        if (!rd_u32(r, swap, &n)) goto bad;
        if (n > (uint32_t)(r->e - r->p)) { seterr("wkb: bad ring count"); goto bad; }
        for (uint32_t i = 0; i < n; i++) {
            uint32_t k;
            sfg *ring = sfg_new(T_LINE, z, m);
            sfg_add(g, ring);
            if (!rd_u32(r, swap, &k) || !rd_coords(r, swap, (int)k, st, &ring->c)) goto bad;
            ring->np = (int)k;
        }
        break;
    default:
        if (!rd_u32(r, swap, &n)) goto bad;
        if (n > (uint32_t)(r->e - r->p)) { seterr("wkb: bad member count"); goto bad; }
        for (uint32_t i = 0; i < n; i++) {
            sfg *c = parse_wkb(r, 0, depth + 1);
            if (!c) goto bad;
            sfg_add(g, c);
            int want = base == T_MPOINT ? T_POINT : base == T_MLINE ? T_LINE :
                       base == T_MPOLY || base == T_PS ? T_POLY : base == T_TIN ? T_TRI : 0;
            if (want && c->type != want) { seterr("wkb: %s cannot hold a %s", type_name(base), type_name(c->type)); goto bad; }
            if (c->z != z || c->m != m) { seterr("wkb: members of a %s differ in dimension", type_name(base)); goto bad; }
        }
        break;
    }
    return g;
bad:
    seterr("wkb: truncated or malformed");
    sfg_free(g);
    return NULL;
}

/* SQL NULL (empty byte vector) gives *g = NULL and returns 1 */
static int read_sfg(K b, sfg **g) {
    *g = NULL;
    if (b->t != KG) { seterr("type: geometry must be a byte vector"); return 0; }
    if (b->n == 0) return 1;
    rd r = { kG(b), kG(b) + b->n };
    *g = parse_wkb(&r, 1, 0);
    if (*g && r.p != r.e) { seterr("wkb: trailing bytes"); sfg_free(*g); *g = NULL; }
    return *g != NULL;
}

/* ---------- WKB writing ---------- */

typedef struct { unsigned char *p; size_t n, cap; } wbuf;
static void wb_put(wbuf *b, const void *v, size_t k) {
    if (b->n + k > b->cap) { b->cap = (b->n + k) * 2 + 64; b->p = realloc(b->p, b->cap); }
    memcpy(b->p + b->n, v, k); b->n += k;
}
static void wb_u32(wbuf *b, uint32_t v) { if (!host_le()) v = __builtin_bswap32(v); wb_put(b, &v, 4); }
static void wb_f64(wbuf *b, double v) { uint64_t x; memcpy(&x, &v, 8); if (!host_le()) x = __builtin_bswap64(x); wb_put(b, &x, 8); }

enum { W_EWKB = 0, W_ISO = 1, W_GEOS = 2 };   /* W_GEOS: EWKB with PS/TIN as MultiPolygon, Triangle as Polygon */

static void write_wkb(wbuf *b, const sfg *g, int flavor, int top) {
    unsigned char order = 1;
    wb_put(b, &order, 1);
    int type = g->type;
    if (flavor == W_GEOS) type = (type == T_PS || type == T_TIN) ? T_MPOLY : type == T_TRI ? T_POLY : type;
    uint32_t code;
    if (flavor == W_ISO) code = (uint32_t)type + (g->z && g->m ? 3000 : g->m ? 2000 : g->z ? 1000 : 0);
    else {
        code = (uint32_t)type | (g->z ? 0x80000000u : 0) | (g->m ? 0x40000000u : 0);
        if (top && g->srid) code |= 0x20000000u;
    }
    wb_u32(b, code);
    if (flavor != W_ISO && top && g->srid) wb_u32(b, (uint32_t)g->srid);
    int st = stride(g);
    switch (g->type) {
    case T_POINT:
        for (int i = 0; i < st; i++) wb_f64(b, g->np ? g->c[i] : NAN);
        break;
    case T_LINE:
        wb_u32(b, (uint32_t)g->np);
        for (long i = 0; i < (long)g->np * st; i++) wb_f64(b, g->c[i]);
        break;
    case T_POLY: case T_TRI:
        wb_u32(b, (uint32_t)g->n);
        for (int r = 0; r < g->n; r++) {
            wb_u32(b, (uint32_t)g->s[r]->np);
            for (long i = 0; i < (long)g->s[r]->np * st; i++) wb_f64(b, g->s[r]->c[i]);
        }
        break;
    default:
        wb_u32(b, (uint32_t)g->n);
        for (int i = 0; i < g->n; i++) write_wkb(b, g->s[i], flavor, 0);
        break;
    }
}

static K sfg_k(const sfg *g, int flavor) {
    wbuf b = {0};
    write_wkb(&b, g, flavor, 1);
    K r = ktn(KG, (J)b.n);
    memcpy(kG(r), b.p, b.n);
    free(b.p);
    return r;
}
static K knull(void) { return ktn(KG, 0); }

/* ---------- GEOS bridge ---------- */

static GEOSGeometry *to_geos(const sfg *g) {
    wbuf b = {0};
    write_wkb(&b, g, W_GEOS, 1);
    GEOSGeometry *o = GEOSWKBReader_read_r(ctx, wkbr, b.p, b.n);
    free(b.p);
    if (!o) geos_fail("geos");
    return o;
}
/* GEOS result -> tree with the given SRID (o is consumed) */
static sfg *from_geos(GEOSGeometry *o, int srid) {
    if (!o) return NULL;
    size_t n = 0;
    GEOSSetSRID_r(ctx, o, 0);
    unsigned char *buf = GEOSWKBWriter_write_r(ctx, wkbw, o, &n);
    GEOSGeom_destroy_r(ctx, o);
    if (!buf) { geos_fail("geos"); return NULL; }
    rd r = { buf, buf + n };
    sfg *g = parse_wkb(&r, 1, 0);
    GEOSFree_r(ctx, buf);
    if (g) g->srid = srid;
    return g;
}

/* ---------- OGR bridge (PolyhedralSurface, TIN, Triangle; transformations) ---------- */

static OGRGeometryH to_ogr(const sfg *g) {
    wbuf b = {0};
    write_wkb(&b, g, W_ISO, 1);
    OGRGeometryH o = NULL;
    OGRErr e = OGR_G_CreateFromWkb(b.p, NULL, &o, (int)b.n);
    free(b.p);
    if (e != OGRERR_NONE) { seterr("ogr: cannot import geometry (%s)", CPLGetLastErrorMsg()); return NULL; }
    return o;
}
static sfg *from_ogr(OGRGeometryH o, int srid) {
    int n = OGR_G_WkbSize(o);
    unsigned char *buf = malloc((size_t)n);
    OGR_G_ExportToIsoWkb(o, wkbNDR, buf);
    rd r = { buf, buf + n };
    sfg *g = parse_wkb(&r, 1, 0);
    free(buf);
    if (g) g->srid = srid;
    return g;
}

/* ---------- argument shapes ---------- */

typedef struct { K x; J n; int atom; } garg;
static int geom_arg(K x, garg *a) {
    a->x = x;
    if (x->t == KG) { a->atom = 1; a->n = 1; return 1; }
    if (x->t == 0) {
        for (J i = 0; i < x->n; i++) if (kK(x)[i]->t != KG) { seterr("type: geometries are byte vectors"); return 0; }
        a->atom = 0; a->n = x->n; return 1;
    }
    seterr("type: expected a geometry (byte vector) or a list of them");
    return 0;
}
static K gat(garg *a, J i) { return a->atom ? a->x : kK(a->x)[i]; }

/* numeric argument: atom broadcasts; nulls become NaN */
typedef struct { K x; J n; int atom; } narg;
static int num_arg(K x, narg *a) {
    a->x = x;
    switch (x->t) {
    case -KF: case -KJ: case -KI: case -KH: case -KE: a->atom = 1; a->n = 1; return 1;
    case KF: case KJ: case KI: case KH: case KE: a->atom = 0; a->n = x->n; return 1;
    default: seterr("type: expected a number or a numeric vector"); return 0;
    }
}
static double nget(narg *a, J i) {
    K x = a->x; J k = a->atom ? 0 : i;
    switch (x->t) {
    case -KF: return x->f; case -KJ: return x->j == nj ? NAN : (double)x->j; case -KI: return x->i == ni ? NAN : (double)x->i;
    case -KH: return x->h == nh ? NAN : (double)x->h; case -KE: return (double)x->e;
    case KF: return kF(x)[k]; case KJ: return kJ(x)[k] == nj ? NAN : (double)kJ(x)[k];
    case KI: return kI(x)[k] == ni ? NAN : (double)kI(x)[k]; case KH: return kH(x)[k] == nh ? NAN : (double)kH(x)[k];
    case KE: return (double)kE(x)[k];
    }
    return NAN;
}
/* combined length of broadcast arguments (lengths < 0 mean "atom"), or -1 on mismatch */
static J bcast(int k, const J *len) {
    J n = -2;
    for (int i = 0; i < k; i++) {
        if (len[i] < 0) continue;
        if (n == -2) n = len[i];
        else if (n != len[i]) return -1;
    }
    return n == -2 ? -3 : n;    /* -3: every argument was an atom */
}

/* result vector of one type, collapsing to an atom when every input was an atom */
typedef struct { K r; int atom; J n; } out;
static void out_init(out *o, J n, I t) { o->atom = n == -3; o->n = o->atom ? 1 : n; o->r = ktn(t, o->n); }
static K out_done(out *o) {
    if (!o->atom) return o->r;
    K r = o->r, a;
    switch (r->t) {
    case KF: a = kf(kF(r)[0]); break;
    case KJ: a = kj(kJ(r)[0]); break;
    case KB: a = kb(kG(r)[0]); break;
    case KS: a = ks(kS(r)[0]); break;
    default: a = r1(kK(r)[0]); break;
    }
    r0(r);
    return a;
}
static K out_fail(out *o, J filled, const char *fallback) {
    if (o->r->t == 0) o->r->n = filled;   /* only the filled slots of a general list are owned */
    r0(o->r);
    return qraise(fallback);
}
static J ilen(garg *a) { return a->atom ? -1 : a->n; }
static J nlen(narg *a) { return a->atom ? -1 : a->n; }

static int srid_check(const sfg *a, const sfg *b) {
    if (a->srid != b->srid && a->srid && b->srid) {
        seterr("operation on mixed SRIDs (%d and %d)", a->srid, b->srid);
        return 0;
    }
    return 1;
}

/* ---------- text input ---------- */

static sfg *from_wkt(const char *s) {
    int srid = 0;
    if (!strncasecmp(s, "SRID=", 5)) {           /* EWKT */
        char *end;
        srid = (int)strtol(s + 5, &end, 10);
        if (*end != ';') { seterr("wkt: bad SRID= prefix"); return NULL; }
        s = end + 1;
    }
    geosmsg[0] = 0;
    GEOSGeometry *o = GEOSWKTReader_read_r(ctx, wktr, s);
    if (o) {
        sfg *g = from_geos(o, srid);
        return g;
    }
    char gmsg[400]; snprintf(gmsg, sizeof gmsg, "%s", geosmsg); geosmsg[0] = 0;
    /* GEOS has no PolyhedralSurface/TIN/Triangle: OGR reads those */
    char *copy = strdup(s), *p = copy;
    OGRGeometryH og = NULL;
    OGRErr e = OGR_G_CreateFromWkt(&p, NULL, &og);
    free(copy);
    if (e != OGRERR_NONE || !og) { seterr("wkt: %s", gmsg[0] ? gmsg : "cannot parse"); return NULL; }
    sfg *g = from_ogr(og, srid);
    OGR_G_DestroyGeometry(og);
    return g;
}

static char *kstr(K s) {
    if (s->t == -KC) { char *c = malloc(2); c[0] = (char)s->g; c[1] = 0; return c; }
    if (s->t == -KS) return strdup(s->s);
    if (s->t != KC) return NULL;
    char *c = malloc((size_t)s->n + 1);
    memcpy(c, kC(s), (size_t)s->n); c[s->n] = 0;
    return c;
}

/* fromtext[wkt;srid;type]: type 0 = any, else the required SFA type code (mismatch -> NULL).
 * srid null keeps an EWKT SRID= prefix (or 0). */
K qsf_fromtext(K x, K srid, K want) {
    ensure();
    int atom = x->t == KC || x->t == -KC || x->t == -KS;
    if (!atom && x->t != 0 && x->t != KS) return krr("type");
    narg sa, wa;
    if (!num_arg(srid, &sa) || !num_arg(want, &wa)) return qraise("type");
    J lens[3] = { atom ? -1 : x->n, nlen(&sa), nlen(&wa) };
    J n = bcast(3, lens);
    if (n == -1) return krr("length");
    out o; out_init(&o, n, 0);
    for (J i = 0; i < o.n; i++) {
        K s = atom ? x : x->t == KS ? NULL : kK(x)[i];
        char *c = s ? kstr(s) : strdup(kS(x)[i]);
        if (!c) { seterr("type: wkt must be a string"); return out_fail(&o, i, "fromtext"); }
        K e;
        if (!c[0]) e = knull();
        else {
            sfg *g = from_wkt(c);
            if (!g) { free(c); return out_fail(&o, i, "fromtext"); }
            double sd = nget(&sa, i), wt = nget(&wa, i);
            if (!isnan(sd)) g->srid = (int)sd;
            e = (!isnan(wt) && wt != 0 && g->type != (int)wt) ? knull() : sfg_k(g, W_EWKB);
            sfg_free(g);
        }
        free(c);
        kK(o.r)[i] = e;
    }
    return out_done(&o);
}

/* fromwkb[wkb;srid;type]: ISO, OGC or extended WKB in either byte order */
K qsf_fromwkb(K x, K srid, K want) {
    ensure();
    garg a; narg sa, wa;
    if (!geom_arg(x, &a) || !num_arg(srid, &sa) || !num_arg(want, &wa)) return qraise("type");
    J lens[3] = { ilen(&a), nlen(&sa), nlen(&wa) };
    J n = bcast(3, lens);
    if (n == -1) return krr("length");
    out o; out_init(&o, n, 0);
    for (J i = 0; i < o.n; i++) {
        sfg *g;
        if (!read_sfg(gat(&a, i), &g)) return out_fail(&o, i, "fromwkb");
        K e = knull();
        if (g) {
            double sd = nget(&sa, i), wt = nget(&wa, i);
            if (!isnan(sd)) g->srid = (int)sd;
            if (isnan(wt) || wt == 0 || g->type == (int)wt) { r0(e); e = sfg_k(g, W_EWKB); }
            sfg_free(g);
        }
        kK(o.r)[i] = e;
    }
    return out_done(&o);
}

/* ---------- text and binary output ---------- */

static int is_empty(const sfg *g);
static K wkt_of(const sfg *g, int ewkt) {
    char *s = NULL; int geosown = 0;
    char emptybuf[64];
    if (is_empty(g) && g->type != T_POINT) {
        /* GEOS drops the dimension of empty collections; write "<TYPE> [Z|M|ZM] EMPTY" here */
        static const char *up[] = { "", "POINT", "LINESTRING", "POLYGON", "MULTIPOINT", "MULTILINESTRING", "MULTIPOLYGON",
            "GEOMETRYCOLLECTION", "", "", "", "", "", "", "", "POLYHEDRALSURFACE", "TIN", "TRIANGLE" };
        snprintf(emptybuf, sizeof emptybuf, "%s%s EMPTY", up[g->type], g->z && g->m ? " ZM" : g->z ? " Z" : g->m ? " M" : "");
        s = emptybuf;
    } else if (has_poly3d(g)) {
        OGRGeometryH og = to_ogr(g);
        if (!og) return NULL;
        if (OGR_G_ExportToIsoWkt(og, &s) != OGRERR_NONE) { OGR_G_DestroyGeometry(og); seterr("ogr: cannot write wkt"); return NULL; }
        OGR_G_DestroyGeometry(og);
    } else {
        GEOSGeometry *o = to_geos(g);
        if (!o) return NULL;
        s = GEOSWKTWriter_write_r(ctx, wktw, o);
        GEOSGeom_destroy_r(ctx, o);
        if (!s) { geos_fail("astext"); return NULL; }
        geosown = 1;
    }
    char pre[32] = "";
    if (ewkt && g->srid) snprintf(pre, sizeof pre, "SRID=%d;", g->srid);
    size_t lp = strlen(pre), ls = strlen(s);
    K r = ktn(KC, (J)(lp + ls));
    memcpy(kC(r), pre, lp); memcpy(kC(r) + lp, s, ls);
    if (geosown) GEOSFree_r(ctx, s); else if (s != emptybuf) CPLFree(s);
    return r;
}

/* astext[g;ewkt] */
K qsf_astext(K x, K ewkt) {
    ensure();
    garg a; if (!geom_arg(x, &a)) return qraise("type");
    out o; out_init(&o, a.atom ? -3 : a.n, 0);
    for (J i = 0; i < o.n; i++) {
        sfg *g;
        if (!read_sfg(gat(&a, i), &g)) return out_fail(&o, i, "astext");
        K e = g ? wkt_of(g, ewkt->g) : ktn(KC, 0);
        sfg_free(g);
        if (!e) return out_fail(&o, i, "astext");
        kK(o.r)[i] = e;
    }
    return out_done(&o);
}

/* asbinary[g;flavor]: 0 ISO WKB (SFA 1.2), 1 extended WKB with SRID (PostGIS), 2 ISO big-endian */
K qsf_asbinary(K x, K flavor) {
    ensure();
    garg a; if (!geom_arg(x, &a)) return qraise("type");
    int f = (int)(flavor->t == -KJ ? flavor->j : flavor->t == -KI ? flavor->i : 0);
    out o; out_init(&o, a.atom ? -3 : a.n, 0);
    for (J i = 0; i < o.n; i++) {
        sfg *g;
        if (!read_sfg(gat(&a, i), &g)) return out_fail(&o, i, "asbinary");
        K e = g ? sfg_k(g, f == 1 ? W_EWKB : W_ISO) : knull();
        if (g && f == 2) {   /* rewrite as XDR via OGR's ISO exporter */
            OGRGeometryH og = to_ogr(g);
            if (!og) { sfg_free(g); r0(e); return out_fail(&o, i, "asbinary"); }
            r0(e);
            e = ktn(KG, OGR_G_WkbSize(og));
            OGR_G_ExportToIsoWkb(og, wkbXDR, kG(e));
            OGR_G_DestroyGeometry(og);
        }
        sfg_free(g);
        kK(o.r)[i] = e;
    }
    return out_done(&o);
}

/* setsrid[g;srid] */
K qsf_setsrid(K x, K srid) {
    ensure();
    garg a; narg sa;
    if (!geom_arg(x, &a) || !num_arg(srid, &sa)) return qraise("type");
    J lens[2] = { ilen(&a), nlen(&sa) };
    J n = bcast(2, lens);
    if (n == -1) return krr("length");
    out o; out_init(&o, n, 0);
    for (J i = 0; i < o.n; i++) {
        sfg *g;
        if (!read_sfg(gat(&a, i), &g)) return out_fail(&o, i, "setsrid");
        double s = nget(&sa, i);
        if (g) g->srid = isnan(s) ? 0 : (int)s;
        kK(o.r)[i] = g ? sfg_k(g, W_EWKB) : knull();
        sfg_free(g);
    }
    return out_done(&o);
}

/* ---------- properties ---------- */

static int dim_of(const sfg *g) {
    switch (g->type) {
    case T_POINT: case T_MPOINT: return 0;
    case T_LINE: case T_MLINE: return 1;
    case T_POLY: case T_MPOLY: case T_PS: case T_TIN: case T_TRI: return 2;
    default: { int d = -1; for (int i = 0; i < g->n; i++) { int k = dim_of(g->s[i]); if (k > d) d = k; } return d; }
    }
}
static int is_empty(const sfg *g) {
    if (g->type == T_POINT || g->type == T_LINE) return g->np == 0;
    for (int i = 0; i < g->n; i++) if (!is_empty(g->s[i])) return 0;
    return 1;
}
static long npoints(const sfg *g) {
    long k = g->np;
    for (int i = 0; i < g->n; i++) k += npoints(g->s[i]);
    return k;
}

/* prop[g;which] -> long (null where the property does not apply)
 *  0 srid  1 dimension  2 coordinate dimension  3 spatial dimension  4 is3D  5 isMeasured
 *  6 isEmpty  7 numPoints (LineString)  8 numInteriorRings (Polygon, Triangle)
 *  9 numGeometries  10 numPatches (PolyhedralSurface, TIN)  11 total vertices  12 type code */
K qsf_prop(K x, K which) {
    ensure();
    garg a; if (!geom_arg(x, &a)) return qraise("type");
    int w = (int)(which->t == -KJ ? which->j : which->i);
    out o; out_init(&o, a.atom ? -3 : a.n, KJ);
    for (J i = 0; i < o.n; i++) {
        sfg *g;
        if (!read_sfg(gat(&a, i), &g)) return out_fail(&o, i, "prop");
        J v = nj;
        if (g) switch (w) {
            case 0: v = g->srid; break;
            case 1: v = dim_of(g); break;
            case 2: v = 2 + g->z + g->m; break;
            case 3: v = 2 + g->z; break;
            case 4: v = g->z; break;
            case 5: v = g->m; break;
            case 6: v = is_empty(g); break;
            case 7: if (g->type == T_LINE) v = g->np; break;
            case 8: if (g->type == T_POLY || g->type == T_TRI) v = g->n > 0 ? g->n - 1 : 0; break;
            case 9: v = is_coll(g->type) ? g->n : is_empty(g) ? 0 : 1; break;
            case 10: if (g->type == T_PS || g->type == T_TIN) v = g->n; break;
            case 11: v = npoints(g); break;
            case 12: v = g->type; break;
        }
        sfg_free(g);
        kJ(o.r)[i] = v;
    }
    return out_done(&o);
}

/* geometrytype[g] -> symbol (SFA instantiable subtype name) */
K qsf_geomtype(K x) {
    ensure();
    garg a; if (!geom_arg(x, &a)) return qraise("type");
    out o; out_init(&o, a.atom ? -3 : a.n, KS);
    for (J i = 0; i < o.n; i++) {
        sfg *g;
        if (!read_sfg(gat(&a, i), &g)) return out_fail(&o, i, "geometrytype");
        kS(o.r)[i] = g ? ss((S)type_name(g->type)) : ss("");
        sfg_free(g);
    }
    return out_done(&o);
}

/* coord[g;which]: 0 X 1 Y 2 Z 3 M of a Point (null for other types, empty points, absent ordinates) */
K qsf_coord(K x, K which) {
    ensure();
    garg a; if (!geom_arg(x, &a)) return qraise("type");
    int w = (int)(which->t == -KJ ? which->j : which->i);
    out o; out_init(&o, a.atom ? -3 : a.n, KF);
    for (J i = 0; i < o.n; i++) {
        sfg *g;
        if (!read_sfg(gat(&a, i), &g)) return out_fail(&o, i, "coord");
        double v = NAN;
        if (g && g->type == T_POINT && g->np) {
            if (w < 2) v = g->c[w];
            else if (w == 2 && g->z) v = g->c[2];
            else if (w == 3 && g->m) v = g->c[2 + g->z];
        }
        sfg_free(g);
        kF(o.r)[i] = v;
    }
    return out_done(&o);
}

/* ---------- parts ---------- */

static sfg *point_of(const sfg *line, int k) {
    sfg *p = sfg_new(T_POINT, line->z, line->m);
    int st = stride(line);
    p->np = 1; p->c = malloc(sizeof(double) * st);
    memcpy(p->c, line->c + (long)k * st, sizeof(double) * st);
    return p;
}

/* part[g;which;n]: 0 StartPoint 1 EndPoint 2 PointN 3 ExteriorRing 4 InteriorRingN 5 GeometryN
 * 6 PatchN; n is 1-based. Inapplicable or out of range gives NULL. */
K qsf_part(K x, K which, K nk) {
    ensure();
    garg a; narg na;
    if (!geom_arg(x, &a) || !num_arg(nk, &na)) return qraise("type");
    int w = (int)(which->t == -KJ ? which->j : which->i);
    J lens[2] = { ilen(&a), nlen(&na) };
    J n = bcast(2, lens);
    if (n == -1) return krr("length");
    out o; out_init(&o, n, 0);
    for (J i = 0; i < o.n; i++) {
        sfg *g;
        if (!read_sfg(gat(&a, i), &g)) return out_fail(&o, i, "part");
        sfg *r = NULL;
        double kd = nget(&na, i);
        int k = isnan(kd) ? -1 : (int)kd;
        if (g) switch (w) {
            case 0: if (g->type == T_LINE && g->np) r = point_of(g, 0); break;
            case 1: if (g->type == T_LINE && g->np) r = point_of(g, g->np - 1); break;
            case 2: if (g->type == T_LINE && k >= 1 && k <= g->np) r = point_of(g, k - 1); break;
            case 3: if ((g->type == T_POLY || g->type == T_TRI)) r = g->n ? sfg_copy(g->s[0]) : sfg_new(T_LINE, g->z, g->m); break;
            case 4: if ((g->type == T_POLY || g->type == T_TRI) && k >= 1 && k < g->n) r = sfg_copy(g->s[k]); break;
            case 5:
                if (is_coll(g->type)) { if (k >= 1 && k <= g->n) r = sfg_copy(g->s[k - 1]); }
                else if (k == 1 && g->type != T_PS && g->type != T_TIN) r = sfg_copy(g);
                break;
            case 6: if ((g->type == T_PS || g->type == T_TIN) && k >= 1 && k <= g->n) r = sfg_copy(g->s[k - 1]); break;
        }
        if (r) r->srid = g->srid;
        kK(o.r)[i] = r ? sfg_k(r, W_EWKB) : knull();
        sfg_free(r); sfg_free(g);
    }
    return out_done(&o);
}

/* ---------- polyhedral surfaces: edges, boundary, closure, neighbours ---------- */

typedef struct { double a[3], b[3]; int patch; } edge;

static int cmp3(const double *p, const double *q) {
    for (int i = 0; i < 3; i++) if (p[i] != q[i]) return p[i] < q[i] ? -1 : 1;
    return 0;
}
static int cmp_edge(const void *x, const void *y) {
    const edge *e = x, *f = y;
    int c = cmp3(e->a, f->a);
    return c ? c : cmp3(e->b, f->b);
}
/* undirected edges of every ring of every patch, endpoints ordered */
static edge *ps_edges(const sfg *g, long *ne) {
    long cap = 0, n = 0; edge *v = NULL;
    for (int p = 0; p < g->n; p++)
        for (int r = 0; r < g->s[p]->n; r++) {
            const sfg *ring = g->s[p]->s[r]; int st = stride(ring);
            for (int k = 0; k + 1 < ring->np; k++) {
                if (n == cap) { cap = cap ? cap * 2 : 64; v = realloc(v, sizeof(edge) * (size_t)cap); }
                double Pa[3] = { ring->c[k * st], ring->c[k * st + 1], ring->z ? ring->c[k * st + 2] : 0 };
                double Qa[3] = { ring->c[(k + 1) * st], ring->c[(k + 1) * st + 1], ring->z ? ring->c[(k + 1) * st + 2] : 0 };
                if (cmp3(Pa, Qa) > 0) { double t[3]; memcpy(t, Pa, sizeof t); memcpy(Pa, Qa, sizeof t); memcpy(Qa, t, sizeof t); }
                memcpy(v[n].a, Pa, sizeof Pa); memcpy(v[n].b, Qa, sizeof Qa); v[n].patch = p; n++;
            }
        }
    if (n > 1) qsort(v, (size_t)n, sizeof(edge), cmp_edge);
    *ne = n;
    return v;
}
/* boundary of a polyhedral surface: edges used by exactly one patch, as a MultiLineString */
static sfg *ps_boundary(const sfg *g) {
    long ne; edge *e = ps_edges(g, &ne);
    sfg *r = sfg_new(T_MLINE, g->z, 0);
    for (long i = 0; i < ne;) {
        long j = i + 1;
        while (j < ne && !cmp_edge(&e[i], &e[j])) j++;
        if (j - i == 1) {
            sfg *l = sfg_new(T_LINE, g->z, 0); int st = stride(l);
            l->np = 2; l->c = malloc(sizeof(double) * 2 * st);
            for (int d = 0; d < st; d++) { l->c[d] = e[i].a[d]; l->c[st + d] = e[i].b[d]; }
            sfg_add(r, l);
        }
        i = j;
    }
    free(e);
    return r;
}
/* patches other than p sharing an edge with patch p (SFA BoundingPolygons) */
static sfg *ps_neighbours(const sfg *g, const sfg *poly) {
    int pi = -1;
    for (int p = 0; p < g->n && pi < 0; p++) {
        const sfg *c = g->s[p];
        if (c->n == poly->n && c->n > 0) {
            int same = 1;
            for (int r = 0; r < c->n && same; r++)
                same = c->s[r]->np == poly->s[r]->np &&
                       !memcmp(c->s[r]->c, poly->s[r]->c, sizeof(double) * (size_t)c->s[r]->np * stride(c));
            if (same) pi = p;
        }
    }
    if (pi < 0) return NULL;
    long ne; edge *e = ps_edges(g, &ne);
    char *mark = calloc((size_t)g->n, 1);
    for (long i = 0; i < ne;) {
        long j = i + 1;
        while (j < ne && !cmp_edge(&e[i], &e[j])) j++;
        int has = 0;
        for (long k = i; k < j; k++) if (e[k].patch == pi) has = 1;
        if (has) for (long k = i; k < j; k++) if (e[k].patch != pi) mark[e[k].patch] = 1;
        i = j;
    }
    sfg *r = sfg_new(T_MPOLY, g->z, g->m);
    for (int p = 0; p < g->n; p++) if (mark[p]) { sfg *c = sfg_copy(g->s[p]); c->type = T_POLY; sfg_add(r, c); }
    free(mark); free(e);
    return r;
}

/* ---------- GEOS unary operations ---------- */

/* unary[g;op]: 0 Boundary 1 Envelope 2 ConvexHull 3 Centroid 4 PointOnSurface 5 MakeValid
 * 6 UnaryUnion 7 Normalize */
K qsf_unary(K x, K op) {
    ensure();
    garg a; if (!geom_arg(x, &a)) return qraise("type");
    int w = (int)(op->t == -KJ ? op->j : op->i);
    static const char *nm[] = { "boundary", "envelope", "convexhull", "centroid", "pointonsurface", "makevalid", "unaryunion", "normalize" };
    if (w < 0 || w > 7) return krr("domain");
    out o; out_init(&o, a.atom ? -3 : a.n, 0);
    for (J i = 0; i < o.n; i++) {
        sfg *g;
        if (!read_sfg(gat(&a, i), &g)) return out_fail(&o, i, nm[w]);
        sfg *r = NULL;
        if (g) {
            if (w == 0 && (g->type == T_PS || g->type == T_TIN)) { r = ps_boundary(g); r->srid = g->srid; }
            else {
                GEOSGeometry *Gm = to_geos(g), *Res = NULL;
                if (!Gm) { sfg_free(g); return out_fail(&o, i, nm[w]); }
                switch (w) {
                case 0: Res = GEOSBoundary_r(ctx, Gm); break;
                case 1: Res = GEOSEnvelope_r(ctx, Gm); break;
                case 2: Res = GEOSConvexHull_r(ctx, Gm); break;
                case 3: Res = GEOSGetCentroid_r(ctx, Gm); break;
                case 4: Res = GEOSPointOnSurface_r(ctx, Gm); break;
                case 5: Res = GEOSMakeValid_r(ctx, Gm); break;
                case 6: Res = GEOSUnaryUnion_r(ctx, Gm); break;
                case 7: Res = GEOSGeom_clone_r(ctx, Gm); if (Res && GEOSNormalize_r(ctx, Res) != 0) { GEOSGeom_destroy_r(ctx, Res); Res = NULL; } break;
                }
                GEOSGeom_destroy_r(ctx, Gm);
                if (!Res) { geos_fail(nm[w]); sfg_free(g); return out_fail(&o, i, nm[w]); }
                r = from_geos(Res, g->srid);
                if (!r) { sfg_free(g); return out_fail(&o, i, nm[w]); }
            }
        }
        kK(o.r)[i] = r ? sfg_k(r, W_EWKB) : knull();
        sfg_free(r); sfg_free(g);
    }
    return out_done(&o);
}

/* bpolys[g;p]: BoundingPolygons of patch p in polyhedral surface g (NULL if p is not a patch) */
K qsf_bpolys(K x, K y) {
    ensure();
    garg a, b;
    if (!geom_arg(x, &a) || !geom_arg(y, &b)) return qraise("type");
    J lens[2] = { ilen(&a), ilen(&b) };
    J n = bcast(2, lens);
    if (n == -1) return krr("length");
    out o; out_init(&o, n, 0);
    for (J i = 0; i < o.n; i++) {
        sfg *g, *p;
        if (!read_sfg(gat(&a, i), &g)) return out_fail(&o, i, "boundingpolygons");
        if (!read_sfg(gat(&b, i), &p)) { sfg_free(g); return out_fail(&o, i, "boundingpolygons"); }
        sfg *r = NULL;
        if (g && p && (g->type == T_PS || g->type == T_TIN) && (p->type == T_POLY || p->type == T_TRI) && p->z == g->z && p->m == g->m)
            r = ps_neighbours(g, p);
        if (r) r->srid = g->srid;
        kK(o.r)[i] = r ? sfg_k(r, W_EWKB) : knull();
        sfg_free(r); sfg_free(g); sfg_free(p);
    }
    return out_done(&o);
}

/* ---------- measures and tests ---------- */

static double line_len(const sfg *g) {
    int st = stride(g); double s = 0;
    for (int k = 0; k + 1 < g->np; k++) {
        double dx = g->c[(k + 1) * st] - g->c[k * st], dy = g->c[(k + 1) * st + 1] - g->c[k * st + 1];
        s += sqrt(dx * dx + dy * dy);
    }
    return s;
}
static double curve_len(const sfg *g) {
    if (g->type == T_LINE) return line_len(g);
    if (g->type == T_MLINE || g->type == T_COLL) { double s = 0; for (int i = 0; i < g->n; i++) s += curve_len(g->s[i]); return s; }
    return 0;
}
/* area of a planar polygon in 3D (Newell's normal), holes subtracted */
static double ring_area3(const sfg *r) {
    int st = stride(r); double nx = 0, ny = 0, nz = 0;
    for (int k = 0; k + 1 < r->np; k++) {
        const double *p = r->c + (long)k * st, *q = r->c + (long)(k + 1) * st;
        double pz = r->z ? p[2] : 0, qz = r->z ? q[2] : 0;
        nx += (p[1] - q[1]) * (pz + qz);
        ny += (pz - qz) * (p[0] + q[0]);
        nz += (p[0] - q[0]) * (p[1] + q[1]);
    }
    return 0.5 * sqrt(nx * nx + ny * ny + nz * nz);
}
static double surface_area3(const sfg *g) {
    if (g->type == T_POLY || g->type == T_TRI) {
        double s = g->n ? ring_area3(g->s[0]) : 0;
        for (int r = 1; r < g->n; r++) s -= ring_area3(g->s[r]);
        return s;
    }
    double s = 0;
    for (int i = 0; i < g->n; i++) s += surface_area3(g->s[i]);
    return s;
}
static int coords_eq2(const sfg *l, int i, int j) {
    int st = stride(l);
    return l->c[i * st] == l->c[j * st] && l->c[i * st + 1] == l->c[j * st + 1];
}

/* measure[g;op] -> float (null where it does not apply; tests give 0/1)
 *  0 Area  1 Length  2 IsSimple  3 IsValid  4 IsClosed  5 IsRing  6 Perimeter */
K qsf_measure(K x, K op) {
    ensure();
    garg a; if (!geom_arg(x, &a)) return qraise("type");
    int w = (int)(op->t == -KJ ? op->j : op->i);
    out o; out_init(&o, a.atom ? -3 : a.n, KF);
    for (J i = 0; i < o.n; i++) {
        sfg *g;
        if (!read_sfg(gat(&a, i), &g)) return out_fail(&o, i, "measure");
        double v = NAN;
        if (g) {
            int ok = 1;
            if (w == 0) {
                if (has_poly3d(g) && g->z) v = surface_area3(g);
                else { GEOSGeometry *Gm = to_geos(g); ok = Gm && GEOSArea_r(ctx, Gm, &v); if (Gm) GEOSGeom_destroy_r(ctx, Gm); }
            } else if (w == 1) v = curve_len(g);
            else if (w == 2 || w == 3) {
                if (g->type == T_PS || g->type == T_TIN) { if (w == 3) v = 1; }   /* GEOS cannot judge solids; patches checked on their own */
                else {
                    GEOSGeometry *Gm = to_geos(g);
                    char c = Gm ? (w == 2 ? GEOSisSimple_r(ctx, Gm) : GEOSisValid_r(ctx, Gm)) : 2;
                    if (Gm) GEOSGeom_destroy_r(ctx, Gm);
                    if (c == 2) ok = 0; else v = c;
                }
            } else if (w == 4) {
                if (g->type == T_LINE) v = g->np > 0 && coords_eq2(g, 0, g->np - 1);
                else if (g->type == T_MLINE) { v = 1; for (int k = 0; k < g->n; k++) if (!(g->s[k]->np > 0 && coords_eq2(g->s[k], 0, g->s[k]->np - 1))) v = 0; }
                else if (g->type == T_PS || g->type == T_TIN) { sfg *b = ps_boundary(g); v = b->n == 0; sfg_free(b); }
            } else if (w == 5) {
                if (g->type == T_LINE) {
                    GEOSGeometry *Gm = to_geos(g);
                    char c = Gm ? GEOSisRing_r(ctx, Gm) : 2;
                    if (Gm) GEOSGeom_destroy_r(ctx, Gm);
                    if (c == 2) ok = 0; else v = c;
                }
            } else if (w == 6) {
                GEOSGeometry *Gm = to_geos(g); ok = Gm && GEOSLength_r(ctx, Gm, &v); if (Gm) GEOSGeom_destroy_r(ctx, Gm);
                if (ok) v -= curve_len(g);
            }
            if (!ok) { geos_fail("measure"); sfg_free(g); return out_fail(&o, i, "measure"); }
        }
        sfg_free(g);
        kF(o.r)[i] = v;
    }
    return out_done(&o);
}

/* ---------- binary predicates ---------- */

typedef char (*pred_fn)(GEOSContextHandle_t, const GEOSGeometry *, const GEOSGeometry *);
typedef char (*prep_fn)(GEOSContextHandle_t, const GEOSPreparedGeometry *, const GEOSGeometry *);
typedef struct { const char *name; pred_fn plain; prep_fn prep, flip; } pdef;

static const pdef PREDS[] = {
    { "equals",     GEOSEquals_r,     NULL,                     NULL },
    { "disjoint",   GEOSDisjoint_r,   GEOSPreparedDisjoint_r,   GEOSPreparedDisjoint_r },
    { "intersects", GEOSIntersects_r, GEOSPreparedIntersects_r, GEOSPreparedIntersects_r },
    { "touches",    GEOSTouches_r,    GEOSPreparedTouches_r,    GEOSPreparedTouches_r },
    { "crosses",    GEOSCrosses_r,    GEOSPreparedCrosses_r,    GEOSPreparedCrosses_r },
    { "within",     GEOSWithin_r,     GEOSPreparedWithin_r,     GEOSPreparedContains_r },
    { "contains",   GEOSContains_r,   GEOSPreparedContains_r,   GEOSPreparedWithin_r },
    { "overlaps",   GEOSOverlaps_r,   GEOSPreparedOverlaps_r,   GEOSPreparedOverlaps_r },
    { "covers",     GEOSCovers_r,     GEOSPreparedCovers_r,     GEOSPreparedCoveredBy_r },
    { "coveredby",  GEOSCoveredBy_r,  GEOSPreparedCoveredBy_r,  GEOSPreparedCovers_r },
};

/* geometries of a broadcast argument, parsed once when it is an atom */
typedef struct { garg a; sfg *t; GEOSGeometry *Gm; } side;
static int side_get(side *s, J i, sfg **t, GEOSGeometry **Gm, int *own) {
    if (s->a.atom) { *t = s->t; *Gm = s->Gm; *own = 0; return 1; }
    *own = 1; *Gm = NULL;
    if (!read_sfg(kK(s->a.x)[i], t)) return 0;
    if (*t && !(*Gm = to_geos(*t))) { sfg_free(*t); return 0; }
    return 1;
}
static int side_open(side *s) {
    s->t = NULL; s->Gm = NULL;
    if (!s->a.atom) return 1;
    if (!read_sfg(s->a.x, &s->t)) return 0;
    if (s->t && !(s->Gm = to_geos(s->t))) { sfg_free(s->t); s->t = NULL; return 0; }
    return 1;
}
static void side_close(side *s) { if (s->Gm) GEOSGeom_destroy_r(ctx, s->Gm); sfg_free(s->t); }

/* pred[a;b;op] -> boolean; op indexes PREDS. NULL on either side gives 0b. */
K qsf_pred(K x, K y, K op) {
    ensure();
    side A, B;
    if (!geom_arg(x, &A.a) || !geom_arg(y, &B.a)) return qraise("type");
    int w = (int)(op->t == -KJ ? op->j : op->i);
    if (w < 0 || w >= (int)(sizeof PREDS / sizeof PREDS[0])) return krr("domain");
    const pdef *p = &PREDS[w];
    J lens[2] = { ilen(&A.a), ilen(&B.a) };
    J n = bcast(2, lens);
    if (n == -1) return krr("length");
    if (!side_open(&A)) return qraise(p->name);
    if (!side_open(&B)) { side_close(&A); return qraise(p->name); }
    out o; out_init(&o, n, KB);
    /* an atom side against a column is prepared once */
    const GEOSPreparedGeometry *PG = NULL; prep_fn pf = NULL;
    if (!o.atom && (A.a.atom != B.a.atom) && p->prep) {
        side *f = A.a.atom ? &A : &B;
        if (f->Gm) { PG = GEOSPrepare_r(ctx, f->Gm); pf = A.a.atom ? p->prep : p->flip; }
    }
    int bad = 0; J i;
    for (i = 0; i < o.n; i++) {
        sfg *ta, *tb; GEOSGeometry *ga, *gb; int oa, ob;
        if (!side_get(&A, i, &ta, &ga, &oa)) { bad = 1; break; }
        if (!side_get(&B, i, &tb, &gb, &ob)) { if (oa) { if (ga) GEOSGeom_destroy_r(ctx, ga); sfg_free(ta); } bad = 1; break; }
        char c = 0;
        if (ta && tb) {
            if (!srid_check(ta, tb)) c = 2;
            else if (PG) c = pf(ctx, PG, A.a.atom ? gb : ga);
            else c = p->plain(ctx, ga, gb);
            if (c == 2) geos_fail(p->name);
        }
        if (oa) { if (ga) GEOSGeom_destroy_r(ctx, ga); sfg_free(ta); }
        if (ob) { if (gb) GEOSGeom_destroy_r(ctx, gb); sfg_free(tb); }
        if (c == 2) { bad = 1; break; }
        kG(o.r)[i] = (G)c;
    }
    if (PG) GEOSPreparedGeom_destroy_r(ctx, PG);
    side_close(&A); side_close(&B);
    if (bad) return out_fail(&o, i, p->name);
    return out_done(&o);
}

/* relate[a;b;pattern]: pattern "" gives the DE-9IM matrix string, else a boolean */
K qsf_relate(K x, K y, K pat) {
    ensure();
    side A, B;
    if (!geom_arg(x, &A.a) || !geom_arg(y, &B.a)) return qraise("type");
    char *pt = kstr(pat);
    if (!pt) return krr("type");
    int matrix = pt[0] == 0;
    J lens[2] = { ilen(&A.a), ilen(&B.a) };
    J n = bcast(2, lens);
    if (n == -1) { free(pt); return krr("length"); }
    if (!side_open(&A)) { free(pt); return qraise("relate"); }
    if (!side_open(&B)) { side_close(&A); free(pt); return qraise("relate"); }
    out o; out_init(&o, n, matrix ? 0 : KB);
    int bad = 0; J i;
    for (i = 0; i < o.n; i++) {
        sfg *ta, *tb; GEOSGeometry *ga, *gb; int oa, ob;
        if (!side_get(&A, i, &ta, &ga, &oa)) { bad = 1; break; }
        if (!side_get(&B, i, &tb, &gb, &ob)) { if (oa) { if (ga) GEOSGeom_destroy_r(ctx, ga); sfg_free(ta); } bad = 1; break; }
        K e = NULL; char c = 0;
        if (ta && tb && !srid_check(ta, tb)) bad = 1;
        else if (matrix) {
            if (ta && tb) {
                char *m = GEOSRelate_r(ctx, ga, gb);
                if (!m) { geos_fail("relate"); bad = 1; }
                else { e = kp(m); GEOSFree_r(ctx, m); }
            } else e = ktn(KC, 0);
        } else if (ta && tb) {
            c = GEOSRelatePattern_r(ctx, ga, gb, pt);
            if (c == 2) { geos_fail("relate"); bad = 1; }
        }
        if (oa) { if (ga) GEOSGeom_destroy_r(ctx, ga); sfg_free(ta); }
        if (ob) { if (gb) GEOSGeom_destroy_r(ctx, gb); sfg_free(tb); }
        if (bad) break;
        if (matrix) kK(o.r)[i] = e; else kG(o.r)[i] = (G)c;
    }
    side_close(&A); side_close(&B); free(pt);
    if (bad) return out_fail(&o, i, "relate");
    return out_done(&o);
}

/* distance[a;b] -> float (NULL or EMPTY on either side gives null) */
K qsf_distance(K x, K y) {
    ensure();
    side A, B;
    if (!geom_arg(x, &A.a) || !geom_arg(y, &B.a)) return qraise("type");
    J lens[2] = { ilen(&A.a), ilen(&B.a) };
    J n = bcast(2, lens);
    if (n == -1) return krr("length");
    if (!side_open(&A)) return qraise("distance");
    if (!side_open(&B)) { side_close(&A); return qraise("distance"); }
    out o; out_init(&o, n, KF);
    const GEOSPreparedGeometry *PG = NULL;
    if (!o.atom && A.a.atom != B.a.atom) { side *f = A.a.atom ? &A : &B; if (f->Gm && !GEOSisEmpty_r(ctx, f->Gm)) PG = GEOSPrepare_r(ctx, f->Gm); }
    int bad = 0; J i;
    for (i = 0; i < o.n; i++) {
        sfg *ta, *tb; GEOSGeometry *ga, *gb; int oa, ob;
        if (!side_get(&A, i, &ta, &ga, &oa)) { bad = 1; break; }
        if (!side_get(&B, i, &tb, &gb, &ob)) { if (oa) { if (ga) GEOSGeom_destroy_r(ctx, ga); sfg_free(ta); } bad = 1; break; }
        double d = NAN;
        if (ta && tb) {
            if (!srid_check(ta, tb)) bad = 1;
            else if (!is_empty(ta) && !is_empty(tb)) {
                int ok = PG ? GEOSPreparedDistance_r(ctx, PG, A.a.atom ? gb : ga, &d) : GEOSDistance_r(ctx, ga, gb, &d);
                if (!ok) { geos_fail("distance"); bad = 1; }
            }
        }
        if (oa) { if (ga) GEOSGeom_destroy_r(ctx, ga); sfg_free(ta); }
        if (ob) { if (gb) GEOSGeom_destroy_r(ctx, gb); sfg_free(tb); }
        if (bad) break;
        kF(o.r)[i] = d;
    }
    if (PG) GEOSPreparedGeom_destroy_r(ctx, PG);
    side_close(&A); side_close(&B);
    if (bad) return out_fail(&o, i, "distance");
    return out_done(&o);
}

/* overlay[a;b;op]: 0 Intersection 1 Union 2 Difference 3 SymDifference */
K qsf_overlay(K x, K y, K op) {
    ensure();
    side A, B;
    if (!geom_arg(x, &A.a) || !geom_arg(y, &B.a)) return qraise("type");
    int w = (int)(op->t == -KJ ? op->j : op->i);
    static const char *nm[] = { "intersection", "union", "difference", "symdifference" };
    if (w < 0 || w > 3) return krr("domain");
    J lens[2] = { ilen(&A.a), ilen(&B.a) };
    J n = bcast(2, lens);
    if (n == -1) return krr("length");
    if (!side_open(&A)) return qraise(nm[w]);
    if (!side_open(&B)) { side_close(&A); return qraise(nm[w]); }
    out o; out_init(&o, n, 0);
    int bad = 0; J i;
    for (i = 0; i < o.n; i++) {
        sfg *ta, *tb; GEOSGeometry *ga, *gb; int oa, ob;
        if (!side_get(&A, i, &ta, &ga, &oa)) { bad = 1; break; }
        if (!side_get(&B, i, &tb, &gb, &ob)) { if (oa) { if (ga) GEOSGeom_destroy_r(ctx, ga); sfg_free(ta); } bad = 1; break; }
        K e = NULL;
        if (!ta || !tb) e = knull();
        else if (!srid_check(ta, tb)) bad = 1;
        else {
            GEOSGeometry *Res = w == 0 ? GEOSIntersection_r(ctx, ga, gb) : w == 1 ? GEOSUnion_r(ctx, ga, gb) :
                              w == 2 ? GEOSDifference_r(ctx, ga, gb) : GEOSSymDifference_r(ctx, ga, gb);
            if (!Res) { geos_fail(nm[w]); bad = 1; }
            else { sfg *r = from_geos(Res, ta->srid ? ta->srid : tb->srid); if (!r) bad = 1; else { e = sfg_k(r, W_EWKB); sfg_free(r); } }
        }
        if (oa) { if (ga) GEOSGeom_destroy_r(ctx, ga); sfg_free(ta); }
        if (ob) { if (gb) GEOSGeom_destroy_r(ctx, gb); sfg_free(tb); }
        if (bad) break;
        kK(o.r)[i] = e;
    }
    side_close(&A); side_close(&B);
    if (bad) return out_fail(&o, i, nm[w]);
    return out_done(&o);
}

/* unionagg[gs]: union of every non-null geometry of a list (aggregate ST_Union) */
K qsf_unionagg(K x) {
    ensure();
    garg a; if (!geom_arg(x, &a)) return qraise("type");
    GEOSGeometry **parts = malloc(sizeof(GEOSGeometry *) * (size_t)(a.n ? a.n : 1));
    int k = 0, srid = 0, z = 0, bad = 0;
    for (J i = 0; i < a.n && !bad; i++) {
        sfg *g;
        if (!read_sfg(gat(&a, i), &g)) { bad = 1; break; }
        if (!g) continue;
        if (srid && g->srid && g->srid != srid) { seterr("operation on mixed SRIDs (%d and %d)", srid, g->srid); bad = 1; }
        if (!srid) srid = g->srid;
        z |= g->z;
        GEOSGeometry *Gm = bad ? NULL : to_geos(g);
        sfg_free(g);
        if (!bad && !Gm) bad = 1;
        if (Gm) parts[k++] = Gm;
    }
    if (bad) { for (int i = 0; i < k; i++) GEOSGeom_destroy_r(ctx, parts[i]); free(parts); return qraise("union"); }
    if (!k) { free(parts); return knull(); }
    GEOSGeometry *C = GEOSGeom_createCollection_r(ctx, GEOS_GEOMETRYCOLLECTION, parts, (unsigned)k);
    free(parts);
    GEOSGeometry *Res = C ? GEOSUnaryUnion_r(ctx, C) : NULL;
    if (C) GEOSGeom_destroy_r(ctx, C);
    if (!Res) { geos_fail("union"); return qraise("union"); }
    sfg *r = from_geos(Res, srid);
    if (!r) return qraise("union");
    K e = sfg_k(r, W_EWKB); sfg_free(r);
    return e;
}

/* buffer[g;d;quadsegs] */
K qsf_buffer(K x, K d, K qs) {
    ensure();
    garg a; narg da;
    if (!geom_arg(x, &a) || !num_arg(d, &da)) return qraise("type");
    int q = (int)(qs->t == -KJ ? qs->j : qs->t == -KI ? qs->i : 8);
    J lens[2] = { ilen(&a), nlen(&da) };
    J n = bcast(2, lens);
    if (n == -1) return krr("length");
    out o; out_init(&o, n, 0);
    for (J i = 0; i < o.n; i++) {
        sfg *g;
        if (!read_sfg(gat(&a, i), &g)) return out_fail(&o, i, "buffer");
        double dd = nget(&da, i);
        K e = knull();
        if (g && !isnan(dd)) {
            GEOSGeometry *Gm = to_geos(g), *Res = Gm ? GEOSBuffer_r(ctx, Gm, dd, q) : NULL;
            if (Gm) GEOSGeom_destroy_r(ctx, Gm);
            sfg *r = Res ? from_geos(Res, g->srid) : NULL;
            if (!r) { if (!Res) geos_fail("buffer"); sfg_free(g); r0(e); return out_fail(&o, i, "buffer"); }
            r0(e); e = sfg_k(r, W_EWKB); sfg_free(r);
        }
        sfg_free(g);
        kK(o.r)[i] = e;
    }
    return out_done(&o);
}

/* ---------- linear referencing by measure ---------- */

static void push_point(sfg *mp, const double *c, int z, int m) {
    int st = 2 + z + m;
    if (mp->n) {   /* drop an immediate repeat */
        const sfg *last = mp->s[mp->n - 1];
        if (!memcmp(last->c, c, sizeof(double) * st)) return;
    }
    sfg *p = sfg_new(T_POINT, z, m);
    p->np = 1; p->c = malloc(sizeof(double) * st);
    memcpy(p->c, c, sizeof(double) * st);
    sfg_add(mp, p);
}
static void interp(const double *p, const double *q, double t, int st, double *out) {
    for (int d = 0; d < st; d++) out[d] = p[d] + t * (q[d] - p[d]);
}

/* LocateAlong on one linestring: points where the measure equals mv */
static void along_line(const sfg *l, double mv, sfg *out) {
    int st = stride(l), mi = 2 + l->z; double c[4];
    if (l->np == 1 && l->c[mi] == mv) push_point(out, l->c, l->z, l->m);
    for (int k = 0; k + 1 < l->np; k++) {
        const double *p = l->c + (long)k * st, *q = p + st;
        double m0 = p[mi], m1 = q[mi];
        if (m0 == m1) {
            if (m0 == mv) { push_point(out, p, l->z, l->m); push_point(out, q, l->z, l->m); }
            continue;
        }
        if ((mv - m0) * (mv - m1) <= 0) {
            interp(p, q, (mv - m0) / (m1 - m0), st, c);
            c[mi] = mv;
            push_point(out, c, l->z, l->m);
        }
    }
}
/* LocateBetween on one linestring: the parts with lo <= m <= hi, as linestrings and points */
static void between_line(const sfg *l, double lo, double hi, sfg *lines, sfg *pts) {
    int st = stride(l), mi = 2 + l->z;
    double *buf = NULL; int nb = 0, cap = 0;
#define PUSHC(cc) do { if (nb == cap) { cap = cap ? cap * 2 : 16; buf = realloc(buf, sizeof(double) * (size_t)cap * st); } \
                       if (!nb || memcmp(buf + (long)(nb - 1) * st, (cc), sizeof(double) * st)) { memcpy(buf + (long)nb * st, (cc), sizeof(double) * st); nb++; } } while (0)
#define FLUSH() do { if (nb == 1) push_point(pts, buf, l->z, l->m); \
                     else if (nb > 1) { sfg *s = sfg_new(T_LINE, l->z, l->m); s->np = nb; s->c = malloc(sizeof(double) * (size_t)nb * st); \
                                         memcpy(s->c, buf, sizeof(double) * (size_t)nb * st); sfg_add(lines, s); } nb = 0; } while (0)
    if (l->np == 1) { double m = l->c[mi]; if (m >= lo && m <= hi) push_point(pts, l->c, l->z, l->m); return; }
    for (int k = 0; k + 1 < l->np; k++) {
        const double *p = l->c + (long)k * st, *q = p + st;
        double m0 = p[mi], m1 = q[mi], c[4];
        double a = fmin(m0, m1), b = fmax(m0, m1);
        if (b < lo || a > hi) { FLUSH(); continue; }
        /* clip the segment to [lo, hi] in its own direction */
        double t0 = 0, t1 = 1;
        if (m1 != m0) {
            double ta = (lo - m0) / (m1 - m0), tb = (hi - m0) / (m1 - m0);
            double tl = fmin(ta, tb), th = fmax(ta, tb);
            t0 = fmax(0, tl); t1 = fmin(1, th);
        }
        if (t0 > 0) FLUSH();
        if (t0 == 0) PUSHC(p); else { interp(p, q, t0, st, c); PUSHC(c); }
        if (t1 == 1) PUSHC(q); else { interp(p, q, t1, st, c); PUSHC(c); FLUSH(); }
    }
    FLUSH();
    free(buf);
#undef PUSHC
#undef FLUSH
}
static int locate_walk(const sfg *g, double lo, double hi, int along, sfg *lines, sfg *pts) {
    int mi = 2 + g->z;
    switch (g->type) {
    case T_POINT:
        if (g->np && g->c[mi] >= lo && g->c[mi] <= hi) push_point(pts, g->c, g->z, g->m);
        return 1;
    case T_LINE:
        if (along) along_line(g, lo, pts); else between_line(g, lo, hi, lines, pts);
        return 1;
    case T_MPOINT: case T_MLINE: case T_COLL:
        for (int i = 0; i < g->n; i++) if (!locate_walk(g->s[i], lo, hi, along, lines, pts)) return 0;
        return 1;
    default:
        seterr("locate: %s is not supported (points and lines only)", type_name(g->type));
        return 0;
    }
}

/* locate[g;lo;hi;along]: LocateAlong (along = 1, lo is the measure) or LocateBetween([lo, hi]).
 * Points only -> MultiPoint, lines only -> MultiLineString, both -> GeometryCollection. */
K qsf_locate(K x, K lo, K hi, K along) {
    ensure();
    garg a; narg la, ha;
    if (!geom_arg(x, &a) || !num_arg(lo, &la) || !num_arg(hi, &ha)) return qraise("type");
    int al = along->g;
    J lens[3] = { ilen(&a), nlen(&la), nlen(&ha) };
    J n = bcast(3, lens);
    if (n == -1) return krr("length");
    out o; out_init(&o, n, 0);
    for (J i = 0; i < o.n; i++) {
        sfg *g;
        if (!read_sfg(gat(&a, i), &g)) return out_fail(&o, i, "locate");
        K e = knull();
        if (g) {
            if (!g->m) { seterr("locate: geometry has no measures"); sfg_free(g); r0(e); return out_fail(&o, i, "locate"); }
            double l = nget(&la, i), h = al ? l : nget(&ha, i);
            if (l > h) { double t = l; l = h; h = t; }
            sfg *lines = sfg_new(T_MLINE, g->z, 1), *pts = sfg_new(T_MPOINT, g->z, 1);
            if (!locate_walk(g, l, h, al, lines, pts)) { sfg_free(lines); sfg_free(pts); sfg_free(g); r0(e); return out_fail(&o, i, "locate"); }
            sfg *r;
            if (al || !lines->n) { r = pts; sfg_free(lines); }
            else if (!pts->n) { r = lines; sfg_free(pts); }
            else { r = sfg_new(T_COLL, g->z, 1); sfg_add(r, lines); sfg_add(r, pts); }
            r->srid = g->srid;
            r0(e); e = sfg_k(r, W_EWKB);
            sfg_free(r);
            sfg_free(g);
        }
        kK(o.r)[i] = e;
    }
    return out_done(&o);
}

/* ---------- constructors ---------- */

/* point[x;y;z;m;srid]: z and m are (::) when absent */
K qsf_point(K x, K y, K z, K m, K srid) {
    ensure();
    narg xa, ya, za = {0}, ma = {0}, sa;
    int hz = z->t != 101, hm = m->t != 101;
    if (!num_arg(x, &xa) || !num_arg(y, &ya) || (hz && !num_arg(z, &za)) || (hm && !num_arg(m, &ma)) || !num_arg(srid, &sa)) return qraise("type");
    J lens[5] = { nlen(&xa), nlen(&ya), hz ? nlen(&za) : -1, hm ? nlen(&ma) : -1, nlen(&sa) };
    J n = bcast(5, lens);
    if (n == -1) return krr("length");
    out o; out_init(&o, n, 0);
    for (J i = 0; i < o.n; i++) {
        sfg *p = sfg_new(T_POINT, hz, hm);
        double s = nget(&sa, i);
        p->srid = isnan(s) ? 0 : (int)s;
        p->np = 1; p->c = malloc(sizeof(double) * 4);
        int k = 0;
        p->c[k++] = nget(&xa, i); p->c[k++] = nget(&ya, i);
        if (hz) p->c[k++] = nget(&za, i);
        if (hm) p->c[k++] = nget(&ma, i);
        int allnan = 1;
        for (int d = 0; d < k; d++) if (!isnan(p->c[d])) allnan = 0;
        if (allnan) p->np = 0;
        kK(o.r)[i] = sfg_k(p, W_EWKB);
        sfg_free(p);
    }
    return out_done(&o);
}

/* bdpoly[g;multi]: BdPolyFromText/WKB core — the area enclosed by a MultiLineString of closed rings */
K qsf_bdpoly(K x, K multi) {
    ensure();
    garg a; if (!geom_arg(x, &a)) return qraise("type");
    out o; out_init(&o, a.atom ? -3 : a.n, 0);
    for (J i = 0; i < o.n; i++) {
        sfg *g;
        if (!read_sfg(gat(&a, i), &g)) return out_fail(&o, i, "bdpoly");
        K e = knull();
        if (g) {
            int bad = 0;
            if (g->type != T_MLINE) { seterr("bdpoly: input must be a MultiLineString"); bad = 1; }
            for (int k = 0; !bad && k < g->n; k++)
                if (g->s[k]->np < 4 || !coords_eq2(g->s[k], 0, g->s[k]->np - 1)) { seterr("bdpoly: ring %d is not closed", k + 1); bad = 1; }
            sfg *r = NULL;
            if (!bad) {
                GEOSGeometry *Gm = to_geos(g), *Res = Gm ? GEOSBuildArea_r(ctx, Gm) : NULL;
                if (Gm) GEOSGeom_destroy_r(ctx, Gm);
                if (!Res) { geos_fail("bdpoly"); bad = 1; }
                else if (!(r = from_geos(Res, g->srid))) bad = 1;
                else if (!multi->g && r->type != T_POLY) { seterr("bdpoly: the rings do not form a single polygon"); bad = 1; }
                else if (multi->g && r->type == T_POLY) { sfg *mp = sfg_new(T_MPOLY, r->z, r->m); mp->srid = r->srid; r->srid = 0; sfg_add(mp, r); r = mp; }
            }
            if (bad) { sfg_free(r); sfg_free(g); r0(e); return out_fail(&o, i, "bdpoly"); }
            r0(e); e = sfg_k(r, W_EWKB); sfg_free(r);
            sfg_free(g);
        }
        kK(o.r)[i] = e;
    }
    return out_done(&o);
}

/* ---------- spatial reference systems ---------- */

static OGRSpatialReferenceH srs_from(const char *def) {
    OGRSpatialReferenceH s = OSRNewSpatialReference(NULL);
    if (OSRSetFromUserInput(s, def) != OGRERR_NONE) { OSRDestroySpatialReference(s); return NULL; }
    OSRSetAxisMappingStrategy(s, OAMS_TRADITIONAL_GIS_ORDER);
    return s;
}

/* srs[def] -> (wkt; authority name; authority code) for "EPSG:4326", a PROJ string or WKT */
K qsf_srs(K def) {
    char *d = kstr(def);
    if (!d) return krr("type");
    OGRSpatialReferenceH s = srs_from(d);
    free(d);
    if (!s) { seterr("srs: cannot interpret the definition (%s)", CPLGetLastErrorMsg()); return qraise("srs"); }
    char *w = NULL;
    OSRExportToWkt(s, &w);
    const char *an = OSRGetAuthorityName(s, NULL), *ac = OSRGetAuthorityCode(s, NULL);
    K r = knk(3, kp(w ? w : (S)""), kp((S)(an ? an : "")), kj(ac ? atol(ac) : nj));
    CPLFree(w);
    OSRDestroySpatialReference(s);
    return r;
}

/* transform[g;from;to;srid]: reproject with PROJ (x = easting/longitude, y = northing/latitude);
 * the result carries srid. Z and M are kept. */
K qsf_transform(K x, K from, K to, K srid) {
    ensure();
    garg a; if (!geom_arg(x, &a)) return qraise("type");
    char *f = kstr(from), *t = kstr(to);
    if (!f || !t) { free(f); free(t); return krr("type"); }
    OGRSpatialReferenceH sf = srs_from(f), st = sf ? srs_from(t) : NULL;
    free(f); free(t);
    if (!sf || !st) { if (sf) OSRDestroySpatialReference(sf); seterr("transform: bad spatial reference"); return qraise("transform"); }
    OGRCoordinateTransformationH ct = OCTNewCoordinateTransformation(sf, st);
    OSRDestroySpatialReference(sf); OSRDestroySpatialReference(st);
    if (!ct) { seterr("transform: %s", CPLGetLastErrorMsg()); return qraise("transform"); }
    int ns = (int)(srid->t == -KJ ? srid->j : srid->i);
    out o; out_init(&o, a.atom ? -3 : a.n, 0);
    J i; int bad = 0;
    for (i = 0; i < o.n; i++) {
        sfg *g;
        if (!read_sfg(gat(&a, i), &g)) { bad = 1; break; }
        K e = knull();
        if (g) {
            sfg *r = NULL;
            if (is_empty(g)) { r = sfg_copy(g); r->srid = ns; }
            else {
                OGRGeometryH og = to_ogr(g);
                if (og && OGR_G_Transform(og, ct) == OGRERR_NONE) r = from_ogr(og, ns);
                else if (og) seterr("transform: %s", CPLGetLastErrorMsg()[0] ? CPLGetLastErrorMsg() : "point outside the projection's domain");
                if (og) OGR_G_DestroyGeometry(og);
            }
            sfg_free(g);
            if (!r) { r0(e); bad = 1; break; }
            r0(e); e = sfg_k(r, W_EWKB); sfg_free(r);
        }
        kK(o.r)[i] = e;
    }
    OCTDestroyCoordinateTransformation(ct);
    if (bad) return out_fail(&o, i, "transform");
    return out_done(&o);
}

/* version[] -> (GEOS; GDAL) */
K qsf_version(K x) {
    (void)x;
    return knk(2, kp((S)GEOSversion()), kp((S)GDALVersionInfo("RELEASE_NAME")));
}
