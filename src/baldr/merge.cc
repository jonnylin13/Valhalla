#include "valhalla/baldr/merge.h"
#include "valhalla/baldr/graphreader.h"
#include "valhalla/midgard/logging.h"

#include <boost/range/adaptor/map.hpp>
#include <boost/optional.hpp>

namespace bra = boost::adaptors;

namespace valhalla {
namespace baldr {
namespace merge {

namespace {

//a place we can mark what edges we've seen, even for the planet we should need < 100mb
struct bitset_t {
  bitset_t(size_t size) {bits.resize(size);}
  void set(const uint64_t id) {
    if (id >= static_cast<uint64_t>(bits.size()) * 64) throw std::runtime_error("id out of bounds");
    bits[id / 64] |= static_cast<uint64_t>(1) << (id % static_cast<uint64_t>(64));
  }
  bool get(const uint64_t id) const {
    if (id >= static_cast<uint64_t>(bits.size()) * 64) throw std::runtime_error("id out of bounds");
    return bits[id / 64] & (static_cast<uint64_t>(1) << (id % static_cast<uint64_t>(64)));
  }
protected:
  std::vector<uint64_t> bits;
};

struct edge_tracker {
  explicit edge_tracker(GraphReader &reader);

  bool get(const GraphId &edge_id) const;
  void set(const GraphId &edge_id);

  typedef std::unordered_map<GraphId, uint64_t> edge_index_t;
  edge_index_t m_edges_in_tiles;
  //this is how we know what i've touched and what we havent
  bitset_t m_edge_set;

  static bitset_t count_all_edges(GraphReader &reader, const edge_index_t &edges);
  static edge_index_t edges_in_tiles(GraphReader &reader);
};

edge_tracker::edge_tracker(GraphReader &reader)
  : m_edges_in_tiles(edges_in_tiles(reader))
  , m_edge_set(count_all_edges(reader, m_edges_in_tiles)) {
}

bool edge_tracker::get(const GraphId &edge_id) const {
  auto itr = m_edges_in_tiles.find(edge_id.Tile_Base());
  assert(itr != m_edges_in_tiles.end());
  return m_edge_set.get(edge_id.id() + itr->second);
}

void edge_tracker::set(const GraphId &edge_id) {
  auto itr = m_edges_in_tiles.find(edge_id.Tile_Base());
  assert(itr != m_edges_in_tiles.end());
  m_edge_set.set(edge_id.id() + itr->second);
}

uint64_t count_tiles_in_levels(GraphReader &reader) {
  uint64_t tile_count = 0;
  for (auto level : reader.GetTileHierarchy().levels() | bra::map_values) {
    tile_count += level.tiles.ncolumns() * level.tiles.nrows();
  }
  return tile_count;
}

edge_tracker::edge_index_t edge_tracker::edges_in_tiles(GraphReader &reader) {
  uint64_t tiles_in_levels = count_tiles_in_levels(reader);

  //keep the global number of edges encountered at the point we encounter each tile
  //this allows an edge to have a sequential global id and makes storing it very small
  LOG_INFO("Enumerating edges...");
  uint64_t edge_count = 0;
  edge_tracker::edge_index_t edges_in_tiles(tiles_in_levels);
  for (auto level : reader.GetTileHierarchy().levels() | bra::map_values) {
    for (uint32_t i = 0; i < level.tiles.TileCount(); ++i) {
      GraphId tile_id{i, level.level, 0};
      if (reader.DoesTileExist(tile_id)) {
        //TODO: just read the header, parsing the whole thing isnt worth it at this point
        edges_in_tiles.emplace(tile_id, edge_count);
        const auto* tile = reader.GetGraphTile(tile_id);
        edge_count += tile->header()->directededgecount();
        // this clears the cache, and the test relies on being able to inject
        // stuff into the cache :-(
        //reader.Clear();
      }
    }
  }

  return std::move(edges_in_tiles);
}

bitset_t edge_tracker::count_all_edges(GraphReader &reader, const edge_tracker::edge_index_t &edges) {
  GraphId max_tile_id;
  uint64_t edge_count = 0;
  for (const auto &entry : edges) {
    if (entry.second >= edge_count) {
      edge_count = entry.second;
      max_tile_id = entry.first;
    }
  }

  const auto* tile = reader.GetGraphTile(max_tile_id);
  edge_count += tile->header()->directededgecount();
  LOG_INFO("Number of edges: " + std::to_string(edge_count));
  return bitset_t(edge_count);
}

namespace iter {

struct edges {
  struct const_iterator {
    const DirectedEdge *ptr;
    GraphId id;

