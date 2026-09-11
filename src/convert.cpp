// The conversion: read STEP, tessellate, emit the sidecars off the SAME face
// traversal that orders the glb primitives, write the glb.
//
// A port of the python reference implementation, in four stages: the skeleton
// plus the three importer pins, the uv/oracle sidecars, the per-body split
// (parts.json), and the torn re-mesh.
#include "convert.h"
#include "json.h"
#include "oracle.h"
#include "parts.h"
#include "refine.h"

#include <cctype>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include <Bnd_Box.hxx>
#include <BRepBndLib.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRep_Tool.hxx>
#include <IFSelect_ReturnStatus.hxx>
#include <Message_ProgressRange.hxx>
#include <RWGltf_CafWriter.hxx>
#include <STEPCAFControl_Reader.hxx>
#include <TColStd_IndexedDataMapOfStringString.hxx>
#include <TCollection_AsciiString.hxx>
#include <TCollection_ExtendedString.hxx>
#include <TDF_LabelSequence.hxx>
#include <TDocStd_Document.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>
#include <TopLoc_Location.hxx>
#include <XCAFDoc_DocumentTool.hxx>
#include <XCAFDoc_ShapeTool.hxx>
#include <XSControl_TransferReader.hxx>
#include <XSControl_WorkSession.hxx>
#include <Interface_InterfaceModel.hxx>
#include <StepData_StepModel.hxx>

// math.radians without <cmath>'s M_PI (not standard on MSVC without a define)
static const double DEG2RAD = 0.017453292519943295;

// ── AUTHOR-UNIT PARSER (unit-abstract, PROTOCOL 2) ───────────────────────────
// mm per author length unit, parsed from the SAME DATA-section unit entities
// OCC reads to normalise (SI_UNIT / CONVERSION_BASED_UNIT in the length
// contexts) — dividing OCC's mm by this recovers the numbers the author typed.
// StepData_StepModel::LocalLengthUnit() is NOT populated by the reader (probed
// 2026-08-20 via OCP: 1.0 on both a mm and a metre file), hence text.

// mm-per-unit of an SI length unit inside a record body, or -1.
static double si_length_mm(const std::string &s)
{
  size_t p = s.find("SI_UNIT");
  while (p != std::string::npos)
  {
    const size_t q = s.find('(', p);
    const size_t r = q == std::string::npos ? q : s.find(')', q);
    if (r == std::string::npos)
      return -1.0;
    const std::string args = s.substr(q + 1, r - q - 1);
    if (args.find(".METRE.") != std::string::npos)
    {
      if (args.find(".MILLI.") != std::string::npos) return 1.0;
      if (args.find(".CENTI.") != std::string::npos) return 10.0;
      if (args.find(".DECI.") != std::string::npos) return 100.0;
      if (args.find(".MICRO.") != std::string::npos) return 1e-3;
      if (args.find(".NANO.") != std::string::npos) return 1e-6;
      if (args.find(".KILO.") != std::string::npos) return 1e6;
      return 1000.0;                       // plain METRE
    }
    p = s.find("SI_UNIT", r);
  }
  return -1.0;
}

// Every "#N" reference inside a record body.
static std::vector<int> step_refs(const std::string &s)
{
  std::vector<int> out;
  for (size_t i = 0; i + 1 < s.size(); ++i)
    if (s[i] == '#' && std::isdigit(static_cast<unsigned char>(s[i + 1])))
    {
      int v = 0;
      size_t j = i + 1;
      while (j < s.size() && std::isdigit(static_cast<unsigned char>(s[j])))
        v = v * 10 + (s[j++] - '0');
      out.push_back(v);
      i = j - 1;
    }
  return out;
}

