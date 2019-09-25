#ifndef ONEFLOW_CORE_OPERATOR_SOFTMAX_REDUCE_MAX_STAGE0_OP_H_
#define ONEFLOW_CORE_OPERATOR_SOFTMAX_REDUCE_MAX_STAGE0_OP_H_

#include "oneflow/core/operator/operator.h"

namespace oneflow {

class SoftmaxReduceMaxStage0Op final : public Operator {
 public:
  OF_DISALLOW_COPY_AND_MOVE(SoftmaxReduceMaxStage0Op);
  SoftmaxReduceMaxStage0Op() = default;
  ~SoftmaxReduceMaxStage0Op() = default;

  void InitFromOpConf() override;
  const PbMessage& GetCustomizedConf() const override;
  bool NeedInBlobWhenBackward() const override { return false; }
  bool NeedOutBlobWhenBackward() const override { return true; }
  void InferBlobDescs(std::function<BlobDesc*(const std::string&)> GetBlobDesc4BnInOp,
                      const ParallelContext* parallel_ctx) const override;


 private:
  bool IsInputBlobAllowedModelSplit(const std::string& ibn) const override { return false; }

  void VirtualGenKernelConf(std::function<const BlobDesc*(const std::string&)> GetBlobDesc4BnInOp,
                            const ParallelContext* parallel_ctx,
                            KernelConf* kernel_conf) const override;
  void GetOpParallelSignatures(
      std::vector<std::unique_ptr<const OpParallelSignature>>*) const override;
};

}  // namespace oneflow

#endif  // ONEFLOW_CORE_OPERATOR_REDUCE_MAX_OP_H_