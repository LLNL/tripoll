// Copyright 2021-2022 Lawrence Livermore National Security, LLC and other
// TriPoll Project Developers. See the top-level COPYRIGHT file for details.
//
// SPDX-License-Identifier: MIT

#include <algorithm.hpp>
#include <iostream>
#include <ordered_directed_graph.hpp>
#include <string>
#include <undirected_graph.hpp>

int main(int argc, char **argv) {
  ygm::comm world(&argc, &argv);
  {
    tripoll::undirected_graph<uint64_t, std::string, std::string> g(world);

    if (world.rank0()) {
      g.async_set_vertex_metadata(1, "first vertex");
      g.async_set_vertex_metadata(2, "second vertex");
      g.async_set_vertex_metadata(3, "third vertex");
      g.async_set_vertex_metadata(4, "fourth vertex");

      g.async_add_edge(1, 2, "first edge");
      g.async_add_edge(2, 3, "second edge");
      g.async_add_edge(3, 4, "third edge");
      g.async_add_edge(2, 4, "fourth edge");
    }

    auto print_lambda_undirected =
        [](uint64_t id, std::string &v_data,
           std::vector<std::pair<uint64_t, std::string>> &edges, int i) {
          std::cout << "Hello. My vertex ID is " << id
                    << ", and my vertex metadata is " << v_data;
          std::cout << "\nI have the following edges:";
          for (auto &ngbr_edgedata : edges) {
            std::cout << "\n\t" << ngbr_edgedata.first << ": "
                      << ngbr_edgedata.second;
          }
          std::cout << "\nYou also sent me " << i << ". Why did you do that?"
                    << std::endl;
        };

    if (world.rank() == 0) {
      g.async_visit_vertex(2, print_lambda_undirected, 42);
    }

    int fav_num = 1;
    auto degree_lambda = [&fav_num](const auto &vtx_id, const auto &data) {
      std::cout << "Vertex " << vtx_id << " has " << data.second.size()
                << " edges";
      std::cout << "\nAlso, my favorite number is " << fav_num++ << std::endl;
    };

    g.for_all_vertices(degree_lambda);

    tripoll::ordered_directed_graph<uint64_t, std::string, std::string,
                                    uint64_t>
        dg(world);

    make_dodgr(g, dg);
    dg.uniquify_edges();

    auto print_lambda_ordered_async =
        [](const uint64_t v, const std::string &v_data, const uint64_t order,
           const auto &adj_list) {
          std::cout << "Vertex " << v << " has order " << order << " and "
                    << adj_list.size() << " edges:";
          for (auto &edge : adj_list) {
            std::cout << "\n\tNgbr: " << edge.get_ngbr_ID()
                      << "\tOrder: " << edge.get_ngbr_order()
                      << "\tMetadata: " << edge.get_edge_metadata();
          }
          std::cout << std::endl;
        };

    auto print_lambda_ordered_all = [](const auto &vtx_id, const auto &data) {
      auto &v_data = data.get_vertex_data();
      auto &order = data.get_vertex_order();
      auto &adj_list = data.get_adjacency_list();
      std::cout << "Vertex " << vtx_id << " has order " << order << " and "
                << adj_list.size() << " edges:";
      for (auto &edge : adj_list) {
        std::cout << "\n\tNgbr: " << edge.get_ngbr_ID()
                  << "\tOrder: " << edge.get_ngbr_order()
                  << "\tMetadata: " << edge.get_edge_metadata()
                  << "\tNgbr Metadata: " << edge.get_target_metadata();
      }
      std::cout << std::endl;
    };

    dg.for_all_vertices(print_lambda_ordered_all);

    auto tc = tc_no_meta(dg);

    world.cout0("Found ", tc, " triangles");

    // Trying metadata version
    static uint64_t static_tc = 0;

    auto tc_increment_lambda =
        [](const auto &p_metadata, const auto &q_metadata,
           const auto &r_metadata, const auto &pq_metadata,
           const auto &pr_metadata, const auto &qr_metadata) {
          std::cout << "Vertex metadata:\n\t" << p_metadata << "\n\t"
                    << q_metadata << "\n\t" << r_metadata
                    << "\n\tEdge metadata:\n\t" << pq_metadata << "\n\t"
                    << pr_metadata << "\n\t" << qr_metadata << std::endl;
          ++static_tc;
        };

    tc_push_only(dg, tc_increment_lambda);

    world.cout0("Metadata version found ", static_tc, " triangles");
  }

  return 0;
}
