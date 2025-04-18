/*!
 * Copyright (c) Alibaba, Inc. and its affiliates.
 * @file    gemm_capsule_op_gpu.cpp
 */

#ifdef ENABLE_CUDA
#include "gemm_capsule_op_gpu.h"

#include <core/kernel/kernel.h>
#include <core/operator/general/gemm_lora/gemm_lora_op_gpu.h>
#include <cuda/cuda_context.h>
#include <runtime/weight/weight_manager_lora.h>
#include <utility/arbiter.h>
#include <utility/datatype_dispatcher.h>

namespace allspark {

AsStatus GemmLoraCapsuleOpGPU::InitV2(const OperatorProto& op_proto,
                                      const DeviceContext& ctx,
                                      const TensorMap& weights_map,
                                      TensorMap& weights_buffer,
                                      TensorMap* tensor_map,
                                      RuntimeContext* runtime_ctx) {
  DLOG(INFO) << "GemmLoraCapsuleOpGPU::InitV2" << std::endl;

  //  Capsule's special Init
  //  taken from AsOperator::Init
  tensor_map_ = tensor_map;
  op_name_ = op_proto.op_name();
  in_names_.clear();
  for (auto& t : op_proto.inputs()) {
    const std::string& t_name = t.name();
    if (tensor_map_->count(t_name) == 0) {
      tensor_map_->insert(std::make_pair(
          t_name, std::make_unique<AsTensor>(t, ctx.GetDeviceType())));
    }
    in_names_.emplace_back(t_name);
  }
  for (auto& t : op_proto.outputs()) {
    const std::string& t_name = t.name();
    if (tensor_map_->count(t_name) == 0) {
      tensor_map_->insert(std::make_pair(
          t_name, std::make_unique<AsTensor>(t, ctx.GetDeviceType())));
    }
    out_names_.emplace_back(t_name);
  }
  ctx_ = &ctx;
  // taken from AsOperator::Init END

  std::string act_str = "";
  OperatorProto mutable_op_proto;
  mutable_op_proto.CopyFrom(op_proto);

  const auto& orig_attr_map = op_proto.attr();
  if (orig_attr_map.count("InnerGemmType")) {
    inner_gemm_type_ = orig_attr_map.at("InnerGemmType");
  }

  // 组装Lora op_list
  std::string op_name_pattern =
      util::StringUtil::RemoveLayerNumber(op_proto.op_name());
  // base
  auto op = OpFactory::getInstance().GetOperator(
      {inner_gemm_type_, ctx.GetDeviceType()})();
  mutable_op_proto.set_op_type(inner_gemm_type_);
  auto& attr_map = *mutable_op_proto.mutable_attr();
  auto input = mutable_op_proto.mutable_inputs(0);
  auto output = mutable_op_proto.mutable_outputs(0);
  capsule_out_name_ = std::string(output->name());
  base_out_name_ = op_name_pattern + ".base_gemm.out";
  output->set_name(base_out_name_);
  if (attr_map.count("activation")) {
    activation_ = *(UnaryType*)(attr_map.at("activation").c_str());
    act_str = attr_map.at("activation");
    attr_map.erase("activation");
  }
  op->CallInit(mutable_op_proto, ctx, weight_manager_, weight_handler_, nullptr,
               rank_info_, tensor_map, profiler_, runtime_ctx);
  lora_op_list_.emplace_back(std::move(op));

#if 0  // 老算子
  // Lora_A
  op =
      OpFactory::getInstance().GetOperator({"GemmLora", ctx.GetDeviceType()})();
  mutable_op_proto.set_op_type("GemmLora");
  mutable_op_proto.set_op_name(op_proto.op_name() + ".lora_A");
  auto weights = mutable_op_proto.mutable_weights();
  if (weights->size() > 1) {
    // lora自己管理bias，不使用Gemm的bias设置
    weights->DeleteSubrange(1, weights->size() - 1);
  }
  auto weight = mutable_op_proto.mutable_weights(0);
  weight->set_name(mutable_op_proto.op_name() + ".weight");
  output->set_name(op_name_pattern + ".lora_A.out");
  attr_map.erase("alpha");  // loraA 没有伸缩
  op->CallInit(mutable_op_proto, ctx, weight_manager_, weight_handler_,
               lora_manager_, rank_info_, tensor_map, profiler_, runtime_ctx);
  lora_op_list_.emplace_back(std::move(op));

  // Lora_B
  op =
      OpFactory::getInstance().GetOperator({"GemmLora", ctx.GetDeviceType()})();
  mutable_op_proto.set_op_type("GemmLora");
  mutable_op_proto.set_op_name(op_proto.op_name() + ".lora_B");
  input->set_name(output->name());
  weight->set_name(mutable_op_proto.op_name() + ".weight");
  output->set_name(op_name_pattern + ".lora_B.out");
  attr_map.erase("alpha");  // 优化aslora后，使用默认1.0
  op->CallInit(mutable_op_proto, ctx, weight_manager_, weight_handler_,
               lora_manager_, rank_info_, tensor_map, profiler_, runtime_ctx);
  lora_op_list_.emplace_back(std::move(op));
#endif

#if 1
  // 采用新算子
  op =
      OpFactory::getInstance().GetOperator({"SgmvLora", ctx.GetDeviceType()})();
  mutable_op_proto.set_op_type("SgmvLora");
  mutable_op_proto.set_op_name(op_proto.op_name() + ".sgmv");
  auto weights = mutable_op_proto.mutable_weights();
  if (weights->size() == 1) {
    // sgmv需要lora_A、lora_B两个weight
    TensorProto weight2;
    weight2.set_name(op_proto.op_name());  // basic name
    weight2.set_data("");
    mutable_op_proto.mutable_weights()->AddAllocated(new TensorProto(weight2));
  }
  mutable_op_proto.mutable_weights(0)->set_name(op_proto.op_name() +
                                                ".lora_A.weight");
  mutable_op_proto.mutable_weights(1)->set_name(op_proto.op_name() +
                                                ".lora_B.weight");
  output->set_name(op_name_pattern + ".sgmv.out");
  op->CallInit(mutable_op_proto, ctx, weight_manager_, weight_handler_,
               lora_manager_, rank_info_, tensor_map, profiler_, runtime_ctx);
  lora_op_list_.emplace_back(std::move(op));
#endif

  // Binary-Add
  op = OpFactory::getInstance().GetOperator({"Binary", ctx.GetDeviceType()})();
  mutable_op_proto.set_op_type("Binary");
  mutable_op_proto.set_op_name(op_proto.op_name() + ".base_add_lora");
  input->set_name(output->name());
  TensorProto input2;
  input2.set_name(base_out_name_);
  input2.set_data("");
  mutable_op_proto.mutable_inputs()->AddAllocated(new TensorProto(input2));
  bin_add_out_name_ = activation_ == UNARYTYPE_UNDEFINED
                          ? capsule_out_name_
                          : op_name_pattern + ".base_add_lora.out";
  output->set_name(bin_add_out_name_);
  mutable_op_proto.clear_weights();
  attr_map.clear();
  int binary_type = ADD;
  attr_map["binary_type"] =
      std::string(reinterpret_cast<char*>(&binary_type), sizeof(int));
  op->CallInit(mutable_op_proto, ctx, weight_manager_, weight_handler_, nullptr,
               rank_info_, tensor_map, profiler_, runtime_ctx);
  lora_op_list_.emplace_back(std::move(op));

  // activation
  if (activation_ != UNARYTYPE_UNDEFINED) {
    op = OpFactory::getInstance().GetOperator({"Unary", ctx.GetDeviceType()})();
    mutable_op_proto.set_op_type("Unary");
    mutable_op_proto.set_op_name(op_proto.op_name() + ".act");
    mutable_op_proto.mutable_inputs()->DeleteSubrange(
        1,
        mutable_op_proto.mutable_inputs()->size() - 1);  // 恢复成1个输入
    input->set_name(bin_add_out_name_);
    output->set_name(capsule_out_name_);
    attr_map.clear();
    attr_map["unary_type"] = act_str;
    op->CallInit(mutable_op_proto, ctx, weight_manager_, weight_handler_,
                 nullptr, rank_info_, tensor_map, profiler_, runtime_ctx);
    lora_op_list_.emplace_back(std::move(op));
  }

  return AsStatus::ALLSPARK_SUCCESS;
}

void GemmLoraCapsuleOpGPU::SwitchLoraGraph(bool use_std_gemm_graph) {
  if (activation_ == UNARYTYPE_UNDEFINED) {  // 无act
    if (use_std_gemm_graph) {  // 无lora 标准gemm，没有act：
                               // 最后一个op是bin_add.  只call 标准gemm
      // base_gemm 输出到最终结果。
      lora_op_list_.front()->UpdateOutName(0, capsule_out_name_);
    } else {  // lora：最后一个op是bin_add
      // base_gemm 输出到base_out_name
      lora_op_list_.front()->UpdateOutName(0, base_out_name_);
    }
  } else {                     // 有act
    if (use_std_gemm_graph) {  // 无lora，最后一个op是act，会被call
      // act 从base_out_name 获取输入， (输出一定是到最终结果)
      lora_op_list_.back()->UpdateInName(0, base_out_name_);
    } else {  // lora：最后一个op是act
      // act从bin_add_out_name 获取输入(act输出一定是到最终结果)
      lora_op_list_.back()->UpdateInName(0, bin_add_out_name_);
    }
  }
}

AsStatus GemmLoraCapsuleOpGPU::Reshape(RuntimeContext* runtime_ctx) {
  DLOG(INFO) << "GemmLoraCapsuleOpGPU::Reshape" << std::endl;
  AsStatus ret = AsStatus::ALLSPARK_SUCCESS;

  // 没加载lora, 走首尾gemm+activation流程
  if (lora_manager_->IsEmpty()) {
    SwitchLoraGraph(true);
    // TODO: use unified graph, ditto
    AS_CHECK_STATUS(lora_op_list_.front()->CallReshape(runtime_ctx));
    if (activation_ != UNARYTYPE_UNDEFINED) {
      AS_CHECK_STATUS(lora_op_list_.back()->CallReshape(runtime_ctx));
    }
    return AsStatus::ALLSPARK_SUCCESS;
  }

  // batch中只要有一个lora就走Lora全流程，全无lora则走首尾gemm+activation流程
  has_lora_in_batch_ = false;
  std::string lora_name = "";
  if (runtime_ctx->is_context) {
    lora_name = runtime_ctx->GetContextGenCtx()->gen_cfg.lora_name;
  } else {
    auto batchsize = runtime_ctx->GetGenCtxListSize();
    for (auto i = 0; i < batchsize && lora_name.empty(); i++) {
      lora_name = runtime_ctx->GetGenCtx(i)->gen_cfg.lora_name;
    }
  }
  if (!lora_name.empty()) {
    has_lora_in_batch_ = true;
  }

  if (has_lora_in_batch_) {
    SwitchLoraGraph(false);
    for (auto& op : lora_op_list_) {
      ret = op->CallReshape(runtime_ctx);
      AS_CHECK_STATUS(ret);
    }
    return ret;
  } else {
    SwitchLoraGraph(true);
    AS_CHECK_STATUS(lora_op_list_.front()->CallReshape(runtime_ctx));
    if (activation_ != UNARYTYPE_UNDEFINED)
      AS_CHECK_STATUS(lora_op_list_.back()->CallReshape(runtime_ctx));
    return AsStatus::ALLSPARK_SUCCESS;
  }
}

AsStatus GemmLoraCapsuleOpGPU::Forward(RuntimeContext* runtime_ctx) {
  DLOG(INFO) << "GemmLoraCapsuleOpGPU::Forward" << std::endl;
  AsStatus ret = AsStatus::ALLSPARK_SUCCESS;

  if (lora_manager_->IsEmpty()) {
    SwitchLoraGraph(true);
    AS_CHECK_STATUS(lora_op_list_.front()->CallForward(runtime_ctx));
    // lora_op_list_.front()->PrintInformation();
    if (activation_ != UNARYTYPE_UNDEFINED) {
      AS_CHECK_STATUS(lora_op_list_.back()->CallForward(runtime_ctx));
      // lora_op_list_.back()->PrintInformation();
    }
    return AsStatus::ALLSPARK_SUCCESS;
  }

  if (has_lora_in_batch_) {
    SwitchLoraGraph(false);
    for (auto& op : lora_op_list_) {
      ret = op->CallForward(runtime_ctx);
      // op->PrintInformation();
      //   DO_ARBITRATE(0, 1, 0, op);
      AS_CHECK_STATUS(ret);
    }
    return ret;
  } else {
    SwitchLoraGraph(true);
    AS_CHECK_STATUS(lora_op_list_.front()->CallForward(runtime_ctx));
    // lora_op_list_.front()->PrintInformation();
    if (activation_ != UNARYTYPE_UNDEFINED) {
      AS_CHECK_STATUS(lora_op_list_.back()->CallForward(runtime_ctx));
      // lora_op_list_.back()->PrintInformation();
    }
    return AsStatus::ALLSPARK_SUCCESS;
  }
}

REGISTER_OP(GemmLoraCapsule, CUDA, GemmLoraCapsuleOpGPU)
}  // namespace allspark
#endif
