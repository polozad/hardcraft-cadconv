#include "parts.h"

#include <cmath>

#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <Standard_Failure.hxx>
#include <StepRepr_RepresentationItem.hxx>
#include <TDF_Label.hxx>
#include <TDF_LabelSequence.hxx>
#include <TDataStd_Name.hxx>
#include <TCollection_AsciiString.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <gp_Pnt.hxx>

static std::string trimmed(const std::string &s)
{
  const size_t a = s.find_first_not_of(" \t\r\n");
  if (a == std::string::npos)
    return std::string();
  return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}

std::string label_name(const TDF_Label &label)
{
  // STEP part name off the XCAF label (SetNameMode(true) fills it), or "" — the
  // import falls back to the file stem then. Best-effort: a nameless or mute
  // label costs a name, never the part.
  try
  {
    Handle(TDataStd_Name) attr;
    if (label.FindAttribute(TDataStd_Name::GetID(), attr))
    {
      // 0 = convert to UTF-8 rather than replace non-ASCII
      const std::string nm =
          trimmed(TCollection_AsciiString(attr->Get(), 0).ToCString());
      // OCC placeholders are not names: a component label's own name is the
      // "=>[0:1:1:3]" reference token, and a nameless root comes back as the
      // writer's banner. Both would become object names.
      if (!nm.empty() && nm.rfind("=>", 0) != 0
          && nm.find("STEP translator") == std::string::npos)
        return nm;
    }
  }
  catch (const Standard_Failure &)
  {
  }
  return std::string();
}

// The part label a component label points at, or an empty label (a plain leaf).
static TDF_Label referred(const Handle(XCAFDoc_ShapeTool) & shape_tool,
                          const TDF_Label &comp)
{
  try
  {
    TDF_Label ref;
    if (shape_tool->GetReferredShape(comp, ref))
      return ref;
  }
  catch (const Standard_Failure &)
  {
  }
  return TDF_Label();
}

void xcaf_parts(const Handle(XCAFDoc_ShapeTool) & shape_tool,
                const TDF_Label &label, const TopLoc_Location &loc,
                const std::string &name_hint, std::vector<PartIn> &out)
{
  // A STEP assembly arrives as ONE free shape (the root) whose children are
  // component references — walking it is what turns "the whole scene in one
  // mesh" into one object per real part. Each component carries its own
  // placement, accumulated down the branch, so the located leaf shape is what
  // the mesher and the glb writer see.
  try
  {
    if (shape_tool->IsAssembly(label))
    {
      TDF_LabelSequence comps;
      shape_tool->GetComponents(label, comps);
      for (int i = 1; i <= comps.Length(); ++i)
      {
        const TDF_Label comp = comps.Value(i);
        const TopLoc_Location cloc =
            loc.Multiplied(XCAFDoc_ShapeTool::GetLocation(comp));
        const TDF_Label sub = referred(shape_tool, comp);
        // either label may hold the human name and the other a placeholder
        // ("=>[0:1:1:3]" on the component, a bare "0" on the part) — the
        // component's own name wins when it has one
        std::string nm = label_name(comp);
        if (nm.empty() && !sub.IsNull())
          nm = label_name(sub);
        if (nm.empty())
          nm = name_hint;
        xcaf_parts(shape_tool, sub.IsNull() ? comp : sub, cloc, nm, out);
      }
      return;
    }
  }
  catch (const Standard_Failure &)
  {
    // unreadable branch -> treat it as a leaf
  }
  TopoDS_Shape shp = XCAFDoc_ShapeTool::GetShape(label);
  if (shp.IsNull())
    return;
  if (!loc.IsIdentity())
    shp = shp.Moved(loc);
  PartIn p;
  p.shape = shp;
  p.name = name_hint.empty() ? label_name(label) : name_hint;
  out.push_back(p);
}

// The body's OWN name from the STEP file, or "".
//
// Bodies inside one CAD part carry their names on the MANIFOLD_SOLID_BREP entity
// (striker.stp: 'extractor', 'striker_pin', …), which never reaches an XCAF
// label — only the transfer reader's shape->entity map has it. The shape may be
// a located copy (assembly placement), so a miss is retried on the unlocated
// original.
//
// NB the C++ map hands back a bare Handle(Standard_Transient): the DownCast is
// ours to do (the python bindings auto-resolve it). Skip it and every body
// silently loses its name — and with it the per-body split's naming.
static std::string solid_name(const Handle(XSControl_TransferReader) & tr,
                              const TopoDS_Shape &solid)
{
  if (tr.IsNull())
    return std::string();
  const TopoDS_Shape variants[2] = {solid, solid.Located(TopLoc_Location())};
  for (const TopoDS_Shape &shp : variants)
  {
    try
    {
      const Handle(Standard_Transient) ent = tr->EntityFromShapeResult(shp, -1);
      if (ent.IsNull())
        continue;
      const Handle(StepRepr_RepresentationItem) item =
          Handle(StepRepr_RepresentationItem)::DownCast(ent);
      if (item.IsNull() || item->Name().IsNull())
        continue;
      const std::string nm = trimmed(item->Name()->ToCString());
      if (!nm.empty())
        return nm;
    }
    catch (const Standard_Failure &)
    {
      continue;                     // unmapped body -> falls back to an index
    }
  }
  return std::string();
}

