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

#pragma once

#include "column/vectorized_fwd.h"
#include "exec/pipeline/scan/balanced_chunk_buffer.h"
#include "exec/pipeline/scan/chunk_source.h"
#include "exec/pipeline/scan/scan_operator.h"
#include "exec/pipeline/scan/schema_scan_context.h"
#include "runtime/runtime_state.h"
#include "storage/chunk_helper.h"

namespace starrocks {
class SchemaScannerParam;
class SchemaScanner;

namespace pipeline {

class SchemaChunkSource final : public ChunkSource {
public:
    SchemaChunkSource(ScanOperator* op, RuntimeProfile* runtime_profile, MorselPtr&& morsel,
                      const SchemaScanContextPtr& ctx);

    ~SchemaChunkSource() override;

    // Prepare the ChunkSource state. Should not put any blocking RPC calls in it
    Status prepare(RuntimeState* state) override;

    // Start the ChunkSource, may execute some RPC calls to fetch metadata from FE
    Status start(RuntimeState* state) override;

    void close(RuntimeState* state) override;

private:
    Status _read_chunk(RuntimeState* state, ChunkPtr* chunk) override;

    const TupleDescriptor* _dest_tuple_desc;
    std::unique_ptr<SchemaScanner> _schema_scanner;

    SchemaScanContextPtr _ctx;

    RuntimeProfile::Counter* _filter_timer = nullptr;

    // Because schema_scanner returns column data based on column index.
    // So a mapping relationship between the slot list of schema_scanner and the tuple slot list is needed
    // (it can be understood as the relationship between input slots and output slots of schema_scan)
    std::vector<int> _index_map;

    ChunkAccumulator _accumulator;

    std::once_flag _start_once;
};
} // namespace pipeline
} // namespace starrocks
