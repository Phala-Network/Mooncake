#include "memory_location.h"

#include <glog/logging.h>
#include <gtest/gtest.h>
#include <numa.h>
#include <numaif.h>
#include <sys/mman.h>

#include <limits>

namespace {

void checkRegistrationPlanLabel(uint64_t start, size_t length, size_t limit,
                               const std::string &label) {
    mooncake::SegmentsLocationInfo original;
    ASSERT_TRUE(mooncake::parseSegmentsLocation(label, original));
    const auto chunks =
        mooncake::planMemoryRegistration(start, length, label, limit);
    ASSERT_FALSE(chunks.empty());
    size_t covered = 0;
    for (const auto &chunk : chunks) {
        ASSERT_EQ(chunk.start, start + covered);
        ASSERT_GT(chunk.len, 0u);
        if (limit > 0) ASSERT_LE(chunk.len, limit);
        // The old consumer already accepts cpu:N. Compare against the old
        // resolver using ORIGINAL coordinates, including every synthetic byte.
        for (size_t offset = 0; offset < chunk.len; ++offset)
            ASSERT_EQ(chunk.location,
                      mooncake::resolveSegmentsLocation(original, length,
                                                        covered + offset));
        covered += chunk.len;
    }
    ASSERT_EQ(covered, length);
}

void checkRegistrationPlan(uint64_t start, size_t length, size_t limit,
                           const std::vector<int> &nodes) {
    checkRegistrationPlanLabel(
        start, length, limit, mooncake::buildSegmentsLocation(4096, nodes));
}

}  // namespace

TEST(MemoryRegistrationTest, Original64KiBReproduction) {
    const auto chunks = mooncake::planMemoryRegistration(
        0x100000, 65536, "segments:4096:0,1,2,3", 16384);
    ASSERT_EQ(chunks.size(), 4u);
    for (size_t i = 0; i < chunks.size(); ++i) {
        EXPECT_EQ(chunks[i].start, 0x100000u + i * 16384);
        EXPECT_EQ(chunks[i].len, 16384u);
        EXPECT_EQ(chunks[i].location, "cpu:" + std::to_string(i));
    }
    checkRegistrationPlan(0x100000, 65536, 16384, {0, 1, 2, 3});
}

TEST(MemoryRegistrationTest, SplitAtRegionEvenWithoutMrLimit) {
    const auto chunks = mooncake::planMemoryRegistration(
        0x100000, 65536, "segments:4096:0,1,2,3", 0);
    ASSERT_EQ(chunks.size(), 4u);
    checkRegistrationPlan(0x100000, 65536, 0, {0, 1, 2, 3});
}

TEST(MemoryRegistrationTest, MrLimitCrossesRegionBoundary) {
    const auto chunks = mooncake::planMemoryRegistration(
        0x100000, 65536, "segments:4096:0,1,2,3", 24576);
    ASSERT_EQ(chunks.size(), 4u);
    for (const auto &chunk : chunks) EXPECT_EQ(chunk.len, 16384u);
    checkRegistrationPlan(0x100000, 65536, 24576, {0, 1, 2, 3});
}

TEST(MemoryRegistrationTest, MrLimitDoesNotDivideRegion) {
    const auto chunks = mooncake::planMemoryRegistration(
        0x100000, 65536, "segments:4096:0,1,2,3", 12288);
    ASSERT_EQ(chunks.size(), 8u);
    for (size_t i = 0; i < chunks.size(); ++i)
        EXPECT_EQ(chunks[i].len, i % 2 == 0 ? 12288u : 4096u);
    checkRegistrationPlan(0x100000, 65536, 12288, {0, 1, 2, 3});
}

TEST(MemoryRegistrationTest, TailUsesOriginalFloorAndClamp) {
    const auto chunks = mooncake::planMemoryRegistration(
        0x100003, 73, "segments:65536:1,3,5,7", 0);
    ASSERT_EQ(chunks.size(), 4u);
    EXPECT_EQ(chunks[0].len, 18u);
    EXPECT_EQ(chunks[1].len, 18u);
    EXPECT_EQ(chunks[2].len, 18u);
    EXPECT_EQ(chunks[3].len, 19u);
    checkRegistrationPlan(0x100003, 73, 7, {1, 3, 5, 7});
}