// mm per AUTHOR length unit of the file. SI lengths (MILLI/CENTI/…/METRE) and
// CONVERSION_BASED_UNIT (inch = LENGTH_MEASURE(25.4) x an SI base). Multiple
// contexts: most common factor wins. Unparseable -> 1.0 (treat OCC mm as the
// author unit — correct for the mm-authored norm) with a printed note.
static double step_unit_mm(const std::string &step_path)
{
  std::ifstream fh(step_path, std::ios::binary);
  if (!fh)
    return 1.0;
  std::stringstream buf;
  buf << fh.rdbuf();
  const std::string text = buf.str();
  size_t ds = text.find("DATA;");
  if (ds == std::string::npos)
    ds = 0;
  size_t de = text.find("ENDSEC;", ds);
  if (de == std::string::npos)
    de = text.size();

  std::unordered_map<int, std::string> ents;
  size_t pos = ds;
  while (pos < de)
  {
    size_t end = text.find(';', pos);
    if (end == std::string::npos || end > de)
      end = de;
    std::string rec = text.substr(pos, end - pos);
    pos = end + 1;
    const size_t h = rec.find('#');
    if (h == std::string::npos)
      continue;
    size_t j = h + 1;
    int id = 0;
    while (j < rec.size() && std::isdigit(static_cast<unsigned char>(rec[j])))
      id = id * 10 + (rec[j++] - '0');
    const size_t eq = rec.find('=', j);
    if (id > 0 && eq != std::string::npos)
      ents[id] = rec.substr(eq + 1);
  }

  std::map<double, int> factors;
  for (const auto &kv : ents)
  {
    const std::string &body = kv.second;
    if (body.find("LENGTH_UNIT") == std::string::npos)
      continue;
    if (body.find("CONVERSION_BASED_UNIT") != std::string::npos)
    {
      for (const int ref : step_refs(body))
      {
        const auto it = ents.find(ref);
        if (it == ents.end())
          continue;
        const size_t m = it->second.find("LENGTH_MEASURE");
        if (m == std::string::npos)
          continue;
        const size_t q = it->second.find('(', m);
        if (q == std::string::npos)
          continue;
        const double val = std::atof(it->second.c_str() + q + 1);
        double base = -1.0;
        for (const int r2 : step_refs(it->second))
        {
          const auto it2 = ents.find(r2);
          if (it2 != ents.end())
          {
            const double b = si_length_mm(it2->second);
            if (b > 0.0)
              base = b;
          }
        }
        if (val > 0.0)
          ++factors[val * (base > 0.0 ? base : 1.0)];
        break;
      }
    }
    else
    {
      const double f = si_length_mm(body);
      if (f > 0.0)
        ++factors[f];
    }
  }
  if (factors.empty())
  {
    std::printf("hc-cadconv: no length unit parsed from the header — "
                "treating OCC mm as author units\n");
    return 1.0;
  }
  double best = 1.0;
  int n = -1;
  for (const auto &kv : factors)
    if (kv.second > n)
    {
      best = kv.first;
      n = kv.second;
    }
  if (factors.size() > 1)
    std::printf("hc-cadconv: mixed length units in header — using %g mm/unit\n",
                best);
  return best;
}

// Explosion guard (unit-abstract, layer 0): the effective chordal deflection is
// floored at this fraction of the BODY's own bbox extent, so a huge or
// out-of-scale model can never tessellate into tens of millions of triangles
// (measured: 0.05 mm on a 45-m-normalised model = 47+ min / 42 GB; floored =
// seconds). Per-body — a small part in a large assembly meshes by its own size.
static const double DEFL_FLOOR_FRAC = 2e-4;

static double defl_floor_mm(const TopoDS_Shape &shape)
{
  try
  {
    Bnd_Box box;
    BRepBndLib::Add(shape, box, Standard_False);
    if (box.IsVoid())
      return 0.0;
    Standard_Real xmin, ymin, zmin, xmax, ymax, zmax;
    box.Get(xmin, ymin, zmin, xmax, ymax, zmax);
    double ext = xmax - xmin;
    if (ymax - ymin > ext) ext = ymax - ymin;
    if (zmax - zmin > ext) ext = zmax - zmin;
    return ext * DEFL_FLOOR_FRAC;
  }
  catch (const Standard_Failure &)
  {
    return 0.0;
  }
}

