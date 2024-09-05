// Copyright 2021-2022 Lawrence Livermore National Security, LLC and other
// TriPoll Project Developers. See the top-level COPYRIGHT file for details.
//
// SPDX-License-Identifier: MIT

#pragma once

#include <tuple>
#include <vector>
#include <ygm/container/map.hpp>

namespace tripoll {
// This version stores all vertex metadata of target's along edges
template <typename VertexID, typename VertexData, typename EdgeData,
          typename OType, class Compare = std::less<OType>>
class ordered_directed_graph {
public:
  class edge_info_type;
  class full_vertex_type;

  using order_type = OType;
  using metadata_adjacency_list_type = std::vector<edge_info_type>;

  ordered_directed_graph(ygm::comm &comm) : m_vertex_map(comm){};

  void async_set_vertex_order(const VertexID &vtx, const order_type &order) {
    auto set_order_lambda = [](const VertexID &vtx, full_vertex_type &vtx_data,
                               const order_type &order) {
      vtx_data.set_vertex_order(order);
    };

    m_vertex_map.async_visit(vtx, set_order_lambda, order);
  }

  void async_add_ordered_edge(const VertexID &vtx1, const VertexID &vtx2,
                              const EdgeData &edge_data) {
    auto outer_forward_edge_info_lambda =
        [](const VertexID &vtx, full_vertex_type &vtx_data,
           const VertexID &ngbr_vtx, const EdgeData &edge_data, auto map_ptr) {
          const order_type &my_order = vtx_data.get_vertex_order();
          const VertexData &my_vtx_metadata = vtx_data.get_vertex_data();
          auto outer_send_order_lambda =
              [](const VertexID &vtx, full_vertex_type &vtx_data,
                 const order_type &order, const VertexData &vtx_metadata,
                 const VertexID &ngbr_vtx, const EdgeData &edge_data,
                 auto map_ptr) {
                const order_type &my_order = vtx_data.get_vertex_order();
                const VertexData &my_vtx_metadata = vtx_data.get_vertex_data();
                // Insert vertex if I have lower order than neighbor
                if (my_order < order || (my_order == order && vtx < ngbr_vtx)) {
                  vtx_data.adjacency_list_push_back(
                      edge_info_type(ngbr_vtx, order, edge_data, vtx_metadata));
                } else { // Send message back to insert at original vertex
                  auto return_lambda = [](const VertexID &vtx,
                                          full_vertex_type &vtx_data,
                                          const order_type &order,
                                          const VertexData &vtx_metadata,
                                          const VertexID &ngbr_vtx,
                                          const EdgeData &edge_data) {
                    const order_type &my_order = vtx_data.get_vertex_order();
                    // if (my_order < order ||
                    //(my_order == order && vtx_data.first < ngbr_vtx)) {
                    vtx_data.adjacency_list_push_back(edge_info_type(
                        ngbr_vtx, order, edge_data, vtx_metadata));
                    //} else {
                    // std::cout << "Not inserting self-loop" <<
                    // std::endl;
                    //}
                  };
                  map_ptr->async_visit(ngbr_vtx, return_lambda, my_order,
                                       my_vtx_metadata, vtx, edge_data);
                }
              };
          map_ptr->async_visit(ngbr_vtx, outer_send_order_lambda, my_order,
                               my_vtx_metadata, vtx, edge_data, map_ptr);
        };

    // Don't send self-loops
    if (vtx1 != vtx2) {
      m_vertex_map.async_visit(vtx1, outer_forward_edge_info_lambda, vtx2,
                               edge_data, m_vertex_map.get_ygm_ptr());
    }
  }

  void async_set_vertex_metadata(const VertexID &vtx,
                                 const VertexData &vtx_data) {
    auto set_vertex_metadata_lambda = [](const VertexID &vtx,
                                         full_vertex_type &vtx_data,
                                         const VertexData &vertex_data) {
      vtx_data.set_vertex_data(vertex_data);
    };
    m_vertex_map.async_visit(vtx, set_vertex_metadata_lambda, vtx_data);
  }

