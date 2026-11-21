/**
 * @file ldn_node_mapper_tests.cpp
 * @brief Unit tests for the real LdnNodeMapper (ldn/ldn_node_mapper.cpp)
 *
 * Built against the host Stratosphere stand-in in tests/stubs.
 */

#include <cstdio>
#include <cstdint>

#include <cstring>

#include "test_harness.hpp"
#include "ldn/ldn_node_mapper.hpp"

using ams::mitm::ldn::LdnNodeMapper;
using ams::mitm::ldn::NetworkInfo;

static NetworkInfo make_info(u8 count) {
    NetworkInfo info;
    std::memset(&info, 0, sizeof(info));
    info.ldn.nodeCount = count;
    return info;
}

TEST(a_new_mapper_is_empty_and_has_no_local_node) {
    LdnNodeMapper m;
    ASSERT_EQ(m.GetConnectedCount(), static_cast<size_t>(0));
    ASSERT_EQ(m.GetLocalNodeId(), static_cast<u8>(0xFF));
    for (u32 i = 0; i < LdnNodeMapper::MaxNodes; ++i) {
        ASSERT_FALSE(m.IsNodeConnected(i));
        ASSERT_EQ(m.GetNodeIp(i), 0u);
    }
}

TEST(add_node_connects_it_and_stores_the_address) {
    LdnNodeMapper m;
    m.AddNode(3, 0x0A720004);
    ASSERT_TRUE(m.IsNodeConnected(3));
    ASSERT_EQ(m.GetNodeIp(3), 0x0A720004u);
    ASSERT_EQ(m.GetConnectedCount(), static_cast<size_t>(1));
}

TEST(add_node_updates_an_existing_entry) {
    LdnNodeMapper m;
    m.AddNode(1, 1);
    m.AddNode(1, 2);
    ASSERT_EQ(m.GetNodeIp(1), 2u);
    ASSERT_EQ(m.GetConnectedCount(), static_cast<size_t>(1));
}

TEST(out_of_range_node_ids_are_ignored_everywhere) {
    LdnNodeMapper m;
    m.AddNode(LdnNodeMapper::MaxNodes, 5);
    m.RemoveNode(LdnNodeMapper::MaxNodes);
    ASSERT_EQ(m.GetConnectedCount(), static_cast<size_t>(0));
    ASSERT_FALSE(m.IsNodeConnected(LdnNodeMapper::MaxNodes));
    ASSERT_EQ(m.GetNodeIp(LdnNodeMapper::MaxNodes), 0u);
    ASSERT_FALSE(m.ShouldRouteToNode(LdnNodeMapper::BroadcastNodeId, 0, LdnNodeMapper::MaxNodes));
}

TEST(remove_node_disconnects_but_keeps_the_address) {
    LdnNodeMapper m;
    m.AddNode(2, 0x42);
    m.RemoveNode(2);
    ASSERT_FALSE(m.IsNodeConnected(2));
    ASSERT_EQ(m.GetNodeIp(2), 0x42u);
    ASSERT_EQ(m.GetConnectedCount(), static_cast<size_t>(0));
}

TEST(clear_resets_nodes_and_the_local_node_id) {
    LdnNodeMapper m;
    m.AddNode(0, 1);
    m.AddNode(7, 2);
    m.SetLocalNodeId(4);
    ASSERT_EQ(m.GetLocalNodeId(), static_cast<u8>(4));
    m.Clear();
    ASSERT_EQ(m.GetConnectedCount(), static_cast<size_t>(0));
    ASSERT_EQ(m.GetNodeIp(7), 0u);
    ASSERT_EQ(m.GetLocalNodeId(), static_cast<u8>(0xFF));
}

