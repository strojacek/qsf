/ st.q — OGC Simple Features (SFA 1.2.1 / ISO 19125) for q: the SQL-option functions without
/ their ST_ prefix, the GEOMETRY_COLUMNS and SPATIAL_REF_SYS catalogues, and AddGeometryColumn.
/ Needs qsf.so (GEOS + GDAL). Set .st.lib before loading to use another path (default ./qsf).
/ -
/ Geometries are extended-WKB byte vectors that carry their SRID; a column is a list of them.
/ An empty byte vector is SQL NULL. Every function takes one geometry or a column, and binary
/ functions broadcast an atom against a column. Booleans come back as q booleans.

.st.lib:@[value;`.st.lib;`:./qsf];
.st.R.fromtext:  .st.lib 2:(`qsf_fromtext;3);
.st.R.fromwkb:   .st.lib 2:(`qsf_fromwkb;3);
.st.R.astext:    .st.lib 2:(`qsf_astext;2);
.st.R.asbinary:  .st.lib 2:(`qsf_asbinary;2);
.st.R.setsrid:   .st.lib 2:(`qsf_setsrid;2);
.st.R.prop:      .st.lib 2:(`qsf_prop;2);
.st.R.geomtype:  .st.lib 2:(`qsf_geomtype;1);
.st.R.coord:     .st.lib 2:(`qsf_coord;2);
.st.R.part:      .st.lib 2:(`qsf_part;3);
.st.R.unary:     .st.lib 2:(`qsf_unary;2);
.st.R.bpolys:    .st.lib 2:(`qsf_bpolys;2);
.st.R.measure:   .st.lib 2:(`qsf_measure;2);
.st.R.pred:      .st.lib 2:(`qsf_pred;3);
.st.R.relate:    .st.lib 2:(`qsf_relate;3);
.st.R.distance:  .st.lib 2:(`qsf_distance;2);
.st.R.overlay:   .st.lib 2:(`qsf_overlay;3);
.st.R.unionagg:  .st.lib 2:(`qsf_unionagg;1);
.st.R.buffer:    .st.lib 2:(`qsf_buffer;3);
.st.R.locate:    .st.lib 2:(`qsf_locate;4);
.st.R.point:     .st.lib 2:(`qsf_point;5);
.st.R.bdpoly:    .st.lib 2:(`qsf_bdpoly;2);
.st.R.srs:       .st.lib 2:(`qsf_srs;1);
.st.R.transform: .st.lib 2:(`qsf_transform;4);
.st.version:     .st.lib 2:(`qsf_version;1);

.st.NULL:`byte$();
.st.R.b:{`boolean$0f^x};                                / float tests -> boolean (null -> 0b)
.st.R.fb:{$[0>type x;$[null x;0N;`long$x];?[null x;0N;`long$x]]}; / float tests -> long with nulls kept
.st.R.typecode:`Point`LineString`Polygon`MultiPoint`MultiLineString`MultiPolygon`GeometryCollection`PolyhedralSurface`TIN`Triangle!1 2 3 4 5 6 7 15 16 17;

/ ---------- constructors (SFA SQL option 7.2.6-7.2.8) ----------
/ srid: an integer, or 0N to keep an EWKT "SRID=n;" prefix (else 0)
.st.GeomFromText:{[wkt;srid] .st.R.fromtext[wkt;srid;0]};
.st.GeometryFromText:.st.GeomFromText;
.st.PointFromText:{[wkt;srid] .st.R.fromtext[wkt;srid;1]};
.st.LineFromText:{[wkt;srid] .st.R.fromtext[wkt;srid;2]};
.st.LineStringFromText:.st.LineFromText;
.st.PolyFromText:{[wkt;srid] .st.R.fromtext[wkt;srid;3]};
.st.PolygonFromText:.st.PolyFromText;
.st.MPointFromText:{[wkt;srid] .st.R.fromtext[wkt;srid;4]};
.st.MultiPointFromText:.st.MPointFromText;
.st.MLineFromText:{[wkt;srid] .st.R.fromtext[wkt;srid;5]};
.st.MultiLineStringFromText:.st.MLineFromText;
.st.MPolyFromText:{[wkt;srid] .st.R.fromtext[wkt;srid;6]};
.st.MultiPolygonFromText:.st.MPolyFromText;
.st.GeomCollFromText:{[wkt;srid] .st.R.fromtext[wkt;srid;7]};
.st.PolyhedralSurfaceFromText:{[wkt;srid] .st.R.fromtext[wkt;srid;15]};
.st.TINFromText:{[wkt;srid] .st.R.fromtext[wkt;srid;16]};
.st.TriangleFromText:{[wkt;srid] .st.R.fromtext[wkt;srid;17]};
.st.BdPolyFromText:{[wkt;srid] .st.R.bdpoly[.st.MLineFromText[wkt;srid];0b]};
.st.BdMPolyFromText:{[wkt;srid] .st.R.bdpoly[.st.MLineFromText[wkt;srid];1b]};
.st.GeomFromEWKT:{[wkt] .st.R.fromtext[wkt;0N;0]};

