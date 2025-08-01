/*******************************************************************************
* Copyright 2022-2025 Intel Corporation
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

#ifndef GPU_INTEL_JIT_CONV_PLAN_HPP
#define GPU_INTEL_JIT_CONV_PLAN_HPP

#include <sstream>
#include <string>

#include "gpu/intel/jit/conv/grf_usage.hpp"
#include "gpu/intel/jit/conv/plan_utils.hpp"
#include "gpu/intel/jit/conv/zp_plan.hpp"
#include "gpu/intel/jit/ir/fma.hpp"
#include "gpu/intel/jit/ir/gemm_schedule.hpp"
#include "gpu/intel/jit/ir/send_plan.hpp"
#include "gpu/intel/jit/ir/tensor.hpp"
#include "gpu/intel/jit/utils/utils.hpp"

namespace dnnl {
namespace impl {
namespace gpu {
namespace intel {
namespace jit {

struct reorder_plan_t : public base_plan_t {
    layout_t src;
    layout_t dst;
    int split_factor = 1;

    using base_plan_t::base_plan_t;

    explicit operator bool() const { return !src.is_empty(); }

    bool can_split(int factor) const;
    void set_split(int factor = 1);
    stmt_t create_stmt(const expr_t &src_buf, const expr_t &dst_buf) const;
    dim_t src_buf_size() const;
    dim_t estimate_regs() const;

    std::string str(const std::string &tag = "reorder") const {
        ostringstream_t oss;
        oss << tag << ": src:" << src << " -> dst:" << dst;
        return oss.str();
    }

    IR_DEFINE_DUMP()
};

struct reduce_plan_t : public base_plan_t {
    layout_t src;
    layout_t dst;
    uint32_t mask = 0;
    int split_factor = 1;

    using base_plan_t::base_plan_t;

    explicit operator bool() const { return !src.is_empty(); }
    dim_t dst_buf_size() const;
    bool can_split(int factor) const;
    void set_split(int factor = 1);
    stmt_t create_stmt(const expr_t &src_buf, const expr_t &dst_buf) const;
    int estimate_regs() const;

    std::string str(const std::string &tag = "reduce") const {
        ostringstream_t oss;
        oss << tag << ": src:" << src << " -> dst:" << dst;
        return oss.str();
    }

    IR_DEFINE_DUMP()
};

struct slm_plan_t : public base_plan_t {
    layout_t a_layout;
    layout_t b_layout;
    send_plan_t a_g2s_load;
    send_plan_t b_g2s_load;
    tile_coord_t x_reduce_tile_coord;
    reduce_plan_t x_reduce;
    reorder_plan_t a_reorder;
    reorder_plan_t b_reorder;
    send_plan_t a_g2s_store;
    send_plan_t b_g2s_store;
    grid_info_t a_grid;
    grid_info_t b_grid;

    slm_plan_t(const hw_t &hw)
        : base_plan_t(hw), x_reduce(hw), a_reorder(hw), b_reorder(hw) {}

    explicit operator bool() const { return has_a() || has_b(); }
    bool has_a() const { return (bool)a_g2s_load; }
    bool has_b() const { return (bool)b_g2s_load; }
    int slm_size() const { return (int)(a_layout.size() + b_layout.size()); }
    std::string str() const;

    IR_DEFINE_DUMP()
};

struct prefetch_plan_t : public base_plan_t {
    send_plan_t a_prefetch;
    send_plan_t b_prefetch;
    grid_info_t a_grid;
    grid_info_t b_grid;

    using base_plan_t::base_plan_t;

    explicit operator bool() const { return a_prefetch || b_prefetch; }

    bool has_a() const { return (bool)a_prefetch; }
    bool has_b() const { return (bool)b_prefetch; }

    int estimate_regs(bool reuse_headers) const;
    std::string str() const;

    IR_DEFINE_DUMP()
};

struct x2r_plan_t : public base_plan_t {
    send_plan_t a_load;
    send_plan_t b_load;
    tile_coord_t x_reduce_tile_coord;
    reduce_plan_t x_reduce;
    reorder_plan_t a_reorder;
    reorder_plan_t b_reorder;
    layout_t a_layout;
    layout_t b_layout;
    abc_kind_t split_abc = abc_kind_t::undef;
    int split_factor = 1;

    x2r_plan_t(const hw_t &hw)
        : base_plan_t(hw), x_reduce(hw), a_reorder(hw), b_reorder(hw) {}

    bool can_split(abc_kind_t abc, int factor) const;
    void set_split(abc_kind_t abc = abc_kind_t::undef, int factor = 1);

    int a_buf_size() const {
        int a_size = into<int>(a_layout.size());
        if (split_abc == abc_kind_t::a)
            a_size = utils::div_up(a_size, split_factor);
        return utils::rnd_up(a_size, grf_size());
    }

    int b_buf_size() const {
        int b_size = into<int>(b_layout.size());
        if (split_abc == abc_kind_t::b)
            b_size = utils::div_up(b_size, split_factor);
        return utils::rnd_up(b_size, grf_size());
    }

    int estimate_regs(bool reuse_headers) const;
    std::string str() const;

    IR_DEFINE_DUMP()
};

struct fma_plan_t : public base_plan_t {
    layout_t a_layout;
    layout_t b_layout;
    layout_t c_layout;
    layout_t c_prb_layout;
    fma_kind_t fma_kind = fma_kind_t::undef;
    int b_blk = 0;
    int m_blk = 0;
    int n_blk = 0;
    int k_blk = 0;
    abc_kind_t split_abc = abc_kind_t::undef;
    int split_factor = 1;

    using base_plan_t::base_plan_t;

    int max_bmn_blk() const {
        int ret = 0;
        ret = std::max(ret, b_blk);
        ret = std::max(ret, m_blk);
        ret = std::max(ret, n_blk);
        return ret;
    }

    bool can_split(abc_kind_t abc, int factor) const;
    void set_split(abc_kind_t abc, int factor);
    bool is_a_broadcast() const { return b_blk * m_blk * k_blk == 1; }
    bool is_b_broadcast() const { return b_blk * k_blk * n_blk == 1; }
    int a_buf_size() const;
    int b_buf_size() const;
    int bmnk_split_idx(bmnk_kind_t bmnk, int split_off, bool is_start) const;
    int bmnk_start_idx(bmnk_kind_t bmnk, int subtile_idx) const;
    int bmnk_stop_idx(bmnk_kind_t bmnk, int subtile_idx) const;

    std::vector<func_t> create_fma_funcs(const hw_t &hw) const;
    static stmt_t create_fma_block(const std::vector<func_t> &fmas,
            const expr_t &a, const expr_t &b, const expr_t &c);
    stmt_t create_stmt(ir_context_t &ir_ctx, buffer_manager_t &buf_mgr,
            const std::string &a, const std::string &b, const std::string &c,
            int subtile_idx) const;

    int estimate_regs() const;
    std::string str() const;

    IR_DEFINE_DUMP()
};

struct conv_plan_t : public base_plan_t {
    expr_t ap_buf;
    expr_t bp_buf;
    expr_t cp_buf;
    constraint_set_t init_cset;
    gemm_schedule_t gemm_schedule;
    view_t bia_view;
    slm_plan_t slm;
    prefetch_plan_t prefetch;
    x2r_plan_t x2r;
    fma_plan_t fma;
    zp_plan_t zp;
    abc_kind_t split_abc = abc_kind_t::undef;
    int split_factor = 1;
    bool reuse_headers = false;
    int max_gmem_bufs = 0;
    int reserved_regs = -1;

    conv_plan_t(const hw_t &hw)
        : base_plan_t(hw), slm(hw), prefetch(hw), x2r(hw), fma(hw), zp(hw) {}

    const tile_coord_t &x_reduce_tile_coord() const {
        if (!x2r.x_reduce_tile_coord.is_empty()) return x2r.x_reduce_tile_coord;
        if (!slm.x_reduce_tile_coord.is_empty()) return slm.x_reduce_tile_coord;
        gpu_error_not_expected();
        return x2r.x_reduce_tile_coord;
    }

    bool can_split(abc_kind_t abc, int factor) const;
    void set_split(abc_kind_t abc, int factor);
    bool uses_2d_load(abc_kind_t abc) const;
    grf_usage_t grf_usage() const;
    void reset();
    std::string str() const;

    IR_DEFINE_DUMP()
};

class conv_config_t;

status_t init_plan(conv_config_t &cfg);

} // namespace jit
} // namespace intel
} // namespace gpu
} // namespace impl
} // namespace dnnl

#endif
