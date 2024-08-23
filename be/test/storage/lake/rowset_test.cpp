// Copyright 2021-present StarRocks, Inc. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <gtest/gtest.h>

#include "column/chunk.h"
#include "column/fixed_length_column.h"
#include "column/schema.h"
#include "column/vectorized_fwd.h"
#include "common/logging.h"
#include "storage/chunk_helper.h"
#include "storage/lake/metacache.h"
#include "storage/lake/tablet_manager.h"
#include "storage/lake/tablet_writer.h"
#include "storage/tablet_schema.h"
#include "test_util.h"
#include "testutil/assert.h"
#include "testutil/id_generator.h"

namespace starrocks::lake {

using namespace starrocks;

class LakeRowsetTest : public TestBase {
public:
    LakeRowsetTest() : TestBase(kTestDirectory) {
        _tablet_metadata = generate_simple_tablet_metadata(DUP_KEYS);
        _tablet_schema = TabletSchema::create(_tablet_metadata->schema());
        _schema = std::make_shared<Schema>(ChunkHelper::convert_schema(_tablet_schema));
    }

    void SetUp() override {
        clear_and_init_test_dir();
        CHECK_OK(_tablet_mgr->put_tablet_metadata(*_tablet_metadata));
    }

    void TearDown() override { remove_test_dir_ignore_error(); }

    void create_rowsets_for_testing() {
        std::vector<int> k0{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22};
        std::vector<int> v0{2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 22, 24, 26, 28, 30, 32, 34, 36, 38, 40, 41, 44};

        std::vector<int> k1{30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41};
        std::vector<int> v1{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};

        auto c0 = Int32Column::create();
        auto c1 = Int32Column::create();
        auto c2 = Int32Column::create();
        auto c3 = Int32Column::create();
        c0->append_numbers(k0.data(), k0.size() * sizeof(int));
        c1->append_numbers(v0.data(), v0.size() * sizeof(int));
        c2->append_numbers(k1.data(), k1.size() * sizeof(int));
        c3->append_numbers(v1.data(), v1.size() * sizeof(int));

        Chunk chunk0({c0, c1}, _schema);
        Chunk chunk1({c2, c3}, _schema);

        ASSIGN_OR_ABORT(auto tablet, _tablet_mgr->get_tablet(_tablet_metadata->id()));

        {
            int64_t txn_id = next_id();
            // write rowset 1 with 2 segments
            ASSIGN_OR_ABORT(auto writer, tablet.new_writer(kHorizontal, txn_id));
            ASSERT_OK(writer->open());

            // write rowset data
            // segment #1
            ASSERT_OK(writer->write(chunk0));
            ASSERT_OK(writer->write(chunk1));
            ASSERT_OK(writer->finish());

            // segment #2
            ASSERT_OK(writer->write(chunk0));
            ASSERT_OK(writer->write(chunk1));
            ASSERT_OK(writer->finish());

            auto files = writer->files();
            ASSERT_EQ(2, files.size());

            // add rowset metadata
            auto* rowset = _tablet_metadata->add_rowsets();
            rowset->set_overlapped(true);
            rowset->set_id(1);
            auto* segs = rowset->mutable_segments();
            for (auto& file : writer->files()) {
                segs->Add(std::move(file.path));
            }

            writer->close();
        }

        // write tablet metadata
        _tablet_metadata->set_version(2);
        CHECK_OK(_tablet_mgr->put_tablet_metadata(*_tablet_metadata));
    }

protected:
    constexpr static const char* const kTestDirectory = "test_lake_rowset";

