// Copyright 2018 Blender Foundation. All rights reserved.
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software Foundation,
// Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
//
// Author: Sergey Sharybin

#include "internal/evaluator/evaluator_impl.h"

#include <cassert>
#include <cstdio>

#ifdef _MSC_VER
#  include <iso646.h>
#endif

#include <opensubdiv/far/patchMap.h>
#include <opensubdiv/far/patchTable.h>
#include <opensubdiv/far/patchTableFactory.h>
#include <opensubdiv/osd/cpuEvaluator.h>
#include <opensubdiv/osd/cpuPatchTable.h>
#include <opensubdiv/osd/cpuVertexBuffer.h>
#include <opensubdiv/osd/glComputeEvaluator.h>
#include <opensubdiv/osd/glPatchTable.h>
#include <opensubdiv/osd/glVertexBuffer.h>
#include <opensubdiv/osd/mesh.h>
#include <opensubdiv/osd/types.h>
#include <opensubdiv/version.h>

#include "MEM_guardedalloc.h"

#include "internal/base/type.h"
#include "internal/evaluator/patch_map.h"
#include "internal/topology/topology_refiner_impl.h"
#include "opensubdiv_evaluator_capi.h"
#include "opensubdiv_topology_refiner_capi.h"

using OpenSubdiv::Far::PatchDescriptor;
using OpenSubdiv::Far::PatchTable;
using OpenSubdiv::Far::PatchTableFactory;
using OpenSubdiv::Far::StencilTable;
using OpenSubdiv::Far::StencilTableFactory;
using OpenSubdiv::Far::TopologyRefiner;
using OpenSubdiv::Osd::BufferDescriptor;
using OpenSubdiv::Osd::CpuEvaluator;
using OpenSubdiv::Osd::CpuPatchTable;
using OpenSubdiv::Osd::CpuVertexBuffer;
using OpenSubdiv::Osd::GLComputeEvaluator;
using OpenSubdiv::Osd::GLPatchTable;
using OpenSubdiv::Osd::GLStencilTableSSBO;
using OpenSubdiv::Osd::GLVertexBuffer;
using OpenSubdiv::Osd::PatchArray;
using OpenSubdiv::Osd::PatchArrayVector;
using OpenSubdiv::Osd::PatchCoord;

namespace blender {
namespace opensubdiv {

// Base class for the implementation of the evaluators.
class EvalOutputAPI::EvalOutput {
 public:
  virtual ~EvalOutput() = default;

  virtual void updateData(const float *src, int start_vertex, int num_vertices) = 0;

  virtual void updateVaryingData(const float *src, int start_vertex, int num_vertices) = 0;

  virtual void updateFaceVaryingData(const int face_varying_channel,
                                     const float *src,
                                     int start_vertex,
                                     int num_vertices) = 0;

  virtual void refine() = 0;

  // NOTE: P must point to a memory of at least float[3]*num_patch_coords.
  virtual void evalPatches(const PatchCoord *patch_coord,
                           const int num_patch_coords,
                           float *P) = 0;

  // NOTE: P, dPdu, dPdv must point to a memory of at least float[3]*num_patch_coords.
  virtual void evalPatchesWithDerivatives(const PatchCoord *patch_coord,
                                          const int num_patch_coords,
                                          float *P,
                                          float *dPdu,
                                          float *dPdv) = 0;

  // NOTE: varying must point to a memory of at least float[3]*num_patch_coords.
  virtual void evalPatchesVarying(const PatchCoord *patch_coord,
                                  const int num_patch_coords,
                                  float *varying) = 0;

  virtual void evalPatchesFaceVarying(const int face_varying_channel,
                                      const PatchCoord *patch_coord,
                                      const int num_patch_coords,
                                      float face_varying[2]) = 0;

  // The following interfaces are dependant on the actual evaluator type (CPU, OpenGL, etc.) which
  // have slightly different APIs to access patch arrays, as well as different types for their
  // data structure. They need to be overridden in the specific instances of the EvalOutput derived
  // classes if needed, while the interfaces above are overriden through VolatileEvalOutput.

  virtual void wrapPatchArraysBuffer(OpenSubdiv_BufferInterface * /*patch_arrays_buffer*/)
  {
  }

  virtual void wrapPatchIndexBuffer(OpenSubdiv_BufferInterface * /*patch_index_buffer*/)
  {
  }

  virtual void wrapPatchParamBuffer(OpenSubdiv_BufferInterface * /*patch_param_buffer*/)
  {
  }

  virtual void wrapSrcBuffer(OpenSubdiv_BufferInterface * /*src_buffer*/)
  {
  }

  virtual void wrapFVarPatchArraysBuffer(const int /*face_varying_channel*/,
                                         OpenSubdiv_BufferInterface * /*patch_arrays_buffer*/)
  {
  }

  virtual void wrapFVarPatchIndexBuffer(const int /*face_varying_channel*/,
                                        OpenSubdiv_BufferInterface * /*patch_index_buffer*/)
  {
  }

  virtual void wrapFVarPatchParamBuffer(const int /*face_varying_channel*/,
                                        OpenSubdiv_BufferInterface * /*patch_param_buffer*/)
  {
  }