// The STEP #ident of the ADVANCED_FACE a shape face came from, or 0. THE exact
// face<->primitive correlation: the identity
// hashes are computed off the .stp TEXT per entity — correlating them by shell
// ORDER broke on re-exports (Plasticity renumbers entities, the sort-by-id
// order permutes, every identity lands on the wrong face — live round 9).
// A located copy (assembly placement) may miss the map — retried unlocated.
static int face_eid(const Handle(XSControl_TransferReader) & tr,
                    const Handle(StepData_StepModel) & model,
                    const TopoDS_Face &face)
{
  if (tr.IsNull() || model.IsNull())
    return 0;
  const TopoDS_Shape variants[2] = {face, face.Located(TopLoc_Location())};
  for (const TopoDS_Shape &shp : variants)
  {
    try
    {
      const Handle(Standard_Transient) ent = tr->EntityFromShapeResult(shp, -1);
      if (ent.IsNull())
        continue;
      const int num = model->Number(ent);
      if (num <= 0)
        continue;
      const int ident = model->IdentLabel(ent);
      return ident > 0 ? ident : num;
    }
    catch (const Standard_Failure &)
    {
      continue;
    }
  }
  return 0;
}

// How many faces the STEP FILE declares, or 0 when the model cannot be walked.
// Compared against the faces that actually reached the shape — the difference is
// what the reader DROPPED (live striker.stp: 3 OFFSET_SURFACEs over C0 splines,
// which OCC fails to build and takes the whole face with it). Without this count
// a body goes missing silently.
static int count_step_faces(STEPCAFControl_Reader &reader)
{
  try
  {
    const Handle(Interface_InterfaceModel) model =
        reader.ChangeReader().WS()->Model();
    if (model.IsNull())
      return 0;
    int n = 0;
    for (int i = 1; i <= model->NbEntities(); ++i)
    {
      const char *nm = model->Value(i)->DynamicType()->Name();
      if (nm && (std::string(nm) == "StepShape_AdvancedFace"
                 || std::string(nm) == "StepShape_FaceSurface"))
        ++n;
    }
    return n;
  }
  catch (const Standard_Failure &)
  {
    return 0;
  }
}

static void write_uv_sidecar(const std::string &glb_path,
                             const std::vector<UvRecord> &uv_faces)
{
  // The glb itself carries no TEXCOORD (RWGltf writes it only for textured
  // materials), so the CAD development travels beside it. Consumed by cadio/ops.py.
  Json j;
  j.begin_obj();
  j.kv("v", 1);
  j.key("faces");
  j.begin_arr();
  for (const UvRecord &r : uv_faces)
  {
    if (!r.present)
    {
      j.null();                     // the mesher kept no UV nodes for this face
      continue;
    }
    j.begin_obj();
    j.kv("n", r.n);
    j.key("uv");
    j.begin_arr();
    for (double v : r.uv)
      j.num(v);
    j.end_arr();
    j.kv("pu", r.pu);
    j.kv("pv", r.pv);
    j.end_obj();
  }
  j.end_arr();
  j.end_obj();
  j.write(glb_path + ".uv.json");   // absent = the channel is simply off
}

