/**
 * Copyright (c) 2016-present, Facebook, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <memory>
#include "blobs_queue.h"
#include "caffe2/core/operator.h"
#include "caffe2/utils/math.h"

namespace caffe2 {

template <typename Context>
class CreateBlobsQueueOp final : public Operator<Context> {
 public:
  USE_OPERATOR_CONTEXT_FUNCTIONS;

  CreateBlobsQueueOp(const OperatorDef& operator_def, Workspace* ws)
      : Operator<Context>(operator_def, ws),
        ws_(ws),
        name(operator_def.output().Get(0)) {}

  bool RunOnDevice() override {
    const auto capacity = GetSingleArgument("capacity", 1);
    const auto numBlobs = GetSingleArgument("num_blobs", 1);
    const auto enforceUniqueName =
        GetSingleArgument("enforce_unique_name", false);
    const auto fieldNames =
        OperatorBase::template GetRepeatedArgument<std::string>("field_names");
    CAFFE_ENFORCE_EQ(this->OutputSize(), 1);
    auto queuePtr = Operator<Context>::Outputs()[0]
                        ->template GetMutable<std::shared_ptr<BlobsQueue>>();
    CAFFE_ENFORCE(queuePtr);
    *queuePtr = std::make_shared<BlobsQueue>(
        ws_, name, capacity, numBlobs, enforceUniqueName, fieldNames);
    return true;
  }

 private:
  Workspace* ws_{nullptr};
  const std::string name;
};

template <typename Context>
class EnqueueBlobsOp final : public Operator<Context> {
 public:
  USE_OPERATOR_CONTEXT_FUNCTIONS;
  using Operator<Context>::Operator;
  bool RunOnDevice() override {
    CAFFE_ENFORCE(InputSize() > 1);
    auto queue = Operator<Context>::Inputs()[0]
                     ->template Get<std::shared_ptr<BlobsQueue>>();
    CAFFE_ENFORCE(queue && OutputSize() == queue->getNumBlobs());
    return queue->blockingWrite(this->Outputs());
  }

 private:
};

template <typename Context>
class DequeueBlobsOp final : public Operator<Context> {
 public:
  USE_OPERATOR_CONTEXT_FUNCTIONS;

  DequeueBlobsOp(const OperatorDef& operator_def, Workspace* ws)
      : Operator<Context>(operator_def, ws) {
    timeout_secs_ = OperatorBase::GetSingleArgument<float>("timeout_secs", 0);
  }

  bool RunOnDevice() override {
    CAFFE_ENFORCE(InputSize() == 1);
    auto queue =
        OperatorBase::Inputs()[0]->template Get<std::shared_ptr<BlobsQueue>>();
    CAFFE_ENFORCE(queue && OutputSize() == queue->getNumBlobs());
    return queue->blockingRead(this->Outputs(), timeout_secs_);
  }

 private:
  float timeout_secs_;
};

template <typename Context>
class CloseBlobsQueueOp final : public Operator<Context> {
 public:
  USE_OPERATOR_CONTEXT_FUNCTIONS;
  using Operator<Context>::Operator;
  bool RunOnDevice() override {
    CAFFE_ENFORCE_EQ(InputSize(), 1);
    auto queue =
        OperatorBase::Inputs()[0]->template Get<std::shared_ptr<BlobsQueue>>();
    CAFFE_ENFORCE(queue);
    queue->close();
    return true;
  }

 private:
};

template <typename Context>
class SafeEnqueueBlobsOp final : public Operator<Context> {
 public:
  USE_OPERATOR_CONTEXT_FUNCTIONS;
  using Operator<Context>::Operator;
  bool RunOnDevice() override {
    auto queue = Operator<Context>::Inputs()[0]
                     ->template Get<std::shared_ptr<BlobsQueue>>();
    CAFFE_ENFORCE(queue);
    auto size = queue->getNumBlobs();
    CAFFE_ENFORCE(
        OutputSize() == size + 1,
        "Expected " + caffe2::to_string(size + 1) + ", " + " got: " +
            caffe2::to_string(size));
    bool status = queue->blockingWrite(this->Outputs());
    Output(size)->Resize();
    math::Set<bool, Context>(
        1, !status, Output(size)->template mutable_data<bool>(), &context_);
    return true;
  }
};

template <typename Context>
class SafeDequeueBlobsOp final : public Operator<Context> {
 public:
  USE_OPERATOR_CONTEXT_FUNCTIONS;
  using Operator<Context>::Operator;

  SafeDequeueBlobsOp(const OperatorDef& operator_def, Workspace* ws)
      : Operator<Context>(operator_def, ws),
        numRecords_(OperatorBase::GetSingleArgument<int>("num_records", 1)) {
    CAFFE_ENFORCE_GT(numRecords_, 0);
  }

  bool dequeueMany(std::shared_ptr<BlobsQueue>& queue) {
    auto size = queue->getNumBlobs();

    if (blobs_.size() != size) {
      blobs_.resize(size);
      blobPtrs_.resize(size);
      for (int col = 0; col < size; ++col) {
        blobPtrs_.at(col) = &blobs_.at(col);
      }
    }

    const int kTensorGrowthPct = 40;
    for (int i = 0; i < numRecords_; ++i) {
      if (!queue->blockingRead(blobPtrs_)) {
        // if we read at least one record, status is still true
        return i > 0;
      }
      for (int col = 0; col < size; ++col) {
        auto* out = this->Output(col);
        const auto& in = blobPtrs_.at(col)->template Get<Tensor<Context>>();
        if (i == 0) {
          out->CopyFrom(in);
        } else {
          auto oldSize = out->size();
          out->Extend(in.dims()[0], kTensorGrowthPct, &context_);
          auto* dst =
              (char*)out->raw_mutable_data() + oldSize * in.meta().itemsize();
          context_.template CopyItems<Context, Context>(
              in.meta(), in.size(), in.raw_data(), dst);
        }
      }
    }
    return true;
  }

  bool dequeueOne(std::shared_ptr<BlobsQueue>& queue) {
    return queue->blockingRead(this->Outputs());
  }

  bool RunOnDevice() override {
    CAFFE_ENFORCE(InputSize() == 1);
    auto queue = Operator<Context>::Inputs()[0]
                     ->template Get<std::shared_ptr<BlobsQueue>>();
    CAFFE_ENFORCE(queue);

    auto size = queue->getNumBlobs();
    CAFFE_ENFORCE_EQ(OutputSize(), size + 1);

    bool status = numRecords_ > 1 ? dequeueMany(queue) : dequeueOne(queue);

    Output(size)->Resize();
    math::Set<bool, Context>(
        1, !status, Output(size)->template mutable_data<bool>(), &context_);
    return true;
  }

 private:
  int numRecords_;
  std::vector<Blob> blobs_;
  std::vector<Blob*> blobPtrs_;
};

template <typename Context>
class WeightedSampleDequeueBlobsOp final : public Operator<Context> {
 public:
  USE_OPERATOR_CONTEXT_FUNCTIONS;

  WeightedSampleDequeueBlobsOp(const OperatorDef& operator_def, Workspace* ws)
      : Operator<Context>(operator_def, ws) {
    vector<float> weights = OperatorBase::GetRepeatedArgument<float>("weights");
    if (weights.empty()) {
      weights.resize(InputSize(), 1.0f);
    }
    CAFFE_ENFORCE_EQ(InputSize(), weights.size());

    float sum = accumulate(weights.begin(), weights.end(), 0.0f);
    CAFFE_ENFORCE(sum > 0.0f, "Sum of weights must be positive");
    cumProbs_.resize(weights.size());
    for (int i = 0; i < weights.size(); i++) {
      cumProbs_[i] = weights[i] / sum;
      CAFFE_ENFORCE_GE(
          cumProbs_[i], 0.0f, "Each probability must be non-negative");
    }
    std::partial_sum(cumProbs_.begin(), cumProbs_.end(), cumProbs_.begin());
    // Put last value to be 1.0001 to avoid numerical issues.
    cumProbs_.back() = 1.0001;

    LOG(INFO) << "Dequeue weights: " << weights;
    LOG(INFO) << "cumProbs: " << cumProbs_;
  }

  bool RunOnDevice() override {
    float r;
    math::RandUniform<float, Context>(1, 0.0f, 1.0f, &r, &context_);
    auto lb = lower_bound(cumProbs_.begin(), cumProbs_.end(), r);
    CAFFE_ENFORCE(lb != cumProbs_.end(), "Cannot find ", r, " in cumProbs_.");

    auto queue = Operator<Context>::Inputs()[lb - cumProbs_.begin()]
                     ->template Get<std::shared_ptr<BlobsQueue>>();

    CAFFE_ENFORCE(queue);
    auto size = queue->getNumBlobs();
    CAFFE_ENFORCE_EQ(OutputSize(), size + 1);
    bool status = queue->blockingRead(this->Outputs());
    Output(size)->Resize();
    math::Set<bool, Context>(
        1, !status, Output(size)->template mutable_data<bool>(), &context_);
    return true;
  }

 private:
  vector<float> cumProbs_;
};
}
