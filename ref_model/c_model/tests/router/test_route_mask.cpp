// Cell-verification of the collective route-mask dual function against
// hand-computed tables (floo_route_xymask.sv:126-164 fork, :200-237 join),
// plus the structural properties both directions must hold on every mesh.
#include "router/route_mask.hpp"
#include "router/router.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <initializer_list>
#include <utility>
#include <vector>

namespace router = ni::cmodel::router;
using router::PortMask;
using router::RouterConfig;
using router::RouterPort;

namespace {

// dst_port_id naming the tile on the router's LOCAL port. Collective route
// masks are a pure coordinate computation, so every route_compute() call in
// this file is a tile destination.
constexpr uint8_t kTilePort = 0;

constexpr uint8_t node_id(uint8_t x, uint8_t y) {
    return static_cast<uint8_t>(x | (y << ni::width::X_WIDTH));
}

RouterConfig cfg_at(uint8_t x, uint8_t y, uint8_t mesh_x_dim, uint8_t mesh_y_dim) {
    RouterConfig c;
    c.x = x;
    c.y = y;
    c.mesh_x_dim = mesh_x_dim;
    c.mesh_y_dim = mesh_y_dim;
    return c;
}

PortMask ports(std::initializer_list<RouterPort> list) {
    PortMask m = 0;
    for (RouterPort p : list) m |= router::port_bit(p);
    return m;
}

RouterPort opposite(RouterPort p) {
    switch (p) {
        case RouterPort::NORTH:
            return RouterPort::SOUTH;
        case RouterPort::SOUTH:
            return RouterPort::NORTH;
        case RouterPort::EAST:
            return RouterPort::WEST;
        default:
            return RouterPort::EAST;  // WEST
    }
}

// Node-id field split, from specgen — same composition node_id() builds and the
// header decodes (route_mask.hpp:60). Never hardcode the field boundary.
constexpr uint8_t kXFieldMask = static_cast<uint8_t>((1u << ni::width::X_WIDTH) - 1);
constexpr uint8_t kYFieldMask = static_cast<uint8_t>((1u << ni::width::Y_WIDTH) - 1);

constexpr uint8_t id_x(uint8_t id) {
    return static_cast<uint8_t>(id & kXFieldMask);
}
constexpr uint8_t id_y(uint8_t id) {
    return static_cast<uint8_t>((id >> ni::width::X_WIDTH) & kYFieldMask);
}

// A node belongs to a masked set when every differing coordinate bit is a
// don't-care. Computed per coordinate with the test's own arithmetic, not the
// header's helper — the test must not check the implementation against itself.
bool is_member(uint8_t id, uint8_t dst_id, uint8_t mask) {
    return ((id_x(id) ^ id_x(dst_id)) & static_cast<uint8_t>(~id_x(mask) & kXFieldMask)) == 0 &&
           ((id_y(id) ^ id_y(dst_id)) & static_cast<uint8_t>(~id_y(mask) & kYFieldMask)) == 0;
}

int popcount8(uint8_t v) {
    int n = 0;
    for (int b = 0; b < 8; ++b) n += (v >> b) & 1;
    return n;
}

struct Mesh {
    uint8_t x_dim;
    uint8_t y_dim;
};

// Non-square meshes are mandatory coverage: the two directions resolve the two
// coordinates in opposite orders, so a square mesh can hide a swapped axis.
const Mesh kMeshes[] = {{2, 2}, {2, 4}, {4, 2}, {4, 4}};

// A mask is legal for a dst_id when the whole masked set lands inside the
// mesh; every member is <= (dst_id | mask) coordinate-wise, so this one test
// covers all 2^n of them.
bool mask_legal(uint8_t addr_x, uint8_t addr_y, uint8_t mask, const Mesh& m) {
    // A bit outside the X|Y fields is not a coordinate don't-care at all.
    if (node_id(id_x(mask), id_y(mask)) != mask) return false;
    return (addr_x | id_x(mask)) < m.x_dim && (addr_y | id_y(mask)) < m.y_dim;
}

struct Cell {
    uint8_t x;
    uint8_t y;
    PortMask expect;
};

}  // namespace

// --- fork, hand-computed tables ---------------------------------------------

