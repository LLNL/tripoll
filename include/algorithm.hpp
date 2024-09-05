// Copyright 2021-2022 Lawrence Livermore National Security, LLC and other
// TriPoll Project Developers. See the top-level COPYRIGHT file for details.
//
// SPDX-License-Identifier: MIT

#pragma once

#include <functional>
#include <ordered_directed_graph.hpp>
#include <undirected_graph.hpp>
#include <ygm/utility.hpp>

namespace tripoll {

// Helper function for progressing through adjacency lists with multi-edges
template <typename ITER>
ITER find_next_iterator(const ITER &iter, const ITER &end) {
  auto next_iter = iter;
  while (*next_iter == *iter) {
    ++next_iter;
    if (next_iter == end) {
      break;
    }
  }
  return next_iter;
}

template <typename VertexID, typename VertexData, typename EdgeData,
          typename OType, class Compare>
size_t dodgr_wedges(ordered_directed_graph<VertexID, VertexData, EdgeData,
                                           OType, Compare> &dg) {
  size_t local_wedges{0};

  auto local_wedges_lambda = [&local_wedges](const auto &vertex_ID,
                                             auto &metadata_edges) {
    const auto degree = metadata_edges.get_adjacency_list().size();
    local_wedges += degree * (degree - 1) / 2;
  };
  dg.for_all_vertices(local_wedges_lambda);

  return dg.comm().all_reduce_sum(local_wedges);
}

template <typename VertexID, typename VertexData, typename EdgeData,
          typename OType, class Compare>
void make_dodgr_remove_top_k(
    undirected_graph<VertexID, VertexData, EdgeData> &g,
    ordered_directed_graph<VertexID, VertexData, EdgeData, OType, Compare> &dg,
    size_t k) {
  // Determine top degree vertices
  if (k > 0) {
    dg.comm().cout0("Determining vertices to remove from graph");
  }

  ygm::container::map<VertexID, OType> degree_map(dg.comm());

  auto fill_map_lambda = [&degree_map](const auto &vertex_ID,
                                       auto &metadata_edges) {
    degree_map.async_insert(vertex_ID, metadata_edges.second.size());
  };

  g.for_all_vertices(fill_map_lambda);

  auto top_vertex_degrees =
      degree_map.topk(k, [](const auto &kv1, const auto &kv2) {
        return kv1.second > kv2.second;
      });

  std::vector<VertexID> top_vertex_ids(top_vertex_degrees.size());
  for (const auto &vtx_deg : top_vertex_degrees) {
    top_vertex_ids.push_back(vtx_deg.first);
  }
  std::sort(top_vertex_ids.begin(), top_vertex_ids.end());

  // Set order for directed graph as vertex degree
  auto set_order_lambda = [&dg, &top_vertex_ids](const auto &vertex_ID,
                                                 auto &metadata_edges) {
    // Don't set order if vertex needs removing
    if (std::binary_search(top_vertex_ids.begin(), top_vertex_ids.end(),
                           vertex_ID)) {
      return;
    }
    OType degree = metadata_edges.second.size();
    dg.async_set_vertex_order(vertex_ID, degree);
  };

  g.for_all_vertices(set_order_lambda);

  // Copy vertex metadata
  auto copy_vertex_data_lambda = [&dg, &top_vertex_ids](const auto &vertex_ID,
                                                        auto &metadata_edges) {
    if (std::binary_search(top_vertex_ids.begin(), top_vertex_ids.end(),
                           vertex_ID)) {
      return;
    }
    const VertexData &v_data = metadata_edges.first;
    dg.async_set_vertex_metadata(vertex_ID, v_data);
  };

  g.for_all_vertices(copy_vertex_data_lambda);

  // Add ordered edges to DODGR
  dg.comm().cout0("Adding directed edges from graph to DODGR");

  auto add_edges_lambda = [&dg, &top_vertex_ids](const auto &vtx1,
                                                 auto &metadata_edges) {
    if (std::binary_search(top_vertex_ids.begin(), top_vertex_ids.end(),
                           vtx1)) {
      return;
    }
    auto &adj_list = metadata_edges.second;
    for (auto &edge : adj_list) {
      const VertexID &vtx2 = edge.first;
      const EdgeData &e_data = edge.second;

      if (std::binary_search(top_vertex_ids.begin(), top_vertex_ids.end(),
                             vtx2)) {
        continue;
      }
      // Only add edges once from undirected graph (and avoid self-edges)
      if (vtx1 > vtx2) {
        dg.async_add_ordered_edge(vtx1, vtx2, e_data);
      }
    }
    // Remove edges from original graph as we go
    adj_list.clear();
  };

  g.for_all_vertices(add_edges_lambda);

  // Sort edges by degree
  dg.sort_directed_edges();

  dg.barrier();
}

template <typename VertexID, typename VertexData, typename EdgeData,
          typename OType, class Compare>
void make_dodgr(undirected_graph<VertexID, VertexData, EdgeData> &g,
                ordered_directed_graph<VertexID, VertexData, EdgeData, OType,
                                       Compare> &dg) {
  make_dodgr_remove_top_k(g, dg, 0);
}

template <typename VertexID, typename VertexData, typename EdgeData,
          typename OType, class Compare>
uint64_t tc_no_meta(ordered_directed_graph<VertexID, VertexData, EdgeData,
                                           OType, Compare> &dg) {
  static uint64_t tc{0};

  static auto compare_adj_lists_lambda = [](const auto &short_list,
                                            const auto &long_list) {
    uint64_t to_return{0};
    auto short_lower = short_list.begin();
    auto short_end = short_list.end();
    auto long_lower = long_list.begin();
    auto long_end = long_list.end();
    while (short_lower != short_end) {
      int num_short_elements{0};
      auto curr_short = short_lower;
      while (*short_lower == *curr_short) {
        num_short_elements++;
        short_lower++;
      }

      long_lower =
          std::lower_bound(long_lower, long_end, *short_lower,
                           std::greater<typename std::remove_reference<
                               decltype(long_list)>::type::value_type>());

      if (*long_lower == *curr_short) {
        int num_long_elements{0};
        auto curr_long = long_lower;
        while (*long_lower == *curr_long) {
          num_long_elements++;
          long_lower++;
        }

        to_return += num_short_elements * num_short_elements;
      }
    }
    return to_return;
  };

  auto q_triangle_count_lambda = [](const auto &vtx_id, const auto &vtx_data,
                                    const auto &vtx_order,
                                    const auto &q_adj_list, auto &p_adj_list) {
    typename std::remove_reference<decltype(q_adj_list)>::type *short_adj_list,
        *long_adj_list;
    if (q_adj_list.size() < p_adj_list.size()) {
      short_adj_list = &q_adj_list;
      long_adj_list = &p_adj_list;
    } else {
      short_adj_list = &p_adj_list;
      long_adj_list = &q_adj_list;
    }
    tc += compare_adj_lists_lambda(*short_adj_list, *long_adj_list);
  };

  auto start_triangles_lambda =
      [&dg, &q_triangle_count_lambda](const auto &vtx_id, auto &data) {
        auto &v_data = data.get_vertex_data();
        auto &order = data.get_vertex_order();
        auto &adj_list = data.get_adjacency_list();

        // Copy of adjacency list for truncating and sending to q
        typename std::remove_reference<decltype(adj_list)>::type tmp_adj_list(
            adj_list.begin(), adj_list.end());

        auto edge_iter = adj_list.rbegin();
        auto edge_end = adj_list.rend();
        while (edge_iter != edge_end) {
          tmp_adj_list.pop_back();
          if (tmp_adj_list.size() == 0)
            break;

          auto &ngbr_id = edge_iter->get_ngbr_ID();
          dg.async_visit_vertex(ngbr_id, q_triangle_count_lambda, tmp_adj_list);
          edge_iter++;
        }
      };

  dg.for_all_vertices(start_triangles_lambda);

  dg.barrier();

  return dg.comm().all_reduce_sum(tc);
}

template <typename VertexID, typename VertexData, typename EdgeData,
          typename OType, class Compare, typename Callback,
          typename... CallbackArgs>
void tc_push_only(
    ordered_directed_graph<VertexID, VertexData, EdgeData, OType, Compare> &dg,
    Callback c, const CallbackArgs &...args) {

  dg.comm().cout0("Push-only");

  static size_t num_edges_sent{0};

  static auto callback_tuple =
      std::make_tuple(std::forward<const CallbackArgs>(args)...);

  // This lambda performs the merge-path adjacency list intersection when both
  // adjacency lists have been moved to the same compute MPI rank
  static auto linear_search_adj_lists_lambda =
      [](const auto &p_adj_list, const auto &q_adj_list, const auto &p_metadata,
         const auto &q_metadata, const auto &pq_metadata) {
        auto p_lower = p_adj_list.begin();
        auto p_end = p_adj_list.end();
        auto q_lower = q_adj_list.begin();
        auto q_end = q_adj_list.end();

        Callback *c;
        while (p_lower != p_end && q_lower != q_end) {
          if (*p_lower == *q_lower) {
            auto p_upper = find_next_iterator(p_lower, p_end);
            auto q_upper = find_next_iterator(q_lower, q_end);

            // Apply callback on all combinations of matching edges
            const auto &r_metadata = q_lower->get_target_metadata();
            for (auto curr_p = p_lower; curr_p != p_upper; ++curr_p) {
              const auto &p_edge_metadata = curr_p->get_edge_metadata();
              for (auto curr_q = q_lower; curr_q != q_upper; ++curr_q) {
                const auto q_edge_metadata = curr_q->get_edge_metadata();
                std::apply(
                    *c, std::tuple_cat(std::make_tuple(p_metadata, q_metadata,
                                                       r_metadata, pq_metadata,
                                                       p_edge_metadata,
                                                       q_edge_metadata),
                                       callback_tuple));
              }
            }
            p_lower = p_upper;
            q_lower = q_upper;
          } else {
            if (*p_lower > *q_lower) {
              p_lower = find_next_iterator(p_lower, p_end);
            } else {
              q_lower = find_next_iterator(q_lower, q_end);
            }
          }
        }
      };

  // This is the lambda executed on the remote rank after an adjacency list has
  // been sent. It does nothing more than call the merge-path intersection
  // lambda on the two adjacency lists
  auto q_triangle_count_lambda =
      [](const auto &vtx_id, const auto &vtx_data, const auto &vtx_order,
         const auto &q_adj_list, auto &p_adj_list, const auto &p_metadata,
         const auto &pq_metadata_vec) {
        linear_search_adj_lists_lambda(p_adj_list, q_adj_list, p_metadata,
                                       vtx_data, pq_metadata_vec);
      };

  // Begins triangle counting by iterating over all vertices, identifying wedges
  // and sending adjacency lists as it goes
  auto start_triangles_lambda = [&dg, &q_triangle_count_lambda](
                                    const auto &vtx_id, auto &data) {
    const auto &v_data = data.get_vertex_data();
    const auto &order = data.get_vertex_order();
    const auto &adj_list = data.get_adjacency_list();

    // Copy of adjacency list for truncating and sending to q
    typename std::remove_const_t<std::remove_reference_t<decltype(adj_list)>>
        tmp_adj_list(adj_list.begin(), adj_list.end());

    auto edge_iter = adj_list.rbegin();
    auto edge_end = adj_list.rend();
    while (edge_iter != edge_end) {
      tmp_adj_list.pop_back();
      if (tmp_adj_list.size() == 0)
        break;

      num_edges_sent += tmp_adj_list.size();

      auto &ngbr_id = edge_iter->get_ngbr_ID();
      auto &edge_data = edge_iter->get_edge_metadata();
      dg.async_visit_vertex(ngbr_id, q_triangle_count_lambda, tmp_adj_list,
                            v_data, edge_data);
      edge_iter++;
    }
  };

  dg.for_all_vertices(start_triangles_lambda);

  dg.barrier();

  dg.comm().cout0("Sent a total of ", dg.comm().all_reduce_sum(num_edges_sent),
                  " edges");
}

template <typename VertexID, typename VertexData, typename EdgeData,
          typename OType, class Compare, typename Callback,
          typename... CallbackArgs>
void tc_push_pull(
    ordered_directed_graph<VertexID, VertexData, EdgeData, OType, Compare> &dg,
    Callback c, const CallbackArgs &...args) {
  using dodgr_type =
      ordered_directed_graph<VertexID, VertexData, EdgeData, OType, Compare>;
  using dodgr_vertex_type = typename dodgr_type::full_vertex_type;
  using dodgr_edge_type = typename dodgr_type::edge_info_type;

  dg.comm().cout0("Push-pull");

  static size_t num_edges_sent{0};

  static auto callback_tuple =
      std::make_tuple(std::forward<const CallbackArgs>(args)...);

  // Should not need these pointers...
  auto comm_ptr = dg.comm().make_ygm_ptr(dg.comm());
  auto dg_ptr = dg.comm().make_ygm_ptr(dg);

  // Determine communication pattern for rank
  dg.comm().cout0("Determining vertices to pull vs push");
  ygm::timer tc_timer{};

  static std::unordered_map<
      VertexID,
      std::pair<size_t, std::vector<std::tuple<const dodgr_vertex_type *,
                                               const dodgr_edge_type *>>>>
      to_pull_from_map;
  to_pull_from_map.clear();
  static std::unordered_map<VertexID, std::vector<int>> pulled_from_ranks;
  pulled_from_ranks.clear();

  auto count_adjacency_list_lengths = [&dg](const auto &vtx_id,
                                            const auto &data) {
    const auto &adj_list = data.get_adjacency_list();

    auto ptr = data.get_pointer();

    int curr_adj_length{0};
    auto adj_list_end = adj_list.end();
    for (auto iter = adj_list.begin(); iter != adj_list_end;) {
      auto begin_iter = iter;
      auto &ngbr_id = iter->get_ngbr_ID();

      while (ngbr_id == iter->get_ngbr_ID()) {
        ++iter;
        ++curr_adj_length;
        if (iter == adj_list_end) {
          break;
        }
      }

      if (dg.owner(ngbr_id) != dg.comm().rank()) {
        to_pull_from_map[ngbr_id].first += curr_adj_length;
        to_pull_from_map[ngbr_id].second.push_back(
            std::make_tuple(ptr, &*begin_iter));
      }
    }
  };

  dg.for_all_vertices(count_adjacency_list_lengths);

  // Figure out which vertices to pull and which to push
  static std::vector<VertexID> to_clear_pull_info;
  to_clear_pull_info.clear();

  auto push_vs_pull_lambda = [](const auto &vtx_id, const auto &vtx_data,
                                const auto &vtx_order, const auto &adj_list,
                                const auto to_receive_length_sum,
                                const int from, auto comm_ptr) {
    auto no_pull_response = [](const auto &remote_vtx_id) {
      to_clear_pull_info.push_back(remote_vtx_id);
    };

    const auto q_adj_list_length = adj_list.size();
    if (q_adj_list_length < to_receive_length_sum) {
      pulled_from_ranks[vtx_id].push_back(from);
    } else {
      comm_ptr->async(from, no_pull_response, vtx_id);
    }
  };

  int pull_threshold = 1;
  const auto my_rank = dg.comm().rank();
  for (const auto &to_pull_vtx : to_pull_from_map) {
    if (to_pull_vtx.second.first > pull_threshold) {
      dg.async_visit_vertex(to_pull_vtx.first, push_vs_pull_lambda,
                            to_pull_vtx.second.first, my_rank, comm_ptr);
    } else {
      to_clear_pull_info.push_back(to_pull_vtx.first);
    }
  }

  dg.barrier();

  for (const auto &remote_vtx_id : to_clear_pull_info) {
    to_pull_from_map.erase(remote_vtx_id);
  }

  dg.comm().cout0("Push vs Pull determination time: ", tc_timer.elapsed(),
                  " seconds");
  tc_timer.reset();
  dg.comm().cout0("Pulling ", dg.comm().all_reduce_sum(to_pull_from_map.size()),
                  " vertices globally");

  // Triangle enumeration

  static auto linear_search_adj_lists_lambda =
      [](const auto &p_adj_list, const auto &q_adj_list, const auto &p_metadata,
         const auto &q_metadata, const auto &pq_metadata_vec) {
        auto p_lower = p_adj_list.begin();
        auto p_end = p_adj_list.end();
        auto q_lower = q_adj_list.begin();
        auto q_end = q_adj_list.end();

        Callback *c;
        while (p_lower != p_end && q_lower != q_end) {
          if (*p_lower == *q_lower) {
            auto p_upper = find_next_iterator(p_lower, p_end);
            auto q_upper = find_next_iterator(q_lower, q_end);

            // Apply callback on all combinations of matching edges
            const auto &r_metadata = q_lower->get_target_metadata();
            for (auto curr_p = p_lower; curr_p != p_upper; ++curr_p) {
              const auto &p_edge_metadata = curr_p->get_edge_metadata();
              for (auto curr_q = q_lower; curr_q != q_upper; ++curr_q) {
                const auto q_edge_metadata = curr_q->get_edge_metadata();
                for (const auto &pq_metadata : pq_metadata_vec) {
                  std::apply(
                      *c, std::tuple_cat(
                              std::make_tuple(p_metadata, q_metadata,
                                              r_metadata, pq_metadata,
                                              p_edge_metadata, q_edge_metadata),
                              callback_tuple));
                }
              }
            }
            p_lower = p_upper;
            q_lower = q_upper;
          } else {
            if (*p_lower > *q_lower) {
              p_lower = find_next_iterator(p_lower, p_end);
            } else {
              q_lower = find_next_iterator(q_lower, q_end);
            }
          }
        }
      };

  // Begin push phase
  auto q_triangle_count_lambda =
      [](const auto &vtx_id, const auto &vtx_data, const auto &vtx_order,
         const auto &q_adj_list, auto &p_adj_list, const auto &p_metadata,
         const auto &pq_metadata_vec) {
        linear_search_adj_lists_lambda(p_adj_list, q_adj_list, p_metadata,
                                       vtx_data, pq_metadata_vec);
      };

  auto start_triangles_lambda = [&dg, &q_triangle_count_lambda](
                                    const auto &vtx_id, const auto &data) {
    const auto &v_data = data.get_vertex_data();
    const auto &order = data.get_vertex_order();
    const auto &adj_list = data.get_adjacency_list();

    // Copy of adjacency list for truncating and sending to q
    typename std::remove_const_t<std::remove_reference_t<decltype(adj_list)>>
        tmp_adj_list(adj_list.begin(), adj_list.end());

    auto edge_iter = adj_list.rbegin();
    auto edge_end = adj_list.rend();
    while (edge_iter != edge_end) {
      if (tmp_adj_list.size() == 0)
        break;

      ASSERT_RELEASE(edge_iter != edge_end);
      auto &ngbr_id = edge_iter->get_ngbr_ID();

      std::vector<EdgeData> edge_metadata_vec;
      while (ngbr_id == edge_iter->get_ngbr_ID()) {
        edge_metadata_vec.push_back(edge_iter->get_edge_metadata());
        tmp_adj_list.pop_back();
        ++edge_iter;
        if (edge_iter == edge_end) {
          break;
        }
      }

      // Only push if pulling doesn't make sense
      if (to_pull_from_map.find(ngbr_id) == to_pull_from_map.end()) {
        num_edges_sent += tmp_adj_list.size();

        dg.async_visit_vertex(ngbr_id, q_triangle_count_lambda, tmp_adj_list,
                              v_data, edge_metadata_vec);
      }
    }
  };

  dg.for_all_vertices(start_triangles_lambda);

  dg.barrier();
  dg.comm().cout0("Push phase time: ", tc_timer.elapsed(), " seconds");
  tc_timer.reset();

  // End push phase

  // Begin pull phase

  // Lambda to find elements in each adjacency list
  static auto linear_search_adj_lists_pull_lambda =
      [](const auto &p_adj_list, const auto &q_adj_list, const auto &p_metadata,
         const auto &q_metadata, const auto pq_ptr) {
        auto p_lower = p_adj_list.begin();
        auto p_end = p_adj_list.end();
        auto q_lower = q_adj_list.begin();
        auto q_end = q_adj_list.end();

        Callback *c;
        while (p_lower != p_end && q_lower != q_end) {
          if (*p_lower == *q_lower) {
            auto p_upper = find_next_iterator(p_lower, p_end);
            auto q_upper = find_next_iterator(q_lower, q_end);

            // Apply callback on all combinations of matching edges
            const auto &r_metadata = p_lower->get_target_metadata();
            for (auto curr_p = p_lower; curr_p != p_upper; ++curr_p) {
              const auto &p_edge_metadata = curr_p->get_edge_metadata();
              for (auto curr_q = q_lower; curr_q != q_upper; ++curr_q) {
                const auto q_edge_metadata = curr_q->get_edge_metadata();
                std::apply(
                    *c, std::tuple_cat(
                            std::make_tuple(p_metadata, q_metadata, r_metadata,
                                            pq_ptr->get_edge_metadata(),
                                            p_edge_metadata, q_edge_metadata),
                            callback_tuple));
              }
            }
            p_lower = p_upper;
            q_lower = q_upper;
          } else {
            if (*p_lower > *q_lower) {
              p_lower = find_next_iterator(p_lower, p_end);
            } else {
              q_lower = find_next_iterator(q_lower, q_end);
            }
          }
        }
      };

  // Receive q's adjacency list and use it with my p's
  static auto pulled_adjacency_list_lambda =
      [](const auto &q_vtx_id, const auto &q_metadata, const auto &q_order,
         const auto &q_adj_list, auto dg_ptr) {
        const auto &p_vtx_ptr_vec = to_pull_from_map[q_vtx_id].second;
        auto q_size = q_adj_list.size();
        for (const auto p_vtx_ptr : p_vtx_ptr_vec) {
          const auto &p = *std::get<0>(p_vtx_ptr);
          linear_search_adj_lists_pull_lambda(
              p.get_adjacency_list(), q_adj_list, p.get_vertex_data(),
              q_metadata, std::get<1>(p_vtx_ptr));
        }
      };

  // Using an extra async_visit because local_get doesn't appear to be working
  // as intended
  // Sends q to ranks requesting it
  auto send_pulled_vertex_lambda =
      [](const auto &q_vtx_id, const auto &q_metadata, const auto &q_order,
         const auto &q_adj_list, const auto &rank_vec, auto dg_ptr) {
#ifdef TRIPOLL_USE_MCAST
        dg_ptr->comm().async_mcast(rank_vec, pulled_adjacency_list_lambda,
                                   q_vtx_id, q_metadata, q_order, q_adj_list,
                                   dg_ptr);
#else
        for (const auto rank : rank_vec) {
          num_edges_sent += q_adj_list.size();
          dg_ptr->comm().async(rank, pulled_adjacency_list_lambda, q_vtx_id,
                               q_metadata, q_order, q_adj_list, dg_ptr);
        }
#endif
      };

  for (const auto &q_pulled_from_vec : pulled_from_ranks) {
    const auto &q_vtx_id = q_pulled_from_vec.first;
    dg_ptr->async_visit_vertex(q_vtx_id, send_pulled_vertex_lambda,
                               q_pulled_from_vec.second, dg_ptr);
  }

  dg.barrier();

  dg.comm().cout0("Pull phase time: ", tc_timer.elapsed(), " seconds");

  dg.comm().cout0("Sent a total of ", dg.comm().all_reduce_sum(num_edges_sent),
                  " edges");
}
} // namespace tripoll
