// SPDX-License-Identifier: MIT
// Copyright (c) 2018-2022, Advanced Micro Devices, Inc. All rights reserved.

#include <cstdlib>

#include "ck/ck.hpp"
#include "ck/tensor_operation/gpu/device/tensor_layout.hpp"
#include "ck/tensor_operation/gpu/device/gemm_specialization.hpp"
#include "ck/tensor_operation/gpu/device/impl/device_batched_gemm_xdl.hpp"
#include "ck/library/tensor_operation_instance/add_device_operation_instance.hpp"

namespace ck {
namespace tensor_operation {
namespace device {
namespace instance {

using F16 = ck::half_t;
using F32 = float;

using Row = ck::tensor_layout::gemm::RowMajor;
using Col = ck::tensor_layout::gemm::ColumnMajor;

template <ck::index_t... Is>
using S = ck::Sequence<Is...>;

using PassThrough = ck::tensor_operation::element_wise::PassThrough;

// Compilation parameters for a[m, k] * b[n, k] = c[m, n]
using device_batched_gemm_xdl_f16_f16_f16_gmk_gnk_gmn_instances = std::tuple<
    // clang-format off
        //#################| AData| BData| CData| AccData| ALayout| BLayout| CLayout|           A|           B|           C| Block|  MPer|  NPer| K0Per| K1| MPer| NPer| MXdl| NXdl|  ABlockTransfer| ABlockTransfer| ABlockTransfer| ABlockTransfer| ABlockTransfer| ABlockTransfer| ABlockLds|  BBlockTransfer| BBlockTransfer| BBlockTransfer| BlockTransfer| BBlockTransfer| BBlockTransfer| BBlockLds| CThreadTransfer| CThreadTransfer|
        //#################|  Type|  Type|  Type|    Type|        |        |        | Elementwise| Elementwise| Elementwise|  Size| Block| Block| Block|   |  XDL|  XDL|  Per|  Per|   ThreadCluster|  ThreadCluster| SrcAccessOrder|   SrcVectorDim|      SrcScalar|      DstScalar| AddExtraM|   ThreadCluster|  ThreadCluster| SrcAccessOrder|  SrcVectorDim|      SrcScalar|      DstScalar| AddExtraN| SrcDstVectorDim|       DstScalar|
        //#################|      |      |      |        |        |        |        |   Operation|   Operation|   Operation|      |      |      |      |   |     |     | Wave| Wave| Lengths_K0_M_K1|   ArrangeOrder|               |               |      PerVector|   PerVector_K1|          | Lengths_K0_N_K1|   ArrangeOrder|               |              |      PerVector|   PerVector_K1|          |                |       PerVector|
        //#################|      |      |      |        |        |        |        |            |            |            |      |      |      |      |   |     |     |     |     |                |               |               |               |               |               |          |                |               |               |              |               |               |          |                |                |
        DeviceBatchedGemmXdl<  F16,   F16,   F16,     F32,     Row,     Col,     Row, PassThrough, PassThrough, PassThrough,   256,   256,   128,     4,  8,   32,   32,    4,    2,     S<4, 64, 1>,     S<1, 0, 2>,     S<1, 0, 2>,              2,              8,              8,      true,     S<4, 64, 1>,     S<1, 0, 2>,     S<1, 0, 2>,             2,              8,              8,      true,               7,               1>,
        DeviceBatchedGemmXdl<  F16,   F16,   F16,     F32,     Row,     Col,     Row, PassThrough, PassThrough, PassThrough,   256,   128,   256,     4,  8,   32,   32,    2,    4,     S<4, 64, 1>,     S<1, 0, 2>,     S<1, 0, 2>,              2,              8,              8,      true,     S<4, 64, 1>,     S<1, 0, 2>,     S<1, 0, 2>,             2,              8,              8,      true,               7,               1>,
        DeviceBatchedGemmXdl<  F16,   F16,   F16,     F32,     Row,     Col,     Row, PassThrough, PassThrough, PassThrough,   128,   128,   128,     4,  8,   32,   32,    4,    2,     S<4, 32, 1>,     S<1, 0, 2>,     S<1, 0, 2>,              2,              8,              8,      true,     S<4, 32, 1>,     S<1, 0, 2>,     S<1, 0, 2>,             2,              8,              8,      true,               7,               1>,
        DeviceBatchedGemmXdl<  F16,   F16,   F16,     F32,     Row,     Col,     Row, PassThrough, PassThrough, PassThrough,   256,   128,   128,     4,  8,   32,   32,    2,    2,     S<4, 64, 1>,     S<1, 0, 2>,     S<1, 0, 2>,              2,              8,              8,      true,     S<4, 64, 1>,     S<1, 0, 2>,     S<1, 0, 2>,             2,              8,              8,      true,               7,               1>,
        DeviceBatchedGemmXdl<  F16,   F16,   F16,     F32,     Row,     Col,     Row, PassThrough, PassThrough, PassThrough,   128,   128,    64,     4,  8,   32,   32,    2,    2,     S<4, 32, 1>,     S<1, 0, 2>,     S<1, 0, 2>,              2,              8,              8,      true,     S<4, 32, 1>,     S<1, 0, 2>,     S<1, 0, 2>,             2,              8,              8,      true,               7,               1>,
        DeviceBatchedGemmXdl<  F16,   F16,   F16,     F32,     Row,     Col,     Row, PassThrough, PassThrough, PassThrough,   128,    64,   128,     4,  8,   32,   32,    2,    2,     S<4, 32, 1>,     S<1, 0, 2>,     S<1, 0, 2>,              2,              8,              8,      true,     S<4, 32, 1>,     S<1, 0, 2>,     S<1, 0, 2>,             2,              8,              8,      true,               7,               1>,
        DeviceBatchedGemmXdl<  F16,   F16,   F16,     F32,     Row,     Col,     Row, PassThrough, PassThrough, PassThrough,    64,    64,    64,     4,  8,   32,   32,    2,    2,     S<4, 16, 1>,     S<1, 0, 2>,     S<1, 0, 2>,              2,              8,              8,      true,     S<4, 16, 1>,     S<1, 0, 2>,     S<1, 0, 2>,             2,              8,              8,      true,               7,               1>,
        DeviceBatchedGemmXdl<  F16,   F16,   F16,     F32,     Row,     Col,     Row, PassThrough, PassThrough, PassThrough,   256,   128,    64,     4,  8,   32,   32,    2,    1,     S<4, 64, 1>,     S<1, 0, 2>,     S<1, 0, 2>,              2,              8,              8,      true,     S<4, 64, 1>,     S<1, 0, 2>,     S<1, 0, 2>,             2,              8,              8,      true,               7,               1>,
        DeviceBatchedGemmXdl<  F16,   F16,   F16,     F32,     Row,     Col,     Row, PassThrough, PassThrough, PassThrough,   256,    64,   128,     4,  8,   32,   32,    1,    2,     S<4, 64, 1>,     S<1, 0, 2>,     S<1, 0, 2>,              2,              8,              8,      true,     S<4, 64, 1>,     S<1, 0, 2>,     S<1, 0, 2>,             2,              8,              8,      true,               7,               1>,
        DeviceBatchedGemmXdl<  F16,   F16,   F16,     F32,     Row,     Col,     Row, PassThrough, PassThrough, PassThrough,   128,   128,    32,     4,  8,   32,   32,    2,    1,     S<4, 32, 1>,     S<1, 0, 2>,     S<1, 0, 2>,              2,              8,              8,      true,     S<4, 32, 1>,     S<1, 0, 2>,     S<1, 0, 2>,             2,              8,              8,      true,               7,               1>,
        DeviceBatchedGemmXdl<  F16,   F16,   F16,     F32,     Row,     Col,     Row, PassThrough, PassThrough, PassThrough,   128,    32,   128,     4,  8,   32,   32,    1,    2,     S<4, 32, 1>,     S<1, 0, 2>,     S<1, 0, 2>,              2,              8,              8,      true,     S<4, 32, 1>,     S<1, 0, 2>,     S<1, 0, 2>,             2,              8,              8,      true,               7,               1>,
        DeviceBatchedGemmXdl<  F16,   F16,   F16,     F32,     Row,     Col,     Row, PassThrough, PassThrough, PassThrough,    64,    64,    32,     4,  8,   32,   32,    2,    1,     S<4, 16, 1>,     S<1, 0, 2>,     S<1, 0, 2>,              2,              8,              8,      true,     S<4, 16, 1>,     S<1, 0, 2>,     S<1, 0, 2>,             2,              8,              8,      true,               7,               1>,
        DeviceBatchedGemmXdl<  F16,   F16,   F16,     F32,     Row,     Col,     Row, PassThrough, PassThrough, PassThrough,    64,    32,    64,     4,  8,   32,   32,    1,    2,     S<4, 16, 1>,     S<1, 0, 2>,     S<1, 0, 2>,              2,              8,              8,      true,     S<4, 16, 1>,     S<1, 0, 2>,     S<1, 0, 2>,             2,              8,              8,      true,               7,               1>
    // clang-format on
    >;

void add_device_batched_gemm_xdl_f16_f16_f16_gmk_gnk_gmn_instances(
    std::vector<std::unique_ptr<
        DeviceBatchedGemm<Row, Col, Row, F16, F16, F16, PassThrough, PassThrough, PassThrough>>>&
        instances)
{
    add_device_operation_instances(instances,
                                   device_batched_gemm_xdl_f16_f16_f16_gmk_gnk_gmn_instances{});
}

} // namespace instance
} // namespace device
} // namespace tensor_operation
} // namespace ck
