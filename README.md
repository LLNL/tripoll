This repository is a companion to our TriPoll [paper](https://arxiv.org/abs/2107.12330) presented as [
SuperComputing '21](https://dl.acm.org/doi/10.1145/3458817.3476200). This work identifies triangles
in massive graphs with metadata using distributed memory and processes the metadata on triangles in a user-defined manner as they are identified.

# Building
TriPoll requires MPI, CMake, and gcc-8 or later. Communication is handled using [YGM](https://github.com/LLNL/ygm),
which uses MPI internally. CMake is configured to download YGM and its dependencies automatically through FetchContent.

Compiling the code is as simple as
```
mkdir build
cd build
cmake ..
make
```

# Using TriPoll

TriPoll makes use of a small number of data structures and functions that we will briefly cover here.

### Undirected Graph
The undirected graph is a temporary data structure to load data into. Edges are inserted by calling `async_add_edge`,
and vertex metadata is added by calling `async_set_vertex_metadata`.

### Ordered Directed Graph
The `ordered_directed_graph` is the data structure TriPoll algorithms are run on. It creates directed graphs from
undirected graphs by assigning an "order" to each vertex. As edges are added, the "order" of each vertex is looked up,
and a directed edges is added from the lower order vertex to the higher order.

This can be used to assign any ordering on vertices, but it is only used to create degree-ordered directed graphs in
this work, in which edges are pointed from low-degree vertices to high-degree vertices. This construction is facilitated
by the `make_dodgr` function, after all vertices and edges have been read into an `undirected_graph` and the
`ordered_directed_graph` has been initialized.

### Triangle Processing
TriPoll works by identifying triangles in a graph, and executing a user-defined C++ lambda on each triangle's metadata
as they are identified. The signature for these lambdas must be of the form
```
my_lambda(v1_meta, v2_meta, v3_meta, e1,2_meta, e1,3_meta, e2,3_meta, additional_args...).
```
The lambdas expect all of the vertex metadata to be passed, followed by all of the edge metadata, followed finally by
any additional arguments the user wishes to pass to accomplish the desired processing. The additional arguments passed
often take the form of a distributed container to organize and aggregate the output. The order that vertex and edge
metadata is presented to the lambda is not stable and should not be relied upon.

The triangle processing pipeline is run by calls to either `tc_push_only` or `tc_push_pull`. The output of each method
is identical, but the two differ in the internal algorithms used to identify triangles, as described in our paper. Both
functions take an `ordered_directed_graph` that has been constructed, a C++ lambda to execute on each triangle, and any
additional arguments needed by this lambda besides the vertex and edge metadata described above.

# License
TriPoll is distributed under the MIT license.

All new contributions must be made under the MIT license.

See [LICENSE-MIT](LICENSE-MIT), [NOTICE](NOTICE), and [COPYRIGHT](COPYRIGHT) for
details.

SPDX-License-Identifier: MIT

# Release
LLNL-CODE-836649
