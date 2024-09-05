// Copyright 2021-2022 Lawrence Livermore National Security, LLC and other
// TriPoll Project Developers. See the top-level COPYRIGHT file for details.
//
// SPDX-License-Identifier: MIT

#include <algorithm.hpp>
#include <cmath>
#include <iostream>
#include <ordered_directed_graph.hpp>
#include <rmat_edge_generator.hpp>
#include <undirected_graph.hpp>
#include <ygm/container/counting_set.hpp>
#include <ygm/utility.hpp>

// Specialize std::hash for tuples of unsigned ints to use in a counting_set
namespace std {
template <> struct hash<std::tuple<uint64_t, uint64_t, uint64_t>> {
public:
  size_t operator()(const std::tuple<uint64_t, uint64_t, uint64_t> &s) {
    size_t h1 = std::hash<uint64_t>()(std::get<0>(s));
    size_t h2 = std::hash<uint64_t>()(std::get<1>(s));
    size_t h3 = std::hash<uint64_t>()(std::get<2>(s));

    // Bad
    return h1 ^ (h2 << 1) ^ (h3 << 2);
  }
};

template <> struct hash<std::tuple<uint8_t, uint8_t, uint8_t>> {
public:
  size_t operator()(const std::tuple<uint8_t, uint8_t, uint8_t> &s) const {
    return std::get<0>(s) + (std::get<1>(s) << 8) + (std::get<2>(s) << 16);
  }
};
} // namespace std

int main(int argc, char **argv) {
  ygm::comm world(&argc, &argv);

  int rmat_scale = atoi(argv[1]);

  std::string ofname;
  if (argc == 3) {
    ofname = argv[2];
  }

  uint64_t total_num_edges =
      uint64_t(1) << (uint64_t(
          rmat_scale + 4)); /// Number of total edges (avg 16 per vertex)
  uint64_t local_num_edges =
      total_num_edges / world.size() +
      (world.rank() <
       total_num_edges % world.size()); /// Number of edges each rank generates
  bool undirected = false;
  rmat_edge_generator rmat(world.rank(), rmat_scale, local_num_edges, 0.57,
                           0.19, 0.19, 0.05, true, undirected);

  // Build undirected version of graph
  world.cout0("Generating undirected RMAT graph");
  ygm::timer step_timer{};
  ygm::timer preprocess_timer{};

  // Graph with ints for IDs, vertex metadata, and edge metadata.
  tripoll::undirected_graph<uint64_t, uint32_t, uint64_t> g(world);

  auto edge_gen_iter = rmat.begin();
  auto edge_gen_end = rmat.end();
  while (edge_gen_iter != edge_gen_end) {
    auto &edge = *edge_gen_iter;

    auto vtx1 = std::get<0>(edge);
    auto vtx2 = std::get<1>(edge);
    auto min_id = std::min(vtx1, vtx2);

    g.async_add_edge(vtx1, vtx2, min_id);

    ++edge_gen_iter;
  }

  g.barrier();

  world.cout0("RMAT generation time: ", step_timer.elapsed(), " seconds");

  world.cout0("Generated graph with ", g.num_vertices(), " vertices and ",
              g.num_edges(), " edges");
  world.cout0("Maximum original degree: ", g.max_degree(), "\n");

  // Add degree as vertex metadata
  world.cout0("Adding degree as vertex metadata");
  step_timer.reset();

  auto degree_lambda = [&g](const auto &vtx_id, const auto &data) {
    auto degree = data.second.size();
    g.async_set_vertex_metadata(vtx_id, degree);
  };

  g.for_all_vertices(degree_lambda);

  g.barrier();

  world.cout0("Vertex metadata time: ", step_timer.elapsed(), " seconds\n");

  // Remove duplicate edges
  world.cout0("Removing duplicate edges");
  step_timer.reset();
  g.uniquify_edges();

  g.barrier();

  world.cout0("Edge deduplication time: ", step_timer.elapsed(), " seconds\n");

  // Convert to DODGR, and add vertex metadata that is original degree
  world.cout0("Converting graph to DODGR");
  step_timer.reset();

  tripoll::ordered_directed_graph<uint64_t, uint32_t, uint64_t, uint32_t> dodgr(
      world);

  make_dodgr(g, dodgr);

  dodgr.barrier();

  world.cout0("DODGR construction time: ", step_timer.elapsed(), " seconds\n");
  world.cout0("Total preprocessing time: ", preprocess_timer.elapsed(),
              " seconds\n");

  world.cout0("DODGR contains: ", dodgr.num_vertices(), " vertices and ",
              dodgr.num_edges(), " edges");
  world.cout0("Maximum DODGR degree: ", dodgr.max_degree(), "\n");

  // Count triangles
  world.cout0("Counting triangles");

  static uint64_t tc{0};

  ygm::container::counting_set<std::tuple<uint8_t, uint8_t, uint8_t>>
      degree_counts(world);

  static auto find_exponent = [](uint64_t a) {
    return static_cast<uint8_t>(std::floor(log2(a)));
  };

  auto tc_lambda = [](const auto &p_metadata, const auto &q_metadata,
                      const auto &r_metadata, const auto &pq_metadata,
                      const auto &pr_metadata, const auto &qr_metadata,
                      auto degree_map_ptr) {
    ++tc;
    degree_map_ptr->async_insert(std::make_tuple(find_exponent(p_metadata),
                                                 find_exponent(q_metadata),
                                                 find_exponent(r_metadata)));
  };

  step_timer.reset();

  world.cout0("\nUsing push-pull implementation");
  step_timer.reset();

  tc_push_pull(dodgr, tc_lambda, world.make_ygm_ptr(degree_counts));

  tc = world.all_reduce_sum(tc);

  world.cout0("Found ", tc, " triangles in RMAT graph of scale ", rmat_scale,
              " in ", step_timer.elapsed(), " seconds");

  world.cout0(degree_counts.size(), " unique sets of degrees found");

  // Clear output data
  tc = 0;
  degree_counts.clear();

  world.cout0("\nUsing push-only implementation");
  step_timer.reset();

  tc_push_only(dodgr, tc_lambda, world.make_ygm_ptr(degree_counts));

  tc = world.all_reduce_sum(tc);

  world.cout0("Found ", tc, " triangles in RMAT graph of scale ", rmat_scale,
              " in ", step_timer.elapsed(), " seconds");

  world.cout0(degree_counts.size(), " unique sets of degrees found");

  if (ofname.length() > 0) {
    world.cout0("\nSerializing degree counts to file");
    degree_counts.serialize(ofname);
  }
}