    const_iterator() : ptr(nullptr), id(0) {}
    const_iterator(const DirectedEdge *p, GraphId i) : ptr(p), id(i) {}

    const_iterator operator+(uint64_t i) const {
      return const_iterator(ptr + i, id + i);
    }

    const_iterator &operator++() {
      ++ptr;
      id++;
      return *this;
    }

    const_iterator operator++(int) {
      const_iterator ret = *this;
      ++(*this);
      return ret;
    }

    std::pair<const DirectedEdge *const, GraphId> operator*() const {
      return std::pair<const DirectedEdge *const, GraphId>(ptr, GraphId(id));
    }

    bool operator!=(const const_iterator &other) const {
      return (ptr != other.ptr) || (id != other.id);
    }
  };

  edges(GraphReader &reader, GraphId node_id) {
    auto *tile = reader.GetGraphTile(node_id);
    auto *node_info = tile->node(node_id);

    auto edge_idx = node_info->edge_index();
    m_begin = const_iterator(tile->directededge(edge_idx),
                             node_id.Tile_Base() + uint64_t(edge_idx));
    m_end = m_begin + node_info->edge_count();
  }

  const_iterator begin() const { return m_begin; }
  const_iterator end()   const { return m_end; }

private:
  const_iterator m_begin, m_end;
};

} // namespace iter

struct edge_collapser {
  edge_collapser(GraphReader &reader, edge_tracker &tracker, std::function<void(const path &)> func)
    : m_reader(reader)
    , m_tracker(tracker)
    , m_func(func)
    {}

  // returns the pair of nodes reachable from the given @node_id where they
  // are the only two nodes reachable by non-shortcut edges, and none of the
  // edges of @node_id cross into a different level.
  boost::optional<std::pair<GraphId, GraphId> > nodes_reachable_from(GraphId node_id) {
    boost::optional<GraphId> first, second;

    for (const auto &edge : iter::edges(m_reader, node_id)) {
      // nodes which connect to ferries, transit or to a different level
      // shouldn't be collapsed.
      if (edge.first->use() == Use::kFerry ||
          edge.first->use() == Use::kTransitConnection ||
          edge.first->trans_up() || edge.first->trans_down()) {
        return boost::none;
      }

      // shortcut edges should be ignored
      if (edge.first->shortcut()) {
        continue;
      }

      if (first) {
        if (second) {
          // can't add a third, that means this node is a true junction.
          return boost::none;

        } else {
          second = edge.first->endnode();
        }
      } else {
        first = edge.first->endnode();
      }
    }

    if (first && second) {
      return std::make_pair(*first, *second);
    } else {
      return boost::none;
    }
  }

  boost::optional<GraphId> next_node_id(GraphId last_node_id, GraphId node_id) {
    //
    //        -->--     -->--
    //   \   /  e4 \   /  e1 \   /
    //   -(p)       (c)       (n)-
    //   /   \ e3  /   \ e2  /   \
    //        --<--     --<--
    //
    // given p (last_node_id) and c (node_id), return n if there is such a node.
    auto nodes = nodes_reachable_from(node_id);
    if (!nodes) {
      return boost::none;
    }
    assert(nodes->first == last_node_id || nodes->second == last_node_id);
    if (nodes->first == last_node_id) {
      return nodes->second;
    } else {
      return nodes->first;
    }
  }

  GraphId edge_between(GraphId cur, GraphId next) {
    boost::optional<GraphId> edge_id;
    for (const auto &edge : iter::edges(m_reader, cur)) {
      if (edge.first->endnode() == next) {
        edge_id = edge.second;
        break;
      }
    }
    assert(bool(edge_id));
    return *edge_id;
  }

  void explore(GraphId node_id) {
    if (m_seen_nodes.count(node_id) != 0) {
      return;
    }
    m_seen_nodes.insert(node_id);

    auto nodes = nodes_reachable_from(node_id);
    if (!nodes) {
      return;
    }

    path forward(node_id), reverse(node_id);

    explore(node_id, nodes->first,  forward, reverse);
    explore(node_id, nodes->second, reverse, forward);

    m_func(forward);
    m_func(reverse);
  }

