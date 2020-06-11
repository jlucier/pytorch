#pragma once

#include <ATen/BatchedTensorImpl.h>

namespace at {

// This file contains abstractions used for transforming *logical* vmap arguments
// into *physical* arguments. (Keep reading for definitions of these terms).

// NOTE: [Logical vs physical args]
// Consider the following vmap.
//   vmap(vmap(func, in_dims=(2,)), in_dims=(0,))(torch.ones(2, 3, 4))
// This would produce a BatchedTensor wrapping a Tensor of size [2, 3, 4],
// with batch dims 0 and 2:
//   BatchedTensor(ones(2, 3, 4), bdims=[(lvl=1,dim=0),(lvl=2,dim=2)])
//
// We say the *logical* view of the tensor has size [3] -- tensors inside
// `func` appear to have size [3].
// However, the *physical* underlying tensor (the one passed to vmap) has size
// [2, 3, 4].
//
// This notion of logical vs physical also extends to non-tensor arguments.
// Consider the previous tensor; let's assume the user called
// `torch.sum(tensor, dim=0)` inside of `func`. Then the logical
// dimension they are reducing over is dim 0 but the physical dim is dim 1
// (the first non-batch dimension)

// Forward declared; see NOTE: [What is a VmapPhysicalView?]
struct VmapPhysicalView;

// NOTE: [What is an VmapTransform?]
// An *VmapTransform* converts logical views of tensors to physical views.
//
// Batching rules use VmapTransforms to convert logical arguments to
// physical arguments, then call one or more at:: operator that handles the
// physical arguments, and then converts the physical result back to a logical
// argument.

// VmapTransform for operators that take tensors with multiple batch dims.
// Given one or more logical views on Tensors, `logicalToPhysical` 
// permutes all of the batch dims to the front of the tensor, aligns
// and expands the batch dims to match each other (according to their `level`),
// and returns a VmapPhysicalView on the tensor(s).
struct TORCH_API MultiBatchVmapTransform {
  static VmapPhysicalView logicalToPhysical(const Tensor& logical_tensor);
  static std::vector<VmapPhysicalView> logicalToPhysical(TensorList logical_tensors);
};

// VmapTransform for operators that broadcast all inputs.
// Given some logical views on Tensors, `logicalToPhysical`:
// - permutes all of the batch dims to the front of the tensors
// - aligns all the batch dims to the collective levels of all of the tensors.
//   If a tensor does not have a batch dim for a vmap level, then it receives
//   a size-one dimension for said level.
// - aligns the non-batch dims to have the same dimensionality, adding extra
//   size-1 dimensions in between the batch dimensions and the non-batch dimensions
//   so that the batch dimensions are lined up from the right.
//
// For example: given inputs of size (B, 2) and (B, 3, 2) where B is the batch
// dimension, BroadcastingVmapTransform returns VmapPhysicalViews that wrap tensors
// of size (B, 1, 2) and (B, 3, 2).
//
// Given inputs of size (B, 2) and (2,), BroadcastingVmapTransform returns
// VmapPhysicalViews wrapping tensors of size (B, 2) and (1, 2). We don't
// actually *need* to return a tensor of size (B, 2) for the second tensor
// because the broadcasting operation takes care of that for us, but we do
// it anyways to keep things simple.
struct TORCH_API BroadcastingVmapTransform {
  static std::vector<VmapPhysicalView> logicalToPhysical(TensorList logical_tensors);
};

// NOTE: [What is a VmapPhysicalView?]
// VmapPhysicalView represents a physical view on a Tensor.
//
// One can use it to further convert logical dimension indices, logical shapes,
// and more to their physical variants, or convert a new (physical) tensor into
// a logical BatchedTensor. (TODO(rzou): some of these are not yet implemented).
//
// VmapPhysicalView stores a physical tensor with all of its batch dimensions at
// the front and some levels that correspond to said batch dimensions.
//
// The levels bitset specifies which vmap levels correspond to the batch
// dimensions at the front of the tensor. In particular, the number of set bits
// corresponds to the number of batch dimensions on `tensor` and the rightmost
// bit of `levels` specifies the minimum number of nested vmaps we are in at
// this point in time.
struct TORCH_API VmapPhysicalView {
  VmapPhysicalView(Tensor&& tensor, std::bitset<kVmapNumLevels> levels)
      : levels_(levels), tensor_(tensor) {
    TORCH_INTERNAL_ASSERT(!isBatched(tensor));
  }

  Tensor& tensor() { return tensor_; }

  // Maps logical dim indices to physical dim indices. Also does dim wrapping.
  //
  // For example, given:
  //   physical_view = VmapPhysicalView(tensor=ones(2, 3, 4, 5), levels={1, 3})
  //
  // Then physical_view.getPhysicalDims({0, 1}) returns {2, 3}.
  // This is because the size of levels tell us that the first two dimensions
  // of `tensor_` are batch dimensions, so a logical dim of `n` is actually
  // a physical dim of `n + 2`.
  std::vector<int64_t> getPhysicalDims(IntArrayRef logical_dims);
  int64_t getPhysicalDim(int64_t logical_dim);

  // Maps a physical tensor to a new logical tensor (BatchedTensor),
  // using the mapping info stored in this VmapPhysicalView.
  // Assumes that all of the "batch dimensions" are at the front
  // of the physical tensor.
  Tensor newLogicalFromPhysical(const Tensor& physical);

 private:
  int64_t numLogicalDims();
  int64_t numBatchDims();

  std::bitset<kVmapNumLevels> levels_;
  Tensor tensor_;
};


} // namespace at