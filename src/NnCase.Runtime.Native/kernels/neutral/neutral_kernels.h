#pragma once
#include "../utils.h"
#include <runtime/op_utility.h>

namespace nncase
{
namespace kernels
{
    namespace neutral
    {
        template <class TOp>
        void binary(const float *input_a, const float *input_b, float *output, const runtime_shape_t &in_a_shape,
            const runtime_shape_t &in_b_shape, const runtime_shape_t &out_shape, const value_range<float> &fused_activation, TOp &&op)
        {
            for (int32_t d0 = 0; d0 < out_shape[0]; d0++)
            {
                for (int32_t d1 = 0; d1 < out_shape[1]; d1++)
                {
                    for (int32_t d2 = 0; d2 < out_shape[2]; d2++)
                    {
                        for (int32_t d3 = 0; d3 < out_shape[3]; d3++)
                        {
                            runtime_shape_t in_off = { d0, d1, d2, d3 };
                            const auto in_a_off = kernels::details::get_reduced_offset(in_off, in_a_shape);
                            const auto in_b_off = kernels::details::get_reduced_offset(in_off, in_b_shape);
                            const auto a = input_a[offset(in_a_shape, in_a_off)];
                            const auto b = input_b[offset(in_b_shape, in_b_off)];

                            output[offset(out_shape, in_off)] = kernels::details::apply_activation(op(a, b), fused_activation);
                        }
                    }
                }
            }
        }

        inline void concat(xtl::span<const uint8_t *> inputs, uint8_t *output, xtl::span<const int32_t> concat_dims, size_t inner_size, size_t outer_size)
        {
            for (size_t oc = 0; oc < outer_size; oc++)
            {
                for (size_t i = 0; i < inputs.size(); i++)
                {
                    auto size = inner_size * concat_dims[i];
                    auto src = inputs[i] + oc * size;
                    std::copy(src, src + size, output);
                    output += size;
                }
            }
        }

        inline void conv2d(const float *input, float *output, const float *weights, const float *bias, const runtime_shape_t &in_shape,
            int32_t groups, int32_t out_channels, int32_t filter_h, int32_t filter_w, int32_t stride_h, int32_t stride_w, int32_t dilation_h, int32_t dilation_w,
            const padding &padding_h, const padding &padding_w, const value_range<float> &fused_activation)
        {
            const auto out_h = details::get_windowed_output_size(in_shape[2], filter_h, stride_h, dilation_h, padding_h);
            const auto out_w = details::get_windowed_output_size(in_shape[3], filter_w, stride_w, dilation_w, padding_w);
            const auto g_ic = in_shape[1] / groups;
            const auto g_oc = out_channels / groups;

            for (int32_t batch = 0; batch < in_shape[0]; batch++)
            {
                const float *in_batch_p = input + (size_t)batch * in_shape[1] * in_shape[2] * in_shape[3];

                for (int32_t og = 0; og < groups; og++)
                {
                    const float *in_group_p = in_batch_p + (size_t)og * g_ic * in_shape[2] * in_shape[3];
                    const float *w_group_p = weights + (size_t)og * g_oc * g_ic * filter_h * filter_w;

                    for (int32_t oc = 0; oc < g_oc; oc++)
                    {
                        const float *w_oc_p = w_group_p + (size_t)oc * g_ic * filter_h * filter_w;

                        for (int32_t oy = 0; oy < out_h; oy++)
                        {
                            for (int32_t ox = 0; ox < out_w; ox++)
                            {
                                const int32_t in_y_origin = (oy * stride_h) - padding_h.before;
                                const int32_t in_x_origin = (ox * stride_w) - padding_w.before;
                                const int32_t filter_y_start = std::max(0, (-in_y_origin + dilation_h - 1) / dilation_h);
                                const int32_t filter_y_end = std::min(filter_h, (in_shape[2] - in_y_origin + dilation_h - 1) / dilation_h);
                                const int32_t filter_x_start = std::max(0, (-in_x_origin + dilation_w - 1) / dilation_w);
                                const int32_t filter_x_end = std::min(filter_w, (in_shape[3] - in_x_origin + dilation_w - 1) / dilation_w);
                                float value = bias[oc];

                                for (int32_t ic = 0; ic < g_ic; ic++)
                                {
                                    const float *in_c_p = in_group_p + (size_t)ic * in_shape[2] * in_shape[3];
                                    const float *w_ic_p = w_oc_p + (size_t)ic * filter_h * filter_w;

                                    for (int32_t ky = filter_y_start; ky < filter_y_end; ky++)
                                    {
                                        for (int32_t kx = filter_x_start; kx < filter_x_end; kx++)
                                        {
                                            const int32_t in_y = in_y_origin + dilation_h * ky;
                                            const int32_t in_x = in_x_origin + dilation_w * kx;

                                            const float in_v = in_c_p[in_y * in_shape[3] + in_x];
                                            const float w = w_ic_p[ky * filter_w + kx];

                                            value += in_v * w;
                                        }
                                    }
                                }

                                *output++ = details::apply_activation(value, fused_activation);
                            }
                        }
                    }
                }
            }
        }