// 4x4, offset submesh with a non-contiguous Y mask: dst_id (2,1), mask (1,2)
// => members {(2,1),(3,1),(2,3),(3,3)} (row y=2 is skipped). Source (0,0) is a
// corner OUTSIDE the destination set, so the table also pins the X spread that
// carries the flit into the set's column range.
TEST(RouteMaskFork, Mesh4x4OffsetSubmeshFromCornerSource) {
    const uint8_t dst = node_id(2, 1), src = node_id(0, 0), mask = node_id(1, 2);
    const Cell cells[] = {
        {0, 0, ports({RouterPort::EAST})},                      // source, spreads X
        {1, 0, ports({RouterPort::EAST})},                      // in transit on the source row
        {2, 0, ports({RouterPort::EAST, RouterPort::NORTH})},   // first matched column, turns
        {3, 0, ports({RouterPort::NORTH})},                     // last matched column
        {2, 1, ports({RouterPort::LOCAL, RouterPort::NORTH})},  // member, keeps climbing
        {3, 1, ports({RouterPort::LOCAL, RouterPort::NORTH})},
        {2, 2, ports({RouterPort::NORTH})},  // masked-out row, pass-through
        {3, 2, ports({RouterPort::NORTH})},
        {2, 3, ports({RouterPort::LOCAL})},  // top member, leaf
        {3, 3, ports({RouterPort::LOCAL})},  // corner member, leaf
        {0, 1, 0},                           // off-tree
        {1, 3, 0},
    };
    for (const Cell& c : cells) {
        SCOPED_TRACE(testing::Message() << "router (" << int(c.x) << "," << int(c.y) << ")");
        EXPECT_EQ(router::route_mask_fork(dst, src, mask, cfg_at(c.x, c.y, 4, 4)), c.expect);
    }
}

// 2x2, whole-mesh mask from the (0,0) corner: LOCAL membership at the source
// itself (legal per the standing LOCAL ruling) plus a two-level tree.
TEST(RouteMaskFork, Mesh2x2WholeMeshFromSourceMember) {
    const uint8_t dst = node_id(0, 0), src = node_id(0, 0), mask = node_id(1, 1);
    const Cell cells[] = {
        {0, 0, ports({RouterPort::LOCAL, RouterPort::EAST, RouterPort::NORTH})},
        {1, 0, ports({RouterPort::LOCAL, RouterPort::NORTH})},
        {0, 1, ports({RouterPort::LOCAL})},
        {1, 1, ports({RouterPort::LOCAL})},
    };
    for (const Cell& c : cells) {
        SCOPED_TRACE(testing::Message() << "router (" << int(c.x) << "," << int(c.y) << ")");
        EXPECT_EQ(router::route_mask_fork(dst, src, mask, cfg_at(c.x, c.y, 2, 2)), c.expect);
    }
}

// 2x4 (tall), whole-mesh mask issued from the opposite corner (1,3): the spread
// runs West then South, mirroring the previous table's East/North.
TEST(RouteMaskFork, Mesh2x4WholeMeshFromTopCorner) {
    const uint8_t dst = node_id(0, 0), src = node_id(1, 3), mask = node_id(1, 3);
    const Cell cells[] = {
        {1, 3, ports({RouterPort::LOCAL, RouterPort::WEST, RouterPort::SOUTH})},
        {0, 3, ports({RouterPort::LOCAL, RouterPort::SOUTH})},
        {1, 2, ports({RouterPort::LOCAL, RouterPort::SOUTH})},
        {0, 1, ports({RouterPort::LOCAL, RouterPort::SOUTH})},
        {0, 0, ports({RouterPort::LOCAL})},
        {1, 0, ports({RouterPort::LOCAL})},
    };
    for (const Cell& c : cells) {
        SCOPED_TRACE(testing::Message() << "router (" << int(c.x) << "," << int(c.y) << ")");
        EXPECT_EQ(router::route_mask_fork(dst, src, mask, cfg_at(c.x, c.y, 2, 4)), c.expect);
    }
}

// 4x2 (wide), full-row mask on row 0 issued from row 1: the X spread runs on
// the SOURCE's row, not the destination row, and every matched column turns
// South. No router on row 1 is a member.
TEST(RouteMaskFork, Mesh4x2FullRowFromOtherRow) {
    const uint8_t dst = node_id(0, 0), src = node_id(0, 1), mask = node_id(3, 0);
    const Cell cells[] = {
        {0, 1, ports({RouterPort::EAST, RouterPort::SOUTH})},
        {1, 1, ports({RouterPort::EAST, RouterPort::SOUTH})},
        {2, 1, ports({RouterPort::EAST, RouterPort::SOUTH})},
        {3, 1, ports({RouterPort::SOUTH})},
        {0, 0, ports({RouterPort::LOCAL})},
        {3, 0, ports({RouterPort::LOCAL})},
    };
    for (const Cell& c : cells) {
        SCOPED_TRACE(testing::Message() << "router (" << int(c.x) << "," << int(c.y) << ")");
        EXPECT_EQ(router::route_mask_fork(dst, src, mask, cfg_at(c.x, c.y, 4, 2)), c.expect);
    }
}

