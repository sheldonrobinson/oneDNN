/*******************************************************************************
* Copyright 2024-2025 Intel Corporation
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*******************************************************************************/

#include "graph/backend/dnnl/kernels/softmax.hpp"

#include "graph/backend/dnnl/passes/compile_ops.hpp"
#include "graph/backend/dnnl/passes/constant_propagation.hpp"
#include "graph/backend/dnnl/passes/insert_ops.hpp"
#include "graph/backend/dnnl/passes/layout_propagation.hpp"
#include "graph/backend/dnnl/passes/lower.hpp"
#include "graph/backend/dnnl/passes/memory_planning.hpp"
#include "graph/backend/dnnl/passes/transform.hpp"
#include "graph/backend/dnnl/passes/utils.hpp"

#include "graph/backend/dnnl/op_executable.hpp"

namespace dnnl {
namespace impl {
namespace graph {
namespace dnnl_impl {

status_t softmax_fwd_t::compile_impl(const dnnl_partition_impl_t *part,
        const engine_t *g_engine, const std::vector<logical_tensor_t> &inputs,
        const std::vector<logical_tensor_t> &outputs) {
    p_engine_ = make_dnnl_engine(*g_engine);
    g_alloc_
            = reinterpret_cast<graph::allocator_t *>(g_engine->get_allocator());

    subgraph_ = std::make_shared<subgraph_t>(part->get_ops(), p_engine_,
            part->get_fpmath_mode(), part->get_use_blocked_layout(), true);
    BACKEND_DNNL_CHECK(set_given_inputs_outputs(subgraph_, inputs, outputs));

    subgraph_visualizer_t vis(part->id(), [this](const value_t *val) {
        return this->memory_planner_.get_memory_info(val);
    });
    pass_pipeline_t pipeline(vis);

    BACKEND_DNNL_ADD_PASS(pipeline, lower_down);
    BACKEND_DNNL_ADD_PASS(pipeline, fuse_post_typecast_to_predecessor);
    BACKEND_DNNL_ADD_PASS(pipeline, remove_quant_data_with_no_effect);
    BACKEND_DNNL_ADD_PASS(pipeline, replace_quant_data_with_binary_post_op);
    BACKEND_DNNL_ADD_PASS(pipeline, binary_canonicalization);
    BACKEND_DNNL_ADD_PASS(pipeline, binary_broadcast_swap);
    BACKEND_DNNL_ADD_PASS(pipeline, fuse_post_ops);
    BACKEND_DNNL_ADD_PASS(pipeline, convert_to_runtime_dst_scales);
    BACKEND_DNNL_ADD_PASS(pipeline, fuse_dst_scales);
    BACKEND_DNNL_ADD_PASS(pipeline, infer_shape);

    pipeline.reset_visualize_arg(true, false);

    if (enabled_constant_cache()) {
        BACKEND_DNNL_ADD_PASS(pipeline, constant_propagation);
    }

    BACKEND_DNNL_ADD_PASS(pipeline, layout_propagation);

    if (enabled_constant_cache()) {
        BACKEND_DNNL_ADD_PASS(pipeline, constant_propagation);
    }

    auto memory_plan = [&](std::shared_ptr<subgraph_t> &sg) {
        return memory_planner_.run(sg);
    };
    pipeline.reset_visualize_arg(true, true);
    BACKEND_DNNL_ADD_PASS(pipeline, memory_plan);
    BACKEND_DNNL_ADD_PASS(pipeline, compile_ops);

    // Run the added passes
    BACKEND_DNNL_CHECK(pipeline.run(subgraph_));

    // fill information for outputs logical tensors
    for (size_t i = 0; i < outputs.size(); i++) {
        auto &out = const_cast<logical_tensor_t &>(outputs[i]);
        out = subgraph_->outs_[i];
    }

    resource_ctor_ = [this]() {
        return this->memory_planner_.get_exec_args_set().clone();
    };

    const_md_hash_ = generate_constant_md_hash(part->id(),
            memory_planner_.get_exec_args_set().get_persistent_mem_desc_list());

    return status::success;
}

void softmax_fwd_t::prepare_args_set(const execution_args_set_t *res,
        const std::vector<tensor_t> &inputs,
        const std::vector<tensor_t> &outputs, const scratchpad_t &scratchpad) {
    // update the data of partition in/outputs args
    for (const auto &mem_idx : res->get_mems_use_external_inputs()) {
        mem_idx.first.set_data_handle(inputs[mem_idx.second].get_data_handle());
    }
    for (const auto &mem_idx : res->get_mems_use_external_outputs()) {
        mem_idx.first.set_data_handle(
                outputs[mem_idx.second].get_data_handle());
    }

    grantor_t var_grantor = memory_planner_.internal_temporary_grantor(
            scratchpad.get_buffer());

    for (auto &mem_offkey : res->get_mems_use_internal_temporary()) {
        mem_offkey.first.set_data_handle(var_grantor.get(mem_offkey.second));
    }
}

status_t softmax_fwd_t::execute_impl(const stream_t *g_stream,
        const std::vector<tensor_t> &inputs,
        const std::vector<tensor_t> &outputs) {
    dnnl::stream p_stream = make_dnnl_stream(p_engine_, *g_stream);

    // each thread's own local resource
    thread_local_cache_t<execution_args_set_t> res_cache;
    execution_args_set_t *res = res_cache.get_or_add(
            reinterpret_cast<size_t>(this), resource_ctor_);

    temporary_scratchpad_t scratchpad(
            memory_planner_.total_internal_temporary_size(), p_engine_,
            *g_alloc_);
    assertm(scratchpad.size()
                    >= memory_planner_.total_internal_temporary_size(),
            "no enough scratchpad memory");
    prepare_args_set(res, inputs, outputs, scratchpad);

    constant_tensor_cache_t::cached_t c_buffer;
    if (enabled_constant_cache()) {
        const size_t encoded_key
                = encode_constant_cache_key(inputs, const_md_hash_);
        std::promise<constant_tensor_cache_t::cached_t> c_promise;
        constant_tensor_cache_t::value_t cached_value
                = dnnl_constant_cache_get_or_add(p_engine_, encoded_key,
                        memory_planner_.total_internal_persistent_size(),
                        c_promise.get_future());
        bool is_from_cache = cached_value.valid();
        if (is_from_cache) {
            c_buffer = cached_value.get();
            grantor_t c_grantor = memory_planner_.internal_persistent_grantor(
                    c_buffer->data<char>());
            for (auto &mem_offkey : res->get_mems_use_internal_persistent()) {
                mem_offkey.first.set_data_handle(
                        c_grantor.get(mem_offkey.second));
            }
        } else {
            c_buffer = std::make_shared<dnnl_constant_buffer_t>(
                    memory_planner_.total_internal_persistent_size(), p_engine_,
                    g_alloc_);
            grantor_t c_grantor = memory_planner_.internal_persistent_grantor(
                    c_buffer->data<char>());
            for (auto &mem_offkey : res->get_mems_use_internal_persistent()) {
                mem_offkey.first.set_data_handle(
                        c_grantor.get(mem_offkey.second));
            }

            for (size_t i = 0; i < subgraph_->execs_.size(); i++) {
                if (!subgraph_->is_constant_[i]) continue;
                subgraph_->execs_[i]->execute(
                        p_stream, res->get_exec_args()[i]);
            }

            c_promise.set_value(c_buffer);
        }
    }

    for (size_t i = 0; i < subgraph_->execs_.size(); i++) {
        if (subgraph_->is_constant_[i]) continue;
        subgraph_->execs_[i]->execute(p_stream, res->get_exec_args()[i]);
    }

    return status::success;
}

#ifdef DNNL_WITH_SYCL
status_t softmax_fwd_t::sycl_execute_impl(const stream_t *g_stream,
        const std::vector<tensor_t> &inputs,
        const std::vector<tensor_t> &outputs,
        const std::vector<::sycl::event> &sycl_deps,
        ::sycl::event *sycl_event) {

    auto deps = sycl_deps;
    ::sycl::event returned_event;
    dnnl::stream p_stream = make_dnnl_stream(p_engine_, *g_stream);

    // each thread's own local resource
    thread_local_cache_t<execution_args_set_t> res_cache;
    execution_args_set_t *res = res_cache.get_or_add(
            reinterpret_cast<size_t>(this), resource_ctor_);

    temporary_scratchpad_t scratchpad(
            memory_planner_.total_internal_temporary_size(), p_engine_,
            *g_alloc_);
    assertm(scratchpad.size()
                    >= memory_planner_.total_internal_temporary_size(),
            "no enough scratchpad memory");
    prepare_args_set(res, inputs, outputs, scratchpad);

    constant_tensor_cache_t::cached_t c_buffer;
    if (enabled_constant_cache()) {
        const size_t encoded_key
                = encode_constant_cache_key(inputs, const_md_hash_);
        std::promise<constant_tensor_cache_t::cached_t> c_promise;
        constant_tensor_cache_t::value_t cached_value
                = dnnl_constant_cache_get_or_add(p_engine_, encoded_key,
                        memory_planner_.total_internal_persistent_size(),
                        c_promise.get_future());
        bool is_from_cache = cached_value.valid();
        if (is_from_cache) {
            c_buffer = cached_value.get();
            grantor_t c_grantor = memory_planner_.internal_persistent_grantor(
                    c_buffer->data<char>());
            for (auto &mem_offkey : res->get_mems_use_internal_persistent()) {
                mem_offkey.first.set_data_handle(
                        c_grantor.get(mem_offkey.second));
            }
        } else {
            c_buffer = std::make_shared<dnnl_constant_buffer_t>(
                    memory_planner_.total_internal_persistent_size(), p_engine_,
                    g_alloc_);
            grantor_t c_grantor = memory_planner_.internal_persistent_grantor(
                    c_buffer->data<char>());
            for (auto &mem_offkey : res->get_mems_use_internal_persistent()) {
                mem_offkey.first.set_data_handle(
                        c_grantor.get(mem_offkey.second));
            }

            for (size_t i = 0; i < subgraph_->execs_.size(); i++) {
                if (!subgraph_->is_constant_[i]) continue;
                returned_event = subgraph_->execs_[i]->execute_sycl(
                        p_stream, res->get_exec_args()[i], deps);
                deps = {returned_event};
            }

            c_promise.set_value(c_buffer);
        }
    }

    for (size_t i = 0; i < subgraph_->execs_.size(); i++) {
        if (subgraph_->is_constant_[i]) continue;
        returned_event = subgraph_->execs_[i]->execute_sycl(
                p_stream, res->get_exec_args()[i], deps);
        deps = {returned_event};
    }

    scratchpad.set_deps(returned_event);
    if (sycl_event) *sycl_event = returned_event;

    return status::success;
}
#endif

#if DNNL_GPU_RUNTIME == DNNL_RUNTIME_OCL
status_t softmax_fwd_t::ocl_execute_impl(const stream_t *g_stream,
        const std::vector<tensor_t> &inputs,
        const std::vector<tensor_t> &outputs,
        const std::vector<cl_event> &cl_deps, cl_event *ret_event) {

    auto deps = cl_deps;
    cl_event returned_event {};
    dnnl::stream p_stream = make_dnnl_stream(p_engine_, *g_stream);

    // each thread's own local resource
    thread_local_cache_t<execution_args_set_t> res_cache;
    execution_args_set_t *res = res_cache.get_or_add(
            reinterpret_cast<size_t>(this), resource_ctor_);

    temporary_scratchpad_t scratchpad(
            memory_planner_.total_internal_temporary_size(), p_engine_,
            *g_alloc_);
    assertm(scratchpad.size()
                    >= memory_planner_.total_internal_temporary_size(),
            "no enough scratchpad memory");
    prepare_args_set(res, inputs, outputs, scratchpad);

    constant_tensor_cache_t::cached_t c_buffer;
    if (enabled_constant_cache()) {
        const size_t encoded_key
                = encode_constant_cache_key(inputs, const_md_hash_);
        std::promise<constant_tensor_cache_t::cached_t> c_promise;
        constant_tensor_cache_t::value_t cached_value
                = dnnl_constant_cache_get_or_add(p_engine_, encoded_key,
                        memory_planner_.total_internal_persistent_size(),
                        c_promise.get_future());
        bool is_from_cache = cached_value.valid();
        if (is_from_cache) {
            c_buffer = cached_value.get();
            grantor_t c_grantor = memory_planner_.internal_persistent_grantor(
                    c_buffer->data<char>());
            for (auto &mem_offkey : res->get_mems_use_internal_persistent()) {
                mem_offkey.first.set_data_handle(
                        c_grantor.get(mem_offkey.second));
            }
        } else {
            c_buffer = std::make_shared<dnnl_constant_buffer_t>(
                    memory_planner_.total_internal_persistent_size(), p_engine_,
                    g_alloc_);
            grantor_t c_grantor = memory_planner_.internal_persistent_grantor(
                    c_buffer->data<char>());
            for (auto &mem_offkey : res->get_mems_use_internal_persistent()) {
                mem_offkey.first.set_data_handle(
                        c_grantor.get(mem_offkey.second));
            }

            for (size_t i = 0; i < subgraph_->execs_.size(); i++) {
                if (!subgraph_->is_constant_[i]) continue;
                returned_event = subgraph_->execs_[i]->execute_ocl(
                        p_stream, res->get_exec_args()[i], deps);
                deps = {returned_event};
            }

            c_promise.set_value(c_buffer);
        }
    }

    for (size_t i = 0; i < subgraph_->execs_.size(); i++) {
        if (subgraph_->is_constant_[i]) continue;
        returned_event = subgraph_->execs_[i]->execute_ocl(
                p_stream, res->get_exec_args()[i], deps);
        deps = {returned_event};
    }

    scratchpad.set_deps(returned_event);
    if (ret_event) *ret_event = returned_event;

    return status::success;
}
#endif

#if BUILD_TRAINING
status_t softmax_bwd_t::compile_impl(const dnnl_partition_impl_t *part,
        const engine_t *g_engine, const std::vector<logical_tensor_t> &inputs,
        const std::vector<logical_tensor_t> &outputs) {
    p_engine_ = make_dnnl_engine(*g_engine);
    g_alloc_
            = reinterpret_cast<graph::allocator_t *>(g_engine->get_allocator());

    subgraph_ = std::make_shared<subgraph_t>(part->get_ops(), p_engine_,
            part->get_fpmath_mode(), part->get_use_blocked_layout(), true);
    BACKEND_DNNL_CHECK(set_given_inputs_outputs(subgraph_, inputs, outputs));

    subgraph_visualizer_t vis(part->id(), [this](const value_t *val) {
        return this->memory_planner_.get_memory_info(val);
    });
    pass_pipeline_t pipeline(vis);

    BACKEND_DNNL_ADD_PASS(pipeline, lower_down);

    pipeline.reset_visualize_arg(true, false);

    BACKEND_DNNL_ADD_PASS(pipeline, layout_propagation);

    auto memory_plan = [&](std::shared_ptr<subgraph_t> &sg) {
        return memory_planner_.run(sg);
    };
    pipeline.reset_visualize_arg(true, true);
    BACKEND_DNNL_ADD_PASS(pipeline, memory_plan);
    BACKEND_DNNL_ADD_PASS(pipeline, compile_ops);

    // Run the added passes
    BACKEND_DNNL_CHECK(pipeline.run(subgraph_));

    // fill information for outputs logical tensors
    for (size_t i = 0; i < outputs.size(); i++) {
        auto &out = const_cast<logical_tensor_t &>(outputs[i]);
        out = subgraph_->outs_[i];
    }

    resource_ctor_ = [this]() {
        return this->memory_planner_.get_exec_args_set().clone();
    };

    return status::success;
}

void softmax_bwd_t::prepare_args_set(const execution_args_set_t *res,
        const std::vector<tensor_t> &inputs,
        const std::vector<tensor_t> &outputs, const scratchpad_t &scratchpad) {
    // update the data of partition in/outputs args
    for (const auto &mem_idx : res->get_mems_use_external_inputs()) {
        mem_idx.first.set_data_handle(inputs[mem_idx.second].get_data_handle());
    }
    for (const auto &mem_idx : res->get_mems_use_external_outputs()) {
        mem_idx.first.set_data_handle(
                outputs[mem_idx.second].get_data_handle());
    }

    grantor_t var_grantor = memory_planner_.internal_temporary_grantor(
            scratchpad.get_buffer());

    for (auto &mem_offkey : res->get_mems_use_internal_temporary()) {
        mem_offkey.first.set_data_handle(var_grantor.get(mem_offkey.second));
    }
}

status_t softmax_bwd_t::execute_impl(const stream_t *g_stream,
        const std::vector<tensor_t> &inputs,
        const std::vector<tensor_t> &outputs) {
    dnnl::stream p_stream = make_dnnl_stream(p_engine_, *g_stream);

    // each thread's own local resource
    thread_local_cache_t<execution_args_set_t> res_cache;
    execution_args_set_t *res = res_cache.get_or_add(
            reinterpret_cast<size_t>(this), resource_ctor_);

    temporary_scratchpad_t scratchpad(
            memory_planner_.total_internal_temporary_size(), p_engine_,
            *g_alloc_);
    assertm(scratchpad.size()
                    >= memory_planner_.total_internal_temporary_size(),
            "no enough scratchpad memory");
    prepare_args_set(res, inputs, outputs, scratchpad);

    for (size_t i = 0; i < subgraph_->execs_.size(); i++) {
        subgraph_->execs_[i]->execute(p_stream, res->get_exec_args()[i]);
    }

    return status::success;
}

#ifdef DNNL_WITH_SYCL
status_t softmax_bwd_t::sycl_execute_impl(const stream_t *g_stream,
        const std::vector<tensor_t> &inputs,
        const std::vector<tensor_t> &outputs,
        const std::vector<::sycl::event> &sycl_deps,
        ::sycl::event *sycl_event) {

    auto deps = sycl_deps;
    ::sycl::event returned_event;
    dnnl::stream p_stream = make_dnnl_stream(p_engine_, *g_stream);

    // each thread's own local resource
    thread_local_cache_t<execution_args_set_t> res_cache;
    execution_args_set_t *res = res_cache.get_or_add(
            reinterpret_cast<size_t>(this), resource_ctor_);

    temporary_scratchpad_t scratchpad(
            memory_planner_.total_internal_temporary_size(), p_engine_,
            *g_alloc_);
    assertm(scratchpad.size()
                    >= memory_planner_.total_internal_temporary_size(),
            "no enough scratchpad memory");
    prepare_args_set(res, inputs, outputs, scratchpad);

    for (size_t i = 0; i < subgraph_->execs_.size(); i++) {
        returned_event = subgraph_->execs_[i]->execute_sycl(
                p_stream, res->get_exec_args()[i], deps);
        deps = {returned_event};
    }

    scratchpad.set_deps(returned_event);
    if (sycl_event) *sycl_event = returned_event;

    return status::success;
}
#endif

#if DNNL_GPU_RUNTIME == DNNL_RUNTIME_OCL
status_t softmax_bwd_t::ocl_execute_impl(const stream_t *g_stream,
        const std::vector<tensor_t> &inputs,
        const std::vector<tensor_t> &outputs,
        const std::vector<cl_event> &cl_deps, cl_event *ret_event) {

    auto deps = cl_deps;
    cl_event returned_event {};
    dnnl::stream p_stream = make_dnnl_stream(p_engine_, *g_stream);

    // each thread's own local resource
    thread_local_cache_t<execution_args_set_t> res_cache;
    execution_args_set_t *res = res_cache.get_or_add(
            reinterpret_cast<size_t>(this), resource_ctor_);

    temporary_scratchpad_t scratchpad(
            memory_planner_.total_internal_temporary_size(), p_engine_,
            *g_alloc_);
    assertm(scratchpad.size()
                    >= memory_planner_.total_internal_temporary_size(),
            "no enough scratchpad memory");
    prepare_args_set(res, inputs, outputs, scratchpad);

    for (size_t i = 0; i < subgraph_->execs_.size(); i++) {
        returned_event = subgraph_->execs_[i]->execute_ocl(
                p_stream, res->get_exec_args()[i], deps);
        deps = {returned_event};
    }

    scratchpad.set_deps(returned_event);
    if (ret_event) *ret_event = returned_event;

    return status::success;
}
#endif
#endif // BUILD_TRAINING
} // namespace dnnl_impl
} // namespace graph
} // namespace impl
} // namespace dnnl
