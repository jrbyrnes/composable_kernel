// SPDX-License-Identifier: MIT
// Copyright (c) 2018-2022, Advanced Micro Devices, Inc. All rights reserved.

#include <cstdlib>
#include <iostream>
#include <numeric>
#include <type_traits>

#include "ck/ck.hpp"
#include "ck/tensor_operation/gpu/device/tensor_layout.hpp"
#include "ck/tensor_operation/gpu/element/element_wise_operation.hpp"

#include "ck/library/utility/check_err.hpp"
#include "ck/library/utility/device_memory.hpp"
#include "ck/library/utility/host_tensor.hpp"
#include "ck/library/utility/host_tensor_generator.hpp"
#include "ck/library/utility/convolution_parameter.hpp"
#include "ck/library/reference_tensor_operation/cpu/reference_conv_fwd.hpp"

void print_helper_msg()
{
    std::cout << "arg1: verification (0=no, 1=yes)\n"
              << "arg2: initialization (0=no init, 1=integer value, 2=decimal value)\n"
              << "arg3: time kernel (0=no, 1=yes)\n"
              << ck::utils::conv::get_conv_param_parser_helper_msg() << std::endl;
}

template <ck::index_t NDimSpatial,
          typename InKernelDataType,
          typename WeiKernelDataType,
          typename CShuffleDataType,
          typename OutKernelDataType,
          typename InElementOp,
          typename WeiElementOp,
          typename OutElementOp,
          typename InUserDataType,
          typename WeiUserDataType,
          typename OutUserDataType,
          typename DeviceConvNDFwdInstance>
