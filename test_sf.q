/ test_sf.q — OGC Simple Features: the OGC SFS 1.2.1 conformance suite (items T1-T52, from the
/ OGC CITE test scripts as kept in PostGIS's extras/ogc_test_suite), then the parts of SFA 1.2.1
/ that suite does not reach: Z and M, ISO WKB, LocateAlong/LocateBetween, polyhedral surfaces and
/ TINs, BdPoly constructors, transformation, the catalogues, NULL handling.
/ Run with:  q test_sf.q
\l st.q

pass:0; fail:0;
chk:{[name;got;want] $[got~want; pass+:1; [fail+:1; -1 "FAIL ",name,": got ",(-3!got)," want ",-3!want]]};
near:{[name;got;want;tol] $[all tol>=abs got-want; pass+:1; [fail+:1; -1 "FAIL ",name,": got ",(-3!got)," want ",-3!want]]};
/ WKT compared token by token, so "POLYGON( ( 67 13, ...) )" matches "POLYGON ((67 13, ...))"
tok:{" " vs " " sv {x where 0<count each x} " " vs ssr[ssr[ssr[upper x;"(";" ( "];")";" ) "];",";" , "]};
chkwkt:{[name;got;want] chk[name;tok got;tok want]};
/ overlay results: same point set (Equals), same type
chkgeom:{[name;got;want] w:.st.GeomFromText[want;.st.SRID got];
  chk[name;(.st.GeometryType got;.st.Equals[got;w]);(.st.GeometryType w;1b)]};

/ ================= the OGC conformance data (sqltsch.sql) =================
.st.AddSRS[101;`POSC;32214;"PROJCS[\"UTM_ZONE_14N\", GEOGCS[\"World Geodetic System 72\", DATUM[\"WGS_72\",  SPHEROID[\"NWL_10D\", 6378135, 298.26]], PRIMEM[\"Greenwich\", 0], UNIT[\"Meter\", 1.0]], PROJECTION[\"Transverse_Mercator\"], PARAMETER[\"False_Easting\", 500000.0], PARAMETER[\"False_Northing\", 0.0], PARAMETER[\"Central_Meridian\", -99.0], PARAMETER[\"Scale_Factor\", 0.9996], PARAMETER[\"Latitude_of_origin\", 0.0], UNIT[\"Meter\", 1.0]]"];
gt:{.st.GeomFromText[x;101]};
lakes:([] fid:enlist 101; name:enlist `$"Blue Lake"; shore:enlist gt "POLYGON((52 18,66 23,73 9,48 6,52 18),(59 18,67 18,67 13,59 13,59 18))");
road_segments:([] fid:102 103 104 105 106; name:`$("Route 5";"Route 5";"Route 5";"Main Street";"Dirt Road by Green Forest");
  aliases:(`;`$"Main Street";`;`;`); num_lanes:2 4 2 4 1;
  centerline:gt each ("LINESTRING( 0 18, 10 21, 16 23, 28 26, 44 31 )";"LINESTRING( 44 31, 56 34, 70 38 )";
    "LINESTRING( 70 38, 72 48 )";"LINESTRING( 70 38, 84 42 )";"LINESTRING( 28 26, 28 0 )"));
divided_routes:([] fid:enlist 119; name:enlist `$"Route 75"; num_lanes:enlist 4;
  centerlines:enlist gt "MULTILINESTRING((10 48,10 21,10 0),(16 0,16 23,16 48))");
forests:([] fid:enlist 109; name:enlist `$"Green Forest";
  boundary:enlist gt "MULTIPOLYGON(((28 26,28 0,84 0,84 42,28 26),(52 18,66 23,73 9,48 6,52 18)),((59 18,67 18,67 13,59 13,59 18)))");
bridges:([] fid:enlist 110; name:enlist `$"Cam Bridge"; position:enlist gt "POINT( 44 31 )");
streams:([] fid:111 112; name:(`$"Cam Stream";`);
  centerline:gt each ("LINESTRING( 38 48, 44 41, 41 36, 44 31, 52 18 )";"LINESTRING( 76 0, 78 4, 73 9 )"));
buildings:([] fid:113 114; address:`$("123 Main Street";"215 Main Street"); position:gt each ("POINT( 52 30 )";"POINT( 64 33 )");
  footprint:gt each ("POLYGON( ( 50 31, 54 31, 54 29, 50 29, 50 31) )";"POLYGON( ( 66 34, 62 34, 62 32, 66 32, 66 34) )"));
