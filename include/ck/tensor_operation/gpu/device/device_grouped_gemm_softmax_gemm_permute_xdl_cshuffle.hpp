// SPDX-License-Identifier: MIT
// Copyright (c) 2018-2022, Advanced Micro Devices, Inc. All rights reserved.

#pragma once

#include <iostream>
#include <sstream>

#include "ck/utility/common_header.hpp"
#include "ck/tensor_description/tensor_descriptor.hpp"
#include "ck/tensor_description/tensor_descriptor_helper.hpp"
#include "ck/tensor_operation/gpu/device/tensor_layout.hpp"
#include "ck/tensor_operation/gpu/device/device_grouped_gemm_softmax_gemm_permute.hpp"
#include "ck/tensor_operation/gpu/device/gemm_specialization.hpp"
#include "ck/tensor_operation/gpu/device/matrix_padder.hpp"
#include "ck/tensor_operation/gpu/grid/gridwise_batched_gemm_softmax_gemm_xdl_cshuffle_v1.hpp"
#include "ck/host_utility/device_prop.hpp"
#include "ck/host_utility/kernel_launch.hpp"

namespace ck {
namespace tensor_operation {
namespace device {

template <typename GridwiseGemm,
          typename GroupKernelArg,
          typename AElementwiseOperation,
          typename BElementwiseOperation,
          typename AccElementwiseOperation,
          typename B1ElementwiseOperation,
          typename CElementwiseOperation,
          bool HasMainKBlockLoop>
__global__ void
#if CK_USE_LAUNCH_BOUNDS
    __launch_bounds__(CK_MAX_THREAD_PER_BLOCK, CK_MIN_BLOCK_PER_CU)
#endif
        kernel_grouped_gemm_softmax_gemm_xdl_cshuffle_v1(
            const void CK_CONSTANT_ADDRESS_SPACE* group_kernel_args,
            const index_t group_count,
            const AElementwiseOperation a_element_op,
            const BElementwiseOperation b_element_op,
            const AccElementwiseOperation acc_element_op,
            const B1ElementwiseOperation b1_element_op,
            const CElementwiseOperation c_element_op)
{
#if(!defined(__HIP_DEVICE_COMPILE__) || defined(__gfx908__) || defined(__gfx90a__))
    __shared__ char p_shared[GridwiseGemm::GetSharedMemoryNumberOfByte()];

    const index_t block_id = get_block_1d_id();

    const auto arg_ptr = reinterpret_cast<const GroupKernelArg*>(
        cast_pointer_to_generic_address_space(group_kernel_args));

    index_t left     = 0;
    index_t right    = group_count;
    index_t group_id = index_t((left + right) / 2);

    while((!(block_id >= arg_ptr[group_id].block_start_ &&
             block_id < arg_ptr[group_id].block_end_)) &&
          left <= right)
    {
        if(block_id < arg_ptr[group_id].block_start_)
        {
            right = group_id;
        }
        else
        {
            left = group_id;
        }
        group_id = index_t((left + right) / 2);
    }

    // per-group batch offset
    const index_t num_blocks_per_batch = arg_ptr[group_id].num_blocks_per_batch_;
    const index_t g_idx                = __builtin_amdgcn_readfirstlane(
        (block_id - arg_ptr[group_id].block_start_) / num_blocks_per_batch);

    const long_index_t a_batch_offset = __builtin_amdgcn_readfirstlane(
        static_cast<long_index_t>(arg_ptr[group_id].compute_base_ptr_of_batch_.GetABasePtr(g_idx)));
    const long_index_t b_batch_offset = __builtin_amdgcn_readfirstlane(
        static_cast<long_index_t>(arg_ptr[group_id].compute_base_ptr_of_batch_.GetBBasePtr(g_idx)));
    const long_index_t b1_batch_offset = __builtin_amdgcn_readfirstlane(static_cast<long_index_t>(
        arg_ptr[group_id].compute_base_ptr_of_batch_.GetB1BasePtr(g_idx)));
    const long_index_t c_batch_offset  = __builtin_amdgcn_readfirstlane(
        static_cast<long_index_t>(arg_ptr[group_id].compute_base_ptr_of_batch_.GetCBasePtr(g_idx)));

    GridwiseGemm::template Run<HasMainKBlockLoop>(
        arg_ptr[group_id].p_a_grid_ + a_batch_offset,
        arg_ptr[group_id].p_b_grid_ + b_batch_offset,
        arg_ptr[group_id].p_b1_grid_ + b1_batch_offset,
        arg_ptr[group_id].p_c_grid_ + c_batch_offset,
        p_shared,
        a_element_op,
        b_element_op,
        acc_element_op,
        b1_element_op,
        c_element_op,
        arg_ptr[group_id].a_grid_desc_ak0_m_ak1_,
        arg_ptr[group_id].b_grid_desc_bk0_n_bk1_,
        arg_ptr[group_id].b1_grid_desc_bk0_n_bk1_,
        arg_ptr[group_id].c_grid_desc_mblock_mperblock_nblock_nperblock_,
        arg_ptr[group_id].block_2_ctile_map_,
        arg_ptr[group_id].c0_matrix_mask_);
#else
    ignore = group_kernel_args;
    ignore = group_count;
    ignore = a_element_op;
    ignore = b_element_op;
    ignore = acc_element_op;
    ignore = b1_element_op;
    ignore = c_element_op;
#endif // end of if (defined(__gfx908__) || defined(__gfx90a__))
}

// Computes C = A * B0 * B1
//              ^^^^^^ (Acc0)
//              ^^^^^^^^^^^ (Acc1)
template <typename ALayout,
          typename BLayout, // B0Layout
          typename B1Layout,
          typename CPermuteNumDims_G_M_Gemm1N, // Sequence<NumDimG, NumDimM, NumDimGemm1N>
          typename ADataType,
          typename BDataType,
          typename B1DataType,
          typename CDataType,
          typename GemmAccDataType,
          typename CShuffleDataType,
          typename AElementwiseOperation,
          typename BElementwiseOperation,
          typename AccElementwiseOperation,
          typename B1ElementwiseOperation,
          typename CElementwiseOperation,
          GemmSpecialization GemmSpec,
          index_t NumGemmKPrefetchStage,
          index_t BlockSize,
          index_t MPerBlock,
          index_t NPerBlock, // Gemm0NPerBlock
          index_t KPerBlock, // Gemm0KPerBlock
          index_t Gemm1NPerBlock,
          index_t Gemm1KPerBlock,
          index_t AK1,
          index_t BK1,
          index_t B1K1,
          index_t MPerXDL,
          index_t NPerXDL,
          index_t MXdlPerWave,
          index_t NXdlPerWave,
          index_t Gemm1NXdlPerWave,
          typename ABlockTransferThreadClusterLengths_AK0_M_AK1,
          typename ABlockTransferThreadClusterArrangeOrder,
          typename ABlockTransferSrcAccessOrder,
          index_t ABlockTransferSrcVectorDim,
          index_t ABlockTransferSrcScalarPerVector,
          index_t ABlockTransferDstScalarPerVector_AK1,
          bool ABlockLdsExtraM,
          typename BBlockTransferThreadClusterLengths_BK0_N_BK1,
          typename BBlockTransferThreadClusterArrangeOrder,
          typename BBlockTransferSrcAccessOrder,
          index_t BBlockTransferSrcVectorDim,
          index_t BBlockTransferSrcScalarPerVector,
          index_t BBlockTransferDstScalarPerVector_BK1,
          bool BBlockLdsExtraN,
          typename B1BlockTransferThreadClusterLengths_BK0_N_BK1,
          typename B1BlockTransferThreadClusterArrangeOrder,
          typename B1BlockTransferSrcAccessOrder,
          index_t B1BlockTransferSrcVectorDim,
          index_t B1BlockTransferSrcScalarPerVector,
          index_t B1BlockTransferDstScalarPerVector_BK1,
          bool B1BlockLdsExtraN,
          index_t CShuffleMXdlPerWavePerShuffle,
          index_t CShuffleNXdlPerWavePerShuffle,
          typename CShuffleBlockTransferClusterLengths_MBlock_MPerBlock_NBlock_NPerBlock,
          index_t CShuffleBlockTransferScalarPerVector_NPerBlock,
          bool MaskOutUpperTriangle,
          LoopScheduler LoopSched = LoopScheduler::Default>
struct DeviceGroupedGemmSoftmaxGemmPermute_Xdl_CShuffle
    : public DeviceGroupedGemmSoftmaxGemmPermute<ALayout,
                                                 BLayout,
                                                 B1Layout,
                                                 CPermuteNumDims_G_M_Gemm1N,
                                                 ADataType,
                                                 BDataType,
                                                 B1DataType,
                                                 CDataType,
                                                 AElementwiseOperation,
                                                 BElementwiseOperation,
                                                 AccElementwiseOperation,
                                                 B1ElementwiseOperation,
                                                 CElementwiseOperation>
{
    using DeviceOp = DeviceGroupedGemmSoftmaxGemmPermute_Xdl_CShuffle;
    using ProblemDesc =
        typename DeviceGroupedGemmSoftmaxGemmPermute<ALayout,
                                                     BLayout,
                                                     B1Layout,
                                                     CPermuteNumDims_G_M_Gemm1N,
                                                     ADataType,
                                                     BDataType,
                                                     B1DataType,
                                                     CDataType,
                                                     AElementwiseOperation,
                                                     BElementwiseOperation,
                                                     AccElementwiseOperation,
                                                     B1ElementwiseOperation,
                                                     CElementwiseOperation>::ProblemDesc;

    static constexpr auto I0 = Number<0>{};
    static constexpr auto I1 = Number<1>{};
    static constexpr auto I2 = Number<2>{};

    static constexpr auto matrix_padder =
        GemmGemmPadder<GemmSpec, index_t, index_t, index_t, index_t>{
            MPerBlock, NPerBlock, KPerBlock, Gemm1NPerBlock};

    static auto MakeAGridDescriptor_AK0_M_AK1(index_t MRaw, index_t KRaw, index_t StrideA)
    {
        const auto a_grid_desc_mraw_kraw = [&]() {
            if constexpr(is_same_v<tensor_layout::gemm::RowMajor, ALayout>)
            {
                return make_naive_tensor_descriptor(make_tuple(MRaw, KRaw),
                                                    make_tuple(StrideA, I1));
            }
            else if constexpr(is_same_v<tensor_layout::gemm::ColumnMajor, ALayout>)
            {
                return make_naive_tensor_descriptor(make_tuple(MRaw, KRaw),
                                                    make_tuple(I1, StrideA));
            }
        }();

        const auto a_grid_desc_m_k = matrix_padder.PadADescriptor_M_K(a_grid_desc_mraw_kraw);

        const auto M = a_grid_desc_m_k.GetLength(I0);
        const auto K = a_grid_desc_m_k.GetLength(I1);

        const auto AK0 = K / AK1;

        return transform_tensor_descriptor(a_grid_desc_m_k,
                                           make_tuple(make_unmerge_transform(make_tuple(AK0, AK1)),
                                                      make_pass_through_transform(M)),
                                           make_tuple(Sequence<1>{}, Sequence<0>{}),
                                           make_tuple(Sequence<0, 2>{}, Sequence<1>{}));
    }

    static auto MakeBGridDescriptor_BK0_N_BK1(index_t KRaw, index_t NRaw, index_t StrideB)
    {
        const auto b_grid_desc_nraw_kraw = [&]() {
            if constexpr(is_same<tensor_layout::gemm::RowMajor, BLayout>::value)
            {
                return make_naive_tensor_descriptor(make_tuple(NRaw, KRaw),
                                                    make_tuple(I1, StrideB));
            }
            else if constexpr(is_same<tensor_layout::gemm::ColumnMajor, BLayout>::value)
            {
                return make_naive_tensor_descriptor(make_tuple(NRaw, KRaw),
                                                    make_tuple(StrideB, I1));
            }
        }();

        const auto b_grid_desc_n_k = matrix_padder.PadBDescriptor_N_K(b_grid_desc_nraw_kraw);

        const auto N = b_grid_desc_n_k.GetLength(I0);
        const auto K = b_grid_desc_n_k.GetLength(I1);

        const auto BK0 = K / BK1;

        return transform_tensor_descriptor(b_grid_desc_n_k,
                                           make_tuple(make_unmerge_transform(make_tuple(BK0, BK1)),
                                                      make_pass_through_transform(N)),
                                           make_tuple(Sequence<1>{}, Sequence<0>{}),
                                           make_tuple(Sequence<0, 2>{}, Sequence<1>{}));
    }

    // Args: Gemm1KRaw, Gemm1NRaw, StrideB1
    static auto MakeB1GridDescriptor_BK0_N_BK1(index_t KRaw, index_t NRaw, index_t StrideB)
    {
        const auto b1_grid_desc_nraw_kraw = [&]() {
            if constexpr(is_same<tensor_layout::gemm::RowMajor, B1Layout>::value)
            {
                return make_naive_tensor_descriptor(make_tuple(NRaw, KRaw),
                                                    make_tuple(I1, StrideB));
            }
            else if constexpr(is_same<tensor_layout::gemm::ColumnMajor, B1Layout>::value)
            {
                return make_naive_tensor_descriptor(make_tuple(NRaw, KRaw),
                                                    make_tuple(StrideB, I1));
            }
        }();

        const auto b1_grid_desc_n_k = matrix_padder.PadB1Descriptor_N_K(b1_grid_desc_nraw_kraw);

        const auto N = b1_grid_desc_n_k.GetLength(I0);
        const auto K = b1_grid_desc_n_k.GetLength(I1);

        const auto B1K0 = K / B1K1;

        return transform_tensor_descriptor(
            b1_grid_desc_n_k,
            make_tuple(make_unmerge_transform(make_tuple(B1K0, B1K1)),
                       make_pass_through_transform(N)),
            make_tuple(Sequence<1>{}, Sequence<0>{}),
            make_tuple(Sequence<0, 2>{}, Sequence<1>{}));
    }

    // assume C[G0, G1, ..., M0, M1, M2, ..., N0, N1, N2...]
    static auto MakeCGridDescriptor_M_N(const std::vector<index_t>& c_gs_ms_ns_lengths_vec,
                                        const std::vector<index_t>& c_gs_ms_ns_strides_vec)
    {
        constexpr index_t NumDimG = CPermuteNumDims_G_M_Gemm1N::At(I0);
        constexpr index_t NumDimM = CPermuteNumDims_G_M_Gemm1N::At(I1);
        constexpr index_t NumDimN = CPermuteNumDims_G_M_Gemm1N::At(I2); // NumDimGemm1N

        assert(c_gs_ms_ns_lengths_vec.size() == NumDimG + NumDimM + NumDimN &&
               c_gs_ms_ns_strides_vec.size() == NumDimG + NumDimM + NumDimN);

        const auto to_tuple = [&](auto& vec, auto start, auto end) {
            return generate_tuple([&](auto i) { return vec[start + i]; }, Number<end - start>{});
        };

        const auto c_ms_ns_lengths = to_tuple(
            c_gs_ms_ns_lengths_vec, Number<NumDimG>{}, Number<NumDimG + NumDimM + NumDimN>{});
        const auto c_ms_ns_strides = to_tuple(
            c_gs_ms_ns_strides_vec, Number<NumDimG>{}, Number<NumDimG + NumDimM + NumDimN>{});

        // dimension Ids for M0, M1, ...
        constexpr auto mDimIds = typename arithmetic_sequence_gen<0, NumDimM, 1>::type{};

        // dimension Ids for N0, N1, ...
        constexpr auto nDimIds =
            typename arithmetic_sequence_gen<NumDimM, NumDimM + NumDimN, 1>::type{};

        // lengths for M0, M1, ...
        const auto mLengths = get_container_subset(c_ms_ns_lengths, mDimIds);

        // lengths for K0, K1, ...
        const auto nLengths = get_container_subset(c_ms_ns_lengths, nDimIds);

        // naive tensor C[M0, M1, M2, ..., N0, N1, N2...]
        const auto c_grid_desc_ms_ns =
            make_naive_tensor_descriptor(c_ms_ns_lengths, c_ms_ns_strides);

        // transformed tensor C[MRaw = M0 * M1 * M2 * ... , NRaw = N0 * N1 * N2 * ...]
        const auto c_grid_desc_mraw_nraw = transform_tensor_descriptor(
            c_grid_desc_ms_ns,
            make_tuple(make_merge_transform(mLengths), make_merge_transform(nLengths)),
            make_tuple(mDimIds, nDimIds),
            make_tuple(Sequence<0>{}, Sequence<1>{}));

        return matrix_padder.PadCDescriptor_M_N(c_grid_desc_mraw_nraw);
    }

    // assume C[G0, G1, ..., M0, M1, M2, ..., N0, N1, N2...]
    static auto MakeCGridDescriptor_G_M_N(const std::vector<index_t>& c_gs_ms_ns_lengths_vec,
                                          const std::vector<index_t>& c_gs_ms_ns_strides_vec)
    {
        constexpr index_t NumDimG = CPermuteNumDims_G_M_Gemm1N::At(I0);
        constexpr index_t NumDimM = CPermuteNumDims_G_M_Gemm1N::At(I1);
        constexpr index_t NumDimN = CPermuteNumDims_G_M_Gemm1N::At(I2); // NumDimGemm1N

        assert(c_gs_ms_ns_lengths_vec.size() == NumDimG + NumDimM + NumDimN &&
               c_gs_ms_ns_strides_vec.size() == NumDimG + NumDimM + NumDimN);

        const auto to_tuple = [&](auto& vec, auto start, auto end) {
            return generate_tuple([&](auto i) { return vec[start + i]; }, Number<end - start>{});
        };

        const auto c_gs_ms_ns_lengths =
            to_tuple(c_gs_ms_ns_lengths_vec, Number<0>{}, Number<NumDimG + NumDimM + NumDimN>{});
        const auto c_gs_ms_ns_strides =
            to_tuple(c_gs_ms_ns_strides_vec, Number<0>{}, Number<NumDimG + NumDimM + NumDimN>{});

        // dimension Ids for G0, G1, ...
        constexpr auto gDimIds = typename arithmetic_sequence_gen<0, NumDimG, 1>::type{};

        // dimension Ids for M0, M1, ...
        constexpr auto mDimIds =
            typename arithmetic_sequence_gen<NumDimG, NumDimG + NumDimM, 1>::type{};

        // dimension Ids for N0, N1, ...
        constexpr auto nDimIds = typename arithmetic_sequence_gen<NumDimG + NumDimM,
                                                                  NumDimG + NumDimM + NumDimN,
                                                                  1>::type{};

        // lengths for G0, G1, ...
        const auto gLengths = get_container_subset(c_gs_ms_ns_lengths, gDimIds);

        // lengths for M0, M1, ...
        const auto mLengths = get_container_subset(c_gs_ms_ns_lengths, mDimIds);

        // lengths for K0, K1, ...
        const auto nLengths = get_container_subset(c_gs_ms_ns_lengths, nDimIds);

        // naive tensor C[G0, G1, ..., M0, M1, M2, ..., N0, N1, N2...]
        const auto c_grid_desc_gs_ms_ns =
            make_naive_tensor_descriptor(c_gs_ms_ns_lengths, c_gs_ms_ns_strides);

        // transformed tensor C[G = G0 * G1 * ..., MRaw = M0 * M1 * M2 * ... , NRaw = N0 * N1 *
        // N2 * ...]
        const auto c_grid_desc_g_mraw_nraw =
            transform_tensor_descriptor(c_grid_desc_gs_ms_ns,
                                        make_tuple(make_merge_transform(gLengths),
                                                   make_merge_transform(mLengths),
                                                   make_merge_transform(nLengths)),
                                        make_tuple(gDimIds, mDimIds, nDimIds),
                                        make_tuple(Sequence<0>{}, Sequence<1>{}, Sequence<2>{}));

        // this desc is only for calculating batch offset so no padding needed
        return c_grid_desc_g_mraw_nraw;
    }

    using AGridDesc_AK0_M_AK1  = decltype(MakeAGridDescriptor_AK0_M_AK1(1, 1, 1));
    using BGridDesc_BK0_N_BK1  = decltype(MakeBGridDescriptor_BK0_N_BK1(1, 1, 1));
    using B1GridDesc_BK0_N_BK1 = decltype(MakeB1GridDescriptor_BK0_N_BK1(1, 1, 1));
    using CGridDesc_M_N        = decltype(MakeCGridDescriptor_M_N({}, {}));
    using CGridDesc_G_M_N      = decltype(MakeCGridDescriptor_G_M_N({}, {}));

    // to track the points which need to be set to -inf on C0
    // Note: no need to reset M padding value, because they will not be stored out.
    struct C0MatrixMask
    {
        C0MatrixMask(index_t NRaw) : NRaw_(NRaw) {}

        __host__ __device__ bool IsUpperTriangle(index_t m, index_t n) const { return n > m; }

        __host__ __device__ bool IsNOutOfBound(/*index_t m, */ index_t n) const
        {
            return n >= NRaw_;
        }

        __host__ __device__ bool IsMaskedElement(index_t m, index_t n) const
        {
            return IsUpperTriangle(m, n) || IsNOutOfBound(n);
        }

        private:
        // index_t MRaw_;
        index_t NRaw_;
    };

    struct ComputeBasePtrOfStridedBatch
    {
        ComputeBasePtrOfStridedBatch(index_t BatchStrideA,
                                     index_t BatchStrideB,
                                     index_t BatchStrideB1,
                                     CGridDesc_G_M_N c_grid_desc_g_m_n)
            : BatchStrideA_(BatchStrideA),
              BatchStrideB_(BatchStrideB),
              BatchStrideB1_(BatchStrideB1),
              c_grid_desc_g_m_n_(c_grid_desc_g_m_n)
        {
        }

        __host__ __device__ constexpr long_index_t GetABasePtr(index_t g_idx) const
        {
            return g_idx * static_cast<long_index_t>(BatchStrideA_);
        }

        __host__ __device__ constexpr long_index_t GetBBasePtr(index_t g_idx) const
        {
            return g_idx * static_cast<long_index_t>(BatchStrideB_);
        }

        __host__ __device__ constexpr long_index_t GetB1BasePtr(index_t g_idx) const
        {
            return g_idx * static_cast<long_index_t>(BatchStrideB1_);
        }

        __host__ __device__ constexpr long_index_t GetCBasePtr(index_t g_idx) const
        {
            return c_grid_desc_g_m_n_.CalculateOffset(make_multi_index(g_idx, 0, 0));
        }

        private:
        index_t BatchStrideA_;
        index_t BatchStrideB_;
        index_t BatchStrideB1_;
        CGridDesc_G_M_N c_grid_desc_g_m_n_;
    };

    // GridwiseGemm
    using GridwiseGemm = GridwiseBatchedGemmSoftmaxGemm_Xdl_CShuffle<
        ADataType, // TODO: distinguish A/B datatype
        GemmAccDataType,
        CShuffleDataType,
        CDataType,
        AElementwiseOperation,
        BElementwiseOperation,
        AccElementwiseOperation,
        B1ElementwiseOperation,
        CElementwiseOperation,
        InMemoryDataOperationEnum::Set,
        AGridDesc_AK0_M_AK1,
        BGridDesc_BK0_N_BK1,
        B1GridDesc_BK0_N_BK1,
        CGridDesc_M_N,
        NumGemmKPrefetchStage,
        BlockSize,
        MPerBlock,
        NPerBlock,
        KPerBlock,
        Gemm1NPerBlock,
        Gemm1KPerBlock,
        AK1,
        BK1,
        B1K1,
        MPerXDL,
        NPerXDL,
        MXdlPerWave,
        NXdlPerWave,
        Gemm1NXdlPerWave,
        ABlockTransferThreadClusterLengths_AK0_M_AK1,
        ABlockTransferThreadClusterArrangeOrder,
        ABlockTransferSrcAccessOrder,
        ABlockTransferSrcVectorDim,
        ABlockTransferSrcScalarPerVector,
        ABlockTransferDstScalarPerVector_AK1,
        true,
        ABlockLdsExtraM,
        BBlockTransferThreadClusterLengths_BK0_N_BK1,
        BBlockTransferThreadClusterArrangeOrder,
        BBlockTransferSrcAccessOrder,
        BBlockTransferSrcVectorDim,
        BBlockTransferSrcScalarPerVector,
        BBlockTransferDstScalarPerVector_BK1,
        true,
        BBlockLdsExtraN,
        B1BlockTransferThreadClusterLengths_BK0_N_BK1,
        B1BlockTransferThreadClusterArrangeOrder,
        B1BlockTransferSrcAccessOrder,
        B1BlockTransferSrcVectorDim,
        B1BlockTransferSrcScalarPerVector,
        B1BlockTransferDstScalarPerVector_BK1,
        false,
        B1BlockLdsExtraN,
        CShuffleMXdlPerWavePerShuffle,
        CShuffleNXdlPerWavePerShuffle,
        CShuffleBlockTransferClusterLengths_MBlock_MPerBlock_NBlock_NPerBlock,
        CShuffleBlockTransferScalarPerVector_NPerBlock,
        LoopSched,
        matrix_padder.PadN,
        MaskOutUpperTriangle>;

    using Block2CTileMap = OffsettedBlockToCTileMap<typename GridwiseGemm::DefaultBlock2CTileMap>;

    struct GroupKernelArg
    {
        // pointers
        const ADataType* p_a_grid_;
        const BDataType* p_b_grid_;
        const B1DataType* p_b1_grid_;
        CDataType* p_c_grid_;

        // tensor descriptors for block/thread-wise copy
        AGridDesc_AK0_M_AK1 a_grid_desc_ak0_m_ak1_;
        BGridDesc_BK0_N_BK1 b_grid_desc_bk0_n_bk1_;
        B1GridDesc_BK0_N_BK1 b1_grid_desc_bk0_n_bk1_;
        typename GridwiseGemm::CGridDescriptor_MBlock_MPerBlock_NBlock_NPerBlock
            c_grid_desc_mblock_mperblock_nblock_nperblock_;

        // batch & stride
        index_t num_blocks_per_batch_;
        ComputeBasePtrOfStridedBatch compute_base_ptr_of_batch_;

        // check C0 masking and padding
        C0MatrixMask c0_matrix_mask_;

        // block-to-c-tile map
        Block2CTileMap block_2_ctile_map_;

        index_t block_start_, block_end_;
    };

    struct GroupDeviceArg
    {
        // problem definiton
        index_t M;
        index_t N;
        index_t K;
        index_t O;

        // Strides for the last dimensions of C for sanity check of vector load/store
        index_t c_extent_lowest_;
        index_t c_stride_lowest_;

        CGridDesc_M_N c_grid_desc_m_n_;
    };

    // Argument
    // FIXME: constness
    struct Argument : public BaseArgument
    {
        Argument(std::vector<const void*> p_a_vec,
                 std::vector<const void*> p_b_vec,
                 std::vector<const void*> p_b1_vec,
                 std::vector<void*> p_c_vec,
                 std::vector<ProblemDesc> problem_desc_vec,
                 AElementwiseOperation a_element_op,
                 BElementwiseOperation b_element_op,
                 AccElementwiseOperation acc_element_op,
                 B1ElementwiseOperation b1_element_op,
                 CElementwiseOperation c_element_op)
            : a_element_op_{a_element_op},
              b_element_op_{b_element_op},
              acc_element_op_{acc_element_op},
              b1_element_op_{b1_element_op},
              c_element_op_{c_element_op}
        {
            group_count_ = problem_desc_vec.size();

            if(!(group_count_ == p_a_vec.size() && group_count_ == p_b_vec.size() &&
                 group_count_ == p_b1_vec.size() && group_count_ == p_c_vec.size()))
            {
                throw std::runtime_error("wrong! group_count_ != a/b/b1/c_vec.size");
            }

            grid_size_ = 0;

            for(std::size_t i = 0; i < group_count_; i++)
            {
                const auto p_a_grid  = static_cast<const ADataType*>(p_a_vec[i]);
                const auto p_b_grid  = static_cast<const BDataType*>(p_b_vec[i]);
                const auto p_b1_grid = static_cast<const B1DataType*>(p_b1_vec[i]);
                const auto p_c_grid  = static_cast<CDataType*>(p_c_vec[i]);

                const auto a_grid_desc_ak0_m_ak1 = DeviceOp::MakeAGridDescriptor_AK0_M_AK1(
                    problem_desc_vec[i].M, problem_desc_vec[i].K, problem_desc_vec[i].StrideA);
                const auto b_grid_desc_bk0_n_bk1 = DeviceOp::MakeBGridDescriptor_BK0_N_BK1(
                    problem_desc_vec[i].K, problem_desc_vec[i].N, problem_desc_vec[i].StrideB0);
                const auto b1_grid_desc_bk0_n_bk1 = DeviceOp::MakeB1GridDescriptor_BK0_N_BK1(
                    problem_desc_vec[i].N, problem_desc_vec[i].O, problem_desc_vec[i].StrideB1);
                const auto c_grid_desc_m_n = DeviceOp::MakeCGridDescriptor_M_N(
                    problem_desc_vec[i].c_gs_ms_os_lengths, problem_desc_vec[i].c_gs_ms_os_strides);

                const auto c_grid_desc_mblock_mperblock_nblock_nperblock =
                    GridwiseGemm::MakeCGridDescriptor_MBlock_MPerBlock_NBlock_NPerBlock(
                        c_grid_desc_m_n);

                const index_t BlockStart     = grid_size_;
                const auto block_2_ctile_map = Block2CTileMap(c_grid_desc_m_n, BlockStart);
                const index_t grid_size_grp = block_2_ctile_map.CalculateGridSize(c_grid_desc_m_n) *
                                              problem_desc_vec[i].Batch;
                const index_t BlockEnd = grid_size_ + grid_size_grp;

                // batch stride
                // TODO ANT: only keep batch stride in tensor desc to reduce scalar cache pressure
                const auto c_grid_desc_g_m_n = DeviceOp::MakeCGridDescriptor_G_M_N(
                    problem_desc_vec[i].c_gs_ms_os_lengths, problem_desc_vec[i].c_gs_ms_os_strides);
                const auto compute_base_ptr_of_batch =
                    ComputeBasePtrOfStridedBatch(problem_desc_vec[i].BatchStrideA,
                                                 problem_desc_vec[i].BatchStrideB0,
                                                 problem_desc_vec[i].BatchStrideB1,
                                                 c_grid_desc_g_m_n);

                // C0 mask
                const auto c0_matrix_mask = C0MatrixMask(problem_desc_vec[i].N);

                grid_size_ += grid_size_grp;

                group_kernel_args_.push_back({p_a_grid,
                                              p_b_grid,
                                              p_b1_grid,
                                              p_c_grid,
                                              a_grid_desc_ak0_m_ak1,
                                              b_grid_desc_bk0_n_bk1,
                                              b1_grid_desc_bk0_n_bk1,
                                              c_grid_desc_mblock_mperblock_nblock_nperblock,
                                              block_2_ctile_map.CalculateGridSize(c_grid_desc_m_n),
                                              compute_base_ptr_of_batch,
                                              c0_matrix_mask,
                                              block_2_ctile_map,
                                              BlockStart,
                                              BlockEnd});

                group_device_args_.push_back({problem_desc_vec[i].M,
                                              problem_desc_vec[i].N,
                                              problem_desc_vec[i].K,
                                              problem_desc_vec[i].O,
                                              problem_desc_vec[i].c_gs_ms_os_lengths.back(),
                                              problem_desc_vec[i].c_gs_ms_os_strides.back(),
                                              c_grid_desc_m_n});
            }
        }

        std::vector<GroupKernelArg> group_kernel_args_;
        std::vector<GroupDeviceArg> group_device_args_;

        std::size_t group_count_;
        index_t grid_size_;

        AElementwiseOperation a_element_op_;
        BElementwiseOperation b_element_op_;
        AccElementwiseOperation acc_element_op_;
        B1ElementwiseOperation b1_element_op_;
        CElementwiseOperation c_element_op_;
    };

    // Invoker
    struct Invoker : public BaseInvoker
    {
        using Argument = DeviceOp::Argument;

        float Run(const Argument& arg, const StreamConfig& stream_config = StreamConfig{})
        {
            if(!DeviceOp::IsSupportedArgument(arg))
            {
                throw std::runtime_error("wrong! unsupported argument");
            }

            bool all_has_main_k_block_loop  = true;
            bool some_has_main_k_block_loop = false;
            for(std::size_t i = 0; i < arg.group_count_; i++)
            {
                const auto K = arg.group_kernel_args_[i].a_grid_desc_ak0_m_ak1_.GetLength(I0) *
                               arg.group_kernel_args_[i].a_grid_desc_ak0_m_ak1_.GetLength(I2);
                const bool y = GridwiseGemm::CalculateHasMainKBlockLoop(K);
                all_has_main_k_block_loop &= y;
                some_has_main_k_block_loop |= y;
            }

            hipGetErrorString(hipMemcpy(arg.p_workspace_,
                                        arg.group_kernel_args_.data(),
                                        arg.group_kernel_args_.size() * sizeof(GroupKernelArg),
                                        hipMemcpyHostToDevice));

            float ave_time = 0;

            auto launch_kernel = [&](auto has_main_k_block_loop_) {
                const auto kernel =
                    kernel_grouped_gemm_softmax_gemm_xdl_cshuffle_v1<GridwiseGemm,
                                                                     GroupKernelArg,
                                                                     AElementwiseOperation,
                                                                     BElementwiseOperation,
                                                                     AccElementwiseOperation,
                                                                     B1ElementwiseOperation,
                                                                     CElementwiseOperation,
                                                                     has_main_k_block_loop_>;

                return launch_and_time_kernel(
                    stream_config,
                    kernel,
                    dim3(arg.grid_size_),
                    dim3(BlockSize),
                    0,
                    cast_pointer_to_constant_address_space(arg.p_workspace_),
                    arg.group_count_,
                    arg.a_element_op_,
                    arg.b_element_op_,
                    arg.acc_element_op_,
                    arg.b1_element_op_,
                    arg.c_element_op_);
            };

            // Gemm1_K is split into Gemm1_K0/K1 where K1 is known at compile time, so we only need
            // to concern Gemm0's loop
            if(all_has_main_k_block_loop)
            {
                ave_time = launch_kernel(integral_constant<bool, true>{});
            }
            else if(!some_has_main_k_block_loop)
            {
                ave_time = launch_kernel(integral_constant<bool, false>{});
            }
            else
            {
                throw std::runtime_error("wrong! all gemm problems have to simultaneously meet "
                                         "has_main_k_block_loop or no_main_k_block_loop");
            }

            return ave_time;
        }

        // polymorphic
        float Run(const BaseArgument* p_arg,
                  const StreamConfig& stream_config = StreamConfig{}) override
        {
            return Run(*dynamic_cast<const Argument*>(p_arg), stream_config);
        }
    };

    static constexpr bool IsValidCompilationParameter()
    {
        // TODO: properly implement this check
        return true;
    }

    static bool IsSupportedArgument(const Argument& arg)
    {
        if(!(ck::get_device_name() == "gfx908" || ck::get_device_name() == "gfx90a"))
        {
            return false;
        }

        bool all_has_main_k_block_loop  = true;
        bool some_has_main_k_block_loop = false;

        for(std::size_t i = 0; i < arg.group_count_; i++)
        {
            const auto& kernel_arg = arg.group_kernel_args_[i];
            const auto& device_arg = arg.group_device_args_[i];

            // Check if C permute dimension matches GEMM + GEMM shape
            const index_t c_m       = device_arg.c_grid_desc_m_n_.GetLength(I0);
            const index_t c_gemm1n  = device_arg.c_grid_desc_m_n_.GetLength(I1);
            const index_t a_m       = kernel_arg.a_grid_desc_ak0_m_ak1_.GetLength(I1);
            const index_t b1_gemm1n = kernel_arg.b1_grid_desc_bk0_n_bk1_.GetLength(I1);
            if(!(c_m == a_m && c_gemm1n == b1_gemm1n))
            {
                return false;
            }

            // Check if having main loop
            const auto K = kernel_arg.a_grid_desc_ak0_m_ak1_.GetLength(I0) *
                           kernel_arg.a_grid_desc_ak0_m_ak1_.GetLength(I2);
            const bool y = GridwiseGemm::CalculateHasMainKBlockLoop(K);
            all_has_main_k_block_loop &= y;
            some_has_main_k_block_loop |= y;

            // Note: we need raw lengths since threadwise copy can not handle vector load when
            // part of vector is out of bounds
            const auto MRaw      = device_arg.M;
            const auto NRaw      = device_arg.N;
            const auto KRaw      = device_arg.K;
            const auto Gemm1NRaw = device_arg.O;

            // Check scalar per vector requirement
            const auto a_extent_lowest =
                is_same_v<tensor_layout::gemm::RowMajor, ALayout> ? KRaw : MRaw;
            const auto b_extent_lowest =
                is_same_v<tensor_layout::gemm::RowMajor, BLayout> ? NRaw : KRaw;
            const auto b1_extent_lowest =
                is_same_v<tensor_layout::gemm::RowMajor, B1Layout> ? Gemm1NRaw : NRaw;
            const auto c_extent_lowest = device_arg.c_extent_lowest_;

            if(!(a_extent_lowest % ABlockTransferSrcScalarPerVector == 0 &&
                 b_extent_lowest % BBlockTransferSrcScalarPerVector == 0 &&
                 b1_extent_lowest % B1BlockTransferSrcScalarPerVector == 0 &&
                 c_extent_lowest % CShuffleBlockTransferScalarPerVector_NPerBlock == 0))
            {
                return false;
            }

            // Check vector store requirement; assumes last dimension in N to be contiguous
            if(device_arg.c_stride_lowest_ != 1)
            {
                return false;
            }

            if(!GridwiseGemm::CheckValidity(kernel_arg.a_grid_desc_ak0_m_ak1_,
                                            kernel_arg.b_grid_desc_bk0_n_bk1_,
                                            kernel_arg.b1_grid_desc_bk0_n_bk1_,
                                            device_arg.c_grid_desc_m_n_,
                                            kernel_arg.block_2_ctile_map_))
            {
                return false;
            }
        }

        // all gemm problems have to simultaneously meet has_main_k_block_loop or
        // no_main_k_block_loop
        if(!(all_has_main_k_block_loop || !some_has_main_k_block_loop))
        {
            return false;
        }

        return true;
    }

    // polymorphic
    bool IsSupportedArgument(const BaseArgument* p_arg) override
    {
        return IsSupportedArgument(*dynamic_cast<const Argument*>(p_arg));
    }

    static auto MakeArgument(std::vector<const void*> p_a_vec,
                             std::vector<const void*> p_b_vec,
                             std::vector<const void*> p_b1_vec,
                             std::vector<void*> p_c_vec,
                             std::vector<ProblemDesc> problem_desc_vec,
                             AElementwiseOperation a_element_op,
                             BElementwiseOperation b_element_op,
                             AccElementwiseOperation acc_element_op,
                             B1ElementwiseOperation b1_element_op,
                             CElementwiseOperation c_element_op)
    {
        return Argument{p_a_vec,
                        p_b_vec,
                        p_b1_vec,
                        p_c_vec,
                        problem_desc_vec,
                        a_element_op,
                        b_element_op,
                        acc_element_op,
                        b1_element_op,
                        c_element_op};
    }

    static auto MakeInvoker() { return Invoker{}; }

    // polymorphic
    std::unique_ptr<BaseArgument> MakeArgumentPointer(std::vector<const void*> p_a_vec,
                                                      std::vector<const void*> p_b_vec,
                                                      std::vector<const void*> p_b1_vec,
                                                      std::vector<void*> p_c_vec,
                                                      std::vector<ProblemDesc> problem_desc_vec,
                                                      AElementwiseOperation a_element_op,
                                                      BElementwiseOperation b_element_op,
                                                      AccElementwiseOperation acc_element_op,
                                                      B1ElementwiseOperation b1_element_op,
                                                      CElementwiseOperation c_element_op) override
    {
        return std::make_unique<Argument>(p_a_vec,
                                          p_b_vec,
                                          p_b1_vec,
                                          p_c_vec,
                                          problem_desc_vec,
                                          a_element_op,
                                          b_element_op,
                                          acc_element_op,
                                          b1_element_op,
                                          c_element_op);
    }

    // polymorphic
    std::unique_ptr<BaseInvoker> MakeInvokerPointer() override
    {
        return std::make_unique<Invoker>(Invoker{});
    }

    // polymorphic
    std::string GetTypeString() const override
    {
        auto str = std::stringstream();

        // clang-format off
        str << "DeviceGroupedGemmSoftmaxGemmPermute_Xdl_CShuffle"
            << "<"
            << BlockSize << ", "
            << MPerBlock << ", "
            << NPerBlock << ", "
            << KPerBlock << ", "
            << AK1 << ", "
            << BK1 << ", "
            << MPerBlock << ", "
            << Gemm1NPerBlock << ", "
            << Gemm1KPerBlock << ", "
            << B1K1 << ", "
            << getGemmSpecializationString(GemmSpec) << ">";
        // clang-format on

        return str.str();
    }

    size_t GetWorkSpaceSize(const BaseArgument* p_arg) const override
    {
        return dynamic_cast<const Argument*>(p_arg)->group_count_ * sizeof(GroupKernelArg);
    }
};

} // namespace device
} // namespace tensor_operation
} // namespace ck