TEST(MemoryRegistrationTest, CurrentOnePointFiveTiBHas96Mrs) {
    constexpr size_t gib = 1ull << 30;
    const auto chunks = mooncake::planMemoryRegistration(
        0x100000, 1536 * gib, "segments:4096:0,1,2,3", 16 * gib);
    ASSERT_EQ(chunks.size(), 96u);
    for (size_t i = 0; i < chunks.size(); ++i) {
        EXPECT_EQ(chunks[i].start, 0x100000u + i * 16 * gib);
        EXPECT_EQ(chunks[i].len, 16 * gib);
        EXPECT_EQ(chunks[i].location, "cpu:" + std::to_string(i / 24));
    }
}

TEST(MemoryRegistrationTest, PreviousOnePointSevenFiveTiBHas112Mrs) {
    constexpr size_t gib = 1ull << 30;
    const auto chunks = mooncake::planMemoryRegistration(
        0x100000, 1792 * gib, "segments:4096:0,1,2,3", 16 * gib);
    ASSERT_EQ(chunks.size(), 112u);
    for (size_t i = 0; i < chunks.size(); ++i)
        EXPECT_EQ(chunks[i].location, "cpu:" + std::to_string(i / 28));
}

TEST(MemoryRegistrationTest, DirectAndWildcardNamesArePreserved) {
    for (const auto &name : {"cpu:3", "cuda:1", "*", "custom-location"}) {
        const auto chunks =
            mooncake::planMemoryRegistration(0x100000, 33, name, 10);
        ASSERT_EQ(chunks.size(), 4u);
        EXPECT_EQ(chunks.back().len, 3u);
        for (const auto &chunk : chunks) EXPECT_EQ(chunk.location, name);
    }
}

TEST(MemoryRegistrationTest, RejectInvalidSegmentsBeforeRegistration) {
    for (const auto &name : {"segments:", "segments:4096:",
                             "segments:0:0,1", "segments:4096:-1,2",
                             "segments:4096:not-a-node",
                             "segments:18446744073709551616:0,1",
                             "segments:4096:0,2147483648"})
        EXPECT_TRUE(mooncake::planMemoryRegistration(0x100000, 64, name, 16)
                        .empty());
    EXPECT_TRUE(mooncake::planMemoryRegistration(
                    0x100000, 2, "segments:4096:0,1,2", 16)
                    .empty());
}

TEST(MemoryRegistrationTest, LegacyParserTolerantLabelsKeepOriginalCoordinates) {
    // Preserve the old parser's tolerated numeric prefixes/empty tokens. The
    // fix concerns registration coordinates, not tightening the wire grammar.
    for (const auto &name : {"segments:4096:0,,2", "segments:4096:1x,3",
                             "segments:-1:0,1", "segments:4096:,0,1",
                             "segments:4096:0,1,", "segments:4096x:0,1",
                             "segments: 4096:0,1", "segments:4096:0,+1",
                             "segments:4096:0, 1", "segments:0001:03,03"})
        for (size_t limit : {0u, 7u, 16u, 128u})
            checkRegistrationPlanLabel(0x100003, 73, limit, name);
}

TEST(MemoryRegistrationTest, RejectEmptyOrOverflowingAddressRange) {
    EXPECT_TRUE(
        mooncake::planMemoryRegistration(0x100000, 0, "cpu:0", 16).empty());
    EXPECT_TRUE(mooncake::planMemoryRegistration(
                    std::numeric_limits<uint64_t>::max() - 3, 4, "cpu:0", 16)
                    .empty());
}

TEST(MemoryRegistrationTest, ExhaustiveSmallBuffersAgreeWithOldResolver) {
    for (size_t count = 1; count <= 5; ++count) {
        std::vector<int> nodes;
        for (size_t i = 0; i < count; ++i) nodes.push_back(2 * i + 1);
        for (size_t length = count; length < 128; ++length)
            for (size_t limit : {0u, 1u, 3u, 7u, 128u})
                checkRegistrationPlan(0x100003, length, limit, nodes);
    }
}