  void explore(GraphId prev, GraphId cur, path &forward, path &reverse) {
    const auto original_node_id = prev;
    m_seen_nodes.insert(cur);

    boost::optional<GraphId> maybe_next;
    do {
      auto e1 = edge_between(prev, cur);
      forward.push_back(segment(prev, e1, cur));
      m_tracker.set(e1);
      auto e2 = edge_between(cur, prev);
      reverse.push_front(segment(cur, e2, prev));
      m_tracker.set(e2);

      maybe_next = next_node_id(prev, cur);
      if (maybe_next) {
        prev = cur;
        cur = *maybe_next;
        m_seen_nodes.insert(cur);
        if (cur == original_node_id) {
          // circular!
          break;
        }
      }
    } while (maybe_next);
  }

private:
  GraphReader &m_reader;
  edge_tracker &m_tracker;
  std::function<void(const path &)> m_func;
  std::unordered_set<GraphId> m_seen_nodes;
};

path make_single_edge_path(GraphReader &reader, GraphId edge_id) {
  auto *edge = reader.GetGraphTile(edge_id)->directededge(edge_id);
  auto node_id = edge->endnode();
  auto opp_edge_idx = edge->opp_index();
  auto edge_idx = reader.GetGraphTile(node_id)->node(node_id)->edge_index() + opp_edge_idx;
  GraphId opp_edge_id(node_id.tileid(), node_id.level(), edge_idx);
  auto *opp_edge = reader.GetGraphTile(opp_edge_id)->directededge(opp_edge_id);
  auto start_node_id = opp_edge->endnode();

  path p(segment(start_node_id, edge_id, node_id));
  return p;
}

bool check_access(GraphReader &reader, const std::deque<GraphId> &merged) {
  uint32_t access = kAllAccess;
  for (auto edge_id : merged) {
    auto edge = reader.GetGraphTile(edge_id)->directededge(edge_id);
    access &= edge->forwardaccess();
  }
  // be permissive here, as we do want to collect traffoc on most vehicular
  // routes.
  uint32_t vehicular = kAutoAccess | kTruckAccess |
    kTaxiAccess | kBusAccess | kHOVAccess;
  return access & vehicular;
}


} // anonymous namespace

segment::segment(GraphId start, GraphId edge, GraphId end)
  : m_start(start)
  , m_edge(edge)
  , m_end(end)
{}

path::path(segment s)
  : m_start(s.start())
  , m_end(s.end()) {
  m_edges.push_back(s.edge());
}

path::path(GraphId node_id)
  : m_start(node_id)
  , m_end(node_id) {
}

void path::push_back(segment s) {
  assert(s.start() == m_end);
  m_end = s.end();
  m_edges.push_back(s.edge());
}

void path::push_front(segment s) {
  assert(s.end() == m_start);
  m_start = s.start();
  m_edges.push_front(s.edge());
}

void merge(GraphReader &reader, std::function<void(const path &)> func) {
  auto check_func = [&](const path &p) {
    if (check_access(reader, p.m_edges)) {
      func(p);
    }
  };

  edge_tracker tracker(reader);
  edge_collapser e(reader, tracker, check_func);

  for (auto level : reader.GetTileHierarchy().levels() | bra::map_values) {
    for (uint32_t i = 0; i < level.tiles.TileCount(); ++i) {
      GraphId tile_id{i, level.level, 0};
      if (reader.DoesTileExist(tile_id)) {
        const auto *tile = reader.GetGraphTile(tile_id);
        uint32_t node_count = tile->header()->nodecount();
        for (uint32_t i = 0; i < node_count; ++i) {
          GraphId node_id(tile_id.tileid(), tile_id.level(), i);
          e.explore(node_id);
        }
      }
    }
  }

  for (auto level : reader.GetTileHierarchy().levels() | bra::map_values) {
    for (uint32_t i = 0; i < level.tiles.TileCount(); ++i) {
      GraphId tile_id{i, level.level, 0};
      if (reader.DoesTileExist(tile_id)) {
        const auto *tile = reader.GetGraphTile(tile_id);
        const auto num_edges = tile->header()->directededgecount();
        for (uint32_t i = 0; i < num_edges; ++i) {
          GraphId edge_id(tile_id.tileid(), tile_id.level(), i);
          if (!tracker.get(edge_id)) {
            auto p = make_single_edge_path(reader, edge_id);
            check_func(p);
          }
        }
      }
    }
  }
}

}
}
}