        template <class TQ>
        void dequantize(const TQ *input, float *output, size_t count, const quant_param &param)
        {
            float div = 1.f / param.scale;

            for (size_t i = 0; i < count; i++)
            {
                output[i] = (input[i] - param.zero_point) * div;
            }
        }

        inline void matmul(const float *input_a, const float *input_b, float *output, const float *bias, int32_t a_rows, int32_t a_cols, int32_t b_cols, const value_range<float> &fused_activation)
        {
            for (size_t oy = 0; oy < a_rows; oy++)
            {
                for (size_t ox = 0; ox < b_cols; ox++)
                {
                    float value = bias[ox];
                    for (size_t i = 0; i < a_cols; i++)
                    {
                        const auto a = input_a[oy * a_cols + i];
                        const auto b = input_b[i * b_cols + ox];
                        value += a * b;
                    }

                    output[oy * b_cols + ox] = details::apply_activation(value, fused_activation);
                }
            }
        }

        template <class T>
        void pad_impl(const T *input, T *output, const runtime_shape_t &in_shape, const runtime_paddings_t &paddings, T pad_value)
        {
            runtime_shape_t out_shape = { in_shape[0] + paddings[0].sum(),
                in_shape[1] + paddings[1].sum(),
                in_shape[2] + paddings[2].sum(),
                in_shape[3] + paddings[3].sum() };

            for (int d0 = 0; d0 < out_shape[0]; d0++)
            {
                auto d0_origin = -std::min(0, paddings[0].before);
                auto in0 = input + (d0_origin + d0) * in_shape[1] * in_shape[2] * in_shape[3];

                for (int d1 = 0; d1 < out_shape[1]; d1++)
                {
                    auto d1_origin = -std::min(0, paddings[1].before);
                    auto in1 = in0 + (d1_origin + d1) * in_shape[2] * in_shape[3];

                    for (int d2 = 0; d2 < out_shape[2]; d2++)
                    {
                        auto d2_origin = -std::min(0, paddings[2].before);
                        auto in2 = in1 + (d2_origin + d2) * in_shape[3];

                        for (int d3 = 0; d3 < out_shape[3]; d3++)
                        {
                            auto d3_origin = -std::min(0, paddings[3].before);

                            if (d0 < paddings[0].before || d0 >= out_shape[0] - paddings[0].after
                                || d1 < paddings[1].before || d1 >= out_shape[1] - paddings[1].after
                                || d2 < paddings[2].before || d2 >= out_shape[2] - paddings[2].after
                                || d3 < paddings[3].before || d1 >= out_shape[3] - paddings[3].after)
                                *output++ = pad_value;
                            else
                                *output++ = in2[d3_origin + d3];
                        }
                    }
                }
            }
        }

        inline runtime::kernel_call_result pad(size_t elem_size, const uint8_t *input, uint8_t *output, const runtime_shape_t &in_shape, const runtime_paddings_t &paddings, const scalar &pad_value)
        {
#define PAD_IMPL(T)                                                                                                     \
    pad_impl(reinterpret_cast<const T *>(input), reinterpret_cast<T *>(output), in_shape, paddings, pad_value.as<T>()); \
    return runtime::kcr_done;

            switch (elem_size)
            {
            case 1:
                PAD_IMPL(uint8_t);
            case 2:
                PAD_IMPL(uint16_t);
            case 4:
                PAD_IMPL(uint32_t);
            default:
                return runtime::kcr_error;
            }

#undef PAD_IMPL
        }

        template <class TQ>
        void quantize(const float *input, TQ *output, size_t count, const quant_param &param)
        {
            for (size_t i = 0; i < count; i++)
            {
                int32_t tmp = (int32_t)std::round(input[i] * param.scale + param.zero_point);
                output[i] = std::clamp(tmp, (int32_t)std::numeric_limits<TQ>::lowest(), (int32_t)std::numeric_limits<TQ>::max());
            }
        }

        template <class TReducer>
        void reduce(const float *input, float *output, float init_value, const runtime_shape_t &in_shape, const runtime_shape_t &reduced_shape, TReducer &&reducer)
        {
            std::fill(output, output + kernels::details::compute_size(reduced_shape), init_value);

            for (int32_t d0 = 0; d0 < in_shape[0]; d0++)
            {
                for (int32_t d1 = 0; d1 < in_shape[1]; d1++)
                {
                    for (int32_t d2 = 0; d2 < in_shape[2]; d2++)
                    {
                        for (int32_t d3 = 0; d3 < in_shape[3]; d3++)
                        {
                            runtime_shape_t in_off = { d0, d1, d2, d3 };
                            auto out_off = kernels::details::get_reduced_offset(in_off, reduced_shape);
                            const auto a = input[offset(in_shape, in_off)];
                            auto &b = output[offset(reduced_shape, out_off)];
                            b = reducer(b, a);
                        }
                    }
                }
            }
        }