ponds:([] fid:enlist 120; name:enlist `; ptype:enlist `$"Stock Pond";  / the column "type" is a q keyword
  shores:enlist gt "MULTIPOLYGON( ( ( 24 44, 22 42, 24 40, 24 44) ), ( ( 26 44, 26 40, 28 42, 26 44) ) )");
named_places:([] fid:117 118; name:`$("Ashton";"Goose Island");
  boundary:gt each ("POLYGON( ( 62 48, 84 48, 84 30, 56 30, 56 34, 62 48) )";"POLYGON( ( 67 13, 67 18, 59 18, 59 13, 67 13) )"));
map_neatlines:([] fid:enlist 115; neatline:enlist gt "POLYGON( ( 0 0, 0 48, 84 48, 84 0, 0 0 ) )");
/ the CREATE TABLE ... GEOMETRY(type, 101) columns, registered as a SQL implementation would
{.st.RecoverGeometryColumn . x} each ((`lakes;`shore);(`road_segments;`centerline);(`divided_routes;`centerlines);
  (`forests;`boundary);(`bridges;`position);(`streams;`centerline);(`buildings;`position);(`buildings;`footprint);
  (`ponds;`shores);(`named_places;`boundary);(`map_neatlines;`neatline));

/ helpers that mirror the queries' WHERE clauses
place:{first exec boundary from named_places where name=`$x};
lake:{first exec shore from lakes where name=`$x};
road:{first exec centerline from road_segments where fid=x};
route75:first divided_routes`centerlines;
forest:first forests`boundary;
cambridge:first bridges`position;
camstream:first exec centerline from streams where name=`$"Cam Stream";
pond:first ponds`shores;

/ ================= T1-T5: GEOMETRY_COLUMNS and SPATIAL_REF_SYS =================
chk["T1 geometry_columns lists every feature table";asc distinct .st.geometry_columns`f_table_name;
  asc `lakes`road_segments`divided_routes`buildings`forests`bridges`named_places`streams`ponds`map_neatlines];
chk["T2 geometry column of streams";exec f_geometry_column from .st.geometry_columns where f_table_name=`streams;enlist`centerline];
chk["T3 coordinate dimension of streams";exec coord_dimension from .st.geometry_columns where f_table_name=`streams;enlist 2];
chk["T4 srid of streams";exec srid from .st.geometry_columns where f_table_name=`streams;enlist 101];
p5:"PROJCS[\"UTM_ZONE_14N\", GEOGCS[\"World Geodetic System 72\"";
chk["T5 srtext of SRID 101";(count p5)#.st.spatial_ref_sys[101;`srtext];p5];