  virtual void wrapFVarSrcBuffer(const int /*face_varying_channel*/,
                                 OpenSubdiv_BufferInterface * /*src_buffer*/)
  {
  }
};

namespace {

// Array implementation which stores small data on stack (or, rather, in the class itself).
template<typename T, int kNumMaxElementsOnStack> class StackOrHeapArray {
 public:
  StackOrHeapArray()
      : num_elements_(0), heap_elements_(NULL), num_heap_elements_(0), effective_elements_(NULL)
  {
  }

  explicit StackOrHeapArray(int size) : StackOrHeapArray()
  {
    resize(size);
  }

  ~StackOrHeapArray()
  {
    delete[] heap_elements_;
  }

  int size() const
  {
    return num_elements_;
  };

  T *data()
  {
    return effective_elements_;
  }

  void resize(int num_elements)
  {
    const int old_num_elements = num_elements_;
    num_elements_ = num_elements;
    // Early output if allcoation size did not change, or allocation size is smaller.
    // We never re-allocate, sacrificing some memory over performance.
    if (old_num_elements >= num_elements) {
      return;
    }
    // Simple case: no previously allocated buffer, can simply do one allocation.
    if (effective_elements_ == NULL) {
      effective_elements_ = allocate(num_elements);
      return;
    }
    // Make new allocation, and copy elements if needed.
    T *old_buffer = effective_elements_;
    effective_elements_ = allocate(num_elements);
    if (old_buffer != effective_elements_) {
      memcpy(effective_elements_, old_buffer, sizeof(T) * min(old_num_elements, num_elements));
    }
    if (old_buffer != stack_elements_) {
      delete[] old_buffer;
    }
  }

 protected:
  T *allocate(int num_elements)
  {
    if (num_elements < kNumMaxElementsOnStack) {
      return stack_elements_;
    }
    heap_elements_ = new T[num_elements];
    return heap_elements_;
  }

  // Number of elements in the buffer.
  int num_elements_;

  // Elements which are allocated on a stack (or, rather, in the same allocation as the buffer
  // itself).
  // Is used as long as buffer is smaller than kNumMaxElementsOnStack.
  T stack_elements_[kNumMaxElementsOnStack];

  // Heap storage for buffer larger than kNumMaxElementsOnStack.
  T *heap_elements_;
  int num_heap_elements_;

  // Depending on the current buffer size points to rither stack_elements_ or heap_elements_.
  T *effective_elements_;
};

// 32 is a number of inner vertices along the patch size at subdivision level 6.
typedef StackOrHeapArray<PatchCoord, 32 * 32> StackOrHeapPatchCoordArray;

// Buffer which implements API required by OpenSubdiv and uses an existing memory as an underlying
// storage.
template<typename T> class RawDataWrapperBuffer {
 public:
  RawDataWrapperBuffer(T *data) : data_(data)
  {
  }

  T *BindCpuBuffer()
  {
    return data_;
  }

  int BindVBO()
  {
    return 0;
  }

  // TODO(sergey): Support UpdateData().

 protected:
  T *data_;
};

template<typename T> class RawDataWrapperVertexBuffer : public RawDataWrapperBuffer<T> {
 public:
  RawDataWrapperVertexBuffer(T *data, int num_vertices)
      : RawDataWrapperBuffer<T>(data), num_vertices_(num_vertices)
  {
  }

  int GetNumVertices()
  {
    return num_vertices_;
  }

 protected:
  int num_vertices_;
};

class ConstPatchCoordWrapperBuffer : public RawDataWrapperVertexBuffer<const PatchCoord> {
 public:
  ConstPatchCoordWrapperBuffer(const PatchCoord *data, int num_vertices)
      : RawDataWrapperVertexBuffer(data, num_vertices)
  {
  }
};

// Discriminators used in FaceVaryingVolatileEval in order to detect whether we are using adaptive
// patches as the CPU and OpenGL PatchTable have different APIs.
static bool is_adaptive(CpuPatchTable *patch_table)
{
  return patch_table->GetPatchArrayBuffer()[0].GetDescriptor().IsAdaptive();
}

static bool is_adaptive(GLPatchTable *patch_table)
{
  return patch_table->GetPatchArrays()[0].GetDescriptor().IsAdaptive();
}

template<typename EVAL_VERTEX_BUFFER,
         typename STENCIL_TABLE,
         typename PATCH_TABLE,
         typename EVALUATOR,
         typename DEVICE_CONTEXT = void>
class FaceVaryingVolatileEval {
 public:
  typedef OpenSubdiv::Osd::EvaluatorCacheT<EVALUATOR> EvaluatorCache;

  FaceVaryingVolatileEval(int face_varying_channel,
                          const StencilTable *face_varying_stencils,
                          int face_varying_width,
                          PATCH_TABLE *patch_table,
                          EvaluatorCache *evaluator_cache = NULL,
                          DEVICE_CONTEXT *device_context = NULL)
      : face_varying_channel_(face_varying_channel),
        src_face_varying_desc_(0, face_varying_width, face_varying_width),
        patch_table_(patch_table),
        evaluator_cache_(evaluator_cache),
        device_context_(device_context)
  {
    using OpenSubdiv::Osd::convertToCompatibleStencilTable;
    num_coarse_face_varying_vertices_ = face_varying_stencils->GetNumControlVertices();
    const int num_total_face_varying_vertices = face_varying_stencils->GetNumControlVertices() +
                                                face_varying_stencils->GetNumStencils();
    src_face_varying_data_ = EVAL_VERTEX_BUFFER::Create(
        2, num_total_face_varying_vertices, device_context);
    face_varying_stencils_ = convertToCompatibleStencilTable<STENCIL_TABLE>(face_varying_stencils,
                                                                            device_context_);
  }

  ~FaceVaryingVolatileEval()
  {
    delete src_face_varying_data_;
    delete face_varying_stencils_;
  }

  void updateData(const float *src, int start_vertex, int num_vertices)
  {
    src_face_varying_data_->UpdateData(src, start_vertex, num_vertices, device_context_);
  }

  void refine()
  {
    BufferDescriptor dst_face_varying_desc = src_face_varying_desc_;
    dst_face_varying_desc.offset += num_coarse_face_varying_vertices_ *
                                    src_face_varying_desc_.stride;
    const EVALUATOR *eval_instance = OpenSubdiv::Osd::GetEvaluator<EVALUATOR>(
        evaluator_cache_, src_face_varying_desc_, dst_face_varying_desc, device_context_);
    // in and out points to same buffer so output is put directly after coarse vertices, needed in
    // adaptive mode
    EVALUATOR::EvalStencils(src_face_varying_data_,
                            src_face_varying_desc_,
                            src_face_varying_data_,
                            dst_face_varying_desc,
                            face_varying_stencils_,
                            eval_instance,
                            device_context_);
  }

  // NOTE: face_varying must point to a memory of at least float[2]*num_patch_coords.
  void evalPatches(const PatchCoord *patch_coord, const int num_patch_coords, float *face_varying)
  {
    RawDataWrapperBuffer<float> face_varying_data(face_varying);
    BufferDescriptor face_varying_desc(0, 2, 2);
    ConstPatchCoordWrapperBuffer patch_coord_buffer(patch_coord, num_patch_coords);
    const EVALUATOR *eval_instance = OpenSubdiv::Osd::GetEvaluator<EVALUATOR>(
        evaluator_cache_, src_face_varying_desc_, face_varying_desc, device_context_);

    BufferDescriptor src_desc = get_src_varying_desc();

    EVALUATOR::EvalPatchesFaceVarying(src_face_varying_data_,
                                      src_desc,
                                      &face_varying_data,
                                      face_varying_desc,
                                      patch_coord_buffer.GetNumVertices(),
                                      &patch_coord_buffer,
                                      patch_table_,
                                      face_varying_channel_,
                                      eval_instance,
                                      device_context_);
  }