  template <typename Visitor, typename... VisitorArgs>
  void async_visit_vertex(const VertexID &vtx, Visitor visitor,
                          const VisitorArgs &...args) {
    auto visit_wrapper_lambda = [](const VertexID &vtx,
                                   full_vertex_type &vtx_data,
                                   const VisitorArgs &...args) {
      const VertexID &vertex_id = vtx;
      auto data = vtx_data;
      const VertexData &vertex_data = data.get_vertex_data();
      const order_type &vertex_order = data.get_vertex_order();
      const metadata_adjacency_list_type &adjacency_list =
          data.get_adjacency_list();

      Visitor *v;
      (*v)(vertex_id, vertex_data, vertex_order, adjacency_list, args...);
    };
    m_vertex_map.async_visit(vtx, visit_wrapper_lambda, args...);
  }

  template <typename Function> void for_all_vertices(Function fn) {
    m_vertex_map.for_all(fn);
  }

  void sort_directed_edges() {
    auto sort_edges_lambda = [](const auto &vertex_ID, auto &metadata_edges) {
      // auto &edges = metadata_edges.second;
      metadata_adjacency_list_type &adj_list =
          metadata_edges.get_adjacency_list();
      std::sort(adj_list.begin(), adj_list.end(),
                std::greater<edge_info_type>());
    };
    for_all_vertices(sort_edges_lambda);
  }

  void uniquify_edges() {
    sort_directed_edges();
    auto uniquify_edges_lambda = [](const auto &vertex_ID,
                                    auto &metadata_edges) {
      // auto &edges = metadata_edges.second;
      auto &adj_list = metadata_edges.get_adjacency_list();
      adj_list.erase(std::unique(adj_list.begin(), adj_list.end()),
                     adj_list.end());
    };
    for_all_vertices(uniquify_edges_lambda);
  }

  const std::vector<full_vertex_type> local_get_vertex(const VertexID &vtx) {
    return m_vertex_map.local_get(vtx);
  }

  void barrier() { m_vertex_map.comm().barrier(); }

  ygm::comm &comm() { return m_vertex_map.comm(); }

  int owner(const VertexID &vtx) const { return m_vertex_map.owner(vtx); }

  const auto find_local_edges(const VertexID &u, const VertexID &v,
                              const order_type &v_order) {
    const auto adj_list = local_get_vertex(u).front().get_adjacency_list();
    return std::equal_range(adj_list.begin(), adj_list.end(),
                            edge_info_type(v, v_order));
  }

  std::vector<EdgeData> find_edge_metadata_vec(const VertexID &u,
                                               const VertexID &v,
                                               const order_type &v_order) {
    std::vector<EdgeData> to_return;
    auto [edges_begin, edges_end] = find_local_edges(u, v, v_order);
    std::for_each(edges_begin, edges_end, [&to_return](const auto &edge) {
      to_return.push_back(edge.get_edge_metadata());
    });

    return to_return;
  }

  uint64_t num_vertices() { return m_vertex_map.size(); }

  uint64_t num_edges() {
    uint64_t local_count{0};
    auto count_local_lambda = [&local_count](const auto &vertex_ID,
                                             auto &metadata_edges) {
      local_count += metadata_edges.get_adjacency_list().size();
    };
    for_all_vertices(count_local_lambda);

    return m_vertex_map.comm().all_reduce_sum(local_count);
  }

  uint64_t max_degree() {
    uint64_t local_max{0};
    auto max_degree_lambda = [&local_max](const auto &vertex_ID,
                                          auto &metadata_edges) {
      local_max =
          std::max(local_max, metadata_edges.get_adjacency_list().size());
    };
    for_all_vertices(max_degree_lambda);

    return comm().all_reduce_max(local_max);
  }

  class full_vertex_type {
  public:
    full_vertex_type() : m_vtx_data(VertexData{}), m_order(order_type{}){};

    full_vertex_type(const VertexData &vtx_data, const order_type &order)
        : m_vtx_data(vtx_data), m_order(order){};