int run_grouped_conv_fwd_bias_relu_add(bool do_verification,
                                       int init_method,
                                       bool time_kernel,
                                       const ck::utils::conv::ConvParam& conv_param,
                                       const HostTensorDescriptor& in_g_n_c_wis_desc,
                                       const HostTensorDescriptor& wei_g_k_c_xs_desc,
                                       const HostTensorDescriptor& bias_g_n_k_wos_desc,
                                       const HostTensorDescriptor& residual_g_n_k_wos_desc,
                                       const HostTensorDescriptor& out_g_n_k_wos_desc,
                                       const InElementOp& in_element_op,
                                       const WeiElementOp& wei_element_op,
                                       const OutElementOp& out_element_op)
{
    Tensor<InUserDataType> in(in_g_n_c_wis_desc);
    Tensor<WeiUserDataType> wei(wei_g_k_c_xs_desc);
    Tensor<OutUserDataType> bias(bias_g_n_k_wos_desc);
    Tensor<OutUserDataType> residual(residual_g_n_k_wos_desc);
    Tensor<OutUserDataType> out_host(out_g_n_k_wos_desc);
    Tensor<OutKernelDataType> out_device(out_g_n_k_wos_desc);

    std::cout << "in: " << in.mDesc << std::endl;
    std::cout << "wei: " << wei.mDesc << std::endl;
    std::cout << "bias: " << bias.mDesc << std::endl;
    std::cout << "residual: " << residual.mDesc << std::endl;
    std::cout << "out: " << out_host.mDesc << std::endl;

    switch(init_method)
    {
    case 0: break;
    case 1:
        in.GenerateTensorValue(GeneratorTensor_2<InUserDataType>{-5, 5});
        wei.GenerateTensorValue(GeneratorTensor_2<WeiUserDataType>{-5, 5});
        bias.GenerateTensorValue(GeneratorTensor_2<OutUserDataType>{-5, 5});
        break;
    default:
        in.GenerateTensorValue(GeneratorTensor_3<InUserDataType>{0.0, 1.0});
        wei.GenerateTensorValue(GeneratorTensor_3<WeiUserDataType>{-0.5, 0.5});
        bias.GenerateTensorValue(GeneratorTensor_3<OutUserDataType>{-0.5, 0.5});
    }

    DeviceMem in_device_buf(sizeof(InKernelDataType) * in.mDesc.GetElementSpaceSize());
    DeviceMem wei_device_buf(sizeof(WeiKernelDataType) * wei.mDesc.GetElementSpaceSize());
    DeviceMem bias_device_buf(sizeof(OutKernelDataType) * bias.mDesc.GetElementSpaceSize());
    DeviceMem residual_device_buf(sizeof(OutKernelDataType) * residual.mDesc.GetElementSpaceSize());
    DeviceMem out_device_buf(sizeof(OutKernelDataType) * out_device.mDesc.GetElementSpaceSize());

#ifdef CK_EXPERIMENTAL_BIT_INT_EXTENSION_INT4
    const Tensor<InKernelDataType> in_converted(in);
    const Tensor<WeiKernelDataType> wei_converted(wei);
    const Tensor<OutKernelDataType> bias_converted(bias);
    const Tensor<OutKernelDataType> residual_converted(residual);

    in_device_buf.ToDevice(in_converted.mData.data());
    wei_device_buf.ToDevice(wei_converted.mData.data());
    bias_device_buf.ToDevice(bias_converted.mData.data());
    residual_device_buf.ToDevice(residual_converted.mData.data());
#else  // CK_EXPERIMENTAL_BIT_INT_EXTENSION_INT4
    in_device_buf.ToDevice(in.mData.data());
    wei_device_buf.ToDevice(wei.mData.data());
    bias_device_buf.ToDevice(bias.mData.data());
    residual_device_buf.ToDevice(residual.mData.data());
#endif //  CK_EXPERIMENTAL_BIT_INT_EXTENSION_INT4

    std::array<ck::index_t, NDimSpatial + 3> a_g_n_c_wis_lengths{};
    std::array<ck::index_t, NDimSpatial + 3> a_g_n_c_wis_strides{};
    std::array<ck::index_t, NDimSpatial + 3> b_g_k_c_xs_lengths{};
    std::array<ck::index_t, NDimSpatial + 3> b_g_k_c_xs_strides{};
    std::array<ck::index_t, NDimSpatial + 3> d0_g_n_k_wos_lengths{};
    std::array<ck::index_t, NDimSpatial + 3> d0_g_n_k_wos_strides{};
    std::array<ck::index_t, NDimSpatial + 3> d1_g_n_k_wos_lengths{};
    std::array<ck::index_t, NDimSpatial + 3> d1_g_n_k_wos_strides{};
    std::array<ck::index_t, NDimSpatial + 3> e_g_n_k_wos_lengths{};
    std::array<ck::index_t, NDimSpatial + 3> e_g_n_k_wos_strides{};
    std::array<ck::index_t, NDimSpatial> conv_filter_strides{};
    std::array<ck::index_t, NDimSpatial> conv_filter_dilations{};
    std::array<ck::index_t, NDimSpatial> input_left_pads{};
    std::array<ck::index_t, NDimSpatial> input_right_pads{};

    auto copy = [](auto& x, auto& y) { std::copy(x.begin(), x.end(), y.begin()); };

    copy(in_g_n_c_wis_desc.GetLengths(), a_g_n_c_wis_lengths);
    copy(in_g_n_c_wis_desc.GetStrides(), a_g_n_c_wis_strides);
    copy(wei_g_k_c_xs_desc.GetLengths(), b_g_k_c_xs_lengths);
    copy(wei_g_k_c_xs_desc.GetStrides(), b_g_k_c_xs_strides);
    copy(bias_g_n_k_wos_desc.GetLengths(), d0_g_n_k_wos_lengths);
    copy(bias_g_n_k_wos_desc.GetStrides(), d0_g_n_k_wos_strides);
    copy(residual_g_n_k_wos_desc.GetLengths(), d1_g_n_k_wos_lengths);
    copy(residual_g_n_k_wos_desc.GetStrides(), d1_g_n_k_wos_strides);
    copy(out_g_n_k_wos_desc.GetLengths(), e_g_n_k_wos_lengths);
    copy(out_g_n_k_wos_desc.GetStrides(), e_g_n_k_wos_strides);
    copy(conv_param.conv_filter_strides_, conv_filter_strides);
    copy(conv_param.conv_filter_dilations_, conv_filter_dilations);
    copy(conv_param.input_left_pads_, input_left_pads);
    copy(conv_param.input_right_pads_, input_right_pads);

    // do Conv
    auto conv    = DeviceConvNDFwdInstance{};
    auto invoker = conv.MakeInvoker();
    auto argument =
        conv.MakeArgument(in_device_buf.GetDeviceBuffer(),
                          wei_device_buf.GetDeviceBuffer(),
                          std::array<const void*, 2>{bias_device_buf.GetDeviceBuffer(),
                                                     residual_device_buf.GetDeviceBuffer()},
                          out_device_buf.GetDeviceBuffer(),
                          a_g_n_c_wis_lengths,
                          a_g_n_c_wis_strides,
                          b_g_k_c_xs_lengths,
                          b_g_k_c_xs_strides,
                          std::array<std::array<ck::index_t, NDimSpatial + 3>, 2>{
                              {d0_g_n_k_wos_lengths, d1_g_n_k_wos_lengths}},
                          std::array<std::array<ck::index_t, NDimSpatial + 3>, 2>{
                              {d0_g_n_k_wos_strides, d1_g_n_k_wos_strides}},
                          e_g_n_k_wos_lengths,
                          e_g_n_k_wos_strides,
                          conv_filter_strides,
                          conv_filter_dilations,
                          input_left_pads,
                          input_right_pads,
                          in_element_op,
                          wei_element_op,
                          out_element_op);

    if(!conv.IsSupportedArgument(argument))
    {
        throw std::runtime_error(
            "wrong! device_conv with the specified compilation parameters does "
            "not support this Conv problem");
    }

    float avg_time = invoker.Run(argument, StreamConfig{nullptr, time_kernel});

    std::size_t flop      = conv_param.GetFlops();
    std::size_t num_btype = conv_param.GetByte<InUserDataType, WeiUserDataType, OutUserDataType>();

    float tflops     = static_cast<float>(flop) / 1.E9 / avg_time;
    float gb_per_sec = num_btype / 1.E6 / avg_time;
    std::cout << "Perf: " << avg_time << " ms, " << tflops << " TFlops, " << gb_per_sec << " GB/s, "
              << conv.GetTypeString() << std::endl;

    if(do_verification)
    {
        using PassThrough = ck::tensor_operation::element_wise::PassThrough;

        Tensor<CShuffleDataType> c_host(out_g_n_k_wos_desc);

        auto ref_conv = ck::tensor_operation::host::ReferenceConvFwd<NDimSpatial,
                                                                     InUserDataType,
                                                                     WeiUserDataType,
                                                                     CShuffleDataType,
                                                                     InElementOp,
                                                                     WeiElementOp,
                                                                     PassThrough>();

        auto ref_invoker  = ref_conv.MakeInvoker();
        auto ref_argument = ref_conv.MakeArgument(in,
                                                  wei,
                                                  c_host,
                                                  conv_param.conv_filter_strides_,
                                                  conv_param.conv_filter_dilations_,
                                                  conv_param.input_left_pads_,
                                                  conv_param.input_right_pads_,
                                                  in_element_op,
                                                  wei_element_op,
                                                  PassThrough{});

        ref_invoker.Run(ref_argument);

        // TODO: implement elementwise operation for host
        out_host.ForEach([&](auto&, auto idx) {
            out_element_op(out_host(idx), c_host(idx), bias(idx), residual(idx));
        });

        out_device_buf.FromDevice(out_device.mData.data());

#ifdef CK_EXPERIMENTAL_BIT_INT_EXTENSION_INT4
        const Tensor<OutUserDataType> out_device_converted(out_device);

        return ck::utils::check_err(out_device_converted.mData,
                                    out_host.mData,
                                    "Error: incorrect results!",
                                    1e-5f,
                                    1e-4f)
                   ? 0
                   : 1;
#else  // CK_EXPERIMENTAL_BIT_INT_EXTENSION_INT4
        return ck::utils::check_err(
                   out_device.mData, out_host.mData, "Error: incorrect results!", 1e-5f, 1e-4f)
                   ? 0
                   : 1;
#endif // CK_EXPERIMENTAL_BIT_INT_EXTENSION_INT4
    }

    return 0;
}