  EVAL_VERTEX_BUFFER *getSrcBuffer() const
  {
    return src_face_varying_data_;
  }

  int getFVarSrcBufferOffset() const
  {
    BufferDescriptor src_desc = get_src_varying_desc();
    return src_desc.offset;
  }

  PATCH_TABLE *getPatchTable() const
  {
    return patch_table_;
  }

 private:
  BufferDescriptor get_src_varying_desc() const
  {
    // src_face_varying_data_ always contains coarse vertices at the beginning.
    // In adaptive mode they are followed by number of blocks for intermediate
    // subdivision levels, and this is what OSD expects in this mode.
    // In non-adaptive mode (generateIntermediateLevels == false),
    // they are followed by max subdivision level, but they break interpolation as OSD
    // expects only one subd level in this buffer.
    // So in non-adaptive mode we put offset into buffer descriptor to skip over coarse vertices.
    BufferDescriptor src_desc = src_face_varying_desc_;
    if (!is_adaptive(patch_table_)) {
      src_desc.offset += num_coarse_face_varying_vertices_ * src_face_varying_desc_.stride;
    }
    return src_desc;
  }

 protected:
  int face_varying_channel_;

  BufferDescriptor src_face_varying_desc_;

  int num_coarse_face_varying_vertices_;
  EVAL_VERTEX_BUFFER *src_face_varying_data_;
  const STENCIL_TABLE *face_varying_stencils_;

  // NOTE: We reference this, do not own it.
  PATCH_TABLE *patch_table_;

  EvaluatorCache *evaluator_cache_;
  DEVICE_CONTEXT *device_context_;
};

// Volatile evaluator which can be used from threads.
//
// TODO(sergey): Make it possible to evaluate coordinates in chunks.
// TODO(sergey): Make it possible to evaluate multiple face varying layers.
//               (or maybe, it's cheap to create new evaluator for existing
//               topology to evaluate all needed face varying layers?)
template<typename SRC_VERTEX_BUFFER,
         typename EVAL_VERTEX_BUFFER,
         typename STENCIL_TABLE,
         typename PATCH_TABLE,
         typename EVALUATOR,
         typename DEVICE_CONTEXT = void>
class VolatileEvalOutput : public EvalOutputAPI::EvalOutput {
 public:
  typedef OpenSubdiv::Osd::EvaluatorCacheT<EVALUATOR> EvaluatorCache;
  typedef FaceVaryingVolatileEval<EVAL_VERTEX_BUFFER,
                                  STENCIL_TABLE,
                                  PATCH_TABLE,
                                  EVALUATOR,
                                  DEVICE_CONTEXT>
      FaceVaryingEval;

  VolatileEvalOutput(const StencilTable *vertex_stencils,
                     const StencilTable *varying_stencils,
                     const vector<const StencilTable *> &all_face_varying_stencils,
                     const int face_varying_width,
                     const PatchTable *patch_table,
                     EvaluatorCache *evaluator_cache = NULL,
                     DEVICE_CONTEXT *device_context = NULL)
      : src_desc_(0, 3, 3),
        src_varying_desc_(0, 3, 3),
        face_varying_width_(face_varying_width),
        evaluator_cache_(evaluator_cache),
        device_context_(device_context)
  {
    // Total number of vertices = coarse points + refined points + local points.
    int num_total_vertices = vertex_stencils->GetNumControlVertices() +
                             vertex_stencils->GetNumStencils();
    num_coarse_vertices_ = vertex_stencils->GetNumControlVertices();
    using OpenSubdiv::Osd::convertToCompatibleStencilTable;
    src_data_ = SRC_VERTEX_BUFFER::Create(3, num_total_vertices, device_context_);
    src_varying_data_ = SRC_VERTEX_BUFFER::Create(3, num_total_vertices, device_context_);
    patch_table_ = PATCH_TABLE::Create(patch_table, device_context_);
    vertex_stencils_ = convertToCompatibleStencilTable<STENCIL_TABLE>(vertex_stencils,
                                                                      device_context_);
    varying_stencils_ = convertToCompatibleStencilTable<STENCIL_TABLE>(varying_stencils,
                                                                       device_context_);
    // Create evaluators for every face varying channel.
    face_varying_evaluators.reserve(all_face_varying_stencils.size());
    int face_varying_channel = 0;
    for (const StencilTable *face_varying_stencils : all_face_varying_stencils) {
      face_varying_evaluators.push_back(new FaceVaryingEval(face_varying_channel,
                                                            face_varying_stencils,
                                                            face_varying_width,
                                                            patch_table_,
                                                            evaluator_cache_,
                                                            device_context_));
      ++face_varying_channel;
    }
  }

  ~VolatileEvalOutput() override
  {
    delete src_data_;
    delete src_varying_data_;
    delete patch_table_;
    delete vertex_stencils_;
    delete varying_stencils_;
    for (FaceVaryingEval *face_varying_evaluator : face_varying_evaluators) {
      delete face_varying_evaluator;
    }
  }

  // TODO(sergey): Implement binding API.

  void updateData(const float *src, int start_vertex, int num_vertices) override
  {
    src_data_->UpdateData(src, start_vertex, num_vertices, device_context_);
  }

  void updateVaryingData(const float *src, int start_vertex, int num_vertices) override
  {
    src_varying_data_->UpdateData(src, start_vertex, num_vertices, device_context_);
  }

  void updateFaceVaryingData(const int face_varying_channel,
                             const float *src,
                             int start_vertex,
                             int num_vertices) override
  {
    assert(face_varying_channel >= 0);
    assert(face_varying_channel < face_varying_evaluators.size());
    face_varying_evaluators[face_varying_channel]->updateData(src, start_vertex, num_vertices);
  }

  bool hasVaryingData() const
  {
    // return varying_stencils_ != NULL;
    // TODO(sergey): Check this based on actual topology.
    return false;
  }