static void write_oracle_sidecar(const std::string &glb_path,
                                 const std::vector<FaceRecord> &faces,
                                 const std::vector<EdgeRecord> &edges,
                                 const std::vector<SeamRecord> &seams,
                                 const CloneSigs &sigs,
                                 const std::vector<std::string> &sig_order)
{
  // Emptiness rule: an exporter with no analytic
  // curve left (MOI is all-NURBS) yields NO edge oracle — those edges would only
  // widen detection scope, and `has_oracle` keys off edges. Seams ride
  // regardless: a periodic NURBS wall deserves its trace anyway.
  int n_analytic = 0;
  for (const EdgeRecord &e : edges)
    if (e.kind == "line" || e.kind == "circle")
      ++n_analytic;
  bool has_eids = false;
  for (const FaceRecord &f : faces)
    if (f.eid)
    {
      has_eids = true;
      break;
    }
  // eids alone justify the sidecar: the identity correlation must reach
  // the import even for an all-NURBS exporter with no edge oracle.
  if (faces.empty() || (n_analytic == 0 && seams.empty() && !has_eids))
    return;

  Json j;
  j.begin_obj();
  j.kv("v", 1);
  j.key("faces");
  j.begin_arr();
  for (const FaceRecord &f : faces)
  {
    j.begin_obj();
    j.kv("s", f.kind);
    if (f.has_r)
      j.kv("r", f.r);
    if (f.eid)
      j.kv("e", f.eid);
    if (f.g.any())                       // optional placement block (FaceGeom)
    {
      j.key("g");
      j.begin_obj();
      if (f.g.has_o) { j.key("o"); j.begin_arr(); for (double q : f.g.o) j.num(q); j.end_arr(); }
      if (f.g.has_d) { j.key("d"); j.begin_arr(); for (double q : f.g.d) j.num(q); j.end_arr(); }
      if (f.g.has_x) { j.key("x"); j.begin_arr(); for (double q : f.g.x) j.num(q); j.end_arr(); }
      if (f.g.has_rmaj) j.kv("rmaj", f.g.rmaj);
      if (f.g.has_rmin) j.kv("rmin", f.g.rmin);
      if (f.g.has_semi) j.kv("semi", f.g.semi);
      j.end_obj();
    }
    j.end_obj();
  }
  j.end_arr();
  j.key("edges");
  j.begin_arr();
  if (n_analytic)
    for (const EdgeRecord &e : edges)
    {
      j.begin_obj();
      j.key("a"); j.begin_arr(); for (double v : e.a) j.num(v); j.end_arr();
      j.key("b"); j.begin_arr(); for (double v : e.b) j.num(v); j.end_arr();
      j.kv("k", e.kind);
      if (e.has_circle)
      {
        j.kv("r", e.r);
        j.key("c");  j.begin_arr(); for (double v : e.c) j.num(v); j.end_arr();
        j.key("ax"); j.begin_arr(); for (double v : e.ax) j.num(v); j.end_arr();
      }
      j.end_obj();
    }
  j.end_arr();

  std::vector<const std::vector<int> *> clones;
  for (const std::string &s : sig_order)
  {
    const std::vector<int> &g = sigs.at(s);
    if (g.size() > 1)
      clones.push_back(&g);
  }
  if (!clones.empty())
  {
    j.key("clones");
    j.begin_arr();
    for (const std::vector<int> *g : clones)
    {
      j.begin_arr();
      for (int i : *g)
        j.num(i);
      j.end_arr();
    }
    j.end_arr();
  }
  if (!seams.empty())                // absent key = the older behaviour
  {
    j.key("seams");
    j.begin_arr();
    for (const SeamRecord &s : seams)
    {
      j.begin_obj();
      j.kv("f", s.face);
      j.key("p");
      j.begin_arr();
      for (size_t k = 0; k + 2 < s.pts.size(); k += 3)
      {
        j.begin_arr();
        j.num(s.pts[k]); j.num(s.pts[k + 1]); j.num(s.pts[k + 2]);
        j.end_arr();
      }
      j.end_arr();
      j.end_obj();
    }
    j.end_arr();
  }
  j.end_obj();
  j.write(glb_path + ".oracle.json");
}