/ ================= T6-T36: Geometry, Point, Curve, LineString, Surface, Polygon, Collection =================
chk["T6 Dimension(Blue Lake)";.st.Dimension lake "Blue Lake";2];
chk["T7 GeometryType(Route 75)";.st.GeometryType route75;`MultiLineString];
chkwkt["T8 AsText(Goose Island)";.st.AsText place "Goose Island";"POLYGON( ( 67 13, 67 18, 59 18, 59 13, 67 13) )"];
chkwkt["T9 AsText(PolygonFromWKB(AsBinary(Goose Island)))";.st.AsText .st.PolygonFromWKB[.st.AsBinary place "Goose Island";101];"POLYGON( ( 67 13, 67 18, 59 18, 59 13, 67 13) )"];
chk["T10 SRID(Goose Island)";.st.SRID place "Goose Island";101];
chk["T11 IsEmpty(Route 5, Main Street)";.st.IsEmpty first exec centerline from road_segments where name=`$"Route 5", aliases=`$"Main Street";0b];
chk["T12 IsSimple(Blue Lake)";.st.IsSimple lake "Blue Lake";1b];
chkwkt["T13 Boundary(Goose Island)";.st.AsText .st.Boundary place "Goose Island";"LINESTRING( 67 13, 67 18, 59 18, 59 13, 67 13 )"];
chkgeom["T14 Envelope(Goose Island)";.st.Envelope place "Goose Island";"POLYGON( ( 59 13, 59 18, 67 18, 67 13, 59 13) )"];
chk["T15 X(Cam Bridge)";.st.X cambridge;44f];
chk["T16 Y(Cam Bridge)";.st.Y cambridge;31f];
chkwkt["T17 StartPoint(road 102)";.st.AsText .st.StartPoint road 102;"POINT( 0 18 )"];
chkwkt["T18 EndPoint(road 102)";.st.AsText .st.EndPoint road 102;"POINT( 44 31 )"];
chk["T19 IsClosed(Boundary(Goose Island))";.st.IsClosed .st.Boundary place "Goose Island";1b];
chk["T20 IsRing(Boundary(Goose Island))";.st.IsRing .st.Boundary place "Goose Island";1b];
chk["T21 Length(road 106)";.st.Length road 106;26f];
chk["T22 NumPoints(road 102)";.st.NumPoints road 102;5];
chkwkt["T23 PointN(road 102, 1)";.st.AsText .st.PointN[road 102;1];"POINT( 0 18 )"];
chkwkt["T24 Centroid(Goose Island)";.st.AsText .st.Centroid place "Goose Island";"POINT( 63 15.5 )"];
chk["T25 Contains(Goose Island, PointOnSurface(Goose Island))";.st.Contains[place "Goose Island";.st.PointOnSurface place "Goose Island"];1b];
chk["T26 Area(Goose Island)";.st.Area place "Goose Island";40f];
chkwkt["T27 ExteriorRing(Blue Lake)";.st.AsText .st.ExteriorRing lake "Blue Lake";"LINESTRING(52 18, 66 23, 73  9, 48  6, 52 18)"];
chk["T28 NumInteriorRings(Blue Lake)";.st.NumInteriorRings lake "Blue Lake";1];
chkwkt["T29 InteriorRingN(Blue Lake, 1)";.st.AsText .st.InteriorRingN[lake "Blue Lake";1];"LINESTRING(59 18, 67 18, 67 13, 59 13, 59 18)"];
chk["T30 NumGeometries(Route 75)";.st.NumGeometries route75;2];
chkwkt["T31 GeometryN(Route 75, 2)";.st.AsText .st.GeometryN[route75;2];"LINESTRING( 16 0, 16 23, 16 48 )"];
chk["T32 IsClosed(Route 75)";.st.IsClosed route75;0b];
chk["T33 Length(Route 75)";.st.Length route75;96f];
chkwkt["T34 Centroid(Stock Pond)";.st.AsText .st.Centroid pond;"POINT( 25 42 )"];
chk["T35 Contains(Stock Pond, PointOnSurface(Stock Pond))";.st.Contains[pond;.st.PointOnSurface pond];1b];
chk["T36 Area(Stock Pond)";.st.Area pond;8f];

/ ================= T37-T52: relations and analysis =================
chk["T37 Equals(Goose Island, PolygonFromText(...))";.st.Equals[place "Goose Island";.st.PolygonFromText["POLYGON( ( 67 13, 67 18, 59 18, 59 13, 67 13) )";101]];1b];
chk["T38 Disjoint(Route 75, Ashton)";.st.Disjoint[route75;place "Ashton"];1b];
chk["T39 Touches(Cam Stream, Blue Lake)";.st.Touches[camstream;lake "Blue Lake"];1b];
chk["T40 Within(215 Main Street, Ashton)";.st.Within[first exec footprint from buildings where address=`$"215 Main Street";place "Ashton"];1b];
chk["T41 Overlaps(Green Forest, Ashton)";.st.Overlaps[forest;place "Ashton"];1b];
chk["T42 Crosses(road 102, Route 75)";.st.Crosses[road 102;route75];1b];
chk["T43 Intersects(road 102, Route 75)";.st.Intersects[road 102;route75];1b];
chk["T44 Contains(Green Forest, Ashton)";.st.Contains[forest;place "Ashton"];0b];
chk["T45 Relate(Green Forest, Ashton, 'TTTTTTTTT')";.st.Relate[forest;place "Ashton";"TTTTTTTTT"];1b];
chk["T46 Distance(Cam Bridge, Ashton)";.st.Distance[cambridge;place "Ashton"];12f];
chkwkt["T47 Intersection(Cam Stream, Blue Lake)";.st.AsText .st.Intersection[camstream;lake "Blue Lake"];"POINT( 52 18 )"];
chkgeom["T48 Difference(Ashton, Green Forest)";.st.Difference[place "Ashton";forest];"POLYGON( ( 56 34, 62 48, 84 48, 84 42, 56 34) )"];
chkgeom["T49 Union(Blue Lake, Goose Island)";.st.Union[lake "Blue Lake";place "Goose Island"];"POLYGON((52 18,66 23,73 9,48 6,52 18))"];
chkgeom["T50 SymDifference(Blue Lake, Goose Island)";.st.SymDifference[lake "Blue Lake";place "Goose Island"];"POLYGON((52 18,66 23,73 9,48 6,52 18))"];
chk["T51 count buildings within Buffer(bridge, 15)";sum `long$.st.Contains[.st.Buffer[cambridge;15f];buildings`footprint];1];
chkgeom["T52 ConvexHull(Blue Lake)";.st.ConvexHull lake "Blue Lake";"POLYGON((52 18,66 23,73 9,48 6,52 18))"];
-1 "OGC SFS 1.2.1 conformance items: ",string[pass]," passed, ",string[fail]," failed";
conf:pass;

