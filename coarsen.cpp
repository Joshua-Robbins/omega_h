static Read<I8> get_edge_codes(Mesh& mesh) {
  auto edge_cand_codes = mesh.get_array<I8>(EDGE, "collapse_code");
  mesh.remove_tag(EDGE, "collapse_code");
  return edge_cand_codes;
}

static void put_edge_codes(Mesh& mesh, LOs cands2edges, Read<I8> cand_codes) {
  auto edge_cand_codes_w = Write<I8>(mesh.nedges(), DONT_COLLAPSE);
  map_into(cand_codes, cands2edges, edge_cand_codes_w, 1);
  mesh.add_tag(EDGE, "collapse_code", 1, OSH_DONT_TRANSFER,
      Read<I8>(edge_cand_codes_w));
}

static Reals get_edge_quals(Mesh& mesh) {
  auto edge_cand_quals = mesh.get_array<Real>(EDGE, "collapse_qualities");
  mesh.remove_tag(EDGE, "collapse_qualities");
  return edge_cand_quals;
}

static void put_edge_quals(Mesh& mesh, LOs cands2edges, Reals cand_quals) {
  auto edge_cand_quals_w = Write<Real>(mesh.nedges() * 2, -1.0);
  map_into(cand_quals, cands2edges, edge_cand_quals_w, 2);
  mesh.add_tag(EDGE, "collapse_qualities", 2, OSH_DONT_TRANSFER,
      Reals(edge_cand_quals_w));
}

static bool coarsen_element_based1(Mesh& mesh) {
  auto comm = mesh.comm();
  auto edge_cand_codes = get_edge_codes(mesh);
  auto edges_are_cands = each_neq_to(edge_cand_codes, I8(DONT_COLLAPSE));
  auto cands2edges = collect_marked(edges_are_cands);
  auto cand_codes = unmap(cands2edges, edge_cand_codes, 1);
  cand_codes = check_collapse_class(mesh, cands2edges, cand_codes);
  /* edge and endpoints classification check */
  if (comm->reduce_and(max(cand_codes) <= DONT_COLLAPSE)) return false;
  put_edge_codes(mesh, cands2edges, cand_codes);
  return true;
}

static void filter_coarsen_candidates(
    LOs& cands2edges,
    Read<I8>& cand_codes,
    Reals& cand_quals) {
  auto keep = each_neq_to(cand_codes, I8(DONT_COLLAPSE));
  auto new2old = collect_marked(keep);
  cands2edges = unmap(new2old, cands2edges, 1);
  cand_codes = unmap(new2old, cand_codes, 1);
  if (cand_quals.exists())
    cand_quals = unmap(new2old, cand_quals, 2);
}

static bool coarsen_ghosted(Mesh& mesh, Real min_qual, bool improve) {
  auto comm = mesh.comm();
  auto edge_cand_codes = get_edge_codes(mesh);
  auto edges_are_cands = each_neq_to(edge_cand_codes, I8(DONT_COLLAPSE));
  auto cands2edges = collect_marked(edges_are_cands);
  auto cand_edge_codes = unmap(cands2edges, edge_cand_codes, 1);
  auto cand_edge_quals = Reals();
  cand_edge_codes = check_collapse_exposure(mesh, cands2edges, cand_edge_codes);
  filter_coarsen_candidates(cands2edges, cand_edge_codes, cand_edge_quals);
  /* surface exposure (classification) checks */
  if (comm->reduce_and(cands2edges.size() == 0)) return false;
  cand_edge_quals = coarsen_qualities(mesh, cands2edges, cand_edge_codes);
  cand_edge_codes = filter_coarsen_min_qual(
      cand_edge_codes, cand_edge_quals, min_qual);
  if (improve) {
    cand_edge_codes = filter_coarsen_improve(
        mesh, cands2edges, cand_edge_codes, cand_edge_quals);
  }
  filter_coarsen_candidates(cands2edges, cand_edge_codes, cand_edge_quals);
  /* cavity quality checks */
  if (comm->reduce_and(cands2edges.size() == 0)) return false;
  auto verts_are_cands = Read<I8>();
  auto vert_quals = Reals();
  choose_vertex_collapses(mesh, cands2edges, cand_edge_codes, cand_edge_quals,
      verts_are_cands, vert_quals);
  auto verts_are_keys = find_indset(mesh, VERT, vert_quals, verts_are_cands);
  mesh.add_tag(VERT, "key", 1, OSH_DONT_TRANSFER, verts_are_keys);
  mesh.add_tag(VERT, "collapse_quality", 1, OSH_DONT_TRANSFER, vert_quals);
  put_edge_codes(mesh, cands2edges, cand_edge_codes);
  put_edge_quals(mesh, cands2edges, cand_edge_quals);
  auto keys2verts = collect_marked(verts_are_keys);
  set_owners_by_indset(mesh, VERT, keys2verts);
  return true;
}

