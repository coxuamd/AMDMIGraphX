/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2015-2024 Advanced Micro Devices, Inc. All rights reserved.
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

#include "verify_program.hpp"
#include <migraphx/program.hpp>
#include <migraphx/generate.hpp>
#include <migraphx/make_op.hpp>

template <migraphx::shape::type_t DType>
struct test_conv_add : verify_program<test_conv_add<DType>>
{
    migraphx::program create_program() const
    {
        migraphx::program p;
        auto* mm = p.get_main_module();
        auto x     = mm->add_parameter("x", {DType, {1, 8, 4, 4}});
        auto w     = mm->add_literal(migraphx::generate_literal({DType, {2, 8, 3, 3}}, 1));
        auto y     = mm->add_parameter("y", {DType, {1, 8, 4, 4}});
        auto v     = mm->add_literal(migraphx::generate_literal({DType, {2, 8, 3, 3}}, 2));
        auto conv1 = mm->add_instruction(migraphx::make_op("convolution"), x, w);
        auto conv2 = mm->add_instruction(migraphx::make_op("convolution"), y, v);
        auto sum   = mm->add_instruction(migraphx::make_op("add"), conv1, conv2);
        mm->add_instruction(migraphx::make_op("exp"), sum);
        return p;
    }
    std::string section() const { return "conv"; }
};

template struct test_conv_add<migraphx::shape::float_type>;
template struct test_conv_add<migraphx::shape::fp8e4m3fnuz_type>;
template struct test_conv_add<migraphx::shape::fp8e5m2fnuz_type>;
template struct test_conv_add<migraphx::shape::fp8e4m3fn_type>;
template struct test_conv_add<migraphx::shape::fp8e5m2_type>;
