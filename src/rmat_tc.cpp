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

int main(int argc, char **argv) {
  ygm::comm world(&argc, &argv);

  int rmat_scale = atoi(argv[1]);

  uint64_t total_num_edges =
      uint64_t(1) << (uint64_t(
          rmat_scale + 4)); /// Number of total edges (avg 16 per vertex)
  uint64_t local_num_edges =
      total_num_edges / world.size() +
      (world.rank() <
       total_num_edges % world.size()); /// Number of edges each rank generates
  bool undirected = false;
  bool scramble = true;
  rmat_edge_generator rmat(world.rank(), rmat_scale, local_num_edges, 0.57,
                           0.19, 0.19, 0.05, scramble, undirected);

  // Build undirected version of graph
  world.cout0("Generating undirected RMAT graph scale ", rmat_scale);
  ygm::timer step_timer{};
  ygm::timer preprocess_timer{};

  // Graph with ints for IDs, and bools as dummy vertex and edge metadata
  tripoll::undirected_graph<uint64_t, bool, bool> g(world);

  auto edge_gen_iter = rmat.begin();
  auto edge_gen_end = rmat.end();
  while (edge_gen_iter != edge_gen_end) {
    auto &edge = *edge_gen_iter;

    auto vtx1 = std::get<0>(edge);
    auto vtx2 = std::get<1>(edge);

    g.async_add_edge(vtx1, vtx2, true);

    ++edge_gen_iter;
  }

  g.barrier();

  world.cout0("RMAT generation time: ", step_timer.elapsed(), " seconds");

  world.cout0("Generated graph with ", g.num_vertices(), " vertices and ",
              g.num_edges(), " edges");
  world.cout0("Maximum original degree: ", g.max_degree(), "\n");

  // Remove duplicate edges
  world.cout0("Removing duplicate edges");
  step_timer.reset();
  g.uniquify_edges();

  g.barrier();

  world.cout0("Edge deduplication time: ", step_timer.elapsed(), " seconds\n");

  // Convert to DODGR
  world.cout0("Converting graph to DODGR");
  step_timer.reset();

  tripoll::ordered_directed_graph<uint64_t, bool, bool, uint32_t> dodgr(world);

  make_dodgr(g, dodgr);

  dodgr.barrier();

  world.cout0("DODGR construction time: ", step_timer.elapsed(), " seconds\n");
  world.cout0("Total preprocessing time: ", preprocess_timer.elapsed(),
              " seconds\n");

  world.cout0("DODGR contains: ", dodgr.num_vertices(), " vertices and ",
              dodgr.num_edges(), " edges");
  world.cout0("DODGR wedges: ", dodgr_wedges(dodgr));
  world.cout0("Maximum DODGR degree: ", dodgr.max_degree(), "\n");

  // Count triangles
  world.cout0("Counting triangles");
  step_timer.reset();

  static uint64_t tc{0};

  auto tc_lambda = [](const auto &p_metadata, const auto &q_metadata,
                      const auto &r_metadata, const auto &pq_metadata,
                      const auto &pr_metadata,
                      const auto &qr_metadata) { ++tc; };

  world.cout0("\nUsing push-pull implementation");
  step_timer.reset();

  tc_push_pull(dodgr, tc_lambda);

  tc = world.all_reduce_sum(tc);

  world.cout0("Found ", tc, " triangles in RMAT graph of scale ", rmat_scale,
              " in ", step_timer.elapsed(), " seconds");

  // Clear output data
  tc = 0;

  world.cout0("\nUsing push-only implementation");
  step_timer.reset();

  tc_push_only(dodgr, tc_lambda);

  tc = world.all_reduce_sum(tc);

  world.cout0("Found ", tc, " triangles in RMAT graph of scale ", rmat_scale,
              " in ", step_timer.elapsed(), " seconds");
}