    const VertexData &get_vertex_data() const { return m_vtx_data; }

    const order_type &get_vertex_order() const { return m_order; }

    const metadata_adjacency_list_type &get_adjacency_list() const {
      return m_adj_list;
    }

    metadata_adjacency_list_type &get_adjacency_list() { return m_adj_list; }

    void set_vertex_data(const VertexData &d) { m_vtx_data = d; }

    void set_vertex_order(const order_type &o) { m_order = o; }

    void adjacency_list_push_back(const edge_info_type &e) {
      m_adj_list.push_back(e);
    }

    void adjacency_list_push_back(edge_info_type &&e) {
      m_adj_list.push_back(e);
    }

    const full_vertex_type *const get_pointer() const { return this; }

    friend bool operator==(const full_vertex_type &lhs,
                           const full_vertex_type &rhs) {
      return lhs.m_order == rhs.m_order;
    }

    friend bool operator!=(const full_vertex_type &lhs,
                           const full_vertex_type &rhs) {
      return !(lhs == rhs);
    }

    friend bool operator<(const full_vertex_type &lhs,
                          const full_vertex_type &rhs) {
      return lhs.m_order < rhs.m_order;
    }

    friend bool operator<=(const full_vertex_type &lhs,
                           const full_vertex_type &rhs) {
      return (lhs < rhs || lhs == rhs);
    }

    friend bool operator>(const full_vertex_type &lhs,
                          const full_vertex_type &rhs) {
      return !(lhs <= rhs);
    }

    friend bool operator>=(const full_vertex_type &lhs,
                           const full_vertex_type &rhs) {
      return !(lhs < rhs);
    }

  private:
    VertexData m_vtx_data;
    order_type m_order;
    metadata_adjacency_list_type m_adj_list;
  };

  class edge_info_type {
  public:
    edge_info_type(){};

    edge_info_type(const VertexID &vtx_id, const order_type &order)
        : edge_info_type(vtx_id, order, EdgeData(), VertexData()){};

    edge_info_type(const VertexID &vtx_id, const order_type &order,
                   const EdgeData &metadata, const VertexData &target_metadata)
        : m_vtx_id(vtx_id), m_order(order), m_metadata(metadata),
          m_target_metadata(target_metadata){};

    const VertexID &get_ngbr_ID() const { return m_vtx_id; }

    const order_type &get_ngbr_order() const { return m_order; }

    const EdgeData &get_edge_metadata() const { return m_metadata; }

    const VertexData &get_target_metadata() const { return m_target_metadata; }

    friend bool operator==(const edge_info_type &lhs,
                           const edge_info_type &rhs) {
      return lhs.m_vtx_id == rhs.m_vtx_id;
    }

    friend bool operator!=(const edge_info_type &lhs,
                           const edge_info_type &rhs) {
      return !(lhs == rhs);
    }

    friend bool operator<(const edge_info_type &lhs,
                          const edge_info_type &rhs) {
      if (lhs.m_order == rhs.m_order) {
        return lhs.m_vtx_id < rhs.m_vtx_id;
      } else {
        return lhs.m_order < rhs.m_order;
      }
    }

    friend bool operator<=(const edge_info_type &lhs,
                           const edge_info_type &rhs) {
      return (lhs < rhs || lhs == rhs);
    }

    friend bool operator>(const edge_info_type &lhs,
                          const edge_info_type &rhs) {
      return !(lhs <= rhs);
    }

    friend bool operator>=(const edge_info_type &lhs,
                           const edge_info_type &rhs) {
      return !(lhs < rhs);
    }

    template <typename Archive> void serialize(Archive &ar) {
      ar(m_vtx_id, m_order, m_metadata);
    }

  private:
    VertexID m_vtx_id;
    order_type m_order;
    EdgeData m_metadata;
    VertexData m_target_metadata;
  };

private:
  ygm::container::map<VertexID, full_vertex_type> m_vertex_map;
};
} // namespace tripoll