/ ================= Z and M =================
w:{.st.GeomFromText[x;0]};
rt:("POINT Z (1 2 3)";"POINT M (1 2 4)";"POINT ZM (1 2 3 4)";"LINESTRING ZM (0 0 0 0, 1 1 1 1)";
    "POLYGON Z ((0 0 1, 1 0 1, 1 1 1, 0 0 1))";"MULTIPOINT M ((1 2 3), (4 5 6))";
    "GEOMETRYCOLLECTION Z (POINT Z (1 2 3), LINESTRING Z (0 0 0, 1 1 1))";"POINT EMPTY";"LINESTRING EMPTY";"GEOMETRYCOLLECTION EMPTY");
chk["WKT round trip with Z, M, ZM and EMPTY";.st.AsText w each rt;rt];
z:w "POINT ZM (1 2 3 4)";
chk["Is3D / IsMeasured / CoordDim / SpatialDimension";(.st.Is3D z;.st.IsMeasured z;.st.CoordDim z;.st.SpatialDimension z);(1b;1b;4;3)];
chk["X Y Z M";(.st.X z;.st.Y z;.st.Z z;.st.M z);1 2 3 4f];
chk["Z and M of a 2D point are null";(.st.Z w "POINT(1 2)";.st.M w "POINT Z (1 2 3)");0n 0n];
chk["ISO WKB of POINT ZM (type 3001)";.st.AsBinary z;0x01b90b0000000000000000f03f000000000000004000000000000008400000000000001040];
chk["ISO WKB, big-endian";.st.AsBinaryXDR w "POINT(1 2)";0x00000000013ff00000000000004000000000000000];
chk["EWKB carries the SRID";.st.AsEWKB .st.SetSRID[w "POINT(1 2)";4326];0x0101000020e6100000000000000000f03f0000000000000040];
chk["read ISO WKB with Z";.st.AsText .st.GeomFromWKB[0x01e9030000000000000000f03f00000000000000400000000000000840;0];"POINT Z (1 2 3)"];
chk["read OGC 2.5D WKB (Z flag)";.st.AsText .st.GeomFromWKB[0x0101000080000000000000f03f00000000000000400000000000000840;0];"POINT Z (1 2 3)"];
chk["read big-endian WKB";.st.AsText .st.GeomFromWKB[0x00000000013ff00000000000004000000000000000;0];"POINT (1 2)"];
chk["EWKT in, SRID kept";(.st.SRID x;.st.AsEWKT x:.st.GeomFromEWKT "SRID=4326;POINT(1 2)");(4326;"SRID=4326;POINT (1 2)")];
chk["WKB round trip of every geometry type";.st.AsText {.st.GeomFromWKB[.st.AsBinary x;0]} each w each rt;rt];
chk["empty geometry: IsEmpty, Dimension, NumGeometries";(.st.IsEmpty w "POINT EMPTY";.st.Dimension w "LINESTRING EMPTY";.st.NumGeometries w "MULTIPOINT EMPTY");(1b;1;0)];