std::vector<SolidOwner> solid_owners(const TopoDS_Shape &shape,
                                     const Handle(XSControl_TransferReader) & tr,
                                     const std::string &name_hint)
{
  // A CAD "part" is very often a COMPOUND OF BODIES (striker.stp: one XCAF part
  // `slide_internals` = 10 solids) — the split has to reach inside it, or the
  // pack still arrives as one mesh.
  std::vector<SolidOwner> maps;
  for (TopExp_Explorer ex(shape, TopAbs_SOLID); ex.More(); ex.Next())
  {
    maps.emplace_back();
    SolidOwner &o = maps.back();
    TopExp::MapShapes(ex.Current(), TopAbs_FACE, o.faces);
    o.name = solid_name(tr, ex.Current());
    o.shape = ex.Current();
    o.closed = true;
  }
  if (maps.empty())
    return maps;

  // Bodies whose solid FAILED to build in OCC survive as loose shells
  // (striker.stp: 2 of the 12 MANIFOLD_SOLID_BREPs — their entity name is
  // unreachable from the shape map, only the grouping is). Without this they
  // would all land in one lump object.
  TopTools_IndexedMapOfShape owned;
  for (const SolidOwner &o : maps)
    for (int i = 1; i <= o.faces.Extent(); ++i)
      owned.Add(o.faces.FindKey(i));
  int k = 0;
  for (TopExp_Explorer ex(shape, TopAbs_SHELL); ex.More(); ex.Next())
  {
    TopTools_IndexedMapOfShape fm;
    TopExp::MapShapes(ex.Current(), TopAbs_FACE, fm);
    bool all_owned = true;
    for (int i = 1; i <= fm.Extent() && all_owned; ++i)
      all_owned = owned.Contains(fm.FindKey(i));
    if (all_owned)
      continue;                     // a solid's own shell — already covered
    maps.emplace_back();
    SolidOwner &o = maps.back();
    o.faces = fm;
    o.name = name_hint.empty() ? std::string()
                               : name_hint + "_sheet" + std::to_string(k);
    o.shape = ex.Current();
    o.closed = false;
    ++k;
  }
  if (maps.size() <= 1)
    maps.clear();                   // one body (or none) — the whole part is it
  return maps;
}

int owner_of(const std::vector<SolidOwner> &owners, const TopoDS_Face &face)
{
  for (size_t k = 0; k < owners.size(); ++k)
    if (owners[k].faces.Contains(face))
      return static_cast<int>(k);
  return -1;                        // a loose sheet face
}

bool has_solid(const TopoDS_Shape &shape)
{
  // True when `shape` holds at least one SOLID — i.e. a body whose surface is
  // meant to close. Only there does an open mesh edge mean a TEAR: a sheet body
  // (MOI exports 446 loose shells) is open by nature, and counting its natural
  // rim as damage sent the refine pass chasing 8347 phantom tears.
  return TopExp_Explorer(shape, TopAbs_SOLID).More();
}

bool block_closed(const std::vector<SolidOwner> &owners, int owner,
                  const TopoDS_Shape &shape)
{
  if (!owners.empty())
    return owner >= 0 && owners[static_cast<size_t>(owner)].closed;
  return has_solid(shape);
}

std::string block_name(const std::string &part_name, int owner,
                       const std::vector<SolidOwner> &owners)
{
  // The BODY's own STEP name when it has one ('extractor'), else the part's name
  // with a body index ('slide_internals_3'). Owner -1 = faces inside no body.
  if (owners.empty())
    return part_name;
  if (owner >= 0 && !owners[static_cast<size_t>(owner)].name.empty())
    return owners[static_cast<size_t>(owner)].name;
  if (part_name.empty())
    return std::string();
  return owner >= 0 ? part_name + "_" + std::to_string(owner)
                    : part_name + "_loose";
}

TopoDS_Shape block_shape(const std::vector<TopoDS_Face> &faces)
{
  TopoDS_Compound comp;
  BRep_Builder b;
  b.MakeCompound(comp);
  for (const TopoDS_Face &f : faces)
    b.Add(comp, f);
  return comp;
}

void tally_edges(const Handle(Poly_Triangulation) & tri,
                 const TopLoc_Location &loc, EdgeTally &ed)
{
  // Nanometre grid over the raw mm coordinates. OCC tears on sloppy input — the
  // live striker.stp housing carries edges with a 6.8 mm tolerance (on a 20 mm
  // body) and tears in 41 places at the default deflection. Zero-length edges
  // (collapsed triangles) are not tears and are skipped; the import drops those
  // triangles anyway.
  const gp_Trsf t = loc.Transformation();
  std::vector<std::array<long long, 3>> key;
  key.reserve(static_cast<size_t>(tri->NbNodes()));
  for (int k = 1; k <= tri->NbNodes(); ++k)
  {
    const gp_Pnt p = tri->Node(k).Transformed(t);
    key.push_back({static_cast<long long>(std::nearbyint(p.X() * 1e6)),
                   static_cast<long long>(std::nearbyint(p.Y() * 1e6)),
                   static_cast<long long>(std::nearbyint(p.Z() * 1e6))});
  }
  for (int k = 1; k <= tri->NbTriangles(); ++k)
  {
    int a = 0, b = 0, c = 0;
    tri->Triangle(k).Get(a, b, c);
    const int pairs[3][2] = {{a, b}, {b, c}, {c, a}};
    for (const auto &pr : pairs)
    {
      const std::array<long long, 3> &ku = key[pr[0] - 1];
      const std::array<long long, 3> &kv = key[pr[1] - 1];
      if (ku == kv)
        continue;                   // collapsed corner — not a tear
      ed[ku < kv ? std::make_pair(ku, kv) : std::make_pair(kv, ku)] += 1;
    }
  }
}

int tally_tears(const EdgeTally &ed)
{
  int n = 0;
  for (const auto &kv : ed)
    if (kv.second == 1)
      ++n;
  return n;
}