TEST(update_from_network_info_replaces_the_previous_content) {
    LdnNodeMapper m;
    m.AddNode(5, 0x55);

    NetworkInfo info = make_info(2);
    info.ldn.nodes[0].nodeId = 0;
    info.ldn.nodes[0].isConnected = 1;
    info.ldn.nodes[0].ipv4Address = 0x0A720001;
    info.ldn.nodes[1].nodeId = 1;
    info.ldn.nodes[1].isConnected = 1;
    info.ldn.nodes[1].ipv4Address = 0x0A720002;
    m.UpdateFromNetworkInfo(info);

    ASSERT_EQ(m.GetConnectedCount(), static_cast<size_t>(2));
    ASSERT_EQ(m.GetNodeIp(0), 0x0A720001u);
    ASSERT_EQ(m.GetNodeIp(1), 0x0A720002u);
    ASSERT_FALSE(m.IsNodeConnected(5));
    ASSERT_EQ(m.GetNodeIp(5), 0u);
}

TEST(update_from_network_info_skips_disconnected_nodes_and_uses_node_id_as_index) {
    LdnNodeMapper m;
    NetworkInfo info = make_info(2);
    info.ldn.nodes[0].nodeId = 6;
    info.ldn.nodes[0].isConnected = 1;
    info.ldn.nodes[0].ipv4Address = 0x66;
    info.ldn.nodes[1].nodeId = 2;
    info.ldn.nodes[1].isConnected = 0;
    info.ldn.nodes[1].ipv4Address = 0x22;
    m.UpdateFromNetworkInfo(info);
    ASSERT_TRUE(m.IsNodeConnected(6));
    ASSERT_EQ(m.GetNodeIp(6), 0x66u);
    ASSERT_FALSE(m.IsNodeConnected(2));
}

TEST(update_from_network_info_only_reads_node_count_entries) {
    LdnNodeMapper m;
    NetworkInfo info = make_info(1);
    info.ldn.nodes[0].nodeId = 0;
    info.ldn.nodes[0].isConnected = 1;
    info.ldn.nodes[1].nodeId = 1;
    info.ldn.nodes[1].isConnected = 1;
    m.UpdateFromNetworkInfo(info);
    ASSERT_EQ(m.GetConnectedCount(), static_cast<size_t>(1));
}

TEST(update_from_network_info_never_reads_more_than_max_nodes_entries) {
    LdnNodeMapper m;
    NetworkInfo info = make_info(200);
    for (int i = 0; i < ams::mitm::ldn::NodeCountMax; ++i) {
        info.ldn.nodes[i].nodeId = static_cast<s8>(i);
        info.ldn.nodes[i].isConnected = 1;
    }
    m.UpdateFromNetworkInfo(info);
    ASSERT_EQ(m.GetConnectedCount(), static_cast<size_t>(LdnNodeMapper::MaxNodes));
}

TEST(update_from_network_info_rejects_node_ids_outside_the_table) {
    LdnNodeMapper m;
    NetworkInfo info = make_info(3);
    info.ldn.nodes[0].nodeId = -1;
    info.ldn.nodes[0].isConnected = 1;
    info.ldn.nodes[1].nodeId = 8;
    info.ldn.nodes[1].isConnected = 1;
    info.ldn.nodes[2].nodeId = 127;
    info.ldn.nodes[2].isConnected = 1;
    m.UpdateFromNetworkInfo(info);
    ASSERT_EQ(m.GetConnectedCount(), static_cast<size_t>(0));
}

TEST(routing_requires_the_target_to_be_connected) {
    LdnNodeMapper m;
    ASSERT_FALSE(m.ShouldRouteToNode(LdnNodeMapper::BroadcastNodeId, 0, 1));
    ASSERT_FALSE(m.ShouldRouteToNode(1, 0, 1));
}

TEST(broadcast_reaches_every_connected_node_except_the_source) {
    LdnNodeMapper m;
    m.AddNode(0, 1);
    m.AddNode(1, 2);
    ASSERT_FALSE(m.ShouldRouteToNode(LdnNodeMapper::BroadcastNodeId, 0, 0));
    ASSERT_TRUE(m.ShouldRouteToNode(LdnNodeMapper::BroadcastNodeId, 0, 1));
}

TEST(unicast_reaches_only_the_destination) {
    LdnNodeMapper m;
    m.AddNode(0, 1);
    m.AddNode(1, 2);
    ASSERT_TRUE(m.ShouldRouteToNode(1, 0, 1));
    ASSERT_FALSE(m.ShouldRouteToNode(1, 0, 0));
}

int main() {
    return test_report("LdnNodeMapper Tests");
}