        template <class TOp>
        void unary(const float *input, float *output, size_t count, TOp &&op)
        {
            for (size_t i = 0; i < count; i++)
                output[i] = op(input[i]);
        }

        template <class TBinaryOp, class TOutputOp>
        void reduce_window2d(const float *input, float *output, float init_value, const runtime_shape_t &in_shape, int32_t filter_h, int32_t filter_w,
            int32_t stride_h, int32_t stride_w, int32_t dilation_h, int32_t dilation_w, const padding &padding_h, const padding &padding_w,
            const value_range<float> &fused_activation, TBinaryOp &&binary_op, TOutputOp &&window_op)
        {
            const auto out_h = kernels::details::get_windowed_output_size(in_shape[2], filter_h, stride_h, dilation_h, padding_h);
            const auto out_w = kernels::details::get_windowed_output_size(in_shape[3], filter_w, stride_w, dilation_w, padding_w);
            runtime_shape_t out_shape { in_shape[0], in_shape[1], out_h, out_w };

            for (int32_t batch = 0; batch < in_shape[0]; batch++)
            {
                for (int32_t oc = 0; oc < in_shape[1]; oc++)
                {
                    for (int32_t oy = 0; oy < out_h; oy++)
                    {
                        for (int32_t ox = 0; ox < out_w; ox++)
                        {
                            const int32_t in_y_origin = (oy * stride_h) - padding_h.before;
                            const int32_t in_x_origin = (ox * stride_w) - padding_w.before;
                            const int32_t filter_y_start = std::max(0, (-in_y_origin + dilation_h - 1) / dilation_h);
                            const int32_t filter_y_end = std::min(filter_h, (in_shape[2] - in_y_origin + dilation_h - 1) / dilation_h);
                            const int32_t filter_x_start = std::max(0, (-in_x_origin + dilation_w - 1) / dilation_w);
                            const int32_t filter_x_end = std::min(filter_w, (in_shape[3] - in_x_origin + dilation_w - 1) / dilation_w);
                            float value = init_value;
                            int32_t kernel_count = 0;

                            for (int32_t ky = filter_y_start; ky < filter_y_end; ky++)
                            {
                                for (int32_t kx = filter_x_start; kx < filter_x_end; kx++)
                                {
                                    const int32_t in_y = in_y_origin + dilation_h * ky;
                                    const int32_t in_x = in_x_origin + dilation_w * kx;

                                    const float in_v = input[offset(in_shape, { batch, oc, in_y, in_x })];

                                    value = binary_op(value, in_v);
                                    kernel_count++;
                                }
                            }

                            output[offset(out_shape, { batch, oc, oy, ox })] = kernels::details::apply_activation(window_op(value, kernel_count), fused_activation);
                        }
                    }
                }
            }
        }

        template <class T>
        void resize_nearest_neighbor_impl(const T *input, T *output, const runtime_shape_t &in_shape, int32_t out_h, int32_t out_w)
        {
            auto height_scale = (float)in_shape[2] / out_h;
            auto width_scale = (float)in_shape[3] / out_w;

            for (int batch = 0; batch < in_shape[0]; batch++)
            {
                auto in_batch = input + batch * in_shape[1] * in_shape[2] * in_shape[3];

                for (int oc = 0; oc < in_shape[1]; oc++)
                {
                    auto in_c = in_batch + oc * in_shape[2] * in_shape[3];

                    for (int oy = 0; oy < out_h; oy++)
                    {
                        auto in_y = std::min((int32_t)std::floor(oy * height_scale), in_shape[2] - 1);
                        auto in_row = in_c + in_y * in_shape[3];

                        for (int ox = 0; ox < out_w; ox++)
                        {
                            auto in_x = std::min((int32_t)std::floor(ox * width_scale), in_shape[3] - 1);
                            *output++ = in_row[in_x];
                        }
                    }
                }
            }
        }

        inline runtime::kernel_call_result resize_nearest_neighbor(size_t elem_size, const uint8_t *input, uint8_t *output, const runtime_shape_t &in_shape, int32_t out_h, int32_t out_w)
        {
#define RESIZE_NN_IMPL(T)                                                                                                    \
    resize_nearest_neighbor_impl(reinterpret_cast<const T *>(input), reinterpret_cast<T *>(output), in_shape, out_h, out_w); \
    return runtime::kcr_done;

            switch (elem_size)
            {
            case 1:
                RESIZE_NN_IMPL(uint8_t);
            case 2:
                RESIZE_NN_IMPL(uint16_t);
            case 4:
                RESIZE_NN_IMPL(uint32_t);
            default:
                return runtime::kcr_error;
            }

#undef RESIZE_NN_IMPL
        }