// --- join, hand-computed table ----------------------------------------------

// Same member set as the 4x4 fork table (dst_id (2,1), mask (1,2)) collecting
// at (0,0). The shape is NOT the fork tree reversed: replicas merge along X
// inside each member row first, then down the collector's column. (0,1) is the
// only 2-way join; (0,0), (0,2), (1,3), (1,1) are single-input forwards.
TEST(RouteMaskJoin, Mesh4x4OffsetSubmeshToCorner) {
    const uint8_t dst = node_id(0, 0), src = node_id(2, 1), mask = node_id(1, 2);
    const Cell cells[] = {
        {3, 1, ports({RouterPort::LOCAL})},                    // row-1 leaf
        {2, 1, ports({RouterPort::LOCAL, RouterPort::EAST})},  // merges the row-1 pair
        {1, 1, ports({RouterPort::EAST})},                     // forward
        {3, 3, ports({RouterPort::LOCAL})},                    // row-3 leaf
        {2, 3, ports({RouterPort::LOCAL, RouterPort::EAST})},  // merges the row-3 pair
        {1, 3, ports({RouterPort::EAST})},
        {0, 3, ports({RouterPort::EAST})},                     // enters the collector column
        {0, 2, ports({RouterPort::NORTH})},                    // masked-out row, forward
        {0, 1, ports({RouterPort::NORTH, RouterPort::EAST})},  // 2-way join of both rows
        {0, 0, ports({RouterPort::NORTH})},                    // collector, not a member
        {2, 2, 0},                                             // off-tree
        {3, 0, 0},
    };
    for (const Cell& c : cells) {
        SCOPED_TRACE(testing::Message() << "router (" << int(c.x) << "," << int(c.y) << ")");
        EXPECT_EQ(router::route_mask_join(dst, src, mask, cfg_at(c.x, c.y, 4, 4)), c.expect);
    }
}

// --- properties, exhaustive over every mesh / dst_id / peer / legal mask ----

// Membership: n masked bits name exactly 2^n nodes, and the LOCAL bit is set at
// exactly those nodes. Holds identically for both directions — the fork's
// receivers and the join's contributors are the same masked set.
TEST(RouteMaskProperty, LocalBitMarksExactlyThePowerOfTwoMemberSet) {
    for (const Mesh& m : kMeshes) {
        for (uint8_t addr_y = 0; addr_y < m.y_dim; ++addr_y) {
            for (uint8_t addr_x = 0; addr_x < m.x_dim; ++addr_x) {
                for (int mask_i = 0; mask_i < 256; ++mask_i) {
                    const uint8_t mask = static_cast<uint8_t>(mask_i);
                    if (!mask_legal(addr_x, addr_y, mask, m)) continue;
                    const uint8_t dst_id = node_id(addr_x, addr_y);
                    const uint8_t peer = node_id(0, 0);  // source / collector, a corner
                    int fork_members = 0, join_members = 0;
                    for (uint8_t y = 0; y < m.y_dim; ++y) {
                        for (uint8_t x = 0; x < m.x_dim; ++x) {
                            const RouterConfig cfg = cfg_at(x, y, m.x_dim, m.y_dim);
                            const bool member = is_member(node_id(x, y), dst_id, mask);
                            const PortMask fork = router::route_mask_fork(dst_id, peer, mask, cfg);
                            const PortMask join = router::route_mask_join(peer, dst_id, mask, cfg);
                            SCOPED_TRACE(testing::Message()
                                         << "mesh " << int(m.x_dim) << "x" << int(m.y_dim)
                                         << " dst_id " << int(dst_id) << " mask " << int(mask)
                                         << " router (" << int(x) << "," << int(y) << ")");
                            EXPECT_EQ(router::port_in_mask(fork, RouterPort::LOCAL), member);
                            EXPECT_EQ(router::port_in_mask(join, RouterPort::LOCAL), member);
                            fork_members += router::port_in_mask(fork, RouterPort::LOCAL) ? 1 : 0;
                            join_members += router::port_in_mask(join, RouterPort::LOCAL) ? 1 : 0;
                        }
                    }
                    EXPECT_EQ(fork_members, 1 << popcount8(mask));
                    EXPECT_EQ(join_members, 1 << popcount8(mask));
                }
            }
        }
    }
}

