/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2015-2025 Advanced Micro Devices, Inc. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#include <migraphx/onnx/op_parser.hpp>
#include <migraphx/onnx/checks.hpp>
#include <migraphx/ranges.hpp>
#include <migraphx/op/resize.hpp>
#include <migraphx/shape_for_each.hpp>
#include <migraphx/instruction.hpp>
#include <migraphx/make_op.hpp>
#include <bitset>

namespace migraphx {
inline namespace MIGRAPHX_INLINE_NS {
namespace onnx {

/*
 * Algorithm of calc_neighbor_points():
 * Input: vvv_ind, 3-layer vector to compose vector of indices.
 *        in_s, shape to get space index from, using the composed vector of indices.
 * Output: vector contains the result of space index.
 *
 * From vvv_ind:
 *              layer-1: size of 1st dimension, caller will pass as n_bits
 *              layer-2: hardcode to 2 by caller
 *              layer-3: a vector of out_elements (caller pass) integers.
 * vvv_ind = {
 *   {{...}, {...}},
 *   {{...}, {...}},
 *   {{...}, {...}},
 *   ...
 *   {{...}, {...}}
 * };
 *
 * To Compose a series of vector of indices, which will further be used to get space index from
 *   the input shape.
 * indices{} has (2^n_bits) * out_elements members, each member is a vector of n_bits indices.
 * indices = {
 *     {...},
 *     {...},
 *     {...},
 *     ...
 *     {...}
 * };
 *
 * Notate vvv_ind as:
 *   0-1
 *   A B
 *   C D
 *   E F
 *   G H
 * Notate A' as A's transpose.
 * i.e. A  = {0,1,1,0,1};
 *      A' = {{0},
 *            {1},
 *            {1},
 *            {0},
 *            {1}
 *           };
 *
 * Outer loop:
 *   Iterate all values within range [0, (2^n_bits)) and maps to bitset for inner loop (MSB to LSB).
 * Middle loop:
 *   Transform all elements in layer-3: take indices from inner loop to get index from input shape,
     append to vec_ind.
 * Inner loop:
 *   Compose a vector of indices by iterating all layer-1 using current bitset from current element.
 *
 * i.e. val = 6 -> bitset 0110b -> indices: pick each value from A'D'F'G' -> in_s.index(indices)
 */

static std::vector<int>
calc_neighbor_points(const std::vector<std::vector<std::vector<std::size_t>>>& vvv_ind,
                     const shape& in_s)
{
    std::size_t n_bits     = vvv_ind.size();
    std::size_t m_elements = vvv_ind[0][0].size();
    std::vector<int> vec_ind;

    if(n_bits >= std::numeric_limits<std::size_t>::digits)
    {
        MIGRAPHX_THROW("PARSE_RESIZE: Shape dimension " + std::to_string(n_bits) + " exceeds " +
                       std::to_string(std::numeric_limits<std::size_t>::digits));
    }

    for(std::size_t val = 0; val < (std::size_t{1} << n_bits); val++)
    {
        std::bitset<std::numeric_limits<std::size_t>::digits> bits_val = val;
        std::vector<std::size_t> indices(n_bits);
        transform(range(m_elements), std::back_inserter(vec_ind), [&](std::size_t i_element) {
            transform(
                vvv_ind, range(n_bits), indices.begin(), [&](const auto& vv_ind, std::size_t bit) {
                    return vv_ind[bits_val[bit]][i_element];
                });
            return in_s.index(indices);
        });
    }

    return vec_ind;
}

static std::string get_coord_trans_mode(const onnx_parser::attribute_map& attr)
{
    std::string coord_trans_mode = "half_pixel";
    if(contains(attr, "coordinate_transformation_mode"))
    {
        coord_trans_mode = attr.at("coordinate_transformation_mode").s();
        // does not support transformation mode "tf_crop_and_resize"
        if(coord_trans_mode == "tf_crop_and_resize")
        {
            MIGRAPHX_THROW("PARSE_RESIZE: \"tf_crop_and_resize\" mode is not supported!");
        }
    }

    return coord_trans_mode;
}

static std::string get_mode(const onnx_parser::attribute_map& attr)
{
    std::string mode = "nearest";
    if(contains(attr, "mode"))
    {
        mode = attr.at("mode").s();
        if(mode != "nearest" and mode != "linear")
        {
            MIGRAPHX_THROW("PARSE_RESIZE: only nearest and linear modes are supported!");
        }
    }

    return mode;
}

static std::string get_nearest_mode(const onnx_parser::attribute_map& attr)
{
    std::string nearest_mode = "round_prefer_floor";
    if(contains(attr, "nearest_mode"))
    {
        nearest_mode = attr.at("nearest_mode").s();
    }

    return nearest_mode;
}

// "scales" is an attribute of the deprecated Upsample op. ver7 only
static std::vector<double> get_scales(const onnx_parser::attribute_map& attr)
{
    std::vector<double> scales;
    if(contains(attr, "scales"))
    {
        copy(attr.at("scales").floats(), std::back_inserter(scales));
    }

    return scales;
}

// Hunts through the argument list to find either scales or sizes, and
// populates both scales and sizes vectors from it.
// r_arg: a reference to the argument that was found.
//
// return: true if argument is non-static (i.e. if eval() couldn't read it
// at compile time).  If true, we'll need to use Resize op.
static bool parse_args(const std::vector<instruction_ref>& args,
                       const std::vector<size_t>& in_lens,
                       const std::string& onnx_name,
                       std::vector<double>& vec_scale,
                       std::vector<std::size_t>& out_lens,
                       instruction_ref& r_arg)
{
    for(const auto& arg : args)
    {
        if(arg->name() == "undefined" or arg == args.front())
            continue;

        // skip any empty input (some of the Onnx args. are optional)
        auto lens = arg->get_shape().lens();
        if(lens.empty())
            continue;

        r_arg = arg;

        auto type = arg->get_shape().type();
        if(type == shape::int64_type)
        {
            // this argument is output sizes
            auto arg_out_s = arg->eval();
            if(arg_out_s.empty())
                return true;
            arg_out_s.visit([&](const auto& ol) { out_lens.assign(ol.begin(), ol.end()); });

            if(out_lens.size() != in_lens.size())
            {
                MIGRAPHX_THROW("PARSE_" + onnx_name +
                               ": specified output size's rank does not match input size");
            }

            // compute the scales
            vec_scale.resize(in_lens.size());
            std::transform(in_lens.begin(),
                           in_lens.end(),
                           out_lens.begin(),
                           vec_scale.begin(),
                           [](auto iss, auto oss) { return 1.0 * oss / iss; });
            return false;
        }
        else
        {
            // this argument is scale input
            if(lens[0] == in_lens.size())
            {
                auto arg_scale = arg->eval();
                if(arg_scale.empty())
                    return true;

                arg_scale.visit([&](const auto& v) { vec_scale.assign(v.begin(), v.end()); });
            }
            return false;
        }
    }
    MIGRAPHX_THROW("PARSE_" + onnx_name + ": no shapes or scales input provided");
}

struct parse_resize : op_parser<parse_resize>
{
    std::vector<op_desc> operators() const
    {
        return {{"Resize", "resize"}, {"Upsample", "upsample"}};
    }

    // Helper to add a "reshape" and "gather" instruction.  These can implement
    // Nearest mode resizing if all sizes are known at compile time.
    instruction_ref make_gather_instruction(const onnx_parser::node_info& info,
                                            const std::size_t out_elements,
                                            const shape& in_s,
                                            shape& out_s,
                                            const std::vector<size_t>& in_lens,
                                            const std::vector<size_t>& out_lens,
                                            const std::vector<double>& vec_scale,
                                            instruction_ref args_0) const
    {
        std::string nearest_mode = get_nearest_mode(info.attributes);
        std::vector<int> ind(out_elements);

        // map out_idx to in_idx
        auto nearest_op              = op::resize::get_nearest_op(nearest_mode);
        std::string coord_trans_mode = get_coord_trans_mode(info.attributes);
        auto idx_op                  = op::resize::get_original_idx_op(coord_trans_mode);

        shape_for_each(out_s, [&](const auto& out_idx_v, size_t out_idx) {
            std::vector<size_t> in_idx(out_idx_v.size());
            for(auto ii = 0; ii < in_lens.size(); ++ii)
            {
                auto idx_val = idx_op(in_lens[ii], out_lens[ii], out_idx_v[ii], vec_scale[ii]);
                in_idx[ii]   = nearest_op(in_lens[ii], idx_val);
            }

            ind[out_idx] = static_cast<int64_t>(in_s.index(in_idx));
        });
        // reshape input to one-dimension
        std::vector<int64_t> rsp_lens = {static_cast<int64_t>(in_s.elements())};
        auto rsp = info.add_instruction(make_op("reshape", {{"dims", rsp_lens}}), args_0);

        // ins_ind should be a multi dimensional index that will restore original rank
        shape ind_s{shape::int32_type, out_lens};
        auto ins_ind = info.add_literal(literal(ind_s, ind));
        return info.add_instruction(make_op("gather", {{"axis", 0}}), rsp, ins_ind);
    }

    instruction_ref parse(const op_desc& opd,
                          const onnx_parser&,
                          onnx_parser::node_info info,
                          std::vector<instruction_ref> args) const
    {
        // coord transform mode
        std::string coord_trans_mode = get_coord_trans_mode(info.attributes);

        // mode: only nearest and linear modes are supported for now
        std::string mode = get_mode(info.attributes);

        // nearest mode
        std::string nearest_mode = get_nearest_mode(info.attributes);

        auto idx_op = op::resize::get_original_idx_op(coord_trans_mode);

        // check exclude_outside, only support 0
        if(contains(info.attributes, "exclude_outside") and
           info.attributes.at("exclude_outside").i() == 1)
        {
            MIGRAPHX_THROW("PARSE_" + opd.onnx_name + ": exclude_outside 1 is not supported!");
        }

        // input data shape info
        auto in_s    = args[0]->get_shape().to_static(1);
        auto in_lens = in_s.lens();

        // output shape is explicitly specified
        std::vector<std::size_t> out_lens(in_lens.size());

        // scale
        std::vector<double> vec_scale = get_scales(info.attributes);

        // If `scales` was not an attribute, it must be an input
        // bool is_scale_input{true};
        instruction_ref scales_sizes_arg(args[0]);

        // boolean indicates whether the size of the output can be determined
        // at compile time, i.e. its values come from literal input(s) and have
        // no dependencies anywhere in the graph on runtime inputs.
        bool is_constant_scale_input(not vec_scale.empty());
        if(not is_constant_scale_input)
        {
            // Depending on the args, it *must* populate the `vec_scale`, and might populate
            // `out_lens`
            is_constant_scale_input =
                not parse_args(args, in_lens, opd.onnx_name, vec_scale, out_lens, scales_sizes_arg);
        }

        if(is_constant_scale_input)
        {
            if(in_lens.size() != vec_scale.size())
            {
                MIGRAPHX_THROW("PARSE_" + opd.onnx_name +
                               ": ranks of input and scale are different!");
            }

            // if the output was not calculated yet, we update it based on the scales
            if(all_of(out_lens.cbegin(), out_lens.cend(), [](auto o) { return o == 0; }))
            {
                std::transform(
                    in_lens.begin(),
                    in_lens.end(),
                    vec_scale.begin(),
                    out_lens.begin(),
                    [&](auto idx, auto scale) { return static_cast<std::size_t>(idx * scale); });
            }
        }

        if(mode == "nearest")
        {
            if(args[0]->get_shape().dynamic() or not is_constant_scale_input)
            {
                // Resize's compute_shape() will read scales_sizes_arg as "scales" or "sizes"
                // depending on its data type
                return info.add_instruction(
                    make_op("resize",
                            {{"nearest_mode", nearest_mode},
                             {"coordinate_transformation_mode", coord_trans_mode}}),
                    args[0],
                    scales_sizes_arg);
            }
            else
            {
                // If there are no dynamic shapes and size/scale attributes are literals, then
                // all the indexes can be calculated now at compile time and
                // the Resize can be accomplished with Gather operation.  Preferred for
                // better performance.

                shape out_s{in_s.type(), out_lens};
                std::size_t out_elements = out_s.elements();

                return make_gather_instruction(
                    info, out_elements, in_s, out_s, in_lens, out_lens, vec_scale, args[0]);
            }
        }
        // linear mode
        else
        {
            // out_lens and other variables can't be populated if non-constant (runtime) size
            // inputs.
            if(not is_constant_scale_input)
                MIGRAPHX_THROW("PARSE_" + opd.onnx_name +
                               ": linear mode not supported for non-constant inputs");

            shape out_s{in_s.type(), out_lens};
            std::size_t out_elements = out_s.elements();

            // reshape input to one-dimension
            std::vector<int64_t> rsp_lens = {static_cast<int64_t>(in_s.elements())};
            auto rsp = info.add_instruction(make_op("reshape", {{"dims", rsp_lens}}), args[0]);

            auto nearest_floor = op::resize::get_nearest_op("floor");
            auto nearest_ceil  = op::resize::get_nearest_op("ceil");

            // get the number of dimensions
            std::size_t n_dim = out_lens.size();
            std::vector<std::vector<std::size_t>> vv_ind(2, std::vector<std::size_t>(out_elements));
            std::vector<std::vector<std::vector<std::size_t>>> vvv_ind(n_dim, vv_ind);
            std::vector<std::vector<float>> delta(n_dim, std::vector<float>(out_elements));

            shape_for_each(out_s, [&](const auto& out_idx_v, size_t out_idx) {
                for(auto ii = 0; ii < in_lens.size(); ++ii)
                {
                    auto idx_val = idx_op(in_lens[ii], out_lens[ii], out_idx_v[ii], vec_scale[ii]);
                    vvv_ind[ii][0][out_idx] = nearest_floor(in_lens[ii], idx_val);
                    vvv_ind[ii][1][out_idx] = nearest_ceil(in_lens[ii], idx_val);
                    delta[ii][out_idx]      = idx_val - vvv_ind[ii][0][out_idx];
                }
            });

            auto ind = calc_neighbor_points(vvv_ind, in_s);

            auto ind_lens = out_lens;
            ind_lens[0] *= (std::size_t{1} << n_dim);
            shape ind_s{shape::int32_type, ind_lens};
            auto ins_ind = info.add_literal(literal(ind_s, ind));
            auto data    = info.add_instruction(make_op("gather", {{"axis", 0}}), rsp, ins_ind);

            auto dim_lens = out_lens;
            dim_lens[0] *= (std::size_t{1} << (n_dim - 1));
            for(std::size_t i = 0; i < n_dim; ++i)
            {
                shape dim_s{shape::float_type, dim_lens};
                const auto& dim_delta = delta[n_dim - i - 1];
                std::vector<float> delta_data;
                for(std::size_t j = 0; j < dim_lens[0] / out_lens[0]; ++j)
                {
                    delta_data.insert(delta_data.begin(), dim_delta.begin(), dim_delta.end());
                }
                auto ins_delta = info.add_literal(dim_s, delta_data);

                // slice the data
                int64_t slc_stride = dim_lens[0];
                auto low           = info.add_instruction(
                    make_op("slice", {{"axes", {0}}, {"starts", {0}}, {"ends", {slc_stride}}}),
                    data);
                auto hi = info.add_instruction(
                    make_op("slice",
                            {{"axes", {0}}, {"starts", {slc_stride}}, {"ends", {2 * slc_stride}}}),
                    data);
                auto diff = info.add_instruction(make_op("sub"), hi, low);
                auto ddf  = info.add_instruction(make_op("mul"), diff, ins_delta);
                data      = info.add_instruction(make_op("add"), ddf, low);
                dim_lens[0] /= 2;
            }

            return data;
        }
    }
};

} // namespace onnx

} // namespace MIGRAPHX_INLINE_NS
} // namespace migraphx
