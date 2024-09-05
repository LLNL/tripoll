// Copyright 2021-2022 Lawrence Livermore National Security, LLC and other
// TriPoll Project Developers. See the top-level COPYRIGHT file for details.
//
// SPDX-License-Identifier: MIT

#pragma once

#include <tuple>
#include <vector>
#include <ygm/container/map.hpp>

namespace tripoll {
template <typename VertexID, typename VertexData, typename EdgeData>
class undirected_graph {
public:
  using metadata_adjacency_list_type =
      std::vector<std::pair<VertexID, EdgeData>>;
  using full_vertex_type = std::pair<VertexData, metadata_adjacency_list_type>;

  undirected_graph(ygm::comm &comm) : m_vertex_map(comm){};

  void async_add_edge(const VertexID &vtx1, const VertexID &vtx2,
                      const EdgeData &edge_data) {
    auto add_edge_lambda = [](const VertexID &vtx, full_vertex_type &src_data,
                              const VertexID &dest, const EdgeData &edge_data) {
      src_data.second.push_back(std::make_pair(dest, edge_data));
    };

    m_vertex_map.async_visit(vtx1, add_edge_lambda, vtx2, edge_data);
    m_vertex_map.async_visit(vtx2, add_edge_lambda, vtx1, edge_data);
  }

  void async_set_vertex_metadata(const VertexID &vtx,
                                 const VertexData &vtx_data) {
    auto set_vertex_metadata_lambda =
        [](const VertexID &vtx, full_vertex_type &src_data,
           const VertexData &vertex_data) { src_data.first = vertex_data; };

    m_vertex_map.async_visit(vtx, set_vertex_metadata_lambda, vtx_data);
  }

  template <typename Visitor, typename... VisitorArgs>
  void async_visit_vertex(const VertexID &vtx, Visitor visitor,
                          const VisitorArgs &...args) {
    auto visit_wrapper_lambda = [](const VertexID &vtx,
                                   full_vertex_type &src_data,
                                   const VisitorArgs &...args) {
      const VertexID &vertex_id = vtx;
      VertexData &vertex_data = src_data.first;
      metadata_adjacency_list_type &edges = src_data.second;

      Visitor *v;
      (*v)(vertex_id, vertex_data, edges, args...);
    };
    m_vertex_map.async_visit(vtx, visit_wrapper_lambda, args...);
  }

  template <typename Function> void for_all_vertices(Function fn) {
    m_vertex_map.for_all(fn);
  }

  void sort_edges() {
    auto sort_edges_lambda = [](const auto &vertex_ID, auto &metadata_edges) {
      auto &edges = metadata_edges.second;
      std::sort(edges.begin(), edges.end());
    };
    for_all_vertices(sort_edges_lambda);
  }

  void uniquify_edges() {
    sort_edges();
    auto ignore_edge_metadata_comparison = [](auto &edge1, auto &edge2) {
      return edge1.first == edge2.first;
    };

    auto uniquify_edges_lambda =
        [&ignore_edge_metadata_comparison](const auto &vertex_ID,
                                           auto &metadata_edges) {
          auto &edges = metadata_edges.second;
          edges.erase(std::unique(edges.begin(), edges.end(),
                                  ignore_edge_metadata_comparison),
                      edges.end());
        };
    for_all_vertices(uniquify_edges_lambda);
  }

  void barrier() { m_vertex_map.comm().barrier(); }

  void clear() { m_vertex_map.clear(); }

  uint64_t num_vertices() { return m_vertex_map.size(); }

  uint64_t num_edges() {
    uint64_t local_count{0};
    auto count_local_lambda = [&local_count](const auto &vertex_ID,
                                             auto &metadata_edges) {
      local_count += metadata_edges.second.size();
    };
    for_all_vertices(count_local_lambda);

    return m_vertex_map.comm().all_reduce_sum(local_count) / 2;
  }

  uint64_t max_degree() {
    uint64_t local_max{0};
    auto local_max_lambda = [&local_max](const auto &vertex_ID,
                                         auto &metadata_edges) {
      local_max = std::max(local_max, metadata_edges.second.size());
    };
    for_all_vertices(local_max_lambda);

    return m_vertex_map.comm().all_reduce_max(local_max);
  }

private:
  ygm::container::map<VertexID, full_vertex_type> m_vertex_map;
};
} // namespace tripoll
