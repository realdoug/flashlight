/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "flashlight/distributed/DistributedApi.h"

#include <iostream>
#include <list>
#include <memory>
#include <mutex>
#include <stdexcept>

#include <gloo/allreduce_halving_doubling.h>
#include <gloo/config.h>
#include <gloo/mpi/context.h>
#include <gloo/transport/tcp/device.h>
#include <mpi.h>

#include "flashlight/common/CppBackports.h"
#include "flashlight/distributed/backend/utils/LRUCache.h"

namespace {
std::shared_ptr<gloo::mpi::Context> glooContext_;

// Gloo algorithms are "not meant" to be created an deleted often, for some
// strange reason. Therefore, we emulate THD by providing a cache of the last
// few algorithms run. See https://git.io/fNNyc
//
// XXX IMPORTANT XXX
// Caching is performed by raw pointer address. THIS MEANS YOU SHOULD NEVER PASS
// ANY TEMPORARIES TO THE ALLREDUCE/ALLGATHER/BRODACAST CONVENIENCE FUNCTIONS!
// If you do, you might eventually experience hangs and timeouts as temporaries
// are being allocated to previous memory addresses.
const int kGlooCacheSize_ = 10;
using CacheType = fl::detail::LRUCache<std::string, gloo::Algorithm>;
CacheType glooCache_(kGlooCacheSize_);
af::array cacheArr_;
} // namespace

namespace fl {

namespace detail {

std::shared_ptr<gloo::mpi::Context> globalContext() {
  return glooContext_;
}

template <typename T>
inline void allreduceGloo(T* ptr, size_t s) {
  auto key = detail::makeHashKey(ptr, s, "allreduceCpu");
  auto algorithm = glooCache_.get(key);
  if (algorithm == nullptr) {
    using Allreduce = gloo::AllreduceHalvingDoubling<T>;
    algorithm = glooCache_.put(
        key,
        cpp::make_unique<Allreduce>(
            globalContext(),
            std::vector<T*>({ptr}),
            s,
            gloo::ReductionFunction<T>::sum));
  }
  algorithm->run();
}
} // namespace detail

void distributedInit(
    DistributedInit initMethod,
    int /* worldRank */,
    int /* worldSize */,
    const std::unordered_map<std::string, std::string>& /* params = {} */) {
  if (isDistributedInit()) {
    std::cerr << "warning: fl::distributedInit() called more than once\n";
    return;
  }

  if (initMethod != DistributedInit::MPI) {
    throw std::runtime_error(
        "unsupported distributed init method for gloo backend");
  }

  // using MPI
  if (glooContext_ != nullptr) {
    return;
  }
  // TODO: ibverbs support.
  auto glooDev = gloo::transport::tcp::CreateDevice("");

  // Create Gloo context from MPI communicator
  glooContext_ = gloo::mpi::Context::createManaged();
  glooContext_->setTimeout(gloo::kNoTimeout);
  glooContext_->connectFullMesh(glooDev);

  detail::DistributedInfo::getInstance().backend_ = DistributedBackend::GLOO;
  detail::DistributedInfo::getInstance().isInitialized_ = true;
  if (glooContext_->rank == 0) {
    std::cout << "Initialized Gloo successfully!\n";
  }
}

void allReduce(af::array& arr) {
  if (!isDistributedInit()) {
    throw std::runtime_error("distributed environment not initialized");
  }
  size_t arrSize = arr.elements() * af::getSizeOf(arr.type());
  if (arrSize > cacheArr_.elements()) {
    cacheArr_ = af::array(arrSize, af::dtype::b8);
  }
  void* arrPtr = arr.device<void>();
  void* cacheArrPtr = cacheArr_.device<void>();
  memcpy(cacheArrPtr, arrPtr, arrSize);
  switch (arr.type()) {
    case af::dtype::f32:
      detail::allreduceGloo(static_cast<float*>(cacheArrPtr), arr.elements());
      break;
    case af::dtype::f64:
      detail::allreduceGloo(static_cast<double*>(cacheArrPtr), arr.elements());
      break;
    case af::dtype::s32:
      detail::allreduceGloo(static_cast<int*>(cacheArrPtr), arr.elements());
      break;
    case af::dtype::s64:
      detail::allreduceGloo(static_cast<int64_t*>(cacheArrPtr), arr.elements());
      break;
    default:
      throw std::runtime_error("unsupported data type for allreduce with gloo");
  }
  memcpy(arrPtr, cacheArrPtr, arrSize);
  arr.unlock();
  cacheArr_.unlock();
}

int getWorldRank() {
  if (!isDistributedInit()) {
    return 0;
  }
  return detail::globalContext()->rank;
}

int getWorldSize() {
  if (!isDistributedInit()) {
    return 1;
  }
  return detail::globalContext()->size;
}
} // namespace fl