TEST(MemoryLocationTest, MallocSimpleNode0) {
    int size = 4096 * 10;
    void *addr = numa_alloc_onnode(size, 0);
    bool only_first_page = false;
    ASSERT_NE(addr, nullptr);

    auto entries = mooncake::getMemoryLocation(addr, size, only_first_page);
    ASSERT_EQ(entries.size(), static_cast<size_t>(1));

    // check the memory location, no node before page fault
    EXPECT_EQ(entries[0].start, reinterpret_cast<uint64_t>(addr));
    EXPECT_EQ(entries[0].location, mooncake::kWildcardLocation);
    EXPECT_EQ(entries[0].len, static_cast<size_t>(size));

    // trigger page fault
    memset(addr, 0, size);

    entries = mooncake::getMemoryLocation(addr, size, only_first_page);
    ASSERT_EQ(entries.size(), static_cast<size_t>(1));

    // check the memory location, node 0 after page fault
    EXPECT_EQ(entries[0].start, reinterpret_cast<uint64_t>(addr));
    EXPECT_EQ(entries[0].location, "cpu:0");
    EXPECT_EQ(entries[0].len, static_cast<size_t>(size));

    numa_free(addr, size);
}

TEST(MemoryLocationTest, MallocSimpleNodeLargest) {
    int node = numa_max_node();
    LOG(INFO) << "node: " << node;

    std::string location = "cpu:" + std::to_string(node);

    int size = 4096 * 10;
    void *addr = numa_alloc_onnode(size, node);
    ASSERT_NE(addr, nullptr);

    bool only_first_page = false;

    // trigger page fault
    memset(addr, 0, size);

    auto entries = mooncake::getMemoryLocation(addr, size, only_first_page);
    ASSERT_EQ(entries.size(), static_cast<size_t>(1));

    // check the memory location
    EXPECT_EQ(entries[0].start, reinterpret_cast<uint64_t>(addr));
    EXPECT_EQ(entries[0].location, location);
    EXPECT_EQ(entries[0].len, static_cast<size_t>(size));

    numa_free(addr, size);
}

TEST(MemoryLocationTest, MallocMultipleNodes) {
    int nodea = 0;
    int nodeb = numa_max_node();
    LOG(INFO) << "node a: " << nodea << " node b: " << nodeb;

    std::string locationa = "cpu:" + std::to_string(nodea);
    std::string locationb = "cpu:" + std::to_string(nodeb);

    int size = 4096 * 10;
    void *addr = numa_alloc_onnode(size, nodea);
    ASSERT_NE(addr, nullptr);
    ASSERT_EQ((uint64_t)addr % 4096, static_cast<uint64_t>(0));  // page aligned

    bool only_first_page = false;

    // trigger page fault
    memset(addr, 0, size);

    int rc;

    // move first two pages & last one page to nodeb
    void *pages[3] = {addr, (void *)((uint64_t)addr + 4096),
                      (void *)((uint64_t)addr + 4096 * 9)};
    int nodes[3] = {nodeb, nodeb, nodeb};
    int status[3];
    rc = numa_move_pages(0, 3, pages, nodes, status, MPOL_MF_MOVE);
    if (rc != 0) {
        PLOG(ERROR) << "numa_move_pages failed, rc: " << rc;
    }
    ASSERT_EQ(rc, 0);

    // not page aligned
    void *start = (void *)((uint64_t)addr + 1024 * 2);

    auto entries =
        mooncake::getMemoryLocation(start, size - 1024 * 4, only_first_page);

    if (nodea == nodeb) {
        // only one numa node
        ASSERT_EQ(entries.size(), static_cast<size_t>(1));

        // check the first memory location
        EXPECT_EQ(entries[0].start, reinterpret_cast<uint64_t>(start));
        EXPECT_EQ(entries[0].location, locationa);
        EXPECT_EQ(entries[0].len, static_cast<size_t>(size - 1024 * 4));

    } else {
        ASSERT_EQ(entries.size(), static_cast<size_t>(3));

        // check the first memory location
        EXPECT_EQ(entries[0].start, reinterpret_cast<uint64_t>(start));
        EXPECT_EQ(entries[0].location, locationb);
        EXPECT_EQ(entries[0].len, static_cast<size_t>(4096 * 2 - 1024 * 2));

        // check the second memory location
        EXPECT_EQ(entries[1].start,
                  reinterpret_cast<uint64_t>(addr) + 4096 * 2);
        EXPECT_EQ(entries[1].location, locationa);
        EXPECT_EQ(entries[1].len, static_cast<size_t>(4096 * 7));

        // check the third memory location
        EXPECT_EQ(entries[2].start,
                  reinterpret_cast<uint64_t>(addr) + 4096 * 9);
        EXPECT_EQ(entries[2].location, locationb);
        EXPECT_EQ(entries[2].len, static_cast<size_t>(4096 - 1024 * 2));
    }

    numa_free(addr, size);
}