.st.GeomFromWKB:{[wkb;srid] .st.R.fromwkb[wkb;srid;0]};
.st.GeometryFromWKB:.st.GeomFromWKB;
.st.PointFromWKB:{[wkb;srid] .st.R.fromwkb[wkb;srid;1]};
.st.LineFromWKB:{[wkb;srid] .st.R.fromwkb[wkb;srid;2]};
.st.LineStringFromWKB:.st.LineFromWKB;
.st.PolyFromWKB:{[wkb;srid] .st.R.fromwkb[wkb;srid;3]};
.st.PolygonFromWKB:.st.PolyFromWKB;
.st.MPointFromWKB:{[wkb;srid] .st.R.fromwkb[wkb;srid;4]};
.st.MultiPointFromWKB:.st.MPointFromWKB;
.st.MLineFromWKB:{[wkb;srid] .st.R.fromwkb[wkb;srid;5]};
.st.MultiLineStringFromWKB:.st.MLineFromWKB;
.st.MPolyFromWKB:{[wkb;srid] .st.R.fromwkb[wkb;srid;6]};
.st.MultiPolygonFromWKB:.st.MPolyFromWKB;
.st.GeomCollFromWKB:{[wkb;srid] .st.R.fromwkb[wkb;srid;7]};
.st.BdPolyFromWKB:{[wkb;srid] .st.R.bdpoly[.st.MLineFromWKB[wkb;srid];0b]};
.st.BdMPolyFromWKB:{[wkb;srid] .st.R.bdpoly[.st.MLineFromWKB[wkb;srid];1b]};
.st.GeomFromEWKB:{[wkb] .st.R.fromwkb[wkb;0N;0]};

.st.Point:{[x;y] .st.R.point[x;y;::;::;0]};
.st.PointZ:{[x;y;z] .st.R.point[x;y;z;::;0]};
.st.PointM:{[x;y;m] .st.R.point[x;y;::;m;0]};
.st.PointZM:{[x;y;z;m] .st.R.point[x;y;z;m;0]};
.st.SetSRID:{[g;srid] .st.R.setsrid[g;srid]};

/ ---------- output ----------
.st.AsText:{.st.R.astext[x;0b]};
.st.AsEWKT:{.st.R.astext[x;1b]};
.st.AsBinary:{.st.R.asbinary[x;0]};           / ISO WKB, little-endian (NDR)
.st.AsBinaryXDR:{.st.R.asbinary[x;2]};        / ISO WKB, big-endian (XDR)
.st.AsEWKB:{.st.R.asbinary[x;1]};             / extended WKB with SRID (the storage form)