/ ================= LocateAlong / LocateBetween (SFA 6.1.2.6) =================
lm:w "LINESTRING M (0 0 0, 10 0 10, 10 10 20)";
chk["LocateAlong on a line";.st.AsText .st.LocateAlong[lm;15];"MULTIPOINT M ((10 5 15))"];
chk["LocateAlong at a vertex";.st.AsText .st.LocateAlong[lm;10];"MULTIPOINT M ((10 0 10))"];
chk["LocateAlong outside the range";.st.AsText .st.LocateAlong[lm;30];"MULTIPOINT M EMPTY"];
chk["LocateBetween on a line";.st.AsText .st.LocateBetween[lm;5;12];"MULTILINESTRING M ((5 0 5, 10 0 10, 10 2 12))"];
chk["LocateBetween, decreasing measures";.st.AsText .st.LocateBetween[w "LINESTRING M (0 0 10, 10 0 0)";2;4];"MULTILINESTRING M ((6 0 4, 8 0 2))"];
chk["LocateBetween touching one point";.st.AsText .st.LocateBetween[lm;20;25];"MULTIPOINT M ((10 10 20))"];
chk["LocateBetween on points";.st.AsText .st.LocateBetween[w "MULTIPOINT M ((0 0 1), (1 1 5), (2 2 9))";4;9];"MULTIPOINT M ((1 1 5), (2 2 9))"];
chk["LocateAlong interpolates Z";.st.AsText .st.LocateAlong[w "LINESTRING ZM (0 0 5 0, 10 0 15 10)";4];"MULTIPOINT ZM ((4 0 9 4))"];
chk["LocateBetween on a MultiLineString (two pieces)";.st.AsText .st.LocateBetween[w "MULTILINESTRING M ((0 0 0, 4 0 4), (0 1 6, 4 1 10))";3;7];
  "MULTILINESTRING M ((3 0 3, 4 0 4), (0 1 6, 1 1 7))"];
chk["Locate needs measures";@[.st.LocateAlong[;1];w "LINESTRING(0 0,1 1)";{x}];"locate: geometry has no measures"];
chk["Locate rejects polygons";@[.st.LocateAlong[;1];w "POLYGON M ((0 0 0,1 0 1,1 1 2,0 0 0))";{x}] like "locate: Polygon is not supported*";1b];

