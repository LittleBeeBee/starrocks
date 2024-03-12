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

#include <atomic>
#include <utility>

#include "exec/aggregator.h"
#include "exec/pipeline/operator.h"
#include "runtime/runtime_state.h"

namespace starrocks::pipeline {
class AggregateBlockingSinkOperator : public Operator {
public:
    AggregateBlockingSinkOperator(AggregatorPtr aggregator, OperatorFactory* factory, int32_t id, int32_t plan_node_id,
                                  int32_t driver_sequence, const char* name = "aggregate_blocking_sink")
            : Operator(factory, id, name, plan_node_id, false, driver_sequence), _aggregator(std::move(aggregator)) {
        _aggregator->set_aggr_phase(AggrPhase2);
        _aggregator->ref();
    }

    ~AggregateBlockingSinkOperator() override = default;

    bool has_output() const override { return false; }
    bool need_input() const override { return !is_finished(); }
    bool is_finished() const override { return _is_finished || _aggregator->is_finished(); }
    Status set_finishing(RuntimeState* state) override;

    Status prepare(RuntimeState* state) override;
    void close(RuntimeState* state) override;

    StatusOr<ChunkPtr> pull_chunk(RuntimeState* state) override;
    Status push_chunk(RuntimeState* state, const ChunkPtr& chunk) override;
    Status reset_state(RuntimeState* state, const std::vector<ChunkPtr>& refill_chunks) override;

protected:
    // It is used to perform aggregation algorithms shared by
    // AggregateBlockingSourceOperator. It is
    // - prepared at SinkOperator::prepare(),
    // - reffed at constructor() of both sink and source operator,
    // - unreffed at close() of both sink and source operator.
    AggregatorPtr _aggregator = nullptr;

private:
    // Whether prev operator has no output
    std::atomic_bool _is_finished = false;
    // whether enable aggregate group by limit optimize
    bool _agg_group_by_with_limit = false;
    std::atomic<int64_t>* _runtime_limit = nullptr;
};

class AggregateBlockingSinkOperatorFactory final : public OperatorFactory {
public:
    AggregateBlockingSinkOperatorFactory(int32_t id, int32_t plan_node_id, AggregatorFactoryPtr aggregator_factory,
                                         const SpillProcessChannelFactoryPtr& _)
            : OperatorFactory(id, "aggregate_blocking_sink", plan_node_id),
              _aggregator_factory(std::move(aggregator_factory)) {
        _runtime_limit.store(_aggregator_factory->aggregator_param()->limit);
    }

    ~AggregateBlockingSinkOperatorFactory() override = default;

    Status prepare(RuntimeState* state) override;

    OperatorPtr create(int32_t degree_of_parallelism, int32_t driver_sequence) override;

    std::atomic<int64_t>* get_runtime_limit() { return &_runtime_limit; }

private:
    AggregatorFactoryPtr _aggregator_factory;
    std::atomic<int64_t> _runtime_limit;
};
} // namespace starrocks::pipeline