static void coarsen_element_based2(Mesh& mesh) {
  auto verts_are_keys = mesh.get_array<I8>(VERT, "key");
  auto vert_quals = mesh.get_array<Real>(VERT, "collapse_quality");
  auto edge_codes = get_edge_codes(mesh);
  auto edge_quals = get_edge_quals(mesh);
  auto keys2verts = collect_marked(verts_are_keys);
  auto nkeys = keys2verts.size();
  auto rails2edges = LOs();
  auto rail_col_dirs = Read<I8>();
  find_rails(mesh, keys2verts, vert_quals, edge_codes, edge_quals,
      rails2edges, rail_col_dirs);
  auto dead_ents = mark_dead_ents(mesh, rails2edges, rail_col_dirs);
  auto keys2verts_onto = get_verts_onto(mesh, rails2edges, rail_col_dirs);
  auto new_mesh = Mesh();
  new_mesh.set_comm(mesh.comm());
  new_mesh.set_dim(mesh.dim());
  new_mesh.set_partition(mesh.partition());
  auto old_verts2new_verts = LOs();
  auto old_lows2new_lows = LOs();
  for (Int ent_dim = 0; ent_dim <= mesh.dim(); ++ent_dim) {
    auto keys2prods = LOs();
    auto prod_verts2verts = LOs();
    if (ent_dim == VERT) {
      keys2prods = LOs(nkeys + 1, 0);
    } else {
      auto keys2doms = find_coarsen_domains(mesh, keys2verts, ent_dim,
          dead_ents[ent_dim]);
      keys2prods = keys2doms.a2ab;
      prod_verts2verts = coarsen_topology(mesh, keys2verts_onto,
          ent_dim, keys2doms, old_verts2new_verts);
    }
    auto prods2new_ents = LOs();
    auto same_ents2old_ents = LOs();
    auto same_ents2new_ents = LOs();
    auto old_ents2new_ents = LOs();
    modify_ents(mesh, new_mesh, ent_dim, VERT, keys2verts,
        keys2prods, prod_verts2verts, old_lows2new_lows,
        prods2new_ents,
        same_ents2old_ents, same_ents2new_ents,
        old_ents2new_ents);
    if (ent_dim == VERT) old_verts2new_verts = old_ents2new_ents;
    /* TODO: transfer for coarsening ! */
    old_lows2new_lows = old_ents2new_ents;
  }
  mesh = new_mesh;
}

bool coarsen(Mesh& mesh, Real min_qual, bool improve) {
  if (!coarsen_element_based1(mesh)) return false;
  mesh.set_partition(GHOSTED);
  if (!coarsen_ghosted(mesh, min_qual, improve)) return false;
  mesh.set_partition(ELEMENT_BASED);
  coarsen_element_based2(mesh);
  return true;
}

bool coarsen_verts(Mesh& mesh, Read<I8> vert_marks,
    Real min_qual, bool improve) {
  auto ev2v = mesh.ask_verts_of(EDGE);
  Write<I8> edge_codes_w(mesh.nedges(), DONT_COLLAPSE);
  auto f = LAMBDA(LO e) {
    I8 code = DONT_COLLAPSE;
    for (Int eev = 0; eev < 2; ++eev) {
      if (vert_marks[ev2v[e * 2 + eev]]) {
        code = do_collapse(code, eev);
      }
    }
    edge_codes_w[e] = code;
  };
  parallel_for(mesh.nedges(), f);
  mesh.add_tag(EDGE, "collapse_code", 1, OSH_DONT_TRANSFER,
      Read<I8>(edge_codes_w));
  return coarsen(mesh, min_qual, improve);
}