  bool hasFaceVaryingData() const
  {
    return face_varying_evaluators.size() != 0;
  }

  void refine() override
  {
    // Evaluate vertex positions.
    BufferDescriptor dst_desc = src_desc_;
    dst_desc.offset += num_coarse_vertices_ * src_desc_.stride;
    const EVALUATOR *eval_instance = OpenSubdiv::Osd::GetEvaluator<EVALUATOR>(
        evaluator_cache_, src_desc_, dst_desc, device_context_);
    EVALUATOR::EvalStencils(src_data_,
                            src_desc_,
                            src_data_,
                            dst_desc,
                            vertex_stencils_,
                            eval_instance,
                            device_context_);
    // Evaluate varying data.
    if (hasVaryingData()) {
      BufferDescriptor dst_varying_desc = src_varying_desc_;
      dst_varying_desc.offset += num_coarse_vertices_ * src_varying_desc_.stride;
      eval_instance = OpenSubdiv::Osd::GetEvaluator<EVALUATOR>(
          evaluator_cache_, src_varying_desc_, dst_varying_desc, device_context_);
      EVALUATOR::EvalStencils(src_varying_data_,
                              src_varying_desc_,
                              src_varying_data_,
                              dst_varying_desc,
                              varying_stencils_,
                              eval_instance,
                              device_context_);
    }
    // Evaluate face-varying data.
    if (hasFaceVaryingData()) {
      for (FaceVaryingEval *face_varying_evaluator : face_varying_evaluators) {
        face_varying_evaluator->refine();
      }
    }
  }

  // NOTE: P must point to a memory of at least float[3]*num_patch_coords.
  void evalPatches(const PatchCoord *patch_coord, const int num_patch_coords, float *P) override
  {
    RawDataWrapperBuffer<float> P_data(P);
    // TODO(sergey): Support interleaved vertex-varying data.
    BufferDescriptor P_desc(0, 3, 3);
    ConstPatchCoordWrapperBuffer patch_coord_buffer(patch_coord, num_patch_coords);
    const EVALUATOR *eval_instance = OpenSubdiv::Osd::GetEvaluator<EVALUATOR>(
        evaluator_cache_, src_desc_, P_desc, device_context_);
    EVALUATOR::EvalPatches(src_data_,
                           src_desc_,
                           &P_data,
                           P_desc,
                           patch_coord_buffer.GetNumVertices(),
                           &patch_coord_buffer,
                           patch_table_,
                           eval_instance,
                           device_context_);
  }

  // NOTE: P, dPdu, dPdv must point to a memory of at least float[3]*num_patch_coords.
  void evalPatchesWithDerivatives(const PatchCoord *patch_coord,
                                  const int num_patch_coords,
                                  float *P,
                                  float *dPdu,
                                  float *dPdv) override
  {
    assert(dPdu);
    assert(dPdv);
    RawDataWrapperBuffer<float> P_data(P);
    RawDataWrapperBuffer<float> dPdu_data(dPdu), dPdv_data(dPdv);
    // TODO(sergey): Support interleaved vertex-varying data.
    BufferDescriptor P_desc(0, 3, 3);
    BufferDescriptor dpDu_desc(0, 3, 3), pPdv_desc(0, 3, 3);
    ConstPatchCoordWrapperBuffer patch_coord_buffer(patch_coord, num_patch_coords);
    const EVALUATOR *eval_instance = OpenSubdiv::Osd::GetEvaluator<EVALUATOR>(
        evaluator_cache_, src_desc_, P_desc, dpDu_desc, pPdv_desc, device_context_);
    EVALUATOR::EvalPatches(src_data_,
                           src_desc_,
                           &P_data,
                           P_desc,
                           &dPdu_data,
                           dpDu_desc,
                           &dPdv_data,
                           pPdv_desc,
                           patch_coord_buffer.GetNumVertices(),
                           &patch_coord_buffer,
                           patch_table_,
                           eval_instance,
                           device_context_);
  }

  // NOTE: varying must point to a memory of at least float[3]*num_patch_coords.
  void evalPatchesVarying(const PatchCoord *patch_coord,
                          const int num_patch_coords,
                          float *varying) override
  {
    RawDataWrapperBuffer<float> varying_data(varying);
    BufferDescriptor varying_desc(3, 3, 6);
    ConstPatchCoordWrapperBuffer patch_coord_buffer(patch_coord, num_patch_coords);
    const EVALUATOR *eval_instance = OpenSubdiv::Osd::GetEvaluator<EVALUATOR>(
        evaluator_cache_, src_varying_desc_, varying_desc, device_context_);
    EVALUATOR::EvalPatchesVarying(src_varying_data_,
                                  src_varying_desc_,
                                  &varying_data,
                                  varying_desc,
                                  patch_coord_buffer.GetNumVertices(),
                                  &patch_coord_buffer,
                                  patch_table_,
                                  eval_instance,
                                  device_context_);
  }

  void evalPatchesFaceVarying(const int face_varying_channel,
                              const PatchCoord *patch_coord,
                              const int num_patch_coords,
                              float face_varying[2]) override
  {
    assert(face_varying_channel >= 0);
    assert(face_varying_channel < face_varying_evaluators.size());
    face_varying_evaluators[face_varying_channel]->evalPatches(
        patch_coord, num_patch_coords, face_varying);
  }

  SRC_VERTEX_BUFFER *getSrcBuffer() const
  {
    return src_data_;
  }

  PATCH_TABLE *getPatchTable() const
  {
    return patch_table_;
  }

  SRC_VERTEX_BUFFER *getFVarSrcBuffer(const int face_varying_channel) const
  {
    return face_varying_evaluators[face_varying_channel]->getSrcBuffer();
  }

  int getFVarSrcBufferOffset(const int face_varying_channel) const
  {
    return face_varying_evaluators[face_varying_channel]->getFVarSrcBufferOffset();
  }

  PATCH_TABLE *getFVarPatchTable(const int face_varying_channel) const
  {
    return face_varying_evaluators[face_varying_channel]->getPatchTable();
  }

 private:
  SRC_VERTEX_BUFFER *src_data_;
  SRC_VERTEX_BUFFER *src_varying_data_;
  PATCH_TABLE *patch_table_;
  BufferDescriptor src_desc_;
  BufferDescriptor src_varying_desc_;