    std::shared_ptr<TabletMetadata> _tablet_metadata;
    std::shared_ptr<TabletSchema> _tablet_schema;
    std::shared_ptr<Schema> _schema;
};

TEST_F(LakeRowsetTest, test_load_segments) {
    create_rowsets_for_testing();

    ASSIGN_OR_ABORT(auto tablet, _tablet_mgr->get_tablet(_tablet_metadata->id()));
    auto* cache = _tablet_mgr->metacache();

    ASSIGN_OR_ABORT(auto rowsets, tablet.get_rowsets(2));
    ASSERT_EQ(1, rowsets.size());
    auto& rowset = rowsets[0];

    // fill cache: false
    ASSIGN_OR_ABORT(auto segments1, rowset->segments(false));
    ASSERT_EQ(2, segments1.size());
    for (const auto& seg : segments1) {
        auto segment = cache->lookup_segment(seg->file_name());
        ASSERT_TRUE(segment == nullptr);
    }

    // fill data cache: false, fill metadata cache: true
    LakeIOOptions lake_io_opts{.fill_data_cache = false};
    ASSIGN_OR_ABORT(auto segments2, rowset->segments(lake_io_opts, true));
    ASSERT_EQ(2, segments2.size());
    for (const auto& seg : segments2) {
        auto segment = cache->lookup_segment(seg->file_name());
        ASSERT_TRUE(segment != nullptr);
    }
}

TEST_F(LakeRowsetTest, test_segment_update_cache_size) {
    create_rowsets_for_testing();

    ASSIGN_OR_ABORT(auto tablet, _tablet_mgr->get_tablet(_tablet_metadata->id()));
    ASSIGN_OR_ABORT(auto rowsets, tablet.get_rowsets(2));
    ASSIGN_OR_ABORT(auto segments, rowsets[0]->segments(false));

    auto* cache = _tablet_mgr->metacache();

    // get the same segments from the rowset
    auto sample_segment = segments[0];
    std::string path = sample_segment->file_name();
    ASSIGN_OR_ABORT(auto fs, FileSystem::CreateSharedFromString(path));
    auto schema = sample_segment->tablet_schema_share_ptr();

    // create a dummy segment with the same path to cache ahead in metacache,
    // the later segment open operation will not update the mem_usage due to instance mismatch.
    {
        // clean the cache
        cache->prune();
        //create the dummy segment and put it into metacache
        auto dummy_segment =
                std::make_shared<Segment>(fs, FileInfo{path}, sample_segment->id(), schema, _tablet_mgr.get());
        cache->cache_segment(path, dummy_segment);
        EXPECT_EQ(dummy_segment, cache->lookup_segment(path));
        auto sz1 = cache->memory_usage();

        auto mirror_segment =
                std::make_shared<Segment>(fs, FileInfo{path}, sample_segment->id(), schema, _tablet_mgr.get());
        LakeIOOptions lake_io_opts{.fill_data_cache = true};
        auto st = mirror_segment->open(nullptr, nullptr, lake_io_opts);
        EXPECT_TRUE(st.ok());
        auto sz2 = cache->memory_usage();
        // no memory_usage change, because the instance in metacache is different from this mirror_segment
        EXPECT_EQ(sz1, sz2);
    }
    // create the mirror_segment without open, and put it into metacache, get the cache memory_usage,
    // open the segment (during the open, the cache size will be updated), get the cache memory_usage again.
    {
        // clean the cache
        cache->prune();
        //create the dummy segment and put it into metacache
        auto mirror_segment =
                std::make_shared<Segment>(fs, FileInfo{path}, sample_segment->id(), schema, _tablet_mgr.get());
        cache->cache_segment(path, mirror_segment);
        auto sz1 = cache->memory_usage();
        auto ssz1 = mirror_segment->mem_usage();

        LakeIOOptions lake_io_opts{.fill_data_cache = true};
        auto st = mirror_segment->open(nullptr, nullptr, lake_io_opts);
        EXPECT_TRUE(st.ok());
        auto sz2 = cache->memory_usage();
        auto ssz2 = mirror_segment->mem_usage();
        // mem usage updated after the segment is opened.
        EXPECT_LT(sz1, sz2);
        EXPECT_EQ(ssz2 - ssz1, sz2 - sz1);
    }
}

TEST_F(LakeRowsetTest, test_add_partial_compaction_segments_info) {
    create_rowsets_for_testing();

    auto rs = std::make_shared<lake::Rowset>(_tablet_mgr.get(), _tablet_metadata, 0, 1 /* compaction_segment_limit */);
    ASSERT_TRUE(rs->partial_segments_compaction());

    ASSIGN_OR_ABORT(auto segments, rs->segments(false));

    TxnLogPB_OpCompaction op_compaction;
    uint64_t num_rows = 0;
    uint64_t data_size = 0;
    EXPECT_EQ(op_compaction.output_rowset().segments_size(), 0);

    ASSIGN_OR_ABORT(auto tablet, _tablet_mgr->get_tablet(_tablet_metadata->id()));
    int64_t txn_id = next_id();
    ASSIGN_OR_ABORT(auto writer, tablet.new_writer(kHorizontal, txn_id));

    // prepare writer
    {
        std::vector<int> k1{40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51};
        std::vector<int> v1{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};

        auto c0 = Int32Column::create();
        auto c1 = Int32Column::create();
        c0->append_numbers(k1.data(), k1.size() * sizeof(int));
        c1->append_numbers(v1.data(), v1.size() * sizeof(int));
        Chunk chunk0({c0, c1}, _schema);

        ASSERT_OK(writer->open());
        ASSERT_OK(writer->write(chunk0));
        ASSERT_OK(writer->finish());
        ASSERT_EQ(1, writer->files().size());
    }

    EXPECT_TRUE(rs->add_partial_compaction_segments_info(&op_compaction, writer.get(), num_rows, data_size).ok());
    EXPECT_EQ(op_compaction.output_rowset().segments_size(), 2);
    EXPECT_TRUE(num_rows > 0);
    EXPECT_TRUE(data_size > 0);
}

} // namespace starrocks::lake
