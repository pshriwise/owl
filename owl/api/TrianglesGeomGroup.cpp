// ======================================================================== //
// Copyright 2019-2020 Ingo Wald                                            //
//                                                                          //
// Licensed under the Apache License, Version 2.0 (the "License");          //
// you may not use this file except in compliance with the License.         //
// You may obtain a copy of the License at                                  //
//                                                                          //
//     http://www.apache.org/licenses/LICENSE-2.0                           //
//                                                                          //
// Unless required by applicable law or agreed to in writing, software      //
// distributed under the License is distributed on an "AS IS" BASIS,        //
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. //
// See the License for the specific language governing permissions and      //
// limitations under the License.                                           //
// ======================================================================== //

#include "TrianglesGeomGroup.h"
#include "Triangles.h"
#include "Context.h"
#include "ll/Device.h"
#include "ll/DeviceGroup.h"


#include <fstream>

#define LOG(message)                                            \
  if (Context::logging())                                   \
    std::cout << "#owl.ll(" << device->ID << "): "    \
              << message                                        \
              << std::endl

#define LOG_OK(message)                                         \
  if (Context::logging())                                       \
    std::cout << OWL_TERMINAL_GREEN                             \
              << "#owl.ll(" << device->ID << "): "    \
              << message << OWL_TERMINAL_DEFAULT << std::endl

#define CLOG(message)                                   \
  if (Context::logging())                               \
    std::cout << "#owl.ll(" << device->ID << "): "     \
              << message                                \
              << std::endl

#define CLOG_OK(message)                                        \
  if (Context::logging())                                       \
    std::cout << OWL_TERMINAL_GREEN                             \
              << "#owl.ll(" << device->ID << "): "             \
              << message << OWL_TERMINAL_DEFAULT << std::endl


namespace owl {

  TrianglesGeomGroup::TrianglesGeomGroup(Context *const context,
                                         size_t numChildren)
    : GeomGroup(context,numChildren)
  {
    // context->llo->trianglesGeomGroupCreate(this->ID,
    //                                        nullptr,numChildren);
  }

  void TrianglesGeomGroup::updateMotionBounds()
  {
    bounds[0] = bounds[1] = box3f();
    for (auto geom : geometries) {
      TrianglesGeom::SP mesh = geom->as<TrianglesGeom>();
      box3f meshBounds[2];
      mesh->computeBounds(meshBounds);
      for (int i=0;i<2;i++)
        bounds[i].extend(meshBounds[i]);
    }
    PING; PRINT(bounds[0]);
    PRINT(bounds[1]);
  }
  

  
  void TrianglesGeomGroup::buildAccel()
  {
    for (auto device : context->llo->devices) 
      buildAccelOn<true>(device);

    if (context->motionBlurEnabled)
      updateMotionBounds();
  }
  
  void TrianglesGeomGroup::refitAccel()
  {
    for (auto device : context->llo->devices) 
      buildAccelOn<false>(device);
    
    if (context->motionBlurEnabled)
      updateMotionBounds();
  }

