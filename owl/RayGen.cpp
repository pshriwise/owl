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

#include "RayGen.h"
#include "Context.h"

namespace owl {

  RayGenType::RayGenType(Context *const context,
                         Module::SP module,
                         const std::string &progName,
                         size_t varStructSize,
                         const std::vector<OWLVarDecl> &varDecls)
    : SBTObjectType(context,context->rayGenTypes,varStructSize,varDecls),
      module(module),
      progName(progName)
  {
  }
  
  RayGen::RayGen(Context *const context,
                 RayGenType::SP type) 
    : SBTObject(context,context->rayGens,type)
  {
    assert(context);
    assert(type);
    assert(type.get());
    assert(type->module);
    assert(type->progName != "");
    // context->llo->setRayGen(this->ID,
    //                 type->module->ID,
    //                 type->progName.c_str(),
    //                 type->varStructSize);
  }

  RayGen::~RayGen()
  {
    for (auto device : context->getDevices()) {
      SetActiveGPU forLifeTime(device);
      getDD(device).sbtRecordBuffer.free();
    }
  }

  // void RayGen::launch(const vec2i &dims)
  // {
  //   context->llo->launch(this->ID,dims);
  // }

  // void RayGen::launch(const vec2i &dims, const LaunchParams::SP &lp)
  // {
  //   context->llo->launch
  //     (this->ID,dims,lp->ID,
  //      [](uint8_t *output, int devID, const void *cbData)
  //      {
  //        const LaunchParams *lp
  //          = (const LaunchParams *)cbData;
  //        lp->writeVariables(output,devID);
  //      },
  //      (const void *)lp.get());
  // }


  void RayGen::launch(const vec2i &dims)
  {
    launchAsync(dims,context->dummyLaunchParams);
    context->dummyLaunchParams->sync();
    // throw std::runtime_error("only working with lauch params irght now");
  }


  RayGen::DeviceData::DeviceData(const DeviceContext::SP &device,
                                 size_t dataSize)
    : RegisteredObject::DeviceData(device),
      rayGenRecordSize(OPTIX_SBT_RECORD_HEADER_SIZE
                       + smallestMultipleOf<OPTIX_SBT_RECORD_ALIGNMENT>(dataSize))
  {
    SetActiveGPU forLifeTime(device);
    
    PING; PRINT(rayGenRecordSize);
    sbtRecordBuffer.alloc(rayGenRecordSize);
  }


  void RayGen::writeSBTRecord(uint8_t *const sbtRecord,
                              const DeviceContext::SP &device)
  {
    auto &dd = type->getDD(device);
    
    // first, compute pointer to record:
    uint8_t *const sbtRecordHeader = sbtRecord;
    uint8_t *const sbtRecordData   = sbtRecord+OPTIX_SBT_RECORD_HEADER_SIZE;

    // ------------------------------------------------------------------
    // pack record header with the corresponding hit group:
    // ------------------------------------------------------------------
    OPTIX_CALL(SbtRecordPackHeader(dd.pg,sbtRecordHeader));
    
    // ------------------------------------------------------------------
    // then, write the data for that record
    // ------------------------------------------------------------------
    writeVariables(sbtRecordData,device);
  }  



  
  void RayGen::launchAsync(const vec2i &dims,
                           const LaunchParams::SP &lp)
  {
    assert("check valid launch dims" && dims.x > 0);
    assert("check valid launch dims" && dims.y > 0);
      
    assert(!deviceData.empty());
    for (int deviceID=0;deviceID<(int)deviceData.size();deviceID++) {
      DeviceContext::SP device = context->getDevice(deviceID);
      SetActiveGPU forLifeTime(device);
      
      RayGen::DeviceData &rgDD
        = getDD(device);
      LaunchParams::DeviceData &lpDD
        = lp->getDD(device);
      
      lp->writeVariables(lpDD.hostMemory.data(),device);
      lpDD.deviceMemory.uploadAsync(lpDD.hostMemory.data(),lpDD.stream);

      auto &sbt = lpDD.sbt;

      // -------------------------------------------------------
      // set raygen part of SBT 
      // -------------------------------------------------------
      sbt.raygenRecord
        = (CUdeviceptr)rgDD.sbtRecordBuffer.d_pointer;
      assert(sbt.raygenRecord);
      // sbt.raygenRecordSize
      //   = rgDD.deviceMemory.size();
      // assert(sbt.raygenRecordSize);

      // -------------------------------------------------------
      // set miss progs part of SBT 
      // -------------------------------------------------------
      assert("check miss records built" && device->sbt.missProgRecordCount != 0);
      sbt.missRecordBase
        = (CUdeviceptr)device->sbt.missProgRecordsBuffer.get();
      sbt.missRecordStrideInBytes
        = (uint32_t)device->sbt.missProgRecordSize;
      sbt.missRecordCount
        = (uint32_t)device->sbt.missProgRecordCount;
      
      // -------------------------------------------------------
      // set hit groups part of SBT 
      // -------------------------------------------------------
      assert("check hit records built" && device->sbt.hitGroupRecordCount != 0);
      sbt.hitgroupRecordBase
        = (CUdeviceptr)device->sbt.hitGroupRecordsBuffer.get();
      sbt.hitgroupRecordStrideInBytes
        = (uint32_t)device->sbt.hitGroupRecordSize;
      sbt.hitgroupRecordCount
        = (uint32_t)device->sbt.hitGroupRecordCount;
      
      OPTIX_CALL(Launch(device->pipeline,
                        lpDD.stream,
                        (CUdeviceptr)lpDD.deviceMemory.get(),
                        lpDD.deviceMemory.sizeInBytes,
                        &lpDD.sbt,
                        dims.x,dims.y,1
                        ));
    }
  }

} // ::owl