  int num_coarse_vertices_;

  const STENCIL_TABLE *vertex_stencils_;
  const STENCIL_TABLE *varying_stencils_;

  int face_varying_width_;
  vector<FaceVaryingEval *> face_varying_evaluators;

  EvaluatorCache *evaluator_cache_;
  DEVICE_CONTEXT *device_context_;
};

void convertPatchCoordsToArray(const OpenSubdiv_PatchCoord *patch_coords,
                               const int num_patch_coords,
                               const PatchMap *patch_map,
                               StackOrHeapPatchCoordArray *array)
{
  array->resize(num_patch_coords);
  for (int i = 0; i < num_patch_coords; ++i) {
    const PatchTable::PatchHandle *handle = patch_map->FindPatch(
        patch_coords[i].ptex_face, patch_coords[i].u, patch_coords[i].v);
    (array->data())[i] = PatchCoord(*handle, patch_coords[i].u, patch_coords[i].v);
  }
}

static void buildPatchArraysBufferFromVector(const PatchArrayVector &patch_arrays,
                                             OpenSubdiv_BufferInterface *patch_arrays_buffer)
{
  const size_t patch_array_size = sizeof(PatchArray);
  const size_t patch_array_byte_site = patch_array_size * patch_arrays.size();
  patch_arrays_buffer->device_alloc(patch_arrays_buffer, patch_arrays.size());
  patch_arrays_buffer->bind_gpu(patch_arrays_buffer);
  patch_arrays_buffer->device_update(
      patch_arrays_buffer, 0, patch_array_byte_site, &patch_arrays[0]);
}

}  // namespace

// Note: Define as a class instead of typedef to make it possible
// to have anonymous class in opensubdiv_evaluator_internal.h
class CpuEvalOutput : public VolatileEvalOutput<CpuVertexBuffer,
                                                CpuVertexBuffer,
                                                StencilTable,
                                                CpuPatchTable,
                                                CpuEvaluator> {
 public:
  CpuEvalOutput(const StencilTable *vertex_stencils,
                const StencilTable *varying_stencils,
                const vector<const StencilTable *> &all_face_varying_stencils,
                const int face_varying_width,
                const PatchTable *patch_table,
                EvaluatorCache *evaluator_cache = NULL)
      : VolatileEvalOutput<CpuVertexBuffer,
                           CpuVertexBuffer,
                           StencilTable,
                           CpuPatchTable,
                           CpuEvaluator>(vertex_stencils,
                                         varying_stencils,
                                         all_face_varying_stencils,
                                         face_varying_width,
                                         patch_table,
                                         evaluator_cache)
  {
  }
};

class GpuEvalOutput : public VolatileEvalOutput<GLVertexBuffer,
                                                GLVertexBuffer,
                                                GLStencilTableSSBO,
                                                GLPatchTable,
                                                GLComputeEvaluator> {
 public:
  GpuEvalOutput(const StencilTable *vertex_stencils,
                const StencilTable *varying_stencils,
                const vector<const StencilTable *> &all_face_varying_stencils,
                const int face_varying_width,
                const PatchTable *patch_table,
                EvaluatorCache *evaluator_cache = NULL)
      : VolatileEvalOutput<GLVertexBuffer,
                           GLVertexBuffer,
                           GLStencilTableSSBO,
                           GLPatchTable,
                           GLComputeEvaluator>(vertex_stencils,
                                               varying_stencils,
                                               all_face_varying_stencils,
                                               face_varying_width,
                                               patch_table,
                                               evaluator_cache)
  {
  }

  void wrapPatchArraysBuffer(OpenSubdiv_BufferInterface *patch_arrays_buffer) override
  {
    GLPatchTable *patch_table = getPatchTable();
    buildPatchArraysBufferFromVector(patch_table->GetPatchArrays(), patch_arrays_buffer);
  }

  void wrapPatchIndexBuffer(OpenSubdiv_BufferInterface *patch_index_buffer) override
  {
    GLPatchTable *patch_table = getPatchTable();
    patch_index_buffer->wrap_device_handle(patch_index_buffer, patch_table->GetPatchIndexBuffer());
  }

  void wrapPatchParamBuffer(OpenSubdiv_BufferInterface *patch_param_buffer) override
  {
    GLPatchTable *patch_table = getPatchTable();
    patch_param_buffer->wrap_device_handle(patch_param_buffer, patch_table->GetPatchParamBuffer());
  }

  void wrapSrcBuffer(OpenSubdiv_BufferInterface *src_buffer) override
  {
    GLVertexBuffer *vertex_buffer = getSrcBuffer();
    src_buffer->wrap_device_handle(src_buffer, vertex_buffer->BindVBO());
  }

  void wrapFVarPatchArraysBuffer(const int face_varying_channel,
                                 OpenSubdiv_BufferInterface *patch_arrays_buffer) override
  {
    GLPatchTable *patch_table = getFVarPatchTable(face_varying_channel);
    buildPatchArraysBufferFromVector(patch_table->GetFVarPatchArrays(face_varying_channel),
                                     patch_arrays_buffer);
  }

  void wrapFVarPatchIndexBuffer(const int face_varying_channel,
                                OpenSubdiv_BufferInterface *patch_index_buffer) override
  {
    GLPatchTable *patch_table = getFVarPatchTable(face_varying_channel);
    patch_index_buffer->wrap_device_handle(
        patch_index_buffer, patch_table->GetFVarPatchIndexBuffer(face_varying_channel));
  }

  void wrapFVarPatchParamBuffer(const int face_varying_channel,
                                OpenSubdiv_BufferInterface *patch_param_buffer) override
  {
    GLPatchTable *patch_table = getFVarPatchTable(face_varying_channel);
    patch_param_buffer->wrap_device_handle(
        patch_param_buffer, patch_table->GetFVarPatchParamBuffer(face_varying_channel));
  }

  void wrapFVarSrcBuffer(const int face_varying_channel,
                         OpenSubdiv_BufferInterface *src_buffer) override
  {
    GLVertexBuffer *vertex_buffer = getFVarSrcBuffer(face_varying_channel);
    src_buffer->buffer_offset = getFVarSrcBufferOffset(face_varying_channel);
    src_buffer->wrap_device_handle(src_buffer, vertex_buffer->BindVBO());
  }
};

////////////////////////////////////////////////////////////////////////////////
// Evaluator wrappers for anonymous API.

EvalOutputAPI::EvalOutputAPI(EvalOutput *implementation, PatchMap *patch_map)
    : patch_map_(patch_map), implementation_(implementation)
{
}

EvalOutputAPI::~EvalOutputAPI()
{
  delete implementation_;
}

void EvalOutputAPI::setCoarsePositions(const float *positions,
                                       const int start_vertex_index,
                                       const int num_vertices)
{
  // TODO(sergey): Add sanity check on indices.
  implementation_->updateData(positions, start_vertex_index, num_vertices);
}

void EvalOutputAPI::setVaryingData(const float *varying_data,
                                   const int start_vertex_index,
                                   const int num_vertices)
{
  // TODO(sergey): Add sanity check on indices.
  implementation_->updateVaryingData(varying_data, start_vertex_index, num_vertices);
}

void EvalOutputAPI::setFaceVaryingData(const int face_varying_channel,
                                       const float *face_varying_data,
                                       const int start_vertex_index,
                                       const int num_vertices)
{
  // TODO(sergey): Add sanity check on indices.
  implementation_->updateFaceVaryingData(
      face_varying_channel, face_varying_data, start_vertex_index, num_vertices);
}

void EvalOutputAPI::setCoarsePositionsFromBuffer(const void *buffer,
                                                 const int start_offset,
                                                 const int stride,
                                                 const int start_vertex_index,
                                                 const int num_vertices)
{
  // TODO(sergey): Add sanity check on indices.
  const unsigned char *current_buffer = (unsigned char *)buffer;
  current_buffer += start_offset;
  for (int i = 0; i < num_vertices; ++i) {
    const int current_vertex_index = start_vertex_index + i;
    implementation_->updateData(
        reinterpret_cast<const float *>(current_buffer), current_vertex_index, 1);
    current_buffer += stride;
  }
}

void EvalOutputAPI::setVaryingDataFromBuffer(const void *buffer,
                                             const int start_offset,
                                             const int stride,
                                             const int start_vertex_index,
                                             const int num_vertices)
{
  // TODO(sergey): Add sanity check on indices.
  const unsigned char *current_buffer = (unsigned char *)buffer;
  current_buffer += start_offset;
  for (int i = 0; i < num_vertices; ++i) {
    const int current_vertex_index = start_vertex_index + i;
    implementation_->updateVaryingData(
        reinterpret_cast<const float *>(current_buffer), current_vertex_index, 1);
    current_buffer += stride;
  }
}

void EvalOutputAPI::setFaceVaryingDataFromBuffer(const int face_varying_channel,
                                                 const void *buffer,
                                                 const int start_offset,
                                                 const int stride,
                                                 const int start_vertex_index,
                                                 const int num_vertices)
{
  // TODO(sergey): Add sanity check on indices.
  const unsigned char *current_buffer = (unsigned char *)buffer;
  current_buffer += start_offset;
  for (int i = 0; i < num_vertices; ++i) {
    const int current_vertex_index = start_vertex_index + i;
    implementation_->updateFaceVaryingData(face_varying_channel,
                                           reinterpret_cast<const float *>(current_buffer),
                                           current_vertex_index,
                                           1);
    current_buffer += stride;
  }
}

void EvalOutputAPI::refine()
{
  implementation_->refine();
}

void EvalOutputAPI::evaluateLimit(const int ptex_face_index,
                                  float face_u,
                                  float face_v,
                                  float P[3],
                                  float dPdu[3],
                                  float dPdv[3])
{
  assert(face_u >= 0.0f);
  assert(face_u <= 1.0f);
  assert(face_v >= 0.0f);
  assert(face_v <= 1.0f);
  const PatchTable::PatchHandle *handle = patch_map_->FindPatch(ptex_face_index, face_u, face_v);
  PatchCoord patch_coord(*handle, face_u, face_v);
  if (dPdu != NULL || dPdv != NULL) {
    implementation_->evalPatchesWithDerivatives(&patch_coord, 1, P, dPdu, dPdv);
  }
  else {
    implementation_->evalPatches(&patch_coord, 1, P);
  }
}

void EvalOutputAPI::evaluateVarying(const int ptex_face_index,
                                    float face_u,
                                    float face_v,
                                    float varying[3])
{
  assert(face_u >= 0.0f);
  assert(face_u <= 1.0f);
  assert(face_v >= 0.0f);
  assert(face_v <= 1.0f);
  const PatchTable::PatchHandle *handle = patch_map_->FindPatch(ptex_face_index, face_u, face_v);
  PatchCoord patch_coord(*handle, face_u, face_v);
  implementation_->evalPatchesVarying(&patch_coord, 1, varying);
}

void EvalOutputAPI::evaluateFaceVarying(const int face_varying_channel,
                                        const int ptex_face_index,
                                        float face_u,
                                        float face_v,
                                        float face_varying[2])
{
  assert(face_u >= 0.0f);
  assert(face_u <= 1.0f);
  assert(face_v >= 0.0f);
  assert(face_v <= 1.0f);
  const PatchTable::PatchHandle *handle = patch_map_->FindPatch(ptex_face_index, face_u, face_v);
  PatchCoord patch_coord(*handle, face_u, face_v);
  implementation_->evalPatchesFaceVarying(face_varying_channel, &patch_coord, 1, face_varying);
}

void EvalOutputAPI::evaluatePatchesLimit(const OpenSubdiv_PatchCoord *patch_coords,
                                         const int num_patch_coords,
                                         float *P,
                                         float *dPdu,
                                         float *dPdv)
{
  StackOrHeapPatchCoordArray patch_coords_array;
  convertPatchCoordsToArray(patch_coords, num_patch_coords, patch_map_, &patch_coords_array);
  if (dPdu != NULL || dPdv != NULL) {
    implementation_->evalPatchesWithDerivatives(
        patch_coords_array.data(), num_patch_coords, P, dPdu, dPdv);
  }
  else {
    implementation_->evalPatches(patch_coords_array.data(), num_patch_coords, P);
  }
}

void EvalOutputAPI::getPatchMap(OpenSubdiv_BufferInterface *patch_map_handles,
                                OpenSubdiv_BufferInterface *patch_map_quadtree,
                                int *min_patch_face,
                                int *max_patch_face,
                                int *max_depth,
                                int *patches_are_triangular)
{
  *min_patch_face = patch_map_->getMinPatchFace();
  *max_patch_face = patch_map_->getMaxPatchFace();
  *max_depth = patch_map_->getMaxDepth();
  *patches_are_triangular = patch_map_->getPatchesAreTriangular();

  const std::vector<PatchTable::PatchHandle> &handles = patch_map_->getHandles();
  PatchTable::PatchHandle *buffer_handles = static_cast<PatchTable::PatchHandle *>(
      patch_map_handles->alloc(patch_map_handles, handles.size()));
  memcpy(buffer_handles, &handles[0], sizeof(PatchTable::PatchHandle) * handles.size());

  const std::vector<PatchMap::QuadNode> &quadtree = patch_map_->nodes();
  PatchMap::QuadNode *buffer_nodes = static_cast<PatchMap::QuadNode *>(
      patch_map_quadtree->alloc(patch_map_quadtree, quadtree.size()));
  memcpy(buffer_nodes, &quadtree[0], sizeof(PatchMap::QuadNode) * quadtree.size());
}

void EvalOutputAPI::wrapPatchArraysBuffer(OpenSubdiv_BufferInterface *patch_arrays_buffer)
{
  implementation_->wrapPatchArraysBuffer(patch_arrays_buffer);
}

void EvalOutputAPI::wrapPatchIndexBuffer(OpenSubdiv_BufferInterface *patch_index_buffer)
{
  implementation_->wrapPatchIndexBuffer(patch_index_buffer);
}

void EvalOutputAPI::wrapPatchParamBuffer(OpenSubdiv_BufferInterface *patch_param_buffer)
{
  implementation_->wrapPatchParamBuffer(patch_param_buffer);
}

void EvalOutputAPI::wrapSrcBuffer(OpenSubdiv_BufferInterface *src_buffer)
{
  implementation_->wrapSrcBuffer(src_buffer);
}

void EvalOutputAPI::wrapFVarPatchArraysBuffer(const int face_varying_channel,
                                              OpenSubdiv_BufferInterface *patch_arrays_buffer)
{
  implementation_->wrapFVarPatchArraysBuffer(face_varying_channel, patch_arrays_buffer);
}

void EvalOutputAPI::wrapFVarPatchIndexBuffer(const int face_varying_channel,
                                             OpenSubdiv_BufferInterface *patch_index_buffer)
{
  implementation_->wrapFVarPatchIndexBuffer(face_varying_channel, patch_index_buffer);
}

void EvalOutputAPI::wrapFVarPatchParamBuffer(const int face_varying_channel,
                                             OpenSubdiv_BufferInterface *patch_param_buffer)
{
  implementation_->wrapFVarPatchParamBuffer(face_varying_channel, patch_param_buffer);
}

void EvalOutputAPI::wrapFVarSrcBuffer(const int face_varying_channel,
                                      OpenSubdiv_BufferInterface *src_buffer)
{
  implementation_->wrapFVarSrcBuffer(face_varying_channel, src_buffer);
}

}  // namespace opensubdiv
}  // namespace blender