        inline void resize_bilinear(const float *input, float *output, const runtime_shape_t &in_shape, int32_t out_h, int32_t out_w, bool align_corners)
        {
            auto height_scale = (float)in_shape[2] / out_h;
            auto width_scale = (float)in_shape[3] / out_w;
            if (align_corners && out_h > 1)
                height_scale = (float)(in_shape[2] - 1) / (out_h - 1);
            if (align_corners && out_w > 1)
                width_scale = (float)(in_shape[3] - 1) / (out_w - 1);

            auto destIdx = 0;
            for (int batch = 0; batch < in_shape[0]; batch++)
            {
                auto in_batch = input + batch * in_shape[1] * in_shape[2] * in_shape[3];

                for (int oc = 0; oc < in_shape[1]; oc++)
                {
                    auto in_c = in_batch + oc * in_shape[2] * in_shape[3];

                    for (int oy = 0; oy < out_h; oy++)
                    {
                        auto in_y = oy * height_scale;
                        auto in_y0 = (int)std::floor(in_y);
                        auto in_y1 = std::min(in_y0 + 1, in_shape[2] - 1);

                        for (int ox = 0; ox < out_w; ox++)
                        {
                            auto in_x = ox * width_scale;
                            auto in_x0 = (int)std::floor(in_x);
                            auto in_x1 = std::min(in_x0 + 1, in_shape[3] - 1);

                            auto v0 = in_c[in_y0 * in_shape[3] + in_x0];
                            auto v1 = in_c[in_y1 * in_shape[3] + in_x0];
                            auto v2 = in_c[in_y0 * in_shape[3] + in_x1];
                            auto v3 = in_c[in_y1 * in_shape[3] + in_x1];

                            auto a0 = (1 - (in_y - in_y0)) * (1 - (in_x - in_x0));
                            auto a1 = (in_y - in_y0) * (1 - (in_x - in_x0));
                            auto a2 = (1 - (in_y - in_y0)) * (in_x - in_x0);
                            auto a3 = (in_y - in_y0) * (in_x - in_x0);

                            output[destIdx++] = v0 * a0 + v1 * a1 + v2 * a2 + v3 * a3;
                        }
                    }
                }
            }
        }

        inline void softmax(const float *input, float *output, float beta, int32_t outer_size, size_t inner_size)
        {
            for (size_t batch = 0; batch < outer_size; batch++)
            {
                auto src = input + batch * inner_size;
                auto dest = output + batch * inner_size;

                auto max = *std::max_element(src, src + inner_size);
                float sum = 0;

                for (size_t i = 0; i < inner_size; i++)
                {
                    auto value = std::exp((src[i] - max) * beta);
                    sum += value;
                    dest[i] = value;
                }

                for (size_t i = 0; i < inner_size; i++)
                    dest[i] /= sum;
            }
        }

        template <class T>
        void transpose_impl(const T *input, T *output, const runtime_shape_t &in_shape, const runtime_shape_t &perm)
        {
            runtime_shape_t out_shape;
            for (size_t i = 0; i < 4; i++)
                out_shape[i] = in_shape[perm[i]];

            runtime_shape_t i, o;
            for (o[3] = 0; o[3] < out_shape[3]; o[3]++)
            {
                i[perm[3]] = o[3];
                for (o[2] = 0; o[2] < out_shape[2]; o[2]++)
                {
                    i[perm[2]] = o[2];
                    for (o[1] = 0; o[1] < out_shape[1]; o[1]++)
                    {
                        i[perm[1]] = o[1];
                        for (o[0] = 0; o[0] < out_shape[0]; o[0]++)
                        {
                            i[perm[0]] = o[0];
                            output[offset(out_shape, o)] = input[offset(in_shape, i)];
                        }
                    }
                }
            }
        }

        inline runtime::kernel_call_result transpose(size_t elem_size, const uint8_t *input, uint8_t *output, const runtime_shape_t &in_shape, const runtime_shape_t &perm)
        {
#define TRANSPOSE_IMPL(T)                                                                                       \
    transpose_impl(reinterpret_cast<const T *>(input), reinterpret_cast<T *>(output), in_shape, perm); \
    return runtime::kcr_done;

            switch (elem_size)
            {
            case 1:
                TRANSPOSE_IMPL(uint8_t);
            case 2:
                TRANSPOSE_IMPL(uint16_t);
            case 4:
                TRANSPOSE_IMPL(uint32_t);
            default:
                return runtime::kcr_error;
            }

#undef TRANSPOSE_IMPL
        }
    }
}
}
