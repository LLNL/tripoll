// Copyright 2021-2022 Lawrence Livermore National Security, LLC and other
// TriPoll Project Developers. See the top-level COPYRIGHT file for details.
//
// SPDX-License-Identifier: MIT

#include <algorithm.hpp>
#include <cereal/types/utility.hpp>
#include <cmath>
#include <iostream>
#include <ordered_directed_graph.hpp>
#include <rmat_edge_generator.hpp>
#include <string>
#include <undirected_graph.hpp>
#include <ygm/container/bag.hpp>
#include <ygm/container/counting_set.hpp>
#include <ygm/utility.hpp>

int main(int argc, char **argv) {
  ygm::comm world(&argc, &argv);

  ygm::container::bag<std::string> bag_filenames(world);

  if (world.rank0()) {
    for (int i = 1; i < argc; ++i) {
      bag_filenames.async_insert(argv[i]);
    }
  }

  // Build undirected version of graph
  world.cout0("Building undirected graph");
  ygm::timer step_timer{};
  ygm::timer preprocess_timer{};

  tripoll::undirected_graph<uint32_t, bool, bool> g(world);

  auto read_file_lambda = [&g, &world](std::string &fname) {
    std::ifstream ifs(fname.c_str());
    ifs.imbue(std::locale::classic());
    std::string line;
    uint32_t source;
    uint32_t target;
    while (std::getline(ifs, line)) {
      try {
        std::stringstream ss(line);
        ss >> source >> target;
        g.async_add_edge(source, target, true);
      } catch (...) {
        world.cout() << ": Edge Parse Error: " << line << std::endl;
      }
    }
    // world.cout() << fname << " completed!" << std::endl;
  };

  bag_filenames.for_all(read_file_lambda);

  g.barrier();

  g.uniquify_edges();

  world.cout0("Vertices: ", g.num_vertices(), "\nEdges: ", g.num_edges());
  world.cout0("Max original degree: ", g.max_degree());
  world.cout0("Graph creation time: ", step_timer.elapsed(), " seconds\n");

  // Convert to DODGR
  world.cout0("Converting graph to DODGR");
  step_timer.reset();

  tripoll::ordered_directed_graph<uint32_t, bool, bool, uint32_t> dodgr(world);

  make_dodgr(g, dodgr);

  dodgr.barrier();

  g.clear();

  world.cout0("Vertices: ", dodgr.num_vertices(),
              "\nEdges: ", dodgr.num_edges());
  world.cout0("Max DODGR degree: ", dodgr.max_degree());
  world.cout0("DODGR wedges: ", dodgr_wedges(dodgr));
  world.cout0("DODGR construction time: ", step_timer.elapsed(), " seconds\n");
  world.cout0("Total preprocessing time: ", preprocess_timer.elapsed(),
              " seconds\n");

  world.cout0("Counting triangles");

  static uint64_t tc{0};

  auto triangle_callback = [](const auto &p_metadata, const auto &q_metadata,
                              const auto &r_metadata, const auto &pq_metadata,
                              const auto &pr_metadata,
                              const auto &qr_metadata) { tc++; };

  // Push-pull
  world.cout0("Push-pull implementation");
  step_timer.reset();
  tc_push_pull(dodgr, triangle_callback);

  tc = world.all_reduce_sum(tc);

  world.cout0("Found ", tc, " triangles in ", step_timer.elapsed(), " seconds");

  // Push-only
  world.cout0("\nPush-only implementation");
  step_timer.reset();
  tc = 0;
  tc_push_only(dodgr, triangle_callback);

  world.cout0("Found ", tc, " triangles in ", step_timer.elapsed(), " seconds");
}
