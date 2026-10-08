# qgeos

GEOS geometry and GDAL file/CRS/raster functions for q, written against the standard kdb
`k.h` C API. Runs on peachq (the `-glibc` builds, or built from source) and should run on
kdb+ unchanged.

Three libraries that share one geometry format (WKB byte vectors):

- `qsf.so` + `st.q`: the OGC Simple Features standard (SFA 1.2.1 / ISO 19125), with its SQL
  function names, `GEOMETRY_COLUMNS` and `SPATIAL_REF_SYS`. Needs GEOS and GDAL.
- `qgeos.so` + `geos.q`: geometry operations and spatial joins. Needs GEOS.
- `qgdal.so` + `gdal.q`: read/write vector files, reproject, read rasters. Needs GDAL.

## Build and test

```sh
# GEOS >= 3.12 and GDAL >= 3.0 (tested: GEOS 3.12.1, GDAL 3.8.4; tests also use gdal-bin)
#   apt install libgeos-dev libgdal-dev gdal-bin   /   brew install geos gdal
cp /path/to/peachq/third_party/k.h .     # or kdb's k.h
make                                     # or: make qgeos.so / make qgdal.so
make test Q=/path/to/q                   # 43 + 57 + 116 checks
```

## Simple Features (`st.q`)

`st.q` implements OGC Simple Feature Access 1.2.1 (ISO 19125), part 1 (the geometry object
model) and part 2 (the SQL option), on GEOS and GDAL. The names are the SQL functions without
their `ST_` prefix. It passes all 52 items of the OGC SFS 1.2.1 conformance suite (T1-T52, the
OGC CITE scripts as kept in PostGIS's `extras/ogc_test_suite`), plus 64 checks of its own on Z/M,
ISO WKB, measures, polyhedral surfaces, transformation and the catalogues.

```q
\l st.q
lake:.st.GeomFromText["POLYGON((52 18,66 23,73 9,48 6,52 18),(59 18,67 18,67 13,59 13,59 18))";101]
.st.Area lake                                   / 219.5
.st.AsText .st.InteriorRingN[lake;1]            / "LINESTRING (59 18, 67 18, 67 13, 59 13, 59 18)"
.st.Relate[lake;.st.GeomFromText["POINT(60 10)";101];"T********"]   / 1b
r:.st.GeomFromText["LINESTRING M (0 0 0, 10 0 10, 10 10 20)";0]
.st.AsText .st.LocateBetween[r;5;12]            / "MULTILINESTRING M ((5 0 5, 10 0 10, 10 2 12))"
.st.Transform[.st.GeomFromText["POINT(-99 30)";4326];32614]   / EPSG codes come from PROJ
```

Geometries are extended-WKB byte vectors (PostGIS layout) that carry their SRID and their Z and M
flags; a column is a list of them, and every function takes one geometry or a column (binary
functions broadcast an atom against a column, and prepare it once). An empty byte vector,
`.st.NULL`, is SQL NULL: it gives null results (`0n`, `0N`, `""`, NULL geometries, `0b` for
tests). An EMPTY geometry (`POINT EMPTY`) is a real geometry.

| SFA | Functions |
|---|---|
| Constructors | `GeomFromText`, `PointFromText`, `LineFromText`, `PolyFromText`, `MPointFromText`, `MLineFromText`, `MPolyFromText`, `GeomCollFromText`, `BdPolyFromText`, `BdMPolyFromText`, the same with `FromWKB`, `GeomFromEWKT`, `GeomFromEWKB`, `Point`, `PointZ`, `PointM`, `PointZM`, `SetSRID` |
| Output | `AsText`, `AsBinary` (ISO WKB), `AsBinaryXDR`, `AsEWKT`, `AsEWKB` |
| Geometry | `Dimension`, `CoordinateDimension` (`CoordDim`), `SpatialDimension`, `GeometryType`, `SRID`, `Envelope`, `IsEmpty`, `IsSimple`, `Is3D`, `IsMeasured`, `Boundary`, `IsValid`, `MakeValid`, `Normalize`, `NPoints` |
| Relations | `Equals`, `Disjoint`, `Intersects`, `Touches`, `Crosses`, `Within`, `Contains`, `Overlaps`, `Relate[a;b;pattern]`, `RelateMatrix`, `Covers`, `CoveredBy` |
| Measures | `LocateAlong[g;m]`, `LocateBetween[g;m1;m2]` |
| Analysis | `Distance`, `Buffer[g;d]` (`BufferQ` with quadrant segments), `ConvexHull`, `Intersection`, `Union`, `Difference`, `SymDifference`, `UnionAgg` (aggregate) |
| Point, Curve, LineString | `X`, `Y`, `Z`, `M`, `Length`, `StartPoint`, `EndPoint`, `IsClosed`, `IsRing`, `NumPoints`, `PointN` |
| Surface, Polygon | `Area`, `Perimeter`, `Centroid`, `PointOnSurface`, `ExteriorRing`, `NumInteriorRing(s)`, `InteriorRingN` |
| PolyhedralSurface, TIN | `NumPatches`, `PatchN`, `BoundingPolygons[g;patch]`, `IsClosed`, `Area` (3D) |
| GeometryCollection | `NumGeometries`, `GeometryN` |
| Reference systems | `Transform[g;srid]`, `spatial_ref_sys`, `AddSRS[srid;auth;code;srtext]`, `EPSG code` |
| Catalogue | `geometry_columns`, `AddGeometryColumn[t;col;srid;type;dim]`, `DropGeometryColumn`, `RecoverGeometryColumn`, `CheckGeometryColumn` |

How the work is split: WKB parsing and writing, every accessor, `LocateAlong`/`LocateBetween`
and the polyhedral-surface logic (boundary edges, closure, neighbouring patches, 3D area) are
in `qsf.c`. GEOS does the topology, distance and planar area, and writes WKT. OGR reads and
writes WKT for PolyhedralSurface, TIN and Triangle (GEOS has no such types) and transforms
through PROJ.

Notes on the choices the standard leaves open:

- Typed constructors (`PointFromText`, ...) return NULL for text of another type, as PostGIS does.
- `NumPoints`, `PointN`, `StartPoint`, `ExteriorRing`, `X` and the like return NULL when the
  geometry is not of the type they are defined on. `NumGeometries` of a single geometry is 1, and
  `GeometryN[g;1]` is the geometry itself.
- `Length` measures curves (polygons give 0; `Perimeter` measures rings).
- Operations on two geometries with different non-zero SRIDs are an error.
- GEOS works in the plane: a PolyhedralSurface or TIN goes to it as a MultiPolygon (so relations
  and envelopes see its footprint), and measures are dropped from GEOS results. `Area` of a
  polyhedral surface or TIN with Z is the true 3D surface area.
- `LocateAlong` gives a MultiPoint; `LocateBetween` gives a MultiPoint, a MultiLineString or a
  GeometryCollection of both, interpolating Z and M. Both take points and lines; polygons are an error.
- `Transform` looks the SRID up in `spatial_ref_sys`; an SRID that is not there is taken as an
  EPSG code and added from PROJ. Axis order is always x = longitude/easting.
- Tables from `.gdal.read` hold plain ISO WKB: `.st.GeomFromWKB[t`geom;srid]` adds the SRID.
  `geos.q` reads these geometries unchanged.

Timings (one core):

| Case | Time |
|---|---|
| `Point` 1M; `X` 1M | 0.15 / 0.09 s |
| `Within` 1M points vs a polygon (prepared) | 0.26 s |
| `Distance` 1M points vs a polygon | 0.51 s |
| `Area` / `AsText` 100k polygons | 0.14 / 1.0 s |
| `Intersection` 100k polygons vs a polygon | 0.70 s |
| `Transform` 100k points 4326 → 3857 | 0.08 s |

## Use

```q
\l geos.q
sq:.geos.fromwkt "POLYGON((0 0,10 0,10 10,0 10,0 0))"
t:([] id:til 3; geom:.geos.point[1 15 5f;1 5 9f])
select id from t where .geos.within[geom;sq]
update d:.geos.distance[geom;sq] from t
```

Geometries are WKB byte vectors (`4h`); a column is a general list of them.
Every function takes one geometry or a list; binary functions broadcast an atom against a list.
An empty byte vector is a null geometry and gives a null result.

| Function | Args | Returns |
|---|---|---|
| `fromwkt` / `towkt` | string(s) / geom(s) | geom(s) / string(s) |
| `point[x;y]` | floats or longs, atoms or vectors | geom(s) |
| `area`, `length` | geom(s) | float(s) |
| `centroid`, `envelope`, `hull` | geom(s) | geom(s) |
| `buffer[g;r]` | geom(s), radius atom | geom(s) |
| `distance[a;b]` | geom(s), geom(s) | float(s) |
| `intersects`, `contains`, `within` | geom(s), geom(s) | boolean(s) |
| ``sjoin[l;r;`pred]`` | geoms, geoms, `` `intersects`contains`within `` | `(li;ri)` index pairs |
| `nearest[l;r]` | geom(s), geoms | index into `r` (long), `0N` if none |
| `version[]` | | GEOS version string |

When one side of a predicate is a single geometry, it is "prepared" once (GEOS builds an
edge index), so polygon-vs-column tests stay fast: 1M points `within` a polygon in ~170 ms.

## Spatial join

`sjoin` builds an STRtree on the right side, finds candidates by bounding box, then tests the
exact predicate with each right geometry prepared once. Put the side with fewer, larger shapes
(polygons) on the right. Pairs come back ordered by `li`, then `ri`.

```q
j:.geos.sjoin[points`geom;zones`geom;`within]
([] pt:points[`id] j 0; zone:zones[`name] j 1)
update zone:zones[`name] .geos.nearest[geom;zones`geom] from points
```

`nearest` returns the closest right geometry per left one. When several touch it (distance 0),
the lowest index wins; other ties go to whichever the tree finds first.

Timings (one core, GEOS 3.12):

| Case | Time |
|---|---|
| `sjoin` 1M points × 5k polygons, `within` | 1.0 s |
| `sjoin` 100k × 100k polygons, `intersects` | 4.1 s |
| `nearest` 1M points × 5k polygons | 9–14 s |

## GDAL: files, CRS, rasters

```q
\l gdal.q
.gdal.layers `:city.gpkg                          / `parcels`roads
p:.gdal.read[`:city.gpkg;`parcels]                / table + `geom column
.gdal.info[`:city.gpkg;`parcels]                  / count, crs, extent, fields
utm:.gdal.transform[p`geom;"EPSG:4326";"EPSG:32614"]
update area:.geos.area utm from p                 / square metres
.gdal.write[`:out.gpkg;`parcels;p;"EPSG:4326"]    / returns rows written
.gdal.rsample[`:dem.tif;1;p`geom]                 / elevation at each point
```

| Function | Args | Returns |
|---|---|---|
| `layers[path]` | file | layer names |
| `info[path;layer]` | file, layer | dict: `name count geomtype crs extent fields` |
| `read[path;layer]` | file, layer | table, geometry in `geom` |
| `write[path;layer;t;crs]` | file, layer name (` = from file name), table, crs ("" = none) | rows written |
| `transform[g;from;to]` | geom(s), crs, crs | geom(s) |
| `rinfo[path]` | raster file | dict: `width height bands transform crs nodata dtype` |
| `rread[path;band]` | raster file, 1-based band | floats, row-major, nodata → `0n` |
| `rsample[path;band;pts]` | raster file, band, point geom(s) | float per point, outside/nodata → `0n` |
| `version[]` | | GDAL version string |