/ ================= PolyhedralSurface, TIN, Triangle (SFA 6.1.12) =================
cube:w "POLYHEDRALSURFACE Z (((0 0 0,0 1 0,1 1 0,1 0 0,0 0 0)),((0 0 0,0 0 1,0 1 1,0 1 0,0 0 0)),((0 0 0,1 0 0,1 0 1,0 0 1,0 0 0)),((1 1 1,1 0 1,1 0 0,1 1 0,1 1 1)),((1 1 1,1 1 0,0 1 0,0 1 1,1 1 1)),((1 1 1,0 1 1,0 0 1,1 0 1,1 1 1)))";
box:w "POLYHEDRALSURFACE Z (((0 0 0,0 1 0,1 1 0,1 0 0,0 0 0)),((0 0 0,0 0 1,0 1 1,0 1 0,0 0 0)),((0 0 0,1 0 0,1 0 1,0 0 1,0 0 0)),((1 1 1,1 0 1,1 0 0,1 1 0,1 1 1)),((1 1 1,1 1 0,0 1 0,0 1 1,1 1 1)))";
chk["cube: type, NumPatches, Dimension";(.st.GeometryType cube;.st.NumPatches cube;.st.Dimension cube);(`PolyhedralSurface;6;2)];
chk["cube: IsClosed, empty Boundary, surface Area 6";(.st.IsClosed cube;.st.IsEmpty .st.Boundary cube;.st.Area cube);(1b;1b;6f)];
chk["open box: not closed, boundary is the 4 rim edges";(.st.IsClosed box;.st.NumGeometries .st.Boundary box;.st.Length .st.Boundary box);(0b;4;4f)];
chk["BoundingPolygons of the bottom face: its 4 side faces";.st.NumGeometries .st.BoundingPolygons[cube;.st.PatchN[cube;1]];4];
chk["PatchN";.st.AsText .st.PatchN[cube;1];"POLYGON Z ((0 0 0, 0 1 0, 1 1 0, 1 0 0, 0 0 0))"];
chk["PatchN out of range is NULL";.st.PatchN[cube;7];.st.NULL];
tin:w "TIN Z (((0 0 0,0 1 0,1 0 0,0 0 0)),((0 0 0,1 0 0,0 0 1,0 0 0)))";
chk["TIN: NumPatches, 3D area";(.st.NumPatches tin;.st.Area tin);(2;1f)];
chk["TIN round trip through WKB";.st.AsText .st.GeomFromWKB[.st.AsBinary tin;0];.st.AsText tin];
tri:w "TRIANGLE ((0 0,3 0,0 4,0 0))";
chk["Triangle: area, exterior ring, envelope via GEOS";(.st.Area tri;.st.AsText .st.ExteriorRing tri;.st.AsText .st.Envelope tri);(6f;"LINESTRING (0 0, 3 0, 0 4, 0 0)";"POLYGON ((0 0, 3 0, 3 4, 0 4, 0 0))")];
chk["TIN footprint intersects a point (GEOS sees a MultiPolygon)";.st.Intersects[tin;w "POINT(0.2 0.2)"];1b];

/ ================= BdPoly constructors (SFA SQL 7.2.6) =================
bp:.st.BdPolyFromText["MULTILINESTRING((0 0,10 0,10 10,0 10,0 0),(2 2,4 2,4 4,2 4,2 2))";101];
chk["BdPolyFromText: polygon with a hole";(.st.GeometryType bp;.st.Area bp;.st.NumInteriorRings bp;.st.SRID bp);(`Polygon;96f;1;101)];
bm:.st.BdMPolyFromText["MULTILINESTRING((0 0,1 0,1 1,0 1,0 0),(5 5,6 5,6 6,5 6,5 5))";0];
chk["BdMPolyFromText: two polygons";(.st.GeometryType bm;.st.NumGeometries bm;.st.Area bm);(`MultiPolygon;2;2f)];
chk["BdPolyFromText refuses two separate polygons";@[.st.BdPolyFromText[;0];"MULTILINESTRING((0 0,1 0,1 1,0 1,0 0),(5 5,6 5,6 6,5 6,5 5))";{x}];"bdpoly: the rings do not form a single polygon"];
chk["BdPolyFromText refuses an open ring";@[.st.BdPolyFromText[;0];"MULTILINESTRING((0 0,1 0,1 1,0 1))";{x}];"bdpoly: ring 1 is not closed"];

/ ================= typed constructors, NULLs, SRIDs =================
chk["PointFromText of a line is NULL";.st.PointFromText["LINESTRING(0 0,1 1)";0];.st.NULL];
chk["typed constructors accept their own type";.st.GeometryType each (.st.MPolyFromText["MULTIPOLYGON(((0 0,1 0,1 1,0 0)))";0];.st.GeomCollFromText["GEOMETRYCOLLECTION(POINT(1 1))";0]);`MultiPolygon`GeometryCollection];
chk["NULL in, NULL out";(.st.Area .st.NULL;.st.AsText .st.NULL;.st.Intersects[.st.NULL;w "POINT(1 1)"];.st.Union[.st.NULL;w "POINT(1 1)"]);(0n;"";0b;.st.NULL)];
chk["NumPoints of a polygon is NULL (LineString only)";.st.NumPoints place "Goose Island";0N];
chk["PointN out of range is NULL";.st.PointN[road 102;9];.st.NULL];
chk["mixed SRIDs are an error";@[.st.Intersection[place "Goose Island";];.st.SetSRID[place "Ashton";4326];{x}];"operation on mixed SRIDs (101 and 4326)"];
chk["results keep the SRID";.st.SRID (.st.Buffer[cambridge;1f];.st.Centroid forest;.st.Intersection[forest;place "Ashton"]);101 101 101];
/ the forest and Ashton share the edge x = 84, 30 <= y <= 42, so boundary/boundary is 1-dimensional
chk["RelateMatrix";.st.RelateMatrix[forest;place "Ashton"];"212111212"];
w2:.st.SetSRID[.st.GeomFromText["LINESTRING(62 48,84 48)";0];101];
chk["Covers / CoveredBy";(.st.Covers[place "Ashton";w2];.st.CoveredBy[w2;place "Ashton"];.st.Contains[place "Ashton";w2]);110b];   / on the boundary: covered, not contained

/ ================= columns, prepared predicates, aggregates =================
pts:.st.Point[`float$til 100;`float$til 100];
chk["column of points";(count pts;.st.X pts 42);(100;42f)];
sq:w "POLYGON((10 10,30 10,30 30,10 30,10 10))";
chk["Within column vs polygon (prepared)";where .st.Within[pts;sq];11+til 19];
chk["Contains polygon vs column (prepared, flipped)";where .st.Contains[sq;pts];11+til 19];
chk["Distance column vs atom";.st.Distance[pts 0 5;w "POINT(0 3)"];(3f;sqrt 29f)];
chk["UnionAgg of overlapping squares";.st.Area .st.UnionAgg .st.Buffer[.st.Point[0 1f;0 0f];1f];.st.Area .st.Union . .st.Buffer[.st.Point[0 1f;0 0f];1f]];
chk["Perimeter vs Length";(.st.Perimeter place "Goose Island";.st.Length place "Goose Island");26 0f];

/ ================= transformation and the catalogues =================
p4326:.st.GeomFromText["POINT(-99 30)";4326];
u:.st.Transform[p4326;32614];
chk["Transform 4326 -> UTM 14N (EPSG from PROJ)";(.st.SRID u;.st.X u);(32614;500000f)];
near["Transform northing matches gdaltransform";.st.Y u;3318785.35258121;1e-6];
near["Transform back";(.st.X b;.st.Y b:.st.Transform[u;4326]);-99 30f;1e-9];
chk["EPSG systems are added to spatial_ref_sys";`EPSG`EPSG~exec auth_name from .st.spatial_ref_sys where srid in 4326 32614;1b];
near["Transform from the conformance SRID 101 (custom WKT), as gdaltransform";.st.X .st.Transform[cambridge;4326];83.2296310004508;1e-9];
chk["Transform keeps Z and M";.st.CoordDim .st.Transform[.st.GeomFromText["POINT ZM (-99 30 5 7)";4326];32614];4];
t:([] id:1 2 3);
.st.AddGeometryColumn[`t;`geom;4326;`Point;2];
chk["AddGeometryColumn adds a NULL column and registers it";(count t`geom;all .st.NULL~/:t`geom;exec geometry_type from .st.geometry_columns where f_table_name=`t);(3;1b;enlist 1)];
update geom:(.st.SetSRID[.st.Point[1 2 3f;4 5 6f];4326 4326 3857]) from `t;
chk["CheckGeometryColumn finds the row with the wrong SRID";.st.CheckGeometryColumn[`t;`geom];enlist 2];
.st.DropGeometryColumn[`t;`geom];
chk["DropGeometryColumn";(cols t;count select from .st.geometry_columns where f_table_name=`t);(enlist`id;0)];
chk["geometry_type codes (Z, M)";.st.R.gcode ./: ((`Point;3);(`Polygon;4);(`Geometry;2));1001 3003 0];

/ ================= interop with geos.q =================
\l geos.q
chk["geos.q reads the same geometries";.geos.area place "Goose Island";40f];

-1 "Simple Features: ",string[pass]," passed, ",string[fail]," failed (",string[conf]," of them the OGC conformance items)";
exit $[fail;1;0]