// Fork tree: starting at the source, following the port set hop by hop reaches
// every member exactly once, never leaves the mesh, never reconverges, and
// never dead-ends (the set is non-empty at every router the tree reaches).
TEST(RouteMaskProperty, ForkSetSpansATreeOverExactlyTheMembers) {
    for (const Mesh& m : kMeshes) {
        for (uint8_t src_y = 0; src_y < m.y_dim; ++src_y) {
            for (uint8_t src_x = 0; src_x < m.x_dim; ++src_x) {
                for (uint8_t addr_y = 0; addr_y < m.y_dim; ++addr_y) {
                    for (uint8_t addr_x = 0; addr_x < m.x_dim; ++addr_x) {
                        for (int mask_i = 0; mask_i < 256; ++mask_i) {
                            const uint8_t mask = static_cast<uint8_t>(mask_i);
                            if (!mask_legal(addr_x, addr_y, mask, m)) continue;
                            const uint8_t dst_id = node_id(addr_x, addr_y);
                            const uint8_t src = node_id(src_x, src_y);
                            SCOPED_TRACE(testing::Message()
                                         << "mesh " << int(m.x_dim) << "x" << int(m.y_dim)
                                         << " dst " << int(dst_id) << " src " << int(src)
                                         << " mask " << int(mask));
                            std::vector<uint8_t> seen(m.x_dim * m.y_dim, 0);
                            std::vector<std::pair<uint8_t, uint8_t>> pending{{src_x, src_y}};
                            seen[src_y * m.x_dim + src_x] = 1;
                            int delivered = 0;
                            while (!pending.empty()) {
                                const auto [hx, hy] = pending.back();
                                pending.pop_back();
                                const PortMask set = router::route_mask_fork(
                                    dst_id, src, mask, cfg_at(hx, hy, m.x_dim, m.y_dim));
                                ASSERT_NE(set, 0)
                                    << "dead end at (" << int(hx) << "," << int(hy) << ")";
                                if (router::port_in_mask(set, RouterPort::LOCAL)) {
                                    ++delivered;
                                    EXPECT_TRUE(is_member(node_id(hx, hy), dst_id, mask));
                                }
                                const RouterPort dirs[] = {RouterPort::NORTH, RouterPort::EAST,
                                                           RouterPort::SOUTH, RouterPort::WEST};
                                for (RouterPort p : dirs) {
                                    if (!router::port_in_mask(set, p)) continue;
                                    int nx = hx, ny = hy;
                                    if (p == RouterPort::NORTH) ++ny;
                                    if (p == RouterPort::SOUTH) --ny;
                                    if (p == RouterPort::EAST) ++nx;
                                    if (p == RouterPort::WEST) --nx;
                                    ASSERT_TRUE(nx >= 0 && nx < m.x_dim && ny >= 0 && ny < m.y_dim)
                                        << "fork leaves the mesh at (" << int(hx) << "," << int(hy)
                                        << ")";
                                    ASSERT_EQ(seen[ny * m.x_dim + nx], 0)
                                        << "fork reconverges at (" << nx << "," << ny << ")";
                                    seen[ny * m.x_dim + nx] = 1;
                                    pending.push_back(
                                        {static_cast<uint8_t>(nx), static_cast<uint8_t>(ny)});
                                }
                            }
                            EXPECT_EQ(delivered, 1 << popcount8(mask));
                        }
                    }
                }
            }
        }
    }
}