OpenSubdiv_EvaluatorImpl::OpenSubdiv_EvaluatorImpl()
    : eval_output(NULL), patch_map(NULL), patch_table(NULL)
{
}

OpenSubdiv_EvaluatorImpl::~OpenSubdiv_EvaluatorImpl()
{
  delete eval_output;
  delete patch_map;
  delete patch_table;
}

OpenSubdiv_EvaluatorImpl *openSubdiv_createEvaluatorInternal(
    OpenSubdiv_TopologyRefiner *topology_refiner,
    eOpenSubdivEvaluator evaluator_type,
    OpenSubdiv_EvaluatorCacheImpl *evaluator_cache_descr)
{
  // Only CPU and GLCompute are implemented at the moment.
  if (evaluator_type != OPENSUBDIV_EVALUATOR_CPU &&
      evaluator_type != OPENSUBDIV_EVALUATOR_GLSL_COMPUTE) {
    return NULL;
  }
  using blender::opensubdiv::vector;
  TopologyRefiner *refiner = topology_refiner->impl->topology_refiner;
  if (refiner == NULL) {
    // Happens on bad topology.
    return NULL;
  }
  // TODO(sergey): Base this on actual topology.
  const bool has_varying_data = false;
  const int num_face_varying_channels = refiner->GetNumFVarChannels();
  const bool has_face_varying_data = (num_face_varying_channels != 0);
  const int level = topology_refiner->getSubdivisionLevel(topology_refiner);
  const bool is_adaptive = topology_refiner->getIsAdaptive(topology_refiner);
  // Common settings for stencils and patches.
  const bool stencil_generate_intermediate_levels = is_adaptive;
  const bool stencil_generate_offsets = true;
  const bool use_inf_sharp_patch = true;
  // Refine the topology with given settings.
  // TODO(sergey): What if topology is already refined?
  if (is_adaptive) {
    TopologyRefiner::AdaptiveOptions options(level);
    options.considerFVarChannels = has_face_varying_data;
    options.useInfSharpPatch = use_inf_sharp_patch;
    refiner->RefineAdaptive(options);
  }
  else {
    TopologyRefiner::UniformOptions options(level);
    refiner->RefineUniform(options);
  }
  // Generate stencil table to update the bi-cubic patches control vertices
  // after they have been re-posed (both for vertex & varying interpolation).
  //
  // Vertex stencils.
  StencilTableFactory::Options vertex_stencil_options;
  vertex_stencil_options.generateOffsets = stencil_generate_offsets;
  vertex_stencil_options.generateIntermediateLevels = stencil_generate_intermediate_levels;
  const StencilTable *vertex_stencils = StencilTableFactory::Create(*refiner,
                                                                    vertex_stencil_options);
  // Varying stencils.
  //
  // TODO(sergey): Seems currently varying stencils are always required in
  // OpenSubdiv itself.
  const StencilTable *varying_stencils = NULL;
  if (has_varying_data) {
    StencilTableFactory::Options varying_stencil_options;
    varying_stencil_options.generateOffsets = stencil_generate_offsets;
    varying_stencil_options.generateIntermediateLevels = stencil_generate_intermediate_levels;
    varying_stencil_options.interpolationMode = StencilTableFactory::INTERPOLATE_VARYING;
    varying_stencils = StencilTableFactory::Create(*refiner, varying_stencil_options);
  }
  // Face warying stencil.
  vector<const StencilTable *> all_face_varying_stencils;
  all_face_varying_stencils.reserve(num_face_varying_channels);
  for (int face_varying_channel = 0; face_varying_channel < num_face_varying_channels;
       ++face_varying_channel) {
    StencilTableFactory::Options face_varying_stencil_options;
    face_varying_stencil_options.generateOffsets = stencil_generate_offsets;
    face_varying_stencil_options.generateIntermediateLevels = stencil_generate_intermediate_levels;
    face_varying_stencil_options.interpolationMode = StencilTableFactory::INTERPOLATE_FACE_VARYING;
    face_varying_stencil_options.fvarChannel = face_varying_channel;
    all_face_varying_stencils.push_back(
        StencilTableFactory::Create(*refiner, face_varying_stencil_options));
  }
  // Generate bi-cubic patch table for the limit surface.
  PatchTableFactory::Options patch_options(level);
  patch_options.SetEndCapType(PatchTableFactory::Options::ENDCAP_GREGORY_BASIS);
  patch_options.useInfSharpPatch = use_inf_sharp_patch;
  patch_options.generateFVarTables = has_face_varying_data;
  patch_options.generateFVarLegacyLinearPatches = false;
  const PatchTable *patch_table = PatchTableFactory::Create(*refiner, patch_options);
  // Append local points stencils.
  // Point stencils.
  const StencilTable *local_point_stencil_table = patch_table->GetLocalPointStencilTable();
  if (local_point_stencil_table != NULL) {
    const StencilTable *table = StencilTableFactory::AppendLocalPointStencilTable(
        *refiner, vertex_stencils, local_point_stencil_table);
    delete vertex_stencils;
    vertex_stencils = table;
  }
  // Varying stencils.
  if (has_varying_data) {
    const StencilTable *local_point_varying_stencil_table =
        patch_table->GetLocalPointVaryingStencilTable();
    if (local_point_varying_stencil_table != NULL) {
      const StencilTable *table = StencilTableFactory::AppendLocalPointStencilTable(
          *refiner, varying_stencils, local_point_varying_stencil_table);
      delete varying_stencils;
      varying_stencils = table;
    }
  }
  for (int face_varying_channel = 0; face_varying_channel < num_face_varying_channels;
       ++face_varying_channel) {
    const StencilTable *table = StencilTableFactory::AppendLocalPointStencilTableFaceVarying(
        *refiner,
        all_face_varying_stencils[face_varying_channel],
        patch_table->GetLocalPointFaceVaryingStencilTable(face_varying_channel),
        face_varying_channel);
    if (table != NULL) {
      delete all_face_varying_stencils[face_varying_channel];
      all_face_varying_stencils[face_varying_channel] = table;
    }
  }
  // Create OpenSubdiv's CPU side evaluator.
  blender::opensubdiv::EvalOutputAPI::EvalOutput *eval_output = nullptr;

  const bool use_gl_evaluator = evaluator_type == OPENSUBDIV_EVALUATOR_GLSL_COMPUTE;
  if (use_gl_evaluator) {
    blender::opensubdiv::GpuEvalOutput::EvaluatorCache *evaluator_cache = nullptr;
    if (evaluator_cache_descr) {
      evaluator_cache = static_cast<blender::opensubdiv::GpuEvalOutput::EvaluatorCache *>(
          evaluator_cache_descr->eval_cache);
    }

    eval_output = new blender::opensubdiv::GpuEvalOutput(vertex_stencils,
                                                         varying_stencils,
                                                         all_face_varying_stencils,
                                                         2,
                                                         patch_table,
                                                         evaluator_cache);
  }
  else {
    eval_output = new blender::opensubdiv::CpuEvalOutput(
        vertex_stencils, varying_stencils, all_face_varying_stencils, 2, patch_table);
  }

  blender::opensubdiv::PatchMap *patch_map = new blender::opensubdiv::PatchMap(*patch_table);
  // Wrap everything we need into an object which we control from our side.
  OpenSubdiv_EvaluatorImpl *evaluator_descr;
  evaluator_descr = new OpenSubdiv_EvaluatorImpl();

  evaluator_descr->eval_output = new blender::opensubdiv::EvalOutputAPI(eval_output, patch_map);
  evaluator_descr->patch_map = patch_map;
  evaluator_descr->patch_table = patch_table;
  // TOOD(sergey): Look into whether we've got duplicated stencils arrays.
  delete vertex_stencils;
  delete varying_stencils;
  for (const StencilTable *table : all_face_varying_stencils) {
    delete table;
  }
  return evaluator_descr;
}