/ ---------- Geometry (SFA 6.1.2) ----------
.st.SRID:{.st.R.prop[x;0]};
.st.Dimension:{.st.R.prop[x;1]};
.st.CoordinateDimension:{.st.R.prop[x;2]};
.st.CoordDim:.st.CoordinateDimension;
.st.SpatialDimension:{.st.R.prop[x;3]};
.st.Is3D:{`boolean$.st.R.prop[x;4]};
.st.IsMeasured:{`boolean$.st.R.prop[x;5]};
.st.IsEmpty:{`boolean$.st.R.prop[x;6]};
.st.GeometryType:{.st.R.geomtype x};
.st.Envelope:{.st.R.unary[x;1]};
.st.Boundary:{.st.R.unary[x;0]};
.st.IsSimple:{.st.R.b .st.R.measure[x;2]};
.st.IsValid:{.st.R.b .st.R.measure[x;3]};
.st.MakeValid:{.st.R.unary[x;5]};
.st.Normalize:{.st.R.unary[x;7]};
.st.NPoints:{.st.R.prop[x;11]};

/ spatial relations (SFA 6.1.2.3)
.st.Equals:{[a;b] .st.R.pred[a;b;0]};
.st.Disjoint:{[a;b] .st.R.pred[a;b;1]};
.st.Intersects:{[a;b] .st.R.pred[a;b;2]};
.st.Touches:{[a;b] .st.R.pred[a;b;3]};
.st.Crosses:{[a;b] .st.R.pred[a;b;4]};
.st.Within:{[a;b] .st.R.pred[a;b;5]};
.st.Contains:{[a;b] .st.R.pred[a;b;6]};
.st.Overlaps:{[a;b] .st.R.pred[a;b;7]};
.st.Covers:{[a;b] .st.R.pred[a;b;8]};
.st.CoveredBy:{[a;b] .st.R.pred[a;b;9]};
.st.Relate:{[a;b;pattern] .st.R.relate[a;b;pattern]};
.st.RelateMatrix:{[a;b] .st.R.relate[a;b;""]};

/ measure-based linear referencing (SFA 6.1.2.6)
.st.LocateAlong:{[g;m] .st.R.locate[g;m;m;1b]};
.st.LocateBetween:{[g;mstart;mend] .st.R.locate[g;mstart;mend;0b]};

/ spatial analysis (SFA 6.1.2.4)
.st.Distance:{[a;b] .st.R.distance[a;b]};
.st.Buffer:{[g;d] .st.R.buffer[g;d;8]};
.st.BufferQ:{[g;d;quadsegs] .st.R.buffer[g;d;quadsegs]};
.st.ConvexHull:{.st.R.unary[x;2]};
.st.Intersection:{[a;b] .st.R.overlay[a;b;0]};
.st.Union:{[a;b] .st.R.overlay[a;b;1]};
.st.Difference:{[a;b] .st.R.overlay[a;b;2]};
.st.SymDifference:{[a;b] .st.R.overlay[a;b;3]};
.st.SymmetricDifference:.st.SymDifference;
.st.UnionAgg:{.st.R.unionagg x};              / aggregate union of a column

/ ---------- Point, Curve, LineString (SFA 6.1.4-6.1.7) ----------
.st.X:{.st.R.coord[x;0]};
.st.Y:{.st.R.coord[x;1]};
.st.Z:{.st.R.coord[x;2]};
.st.M:{.st.R.coord[x;3]};
.st.Length:{.st.R.measure[x;1]};
.st.StartPoint:{.st.R.part[x;0;0N]};
.st.EndPoint:{.st.R.part[x;1;0N]};
.st.IsClosed:{.st.R.b .st.R.measure[x;4]};
.st.IsRing:{.st.R.b .st.R.measure[x;5]};
.st.NumPoints:{.st.R.prop[x;7]};
.st.PointN:{[g;n] .st.R.part[g;2;n]};

/ ---------- Surface, Polygon, PolyhedralSurface (SFA 6.1.10-6.1.12) ----------
.st.Area:{.st.R.measure[x;0]};
.st.Perimeter:{.st.R.measure[x;6]};
.st.Centroid:{.st.R.unary[x;3]};
.st.PointOnSurface:{.st.R.unary[x;4]};
.st.ExteriorRing:{.st.R.part[x;3;0N]};
.st.NumInteriorRing:{.st.R.prop[x;8]};
.st.NumInteriorRings:.st.NumInteriorRing;
.st.InteriorRingN:{[g;n] .st.R.part[g;4;n]};
.st.NumPatches:{.st.R.prop[x;10]};
.st.PatchN:{[g;n] .st.R.part[g;6;n]};
.st.BoundingPolygons:{[g;p] .st.R.bpolys[g;p]};

/ ---------- GeometryCollection (SFA 6.1.3) ----------
.st.NumGeometries:{.st.R.prop[x;9]};
.st.GeometryN:{[g;n] .st.R.part[g;5;n]};

/ ---------- SPATIAL_REF_SYS (SFA SQL option 7.1.3) ----------
.st.spatial_ref_sys:([srid:`long$()] auth_name:`symbol$(); auth_srid:`long$(); srtext:());
/ AddSRS[srid;auth_name;auth_srid;srtext]
.st.AddSRS:{[srid;an;as;txt] an:$[-11h=type an;an;`$an];
  `.st.spatial_ref_sys upsert ([srid:enlist `long$srid] auth_name:enlist an; auth_srid:enlist `long$as; srtext:enlist txt); srid};
/ EPSG[code]: add an EPSG system from PROJ's database under srid = code
.st.EPSG:{[code] r:.st.R.srs "EPSG:",string code; .st.AddSRS[code;`EPSG;code;r 0]};
.st.R.srtext:{[srid] if[0=srid; '"transform: geometry has SRID 0 (unknown)"];
  if[not srid in key[.st.spatial_ref_sys]`srid; .st.EPSG srid];
  .st.spatial_ref_sys[srid;`srtext]};
/ Transform[g;srid]: reproject from the geometry's SRID (which must be in spatial_ref_sys, or an EPSG code)
.st.Transform:{[g;srid] s:distinct .st.SRID $[0h=type g;g;enlist g]; s:s where not null s;
  if[1<count s; '"transform: mixed SRIDs in the input"];
  if[0=count s; :g];
  .st.R.transform[g;.st.R.srtext first s;.st.R.srtext srid;srid]};

/ ---------- GEOMETRY_COLUMNS (SFA SQL option 7.1.2, "geometry types" implementation) ----------
.st.geometry_columns:([] f_table_catalog:`symbol$(); f_table_schema:`symbol$(); f_table_name:`symbol$();
  f_geometry_column:`symbol$(); coord_dimension:`long$(); srid:`long$(); geometry_type:`long$(); geom_type:`symbol$());
/ SFA geometry type codes: 0 Geometry, 1-7, 15-17, plus 1000 (Z), 2000 (M), 3000 (ZM)
.st.R.gcode:{[typ;dim] base:$[typ=`Geometry;0;.st.R.typecode typ];
  base+$[dim=3;1000;dim=4;3000;0]};
/ AddGeometryColumn[table;column;srid;type;dimension]: adds a column of NULL geometries to a
/ global table (if absent) and registers it. type is a symbol such as `Polygon or `Geometry.
.st.AddGeometryColumn:{[t;c;srid;typ;dim]
  if[not typ in `Geometry,key .st.R.typecode; '"AddGeometryColumn: unknown geometry type ",string typ];
  if[not dim in 2 3 4; '"AddGeometryColumn: dimension must be 2, 3 or 4"];
  if[not c in cols t; t set ![value t;();0b;(enlist c)!enlist (#;(count;`i);enlist enlist .st.NULL)]];
  `.st.geometry_columns upsert (`;`;t;c;`long$dim;`long$srid;.st.R.gcode[typ;dim];typ);
  t};
.st.DropGeometryColumn:{[t;c] t set ![value t;();0b;enlist c];
  delete from `.st.geometry_columns where f_table_name=t, f_geometry_column=c; t};
