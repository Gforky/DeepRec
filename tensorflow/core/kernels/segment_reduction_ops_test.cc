/* Copyright 2015 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include <algorithm>
#include <functional>
#include <random>
#include <vector>

#include "tensorflow/core/common_runtime/device.h"
#include "tensorflow/core/common_runtime/device_factory.h"
#include "tensorflow/core/common_runtime/kernel_benchmark_testlib.h"
#include "tensorflow/core/framework/allocator.h"
#include "tensorflow/core/framework/fake_input.h"
#include "tensorflow/core/framework/node_def_builder.h"
#include "tensorflow/core/framework/op_kernel.h"
#include "tensorflow/core/framework/tensor.h"
#include "tensorflow/core/framework/tensor_testutil.h"
#include "tensorflow/core/framework/types.h"
#include "tensorflow/core/framework/types.pb.h"
#include "tensorflow/core/graph/node_builder.h"
#include "tensorflow/core/graph/testlib.h"
#include "tensorflow/core/kernels/ops_testutil.h"
#include "tensorflow/core/kernels/ops_util.h"
#include "tensorflow/core/platform/test.h"
#include "tensorflow/core/platform/test_benchmark.h"
#include "tensorflow/core/public/session_options.h"
#include "tensorflow/core/public/version.h"

namespace tensorflow {

static Graph* BM_UnsortedSegmentReduction(const string& reduction,
                                        int num_rows, int num_cols,
                                        int segment_size) {
  Graph* g = new Graph(OpRegistry::Global());
  // Create inputs
  gtl::InlinedVector<TensorValue, 4> reduction_inputs;
  TensorShape shape1({num_rows, num_cols});
  Tensor input(DT_FLOAT, shape1);
  // input.flat<float>().setRandom();
  reduction_inputs.push_back({nullptr, &input});

  TensorShape shape2({num_rows});
  Tensor indices(DT_INT32, shape2);
  test::FillFn<int>(&indices,
                    [&segment_size](int i) -> int { return i % segment_size; });
  reduction_inputs.push_back({nullptr, &indices});

  Tensor num_segments(DT_INT32, TensorShape({}));
  num_segments.scalar<int>()() = segment_size;
  reduction_inputs.push_back({nullptr, &num_segments});

  Node* input_in0 = test::graph::Constant(g, input);
  Node* input_in1 = test::graph::Constant(g, indices);
  Node* input_in3 = test::graph::Constant(g, num_segments);

  TF_CHECK_OK(NodeBuilder(g->NewName(reduction), reduction)
                  .Input(input_in0)
                  .Input(input_in1)
                  .Input(input_in3)
                  .Finalize(g, nullptr));
  return g;
}

#define BM_UnsortedReduce(O, NTH, R, C, S)                                              \
  static void BM_##O##_##R##_##C##_##S##_##NTH(int iters) {                             \
    testing::UseRealTime();                                                             \
    testing::BytesProcessed(static_cast<int64>(iters) * R * C * sizeof(float));         \
    SessionOptions opts;                                                                \
    opts.config.set_intra_op_parallelism_threads(NTH);                                  \
    test::Benchmark("cpu", BM_UnsortedSegmentReduction(#O, R, C, S), &opts).Run(iters); \
  }                                                                                     \
  BENCHMARK(BM_##O##_##R##_##C##_##S##_##NTH);

#define BM_UnsortedReduce_NTH(O, R, C, S) \
  BM_UnsortedReduce(O,  1, R, C, S);      \
  BM_UnsortedReduce(O,  2, R, C, S);      \
  BM_UnsortedReduce(O,  4, R, C, S);      \
  BM_UnsortedReduce(O,  8, R, C, S);      \
  BM_UnsortedReduce(O, 16, R, C, S);      \

#define BM_UnsortedReduce_Arg(R, C, S) \
  BM_UnsortedReduce_NTH(UnsortedSegmentSum, R, C, S);

BM_UnsortedReduce_Arg(4096, 1024, 1);
BM_UnsortedReduce_Arg(4096, 1024, 128);
BM_UnsortedReduce_Arg(351, 1, 729);

template <typename Index>
static void BM_SegmentReduction(int iters, const string& reduction,
                                Index num_rows, Index num_cols,
                                Index segment_size) {
  testing::StopTiming();
  std::unique_ptr<Device> device(
      DeviceFactory::NewDevice("CPU", {}, "/job:a/replica:0/task:0"));

  // Create inputs
  gtl::InlinedVector<TensorValue, 4> reduction_inputs;
  TensorShape shape1({num_rows, num_cols});
  Tensor input1(DT_FLOAT, shape1);
  reduction_inputs.push_back({nullptr, &input1});

  TensorShape shape2({num_rows});
  Tensor input2(DataTypeToEnum<Index>::v(), shape2);
  test::FillFn<Index>(&input2, [&num_rows, &segment_size](Index i) -> Index {
    return std::min(i / segment_size, num_rows - 1);
  });
  reduction_inputs.push_back({nullptr, &input2});

  NodeDef reduction_node_def;
  TF_CHECK_OK(NodeDefBuilder(reduction, reduction)
                  .Input(FakeInput(DT_FLOAT))
                  .Input(FakeInput(DataTypeToEnum<Index>::v()))
                  .Finalize(&reduction_node_def));
  Status status;
  std::unique_ptr<OpKernel> reduction_op(
      CreateOpKernel(DEVICE_CPU, device.get(), cpu_allocator(),
                     reduction_node_def, TF_GRAPH_DEF_VERSION, &status));
  OpKernelContext::Params params;
  params.device = device.get();
  params.frame_iter = FrameAndIter(0, 0);
  params.inputs = &reduction_inputs;
  params.op_kernel = reduction_op.get();
  std::vector<AllocatorAttributes> attrs;
  test::SetOutputAttrs(&params, &attrs);

  std::unique_ptr<OpKernelContext> reduction_context(
      new OpKernelContext(&params));

  reduction_op->Compute(reduction_context.get());
  TF_CHECK_OK(reduction_context->status());
  testing::StartTiming();
  for (int i = 0; i < iters; ++i) {
    delete reduction_context->release_output(0).tensor;
    reduction_op->Compute(reduction_context.get());
  }
  int64 bytes_per_iter =
      static_cast<int64>(num_rows * num_cols * sizeof(float));
  testing::BytesProcessed(bytes_per_iter * iters);
}

#define BM_Reduce(O, R, C, S)                                      \
  static void BM_Reduce_##O##_##R##_##C##_##S##_int32(int iters) { \
    BM_SegmentReduction<int32>(iters, #O, R, C, S);                \
  }                                                                \
  static void BM_Reduce_##O##_##R##_##C##_##S##_int64(int iters) { \
    BM_SegmentReduction<int64>(iters, #O, R, C, S);                \
  }                                                                \
  BENCHMARK(BM_Reduce_##O##_##R##_##C##_##S##_int32);              \
  BENCHMARK(BM_Reduce_##O##_##R##_##C##_##S##_int64);

#define BM_Reduce_Arg(R, C, S)    \
  BM_Reduce(SegmentSum, R, C, S); \
  BM_Reduce(SegmentMean, R, C, S);

BM_Reduce_Arg(64, 32, 1);
BM_Reduce_Arg(4096, 128, 1);

BM_Reduce_Arg(16, 8, 2);
BM_Reduce_Arg(64, 32, 2);
BM_Reduce_Arg(4096, 32, 2);
BM_Reduce_Arg(4096, 128, 2);

Node* SegmentSumV2Node(Graph* g, Node* data, Node* seg_ids) {
  Node* ret;
  TF_CHECK_OK(NodeBuilder(g->NewName("segsum"), "SegmentSum")
                  .Input(data)
                  .Input(seg_ids)
                  .Finalize(g, &ret));
  return ret;
}

template <typename Index>
static Graph* SegmentSumV2(Index num_rows, Index num_cols,
                           Index segment_size) {

  Graph* g = new Graph(OpRegistry::Global());

  TensorShape shape1({num_rows, num_cols});
  Tensor input1(DT_FLOAT, shape1);
  input1.flat<float>().setRandom();

  TensorShape shape2({num_rows});
  Tensor input2(DataTypeToEnum<Index>::v(), shape2);
  test::FillFn<Index>(&input2, [&num_rows, &segment_size](Index i) -> Index {
    return std::min(i / segment_size, num_rows - 1);
  });
  Node* data = test::graph::Constant(g, input1);
  Node* seg_ids = test::graph::Constant(g, input2);

  SegmentSumV2Node(g, data, seg_ids);
  return g;
}

#define BM_SEGMENT_SUM_V2(DEVICE, INDEX, R, C)                               \
  static void BM_##DEVICE##_segsum_##INDEX##_##R##_##C(int iters, int s) {  \
    int64 bytes_per_iter = static_cast<int64>(R * C * sizeof(float)); \
    tensorflow::SessionOptions options;                                 \
    options.config.set_inter_op_parallelism_threads(16);                \
    options.config.set_intra_op_parallelism_threads(16);                \
    options.config.mutable_gpu_options()->set_visible_device_list("0"); \
    testing::BytesProcessed(bytes_per_iter * iters); \
    testing::UseRealTime();                                             \
    test::Benchmark(#DEVICE, SegmentSumV2<INDEX>(R,C,s), &options).Run(iters); \
  }                                                                     \
  BENCHMARK(BM_##DEVICE##_segsum_##INDEX##_##R##_##C)->Arg(2)
#if GOOGLE_CUDA
BM_SEGMENT_SUM_V2(gpu, int64, 64, 32);
#endif
BM_SEGMENT_SUM_V2(cpu, int64, 64, 32);

Node* SparseSegmentReductionNode(Graph* g, Node* data,
                                 Node* data_ids, Node* seg_ids, 
                                 bool is_mean, bool is_sqrtn) {
  Node* ret;
  if (is_mean) {
    TF_CHECK_OK(NodeBuilder(g->NewName("sparsesegmean"), "SparseSegmentMean")
                    .Input(data)
                    .Input(data_ids)
                    .Input(seg_ids)
                    .Finalize(g, &ret));
  } else if (is_sqrtn) {
    TF_CHECK_OK(NodeBuilder(g->NewName("sparsesegsqrtn"), "SparseSegmentSqrtN")
                    .Input(data)
                    .Input(data_ids)
                    .Input(seg_ids)
                    .Finalize(g, &ret));
  } else {
    TF_CHECK_OK(NodeBuilder(g->NewName("sparsesegsum"), "SparseSegmentSum")
                    .Input(data)
                    .Input(data_ids)
                    .Input(seg_ids)
                    .Finalize(g, &ret));
  }
  return ret;
}

template <typename Index>
static Graph* SparseSegmentReduction(Index num_rows,
                                     Index num_cols,
                                     Index num_indices,
                                     Index segment_size,
                                     bool is_mean,
                                     bool is_sqrtn) {

  Graph* g = new Graph(OpRegistry::Global());

  TensorShape shape1({num_rows, num_cols});
  Tensor input1(DT_FLOAT, shape1);
  input1.flat<float>().setRandom();
  // indices
  std::vector<Index> indices_total;
  for(Index i=0; i<num_rows; i++) {
    indices_total.emplace_back(i);
  }
  auto rng = std::default_random_engine {};
  std::shuffle(std::begin(indices_total), std::end(indices_total), rng);
  TensorShape shape2({num_indices});
  Tensor input2(DataTypeToEnum<Index>::v(), shape2);
  test::FillFn<Index>(&input2, [&num_rows, &indices_total](Index i) -> Index {
    return static_cast<Index>(indices_total[i%num_rows]);
  });
  // segment_ids
  TensorShape shape3({num_indices});
  Tensor input3(DataTypeToEnum<int32>::v(), shape3);
  test::FillFn<int32>(&input3, [&num_rows, &segment_size](Index i) -> int32 {
    return static_cast<int32>(std::min(i / segment_size, num_rows - 1));
  });

  Node* data = test::graph::Constant(g, input1);
  Node* data_ids = test::graph::Constant(g, input2);
  Node* seg_ids = test::graph::Constant(g, input3);

  SparseSegmentReductionNode(g, data, data_ids, seg_ids, is_mean, is_sqrtn);
  return g;
}

#define BM_SPARSE_SEGMENT_REDUCTION(DEVICE, INDEX, MEAN, SQRTN, R, C, I, S)  \
  static void BM_##DEVICE##_SR_##INDEX##_##MEAN##_##SQRTN##_##R##_\
    ##C##_##I##_##S( \
    int iters, int s) {  \
    int64 bytes_per_iter = static_cast<int64>(I * C * sizeof(float)); \
    tensorflow::SessionOptions options;                                 \
    options.config.set_inter_op_parallelism_threads(16);                \
    options.config.set_intra_op_parallelism_threads(16);                \
    options.config.mutable_gpu_options()->set_visible_device_list("0"); \
    testing::BytesProcessed(bytes_per_iter * iters); \
    testing::UseRealTime();                                             \
    test::Benchmark(#DEVICE, SparseSegmentReduction<INDEX>(R,C,I,s,MEAN,SQRTN),\
    &options).Run(iters); \
  }                                                                     \
  BENCHMARK(BM_##DEVICE##_SR_##INDEX##_##MEAN##_##SQRTN##_##R##_##C##_\
    ##I##_##S)->Arg(S)

BM_SPARSE_SEGMENT_REDUCTION(cpu, int32, false, false, 30000, 32, 6000, 10);
BM_SPARSE_SEGMENT_REDUCTION(cpu, int32, false, false, 30000, 32, 6000, 100);
BM_SPARSE_SEGMENT_REDUCTION(cpu, int32, false, false, 30000, 32, 6000, 1000);
BM_SPARSE_SEGMENT_REDUCTION(cpu, int32, false, false, 30000, 32, 12000, 1000);
BM_SPARSE_SEGMENT_REDUCTION(cpu, int32, false, false, 30000, 32, 1200, 1000);
BM_SPARSE_SEGMENT_REDUCTION(cpu, int32, false, false, 30000, 32, 120000, 1000);
BM_SPARSE_SEGMENT_REDUCTION(cpu, int32, true, false, 30000, 32, 120000, 1000);
BM_SPARSE_SEGMENT_REDUCTION(cpu, int32, false, true, 30000, 32, 120000, 1000);
#if GOOGLE_CUDA
BM_SPARSE_SEGMENT_REDUCTION(gpu, int32, false, false, 30000, 32, 6000, 10);
BM_SPARSE_SEGMENT_REDUCTION(gpu, int32, false, false, 30000, 32, 6000, 100);
BM_SPARSE_SEGMENT_REDUCTION(gpu, int32, false, false, 30000, 32, 6000, 1000);
BM_SPARSE_SEGMENT_REDUCTION(gpu, int32, false, false, 30000, 32, 12000, 1000);
BM_SPARSE_SEGMENT_REDUCTION(gpu, int32, false, false, 30000, 32, 1200, 1000);
BM_SPARSE_SEGMENT_REDUCTION(gpu, int32, false, false, 30000, 32, 120000, 1000);
BM_SPARSE_SEGMENT_REDUCTION(gpu, int32, true, false, 30000, 32, 120000, 1000);
BM_SPARSE_SEGMENT_REDUCTION(gpu, int32, false, true, 30000, 32, 120000, 1000);
#endif

Node* SparseSegmentReductionGradNode(Graph* g, Node* input,
                                     Node* data_ids, Node* seg_ids, 
                                     Node* output_dim0,
                                     bool is_sqrtn) {
  Node* ret;
  if (is_sqrtn) {
    TF_CHECK_OK(NodeBuilder(g->NewName("sparsesegsqrtngrad"), 
                            "SparseSegmentSqrtNGrad")
                  .Input(input)
                  .Input(data_ids)
                  .Input(seg_ids)
                  .Input(output_dim0)
                  .Finalize(g, &ret));
  } else {
    TF_CHECK_OK(NodeBuilder(g->NewName("sparsesegmeangrad"), 
                            "SparseSegmentMeanGrad")
                  .Input(input)
                  .Input(data_ids)
                  .Input(seg_ids)
                  .Input(output_dim0)
                  .Finalize(g, &ret));
  } 
  return ret;
}

template <typename Index>
static Graph* SparseSegmentReductionGrad(Index num_rows,
                                         Index num_cols,
                                         Index num_indices,
                                         Index segment_size,
                                         bool is_sqrtn) {

  Graph* g = new Graph(OpRegistry::Global());
  // indices
  std::vector<Index> indices_total;
  for(Index i=0; i<num_rows; i++) {
    indices_total.emplace_back(i);
  }
  auto rng = std::default_random_engine {};
  std::shuffle(std::begin(indices_total), std::end(indices_total), rng);
  TensorShape shape2({num_indices});
  Tensor input2(DataTypeToEnum<Index>::v(), shape2);
  test::FillFn<Index>(&input2, [&num_rows, &indices_total](Index i) -> Index {
    return static_cast<Index>(indices_total[i%num_rows]);
  });
  // segment_ids
  TensorShape shape3({num_indices});
  Tensor input3(DataTypeToEnum<int32>::v(), shape3);
  test::FillFn<int32>(&input3, [&num_rows, &segment_size](Index i) -> int32 {
    return static_cast<int32>(std::min(i / segment_size, num_rows - 1));
  });
  // input 
  TensorShape shape1({std::min((num_indices-1)/segment_size, num_rows -1) + 1, 
                      num_cols});
  Tensor input1(DataTypeToEnum<float>::v(), shape1);
  input1.flat<float>().setRandom();
  // output_dim0
  Tensor input4(DataTypeToEnum<Index>::v(), TensorShape({}));
  input4.scalar<int32>()() = num_rows;

  Node* input = test::graph::Constant(g, input1);
  Node* data_ids = test::graph::Constant(g, input2);
  Node* seg_ids = test::graph::Constant(g, input3);
  Node* output_dim0 = test::graph::Constant(g, input4); 

  SparseSegmentReductionGradNode(g, input, data_ids, seg_ids, 
                                 output_dim0, is_sqrtn);
  return g;
}

#define BM_SPARSE_SEGMENT_REDUCTION_GRAD(DEVICE, INDEX, SQRTN, R, C, I, S)  \
  static void BM_##DEVICE##_SRG_##INDEX##_##SQRTN##_##R##_##C##_##I##_##S( \
    int iters, int s) {  \
    int64 bytes_per_iter = static_cast<int64>(I * C * sizeof(float)); \
    tensorflow::SessionOptions options;                                 \
    options.config.set_inter_op_parallelism_threads(16);                \
    options.config.set_intra_op_parallelism_threads(16);                \
    options.config.mutable_gpu_options()->set_visible_device_list("0"); \
    testing::BytesProcessed(bytes_per_iter * iters); \
    testing::UseRealTime();                                             \
    test::Benchmark(#DEVICE, SparseSegmentReductionGrad<INDEX>(R,C,I,s,SQRTN),\
    &options).Run(iters); \
  }                                                                     \
  BENCHMARK(BM_##DEVICE##_SRG_##INDEX##_##SQRTN##_##R##_##C##_##I##_##S)->Arg(S)
  
BM_SPARSE_SEGMENT_REDUCTION_GRAD(cpu, int32, false, 30000, 32, 6000, 10);
BM_SPARSE_SEGMENT_REDUCTION_GRAD(cpu, int32, false, 30000, 32, 6000, 100);
BM_SPARSE_SEGMENT_REDUCTION_GRAD(cpu, int32, false, 30000, 32, 6000, 1000);
BM_SPARSE_SEGMENT_REDUCTION_GRAD(cpu, int32, false, 30000, 32, 600, 100);
BM_SPARSE_SEGMENT_REDUCTION_GRAD(cpu, int32, false, 30000, 32, 12000, 100);
#if GOOGLE_CUDA
BM_SPARSE_SEGMENT_REDUCTION_GRAD(gpu, int32, false, 30000, 32, 6000, 10);
BM_SPARSE_SEGMENT_REDUCTION_GRAD(gpu, int32, false, 30000, 32, 6000, 100);
BM_SPARSE_SEGMENT_REDUCTION_GRAD(gpu, int32, false, 30000, 32, 6000, 1000);
BM_SPARSE_SEGMENT_REDUCTION_GRAD(gpu, int32, false, 30000, 32, 600, 100);
BM_SPARSE_SEGMENT_REDUCTION_GRAD(gpu, int32, false, 30000, 32, 12000, 100);
#endif

Node* UnsortedSegmentSumNode(Graph* g, Node* data,
                             Node* seg_ids, Node* seg_num) {
  Node* ret;
  TF_CHECK_OK(NodeBuilder(g->NewName("unsorted_segsum"), "UnsortedSegmentSum")
                  .Input(data)
                  .Input(seg_ids)
                  .Input(seg_num)
                  .Finalize(g, &ret));
  return ret;
}

template <typename Index>
static Graph* UnsortedSegmentSum(Index num_rows, Index num_cols,
                                 Index segment_size) {

  Graph* g = new Graph(OpRegistry::Global());

  TensorShape shape1({num_rows, num_cols});
  Tensor input1(DT_FLOAT, shape1);
  input1.flat<float>().setRandom();

  TensorShape shape2({num_rows});
  Tensor input2(DataTypeToEnum<Index>::v(), shape2);
  test::FillFn<Index>(&input2, [&num_rows, &segment_size](Index i) -> Index {
    return std::min(i / segment_size, num_rows - 1);
  });

  Tensor input3(DataTypeToEnum<Index>::v(), TensorShape({}));
  input3.scalar<Index>()() = num_rows;

  Node* data = test::graph::Constant(g, input1);
  Node* seg_ids = test::graph::Constant(g, input2);
  Node* num_seg = test::graph::Constant(g, input3);

  UnsortedSegmentSumNode(g, data, seg_ids, num_seg);
  return g;
}

#define BM_UNSORTED_SEGMENT_SUM(DEVICE, INDEX, R, C)                 \
  static void BM_##DEVICE##_unsorted_segsum_##INDEX##_##R##_##C( \
    int iters, int s) {  \
    int64 bytes_per_iter = static_cast<int64>(R * C * sizeof(float)); \
    tensorflow::SessionOptions options;                                 \
    options.config.set_inter_op_parallelism_threads(16);                \
    options.config.set_intra_op_parallelism_threads(16);                \
    options.config.mutable_gpu_options()->set_visible_device_list("0"); \
    testing::BytesProcessed(bytes_per_iter * iters); \
    testing::UseRealTime();                                             \
    test::Benchmark(#DEVICE, UnsortedSegmentSum<INDEX>(R,C,s), \
    &options).Run(iters); \
  }                                                                     \
  BENCHMARK(BM_##DEVICE##_unsorted_segsum_##INDEX##_##R##_##C)->Arg(2)

BM_UNSORTED_SEGMENT_SUM(cpu, int32, 64, 32);
#if GOOGLE_CUDA
BM_UNSORTED_SEGMENT_SUM(gpu, int32, 64, 32);
#endif

static void SparseSegmentMeanGradHelper(int iters, float uniqueness, int size) {
  testing::StopTiming();
  Graph* g = new Graph(OpRegistry::Global());
  CHECK_LE(uniqueness, 1.0);
  CHECK_GT(uniqueness, 0.0);

  const int kNumIndices = size;
  Tensor indices(DT_INT32, TensorShape({kNumIndices}));
  auto indices_flat = indices.flat<int32>();
  Tensor segments(DT_INT32, TensorShape({kNumIndices}));
  auto segments_flat = segments.flat<int32>();

  int kUniqueIndices = uniqueness * kNumIndices;
  Tensor output_dim0(DT_INT32, TensorShape({}));
  output_dim0.scalar<int32>()() = kUniqueIndices;

  for (int i = 0; i < kNumIndices; ++i) {
    indices_flat(i) = (i * 31) % kUniqueIndices;
    segments_flat(i) = i * .8;
  }

  const int kDim1 = segments_flat(kNumIndices - 1) + 1;
  const int kDim2 = 128;
  Tensor input(DT_FLOAT, TensorShape({kDim1, kDim2}));
  input.flat<float>().setRandom();

  Node* node;
  TF_CHECK_OK(NodeBuilder(g->NewName("n"), "SparseSegmentMeanGrad")
                  .Input(test::graph::Constant(g, input))
                  .Input(test::graph::Constant(g, indices))
                  .Input(test::graph::Constant(g, segments))
                  .Input(test::graph::Constant(g, output_dim0))
                  .Attr("T", DT_FLOAT)
                  .Finalize(g, &node));

  testing::UseRealTime();
  testing::BytesProcessed(static_cast<int64>(iters) * (kDim1 * kDim2) *
                          sizeof(float));
  testing::StartTiming();
  test::Benchmark("cpu", g).Run(iters);
}

static void BM_SparseSegmentMeanGrad_Low(int iters, int size) {
  return SparseSegmentMeanGradHelper(iters, 1.0, size);
}

static void BM_SparseSegmentMeanGrad_High(int iters, int size) {
  return SparseSegmentMeanGradHelper(iters, 0.01, size);
}

BENCHMARK(BM_SparseSegmentMeanGrad_Low)->Arg(1000)->Arg(100000);
BENCHMARK(BM_SparseSegmentMeanGrad_High)->Arg(1000)->Arg(100000);

}  // namespace tensorflow