int convert(const std::string &step_path, const std::string &glb_path,
            const ConvertOpts &opts)
{
  // unit-abstract (PROTOCOL 2): everything OCC hands us is mm-normalised by
  // the DECLARED header unit; unwind it on output so the glb + sidecars carry
  // the author's own magnitudes. `lin_defl` arrives in author units → OCC mm.
  const double unit_mm = step_unit_mm(step_path);
  ORACLE_SCALE = 1.0 / unit_mm;
  const double lin_mm = opts.lin_defl * unit_mm;
  std::printf("hc-cadconv: author unit = %g mm; deflection %g unit(s) = %g mm\n",
              unit_mm, opts.lin_defl, lin_mm);

  // The document format string is part of the recipe, not decoration: the python
  // reference transfers into a "BinXCAF" document and reader/writer are proven
  // in exactly that combination.
  Handle(TDocStd_Document) doc =
      new TDocStd_Document(TCollection_ExtendedString("BinXCAF"));

  STEPCAFControl_Reader reader;
  reader.SetNameMode(Standard_True);   // part names -> XCAF labels (8c uses them)
  if (reader.ReadFile(step_path.c_str()) != IFSelect_RetDone)
    throw std::runtime_error("STEP read failed: " + step_path);
  if (!reader.Transfer(doc))
    throw std::runtime_error("STEP transfer failed: " + step_path);
  // The glb writer takes its input unit from the DOCUMENT's length unit (the
  // Perform(doc) overload overrides a manual SetInputLengthUnit — probed
  // 2026-08-20 on the python twin). Override the doc: one model unit (OCC mm)
  // worth 1/unit_mm «metres» makes the written numbers the AUTHOR magnitudes.
  XCAFDoc_DocumentTool::SetLengthUnit(doc, 1.0 / unit_mm);

  Handle(XCAFDoc_ShapeTool) shape_tool =
      XCAFDoc_DocumentTool::ShapeTool(doc->Main());
  TDF_LabelSequence labels;
  shape_tool->GetFreeShapes(labels);
  if (labels.Length() == 0)
    throw std::runtime_error("no shapes in STEP");

  // Faces the READER dropped, before any tessellation — the only cheap witness.
  const int n_step_faces = count_step_faces(reader);

  // PARTS, not free shapes: an assembly is ONE free shape whose components are
  // the real parts (walked here); a compound of loose bodies has no tree at all
  // and is exploded by solid below. Either way the import gets one object per
  // part instead of the whole scene welded into one mesh.
  std::vector<PartIn> parts_in;
  for (int i = 1; i <= labels.Length(); ++i)
  {
    const TDF_Label lab = labels.Value(i);
    xcaf_parts(shape_tool, lab, TopLoc_Location(), label_name(lab), parts_in);
  }

  const double ang = opts.ang_defl_deg * DEG2RAD;
  std::vector<UvRecord> uv_faces;
  std::vector<FaceRecord> orc_faces;
  std::vector<EdgeRecord> orc_edges;
  std::vector<SeamRecord> orc_seams;
  CloneSigs sigs;
  std::vector<std::string> sig_order;
  std::vector<PartOut> parts;       // per BODY block, in primitive order
  int n_skipped = 0, n_shape_faces = 0;

  // Seal one block: its edge oracle + the sidecar record.
  auto close_block = [&](const std::vector<TopoDS_Face> &faces, int f0,
                         const std::string &name, EdgeTally &ed, bool closed) {
    const int e0 = static_cast<int>(orc_edges.size());
    if (!faces.empty())
    {
      const std::vector<EdgeRecord> e = edge_oracle(block_shape(faces));
      orc_edges.insert(orc_edges.end(), e.begin(), e.end());
    }
    // only a body meant to CLOSE can be torn — a sheet body's rim is not
    const int torn = closed ? tally_tears(ed) : 0;
    if (torn)
      std::printf("hc-cadconv: %s — tessellation is TORN at %d edge(s) "
                  "(the mesher did not stitch adjacent faces)\n",
                  name.empty() ? "?" : name.c_str(), torn);
    PartOut po;
    po.name = name;
    po.n = static_cast<int>(orc_faces.size()) - f0;
    po.ne = static_cast<int>(orc_edges.size()) - e0;
    po.torn = torn;
    parts.push_back(po);
    ed.clear();
  };

  Handle(XSControl_TransferReader) tr;   // shape -> STEP entity map
  Handle(StepData_StepModel) step_model; // entity -> file #ident
  try
  {
    tr = reader.ChangeReader().WS()->TransferReader();
    step_model = Handle(StepData_StepModel)::DownCast(
        reader.ChangeReader().WS()->Model());
  }
  catch (const Standard_Failure &)
  {
    tr.Nullify();                   // no map: bodies fall back to index names
    step_model.Nullify();
  }

  const int NO_OWNER = INT_MIN;     // the python's `cur is None`
  for (const PartIn &part : parts_in)
  {
    // explosion guard: floor the deflection at 2e-4 x the PART's extent — an
    // out-of-scale model must slow down to «coarser», never blow up to 42 GB
    const double eff_lin =
        lin_mm > defl_floor_mm(part.shape) ? lin_mm : defl_floor_mm(part.shape);
    if (eff_lin > lin_mm)
      std::printf("hc-cadconv: %s — deflection floored %.4g -> %.4g mm "
                  "(2e-4 x body extent)\n",
                  part.name.empty() ? "?" : part.name.c_str(), lin_mm, eff_lin);
    BRepMesh_IncrementalMesh(part.shape, eff_lin, Standard_False, ang,
                             Standard_True);
    // [] = one body (or none) — the whole part is the block
    const std::vector<SolidOwner> owners =
        solid_owners(part.shape, tr, part.name);

    if (opts.refine_torn)
    {
      // BEFORE any record is built: a torn body is re-meshed on its own, so the
      // sidecars describe the triangulation that actually ships. Bodies never
      // share faces, so re-meshing one cannot disturb another.
      for (size_t k = 0; k < owners.size(); ++k)
      {
        if (!owners[k].closed)
          continue;   // a sheet body's open boundary is not a tear
        std::vector<TopoDS_Face> faces;
        for (int j = 1; j <= owners[k].faces.Extent(); ++j)
          faces.push_back(TopoDS::Face(owners[k].faces.FindKey(j)));
        if (body_tears(faces))
          refine_torn(owners[k].shape, faces, eff_lin, ang,
                      block_name(part.name, static_cast<int>(k), owners),
                      defl_floor_mm(owners[k].shape));
      }
      if (owners.empty() && has_solid(part.shape))
      {
        std::vector<TopoDS_Face> faces;
        for (TopExp_Explorer e0(part.shape, TopAbs_FACE); e0.More(); e0.Next())
          faces.push_back(TopoDS::Face(e0.Current()));
        if (body_tears(faces))
          refine_torn(part.shape, faces, eff_lin, ang, part.name,
                      defl_floor_mm(part.shape));
      }
    }

    int cur = NO_OWNER, f0 = static_cast<int>(orc_faces.size());
    std::vector<TopoDS_Face> blk;
    EdgeTally ed;                   // quantised triangle edges of THIS block
    for (TopExp_Explorer ex(part.shape, TopAbs_FACE); ex.More(); ex.Next())
    {                               // SAME order as the glb writer's primitives
      const TopoDS_Face face = TopoDS::Face(ex.Current());
      ++n_shape_faces;
      const int own = owners.empty() ? 0 : owner_of(owners, face);
      if (cur != NO_OWNER && own != cur)
      {
        // body boundary: the traversal is untouched, the block simply ENDS
        // here — one object per body, order still the writer's
        close_block(blk, f0, block_name(part.name, cur, owners), ed,
                    block_closed(owners, cur, part.shape));
        f0 = static_cast<int>(orc_faces.size());
        blk.clear();
      }
      cur = own;
      TopLoc_Location loc;
      const Handle(Poly_Triangulation) tri = BRep_Tool::Triangulation(face, loc);
      if (tri.IsNull() || tri->NbTriangles() == 0)
      {
        // The mesher produced NOTHING for this face (live p320: one offset
        // surface) -> RWGltf emits NO primitive for it. A sidecar record here
        // would SHIFT every later index and the import guards would drop both
        // channels whole — skip it in both. The contract is one record per
        // EMITTED primitive.
        ++n_skipped;
        continue;
      }
      blk.push_back(face);
      tally_edges(tri, loc, ed);
      uv_faces.push_back(face_uv_record(face, tri));
      orc_faces.push_back(face_oracle(face, sigs, sig_order,
                                      static_cast<int>(orc_faces.size())));
      orc_faces.back().eid = face_eid(tr, step_model, face);
      const std::vector<SeamRecord> seams =
          face_seams(face, static_cast<int>(orc_faces.size()) - 1);
      orc_seams.insert(orc_seams.end(), seams.begin(), seams.end());
    }
    const int last = cur == NO_OWNER ? 0 : cur;
    close_block(blk, f0, block_name(part.name, last, owners), ed,
                block_closed(owners, last, part.shape));
  }
  if (n_skipped)
    std::printf("hc-cadconv: %d face(s) had no tessellation — skipped in "
                "sidecars\n", n_skipped);
  const int lost = n_step_faces ? (n_step_faces - n_shape_faces > 0
                                       ? n_step_faces - n_shape_faces : 0)
                                : 0;
  if (lost)
    std::printf("hc-cadconv: %d of %d CAD face(s) were DROPPED BY THE READER "
                "(unbuildable surface) — bodies may be incomplete\n",
                lost, n_step_faces);

  // Part sidecar: the free-shape boundaries of the SAME face traversal that
  // orders the glb primitives — contiguous blocks in primitive order. The
  // import slices prims + both other sidecars by these counts and builds ONE
  // OBJECT PER BODY. A zero-face part (all its faces skipped above) is KEPT so
  // the edge cursor stays aligned; the import skips it.
  {
    Json j;
    j.begin_obj();
    j.kv("v", 1);
    j.key("parts");
    j.begin_arr();
    for (const PartOut &p : parts)
    {
      j.begin_obj();
      j.kv("name", p.name);
      j.kv("n", p.n);
      j.kv("ne", p.ne);
      j.kv("torn", p.torn);
      j.end_obj();
    }
    j.end_arr();
    j.kv("lost", lost);
    j.kv("untessellated", n_skipped);
    j.end_obj();
    j.write(glb_path + ".parts.json");  // absent = one merged object (old path)
  }

  write_uv_sidecar(glb_path, uv_faces);
  write_oracle_sidecar(glb_path, orc_faces, orc_edges, orc_seams, sigs,
                       sig_order);

  RWGltf_CafWriter writer(TCollection_AsciiString(glb_path.c_str()),
                          Standard_True);       // True = binary .glb
  writer.SetMergeFaces(Standard_False);         // PIN: one primitive per CAD face
  // unit-abstract: OCC mm -> AUTHOR units (mm-file: x1, metre-declared:
  // x0.001) — same factor as ORACLE_SCALE so sidecars == primitives. The doc's
  // length unit (set above) is what Perform actually honors; this converter
  // set is kept consistent with it.
  writer.ChangeCoordinateSystemConverter().SetInputLengthUnit(1.0 / unit_mm);
  // PIN: identity coordinate frame — the third pin is simply not touching the
  // converter's default input/output orientation.

  TColStd_IndexedDataMapOfStringString file_info;
  if (!writer.Perform(doc, file_info, Message_ProgressRange()))
    throw std::runtime_error("glb write failed: " + glb_path);
  return labels.Length();
}