/ CheckGeometryColumn[table;column]: rows whose geometry breaks the registered SRID, type or
/ dimension (the constraints a SQL implementation enforces on insert)
.st.CheckGeometryColumn:{[t;c] r:first select from .st.geometry_columns where f_table_name=t, f_geometry_column=c;
  if[null r`f_table_name; '"CheckGeometryColumn: not registered"];
  g:(value t) c; nn:not .st.NULL~/:g;
  bad:nn and (r[`srid]<>.st.SRID g) or (r[`coord_dimension]<>.st.CoordinateDimension g) or
    $[r[`geom_type]=`Geometry;0b;r[`geom_type]<>.st.GeometryType g];
  where bad};
/ RecoverGeometryColumn[table;column]: register an existing column from its contents
.st.RecoverGeometryColumn:{[t;c] g:(value t) c; g:g where not .st.NULL~/:g;
  if[0=count g; '"RecoverGeometryColumn: no geometries"];
  s:distinct .st.SRID g; d:distinct .st.CoordinateDimension g; ty:distinct .st.GeometryType g;
  if[1<count s; '"RecoverGeometryColumn: mixed SRIDs"]; if[1<count d; '"RecoverGeometryColumn: mixed dimensions"];
  .st.AddGeometryColumn[t;c;first s;$[1=count ty;first ty;`Geometry];first d]};