void openSubdiv_deleteEvaluatorInternal(OpenSubdiv_EvaluatorImpl *evaluator)
{
  delete evaluator;
}

OpenSubdiv_EvaluatorCacheImpl::OpenSubdiv_EvaluatorCacheImpl()
{
}

OpenSubdiv_EvaluatorCacheImpl::~OpenSubdiv_EvaluatorCacheImpl()
{
  delete static_cast<blender::opensubdiv::GpuEvalOutput::EvaluatorCache *>(eval_cache);
}

OpenSubdiv_EvaluatorCacheImpl *openSubdiv_createEvaluatorCacheInternal(
    eOpenSubdivEvaluator evaluator_type)
{
  if (evaluator_type != eOpenSubdivEvaluator::OPENSUBDIV_EVALUATOR_GLSL_COMPUTE) {
    return nullptr;
  }
  OpenSubdiv_EvaluatorCacheImpl *evaluator_cache;
  evaluator_cache = new OpenSubdiv_EvaluatorCacheImpl;
  blender::opensubdiv::GpuEvalOutput::EvaluatorCache *eval_cache;
  eval_cache = new blender::opensubdiv::GpuEvalOutput::EvaluatorCache();
  evaluator_cache->eval_cache = eval_cache;
  return evaluator_cache;
}

void openSubdiv_deleteEvaluatorCacheInternal(OpenSubdiv_EvaluatorCacheImpl *evaluator_cache)
{
  delete evaluator_cache;
}