Paths can be `` `:file ``, `` `file `` or `"file"`. Layers can be `` ` `` (first), a name, or an index.
CRS can be `"EPSG:4326"`, a PROJ string or WKT. Coordinates are always x = longitude/easting,
y = latitude/northing, whatever axis order the CRS officially declares.

Type mapping when reading:

| OGR field | q |
|---|---|
| Integer / Integer(Boolean) / Integer64 / Real | int / boolean / long / float |
| String | string (use `` `$ `` for categories) |
| Date / DateTime / Time | date / timestamp (UTC when the source has an offset) / time |
| Binary, lists | byte vector, typed vectors |
| null | q null (`0b` for booleans) |

When writing, the first column of byte vectors is the geometry. Symbols and strings become
String fields; boolean, int, long, float, date, timestamp and time map back as above.
The driver comes from the extension: `.gpkg .geojson .json .geojsonl .shp .fgb .csv .parquet .gml .kml`.
`write` never overwrites: an existing file is an error, and a failed write removes the file it started.

Timings (1M point rows, one core, GDAL 3.8):

| Case | Time |
|---|---|
| `write` GeoPackage / FlatGeobuf | 4.1 s / 2.7 s |
| `read` GeoPackage / FlatGeobuf | 0.64 s / 0.47 s |
| `transform` 4326 → 3857 | 0.42 s |
| `rsample` 1M points | 0.32 s |
| `rread` 2000×2000 band | 0.02 s |

## Notes for extension authors

- `krr()` returns NULL (in kdb+ too), so never store it and test it later; both libraries
  keep the message and raise it once at the end.
- Exported C names are prefixed (`qgeos_*`, `qgdal_*`). A plain `read` or `write` would capture
  GDAL's own calls to libc `read`/`write` inside the loaded library.
- `k.h` defines short macros (`R`, `U`, `nf`, ...); include GEOS/GDAL headers before it.

## Limits

- Linux tested. macOS build flags are in the Makefile but untested; peachq's own macOS symbol export is also unverified.
- The static Linux peachq download cannot load extensions; use the `-glibc` one.
- Each thread that calls in gets its own GEOS context, which is never freed.
- `nearest` costs ~10 µs per row inside GEOS's tree search; fine for 100k rows, slow for 10M.
- Geometries don't carry their CRS; track it yourself (`info` tells you the file's).
- `rread` loads a whole band; there's no windowed read yet, so very large rasters need memory to match.
- `write` creates new files only: no append or update.