  template<bool FULL_REBUILD>
  void TrianglesGeomGroup::buildAccelOn(ll::Device *device) 
  {
    assert("check does not yet exist" && ll->traversable == 0);
    if (FULL_REBUILD)
      assert("check does not yet exist on first build" && ll->bvhMemory.empty());
    else
      assert("check DOES exist on refit" && !ll->bvhMemory.empty());
      
    int oldActive = device->pushActive();
    LOG("building triangles accel over "
        << geometries.size() << " geometries");
    PING;
    size_t sumPrims = 0;
    uint32_t maxPrimsPerGAS = 0;
    optixDeviceContextGetProperty
      (device->context->optixContext,
       OPTIX_DEVICE_PROPERTY_LIMIT_MAX_PRIMITIVES_PER_GAS,
       &maxPrimsPerGAS,
       sizeof(maxPrimsPerGAS));

    DeviceData &dd = getDD(device);
    assert(!geometries.empty());
    TrianglesGeom::SP child0 = geometries[0]->as<TrianglesGeom>();
    assert(child0);
    int numKeys = child0->vertex.buffers.size();
    PRINT(numKeys);
    assert(numKeys > 0);
    const bool hasMotion = (numKeys > 1);
    if (hasMotion) assert(context->motionBlurEnabled);
      
    PING;
    // ==================================================================
    // create triangle inputs
    // ==================================================================
    //! the N build inputs that go into the builder
    std::vector<OptixBuildInput> triangleInputs(geometries.size());
    // /*! *arrays* of the vertex pointers - the buildinputs cointina
    //  *pointers* to the pointers, so need a temp copy here */
    // std::vector<CUdeviceptr> vertexPointers(geometries.size());
    // std::vector<CUdeviceptr> indexPointers(geometries.size());
 
    std::vector<uint32_t> triangleInputFlags(geometries.size());
    // { OPTIX_GEOMETRY_FLAG_DISABLE_ANYHIT

    // now go over all geometries to set up the buildinputs
    PING;
    // std::vector<std::vector<CUdeviceptr>> vertexPointers(geometries.size());
    for (int childID=0;childID<geometries.size();childID++) {
       
      // the child wer're setting them with (with sanity checks)
      TrianglesGeom::SP tris = geometries[0]->as<TrianglesGeom>();
      assert(tris);
      
      if (tris->vertex.buffers.size() != numKeys)
        throw std::runtime_error("invalid combination of meshes with "
                                 "different motion keys in the same "
                                 "triangles geom group");
      // for (auto buffer : tris->vertex.buffers)
      //   vertexPointers[childID].push_back(buffer->getPointer(device->ID));
      TrianglesGeom::DeviceData &trisDD
        = tris->getDD(device);
        
      CUdeviceptr     *d_vertices    = trisDD.vertexPointers.data();
      assert(d_vertices);
      OptixBuildInput &triangleInput = triangleInputs[childID];

      triangleInput.type = OPTIX_BUILD_INPUT_TYPE_TRIANGLES;
      auto &ta = triangleInput.triangleArray;
      ta.vertexFormat        = OPTIX_VERTEX_FORMAT_FLOAT3;
      ta.vertexStrideInBytes = (uint32_t)tris->vertex.stride;
      ta.numVertices         = (uint32_t)tris->vertex.count;
      ta.vertexBuffers       = d_vertices;
      
      ta.indexFormat         = OPTIX_INDICES_FORMAT_UNSIGNED_INT3;
      ta.indexStrideInBytes  = (uint32_t)tris->index.stride;
      ta.numIndexTriplets    = (uint32_t)tris->index.count;
      ta.indexBuffer         = trisDD.indexPointer;
      assert(ta.indexBuffer);

      // -------------------------------------------------------
      // sanity check that we don't have too many prims
      // -------------------------------------------------------
      sumPrims += ta.numIndexTriplets;
      // we always have exactly one SBT entry per shape (i.e., triangle
      // mesh), and no per-primitive materials:
      triangleInputFlags[childID]    = 0;
      ta.flags                       = &triangleInputFlags[childID];
      // iw, jan 7, 2020: note this is not the "actual" number of
      // SBT entires we'll generate when we build the SBT, only the
      // number of per-ray-type 'groups' of SBT entities (i.e., before
      // scaling by the SBT_STRIDE that gets passed to
      // optixTrace. So, for the build input this value remains *1*).
      ta.numSbtRecords               = 1; 
      ta.sbtIndexOffsetBuffer        = 0; 
      ta.sbtIndexOffsetSizeInBytes   = 0; 
      ta.sbtIndexOffsetStrideInBytes = 0; 
    }
      
    PING;
    if (sumPrims > maxPrimsPerGAS) 
      throw std::runtime_error("number of prim in user geom group exceeds "
                               "OptiX's MAX_PRIMITIVES_PER_GAS limit");
        
    // ==================================================================
    // BLAS setup: buildinputs set up, build the blas
    // ==================================================================
      
    // ------------------------------------------------------------------
    // first: compute temp memory for bvh
    // ------------------------------------------------------------------
    OptixAccelBuildOptions accelOptions = {};
    accelOptions.buildFlags =
      OPTIX_BUILD_FLAG_ALLOW_UPDATE
      |
      OPTIX_BUILD_FLAG_PREFER_FAST_TRACE
      |
      OPTIX_BUILD_FLAG_ALLOW_COMPACTION;

    accelOptions.motionOptions.numKeys   = numKeys;
    accelOptions.motionOptions.flags     = 0;
    accelOptions.motionOptions.timeBegin = 0.f;
    accelOptions.motionOptions.timeEnd   = 1.f;
    if (FULL_REBUILD)
      accelOptions.operation            = OPTIX_BUILD_OPERATION_BUILD;
    else
      accelOptions.operation            = OPTIX_BUILD_OPERATION_UPDATE;
      
    PING;
    OptixAccelBufferSizes blasBufferSizes;
    OPTIX_CHECK(optixAccelComputeMemoryUsage
                (device->context->optixContext,
                 &accelOptions,
                 triangleInputs.data(),
                 (uint32_t)triangleInputs.size(),
                 &blasBufferSizes
                 ));
      
    PING;
    // ------------------------------------------------------------------
    // ... and allocate buffers: temp buffer, initial (uncompacted)
    // BVH buffer, and a one-single-size_t buffer to store the
    // compacted size in
    // ------------------------------------------------------------------

    // temp memory:
    DeviceMemory tempBuffer;
    tempBuffer.alloc(FULL_REBUILD
                     ?blasBufferSizes.tempSizeInBytes
                     :blasBufferSizes.tempUpdateSizeInBytes);

    // buffer for initial, uncompacted bvh
    DeviceMemory outputBuffer;
    outputBuffer.alloc(blasBufferSizes.outputSizeInBytes);

    PING;
    // single size-t buffer to store compacted size in
    DeviceMemory compactedSizeBuffer;
    if (FULL_REBUILD)
      compactedSizeBuffer.alloc(sizeof(uint64_t));
      
    PING;
    // ------------------------------------------------------------------
    // now execute initial, uncompacted build
    // ------------------------------------------------------------------
    OptixAccelEmitDesc emitDesc;
    emitDesc.type = OPTIX_PROPERTY_TYPE_COMPACTED_SIZE;
    emitDesc.result = (CUdeviceptr)compactedSizeBuffer.get();

    PING;
    if (FULL_REBUILD) {
      OPTIX_CHECK(optixAccelBuild(device->context->optixContext,
                                  /* todo: stream */0,
                                  &accelOptions,
                                  // array of build inputs:
                                  triangleInputs.data(),
                                  (uint32_t)triangleInputs.size(),
                                  // buffer of temp memory:
                                  (CUdeviceptr)tempBuffer.get(),
                                  tempBuffer.size(),
                                  // where we store initial, uncomp bvh:
                                  (CUdeviceptr)outputBuffer.get(),
                                  outputBuffer.size(),
                                  /* the traversable we're building: */ 
                                  &dd.traversable,
                                  /* we're also querying compacted size: */
                                  &emitDesc,1u
                                  ));
    } else {
      PING; PRINT(dd.bvhMemory.get());
      OPTIX_CHECK(optixAccelBuild(device->context->optixContext,
                                  /* todo: stream */0,
                                  &accelOptions,
                                  // array of build inputs:
                                  triangleInputs.data(),
                                  (uint32_t)triangleInputs.size(),
                                  // buffer of temp memory:
                                  (CUdeviceptr)tempBuffer.get(),
                                  tempBuffer.size(),
                                  // where we store initial, uncomp bvh:
                                  (CUdeviceptr)dd.bvhMemory.get(),
                                  dd.bvhMemory.size(),
                                  /* the traversable we're building: */ 
                                  &dd.traversable,
                                  /* we're also querying compacted size: */
                                  nullptr,0
                                  ));
    }
    CUDA_SYNC_CHECK();
    PING;
      
    // ==================================================================
    // perform compaction
    // ==================================================================

    // alloc the buffer...
    if (FULL_REBUILD) {
      // download builder's compacted size from device
      uint64_t compactedSize;
      compactedSizeBuffer.download(&compactedSize);
        
      dd.bvhMemory.alloc(compactedSize);
      // ... and perform compaction
      OPTIX_CALL(AccelCompact(device->context->optixContext,
                              /*TODO: stream:*/0,
                              // OPTIX_COPY_MODE_COMPACT,
                              dd.traversable,
                              (CUdeviceptr)dd.bvhMemory.get(),
                              dd.bvhMemory.size(),
                              &dd.traversable));
    }
    CUDA_SYNC_CHECK();
    PING;

      
    // ==================================================================
    // aaaaaand .... clean up
    // ==================================================================
    if (FULL_REBUILD)
      outputBuffer.free(); // << the UNcompacted, temporary output buffer
    tempBuffer.free();
    if (FULL_REBUILD)
      compactedSizeBuffer.free();
      
    PING;
    device->popActive(oldActive);

    LOG_OK("successfully build triangles geom group accel");
  }
  
} // ::owl