// Degenerate unicast (mask == 0): along the path the flit actually takes, the
// fork set is the one-hot port route_compute already picks today.
TEST(RouteMaskProperty, ZeroMaskFollowsRouteComputeAlongThePath) {
    for (const Mesh& m : kMeshes) {
        for (uint8_t src_y = 0; src_y < m.y_dim; ++src_y) {
            for (uint8_t src_x = 0; src_x < m.x_dim; ++src_x) {
                for (uint8_t dst_y = 0; dst_y < m.y_dim; ++dst_y) {
                    for (uint8_t dst_x = 0; dst_x < m.x_dim; ++dst_x) {
                        const uint8_t dst = node_id(dst_x, dst_y), src = node_id(src_x, src_y);
                        uint8_t hx = src_x, hy = src_y;
                        for (int hop = 0; hop <= m.x_dim + m.y_dim; ++hop) {
                            const RouterConfig cfg = cfg_at(hx, hy, m.x_dim, m.y_dim);
                            const RouterPort unicast = router::route_compute(dst, kTilePort, cfg);
                            SCOPED_TRACE(testing::Message()
                                         << "mesh " << int(m.x_dim) << "x" << int(m.y_dim)
                                         << " src " << int(src) << " dst " << int(dst) << " at ("
                                         << int(hx) << "," << int(hy) << ")");
                            EXPECT_EQ(router::route_mask_fork(dst, src, 0, cfg),
                                      router::port_bit(unicast));
                            if (unicast == RouterPort::LOCAL) break;
                            if (unicast == RouterPort::NORTH) ++hy;
                            if (unicast == RouterPort::SOUTH) --hy;
                            if (unicast == RouterPort::EAST) ++hx;
                            if (unicast == RouterPort::WEST) --hx;
                        }
                    }
                }
            }
        }
    }
}

// Join round trip: the expected-input set at every router equals exactly the
// inputs on which replicas actually arrive when each member sends its response
// as an ordinary XY unicast back to the collector. This — not "the fork tree
// reversed" — is the property the join has to satisfy, because responses are
// routed by route_compute, whose XY order resolves X inside the MEMBER's own
// row while the forward tree spreads X inside the SOURCE's row. The two trees
// coincide only for masks confined to a single axis.
TEST(RouteMaskProperty, JoinSetMatchesTheActualUnicastReturnPaths) {
    for (const Mesh& m : kMeshes) {
        for (uint8_t col_y = 0; col_y < m.y_dim; ++col_y) {
            for (uint8_t col_x = 0; col_x < m.x_dim; ++col_x) {
                for (uint8_t addr_y = 0; addr_y < m.y_dim; ++addr_y) {
                    for (uint8_t addr_x = 0; addr_x < m.x_dim; ++addr_x) {
                        for (int mask_i = 0; mask_i < 256; ++mask_i) {
                            const uint8_t mask = static_cast<uint8_t>(mask_i);
                            if (!mask_legal(addr_x, addr_y, mask, m)) continue;
                            const uint8_t dst_id = node_id(addr_x, addr_y);
                            const uint8_t collector = node_id(col_x, col_y);
                            SCOPED_TRACE(testing::Message()
                                         << "mesh " << int(m.x_dim) << "x" << int(m.y_dim)
                                         << " collector " << int(collector) << " collective dst_id "
                                         << int(dst_id) << " mask " << int(mask));
                            // Replay every member's unicast response and record
                            // the input port it arrives on at each router.
                            std::vector<PortMask> arrivals(m.x_dim * m.y_dim, 0);
                            for (uint8_t y = 0; y < m.y_dim; ++y) {
                                for (uint8_t x = 0; x < m.x_dim; ++x) {
                                    if (!is_member(node_id(x, y), dst_id, mask)) continue;
                                    uint8_t hx = x, hy = y;
                                    RouterPort arrival = RouterPort::LOCAL;
                                    for (int hop = 0; hop <= m.x_dim + m.y_dim; ++hop) {
                                        arrivals[hy * m.x_dim + hx] |= router::port_bit(arrival);
                                        const RouterPort out = router::route_compute(
                                            collector, kTilePort, cfg_at(hx, hy, m.x_dim, m.y_dim));
                                        if (out == RouterPort::LOCAL) break;
                                        if (out == RouterPort::NORTH) ++hy;
                                        if (out == RouterPort::SOUTH) --hy;
                                        if (out == RouterPort::EAST) ++hx;
                                        if (out == RouterPort::WEST) --hx;
                                        arrival = opposite(out);
                                    }
                                }
                            }
                            for (uint8_t y = 0; y < m.y_dim; ++y) {
                                for (uint8_t x = 0; x < m.x_dim; ++x) {
                                    EXPECT_EQ(
                                        router::route_mask_join(collector, dst_id, mask,
                                                                cfg_at(x, y, m.x_dim, m.y_dim)),
                                        arrivals[y * m.x_dim + x])
                                        << "router (" << int(x) << "," << int(y) << ")";
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
