//===----------------------------------------------------------------------===//
//
// Copyright (C) 2022 Sophgo Technologies Inc.  All rights reserved.
//
// TPU-MLIR is licensed under the 2-Clause BSD License except for the
// third-party components.
//
//===----------------------------------------------------------------------===//

#include "Common.h"
#include "tpu_mlir/Backend/BM168x/BM168x.h"
#include "tpu_mlir/Dialect/Tpu/Transforms/DevParallel/DistributeUtils.h"
#include "tpu_mlir/Dialect/Tpu/Transforms/RewritePattern.inc"
using namespace llvm;
using namespace tpu_mlir::backend;
namespace tpu_mlir {

namespace bm1684x {
class MatMulHdimBatchPattern : public OpRewritePattern<tpu::MatMulOp> {
  // Case1: Permute -> MatMul <- Permute
  // Cast2: Reshape -> MatMul <- Permute
  // Case3: Left    -> MatMul <- Permute
public:
  using OpRewritePattern::OpRewritePattern;
  LogicalResult matchAndRewrite(tpu::MatMulOp op,
                                PatternRewriter &rewriter) const override {

    //  if (module::isAsymmetric()) {
    //    return failure();
    //  }

    // 1. Define Left and Right
    auto left = op.getInput();
    auto right = op.getRight();

    auto stype = module::getStorageType(left);
    auto hdim_is_batch = op.getHdimIsBatch();
    if (stype.isF32() || hdim_is_batch) {
      return failure();
    }

    // 2. Check Left and Right
    auto l_is_weight = module::isWeight(left);
    auto r_is_weight = module::isWeight(right);
    if (l_is_weight && r_is_weight) {
      return failure();
    }
    auto l_op = left.getDefiningOp();
    auto r_op = right.getDefiningOp();
    if (!isa<tpu::PermuteOp>(l_op) && !isa<tpu::PermuteOp>(r_op)) {
      return failure();
    }

    // 3. Convert MatMul to HdimBatch MatMul
    if (!l_is_weight && !r_is_weight) {
      // When Left and Right is Tensor

      auto l_output_shape = module::getShape(l_op->getResult(0));
      auto r_output_shape = module::getShape(r_op->getResult(0));
      // Swap Left and Right
      if (isa<tpu::PermuteOp>(l_op) && !isa<tpu::PermuteOp>(r_op) &&
          l_output_shape[2] == r_output_shape[2]) {
        std::swap(l_op, r_op);
      }

      if (isa<tpu::PermuteOp>(l_op) && isa<tpu::PermuteOp>(r_op)) {
        // Case1
        // Left  -> Permute -\              Left  -\
        //                   ->  MatMul ->         -> MatMul
        // Right -> Permute -/              Right -/
        auto l_trans_op = dyn_cast<tpu::PermuteOp>(l_op);
        auto r_trans_op = dyn_cast<tpu::PermuteOp>(r_op);
        if (!l_trans_op->hasOneUse() || !r_trans_op->hasOneUse()) {
          return failure();
        }
        auto l_order = module::getI64Array(l_trans_op.getOrder());
        auto r_order = module::getI64Array(r_trans_op.getOrder());
        if (false == (l_order->size() == 4 && l_order->at(0) == 0 &&
                      l_order->at(1) == 2 && r_order->size() == 4 &&
                      r_order->at(0) == 0 && r_order->at(1) == 2)) {
          return failure();
        }
        auto l_trans = op.getLeftTranspose();
        auto r_trans = op.getRightTranspose();
        if (l_order->at(2) == 3 && l_order->at(3) == 1) {
          l_trans = !l_trans;
        }
        if (r_order->at(2) == 3 && r_order->at(3) == 1) {
          r_trans = !r_trans;
        }
        if (l_trans == true && r_trans == false) {
          // mm2 not support l_trans && !r_trans
          return failure();
        }
        op->setAttr("hdim_is_batch", rewriter.getBoolAttr(!hdim_is_batch));
        op->setAttr("left_transpose", rewriter.getBoolAttr(l_trans));
        op->setAttr("right_transpose", rewriter.getBoolAttr(r_trans));
        op->setOperand(0, l_trans_op.getInput());
        op->setOperand(1, r_trans_op.getInput());
        rewriter.eraseOp(l_trans_op);
        rewriter.eraseOp(r_trans_op);
      } else if (isa<tpu::ReshapeOp>(l_op) && isa<tpu::PermuteOp>(r_op)) {
        // Case2
        // Left  -> Reshape -\              Left(+ Reshape)-\
        //                   ->  MatMul ->                  -> MatMul
        // Right -> Permute -/              Right          -/
        auto l_trans_op = dyn_cast<tpu::ReshapeOp>(l_op);
        auto r_trans_op = dyn_cast<tpu::PermuteOp>(r_op);
        if (!l_trans_op->hasOneUse() || !r_trans_op->hasOneUse()) {
          return failure();
        }

        auto r_order = module::getI64Array(r_trans_op.getOrder());
        auto r_shape = module::getShape(r_trans_op.getOutput());
        auto r_in_shape = module::getShape(r_trans_op.getInput());
        auto l_in_shape = module::getShape(l_trans_op.getInput());
        auto l_out_shape = module::getShape(l_trans_op.getOutput());
        if (false == (r_order->size() == 4 && r_order->at(0) == 0 &&
                      r_order->at(1) == 2 && l_out_shape[1] == r_shape[1] &&
                      l_in_shape[1] == l_out_shape[2])) {
          return failure();
        }

        auto l_trans = op.getLeftTranspose();
        auto r_trans = op.getRightTranspose();
        if (r_order->at(2) == 3 && r_order->at(3) == 1) {
          r_trans = !r_trans;
        }

        // Check Shape (left.shape[-1] == right.shape[-2])
        bool remove_reshape = l_in_shape.size() == l_out_shape.size();
        if (!(l_in_shape.size() >= 2 && r_in_shape.size() >= 2))
          return failure();
        int l_K_dim = l_in_shape.size() - 1 - l_trans - l_trans * hdim_is_batch;
        int r_K_dim = r_in_shape.size() - 2 + r_trans +
                      r_trans * hdim_is_batch - hdim_is_batch;
        if (l_in_shape[l_K_dim] != r_in_shape[r_K_dim]) {
          if (l_out_shape.size() == 4 && l_out_shape[2] == 1) {
            std::vector<int64_t> new_l_shape = l_out_shape;
            new_l_shape[2] = l_out_shape[1];
            new_l_shape[1] = 1;
            module::setShape(l_trans_op.getOutput(), new_l_shape);
            l_trans_op.getOutput().dump();
            remove_reshape = false;
            l_out_shape = module::getShape(l_trans_op.getOutput());
          } else {
            return failure();
          }
        }

        if (!hdim_is_batch && l_in_shape.size() > 2 && r_in_shape.size() > 2) {
          int min_len = std::min(remove_reshape * l_in_shape.size() +
                                     (1 - remove_reshape) * l_out_shape.size(),
                                 r_in_shape.size());
          for (int i = 0; i < min_len - 2; i++) {
            int ls;
            if (remove_reshape)
              ls = l_in_shape[l_in_shape.size() - 3 - i];
            else
              ls = l_out_shape[l_out_shape.size() - 3 - i];
            int rs = r_in_shape[r_in_shape.size() - 3 - i];
            if (!(ls == rs || ls == 1 || rs == 1)) {
              return failure();
            }
          }
        }

        // Define Param
        op->setAttr("hdim_is_batch", rewriter.getBoolAttr(!hdim_is_batch));
        op->setAttr("left_transpose", rewriter.getBoolAttr(false));
        op->setAttr("right_transpose", rewriter.getBoolAttr(r_trans));
        if (remove_reshape) {
          op->setOperand(0, l_trans_op.getInput());
          rewriter.eraseOp(l_trans_op);
        }
        op->setOperand(1, r_trans_op.getInput());
        rewriter.eraseOp(r_trans_op);
      } else if (!isa<tpu::PermuteOp>(l_op) && isa<tpu::PermuteOp>(r_op)) {
        // Case3
        // Left  ->         -\              Left  Permute -\
        //                   ->  MatMul ->                -> MatMul
        // Right -> Permute -/              Right         -/
        auto l_trans_op = l_op;
        auto r_trans_op = dyn_cast<tpu::PermuteOp>(r_op);
        if (!l_trans_op->hasOneUse() || !r_trans_op->hasOneUse()) {
          return failure();
        }

        auto r_order = module::getI64Array(r_trans_op.getOrder());
        auto r_shape = module::getShape(r_trans_op.getOutput());
        auto l_shape = module::getShape(l_trans_op->getResult(0));
        if (false == (r_order->size() == 4 && r_order->at(0) == 0 &&
                      r_order->at(1) == 2 && l_shape[1] == r_shape[1])) {
          return failure();
        }
        auto op_name = module::getName(l_op->getResult(0)).str();
        // Add ReshapeOp or PermuteOp
        Operation *new_l_trans_op;

        std::vector<NamedAttribute> attrs;
        std::vector<int64_t> out_order = {0, 2, 1, 3};
        auto l_trans_type = RankedTensorType::get(
            {l_shape[0], l_shape[2], l_shape[1], l_shape[3]},
            module::getElementType(left));
        attrs.push_back(rewriter.getNamedAttr(
            "order", rewriter.getI64ArrayAttr(out_order)));
        new_l_trans_op = rewriter.create<tpu::PermuteOp>(
            NameLoc::get(rewriter.getStringAttr(op_name + "_permute")),
            l_trans_type,
            ValueRange{l_trans_op->getResult(0), module::getNoneOp(op)}, attrs);

        auto r_trans = op.getRightTranspose();
        if (r_order->at(2) == 3 && r_order->at(3) == 1) {
          r_trans = !r_trans;
        }

        // Define Param
        op->setAttr("hdim_is_batch", rewriter.getBoolAttr(!hdim_is_batch));
        op->setAttr("left_transpose", rewriter.getBoolAttr(false));
        op->setAttr("right_transpose", rewriter.getBoolAttr(r_trans));
        op->setOperand(0, new_l_trans_op->getResult(0));
        op->setOperand(1, r_trans_op.getInput());
        rewriter.eraseOp(r_trans_op);
      }
    } else if (l_is_weight || r_is_weight) {
      // When Left or Right is weight
      auto trans_op = r_is_weight
                          ? dyn_cast<tpu::PermuteOp>(left.getDefiningOp())
                          : dyn_cast<tpu::PermuteOp>(right.getDefiningOp());
      auto weight_op = l_is_weight ? left.getDefiningOp<top::WeightOp>()
                                   : right.getDefiningOp<top::WeightOp>();
      if (!weight_op->hasOneUse()) {
        return failure();
      }
      if (!(trans_op && trans_op->hasOneUse())) {
        return failure();
      }

      auto order = module::getI64Array(trans_op.getOrder());
      if (false ==
          (order->size() == 4 && order->at(0) == 0 && order->at(1) == 2)) {
        return failure();
      }
      auto l_trans = op.getLeftTranspose();
      auto r_trans = op.getRightTranspose();
      if (r_is_weight && order->at(2) == 3 && order->at(3) == 1) {
        l_trans = !l_trans;
      }
      if (l_is_weight && order->at(2) == 3 && order->at(3) == 1) {
        r_trans = !r_trans;
      }
      if (l_trans == true && r_trans == false) {
        // mm2 not support l_trans && !r_trans
        return failure();
      }

      // transpose the weight
      auto weight_type = module::getElementType(weight_op.getOutput());
      auto weight_shape = module::getShape(weight_op.getOutput());
      if (weight_type.isInteger(8)) {
        auto weight_data = weight_op.read<uint8_t>();
        auto weight_trans =
            std::make_shared<std::vector<uint8_t>>(weight_data->size(), 0);
        function_permute(weight_data->data(), weight_trans->data(),
                         weight_shape, {0, 2, 1, 3});
        std::vector<int64_t> weight_new_shape = {
            weight_shape[0], weight_shape[2], weight_shape[1], weight_shape[3]};
        rewriter.setInsertionPointAfter(op);
        auto type = RankedTensorType::get(weight_new_shape, weight_type);
        auto new_weight = top::WeightOp::create<uint8_t>(op, "transposed",
                                                         *weight_trans, type);
        op->setOperand(0, l_is_weight ? new_weight : trans_op.getInput());
        op->setOperand(1, r_is_weight ? new_weight : trans_op.getInput());
      } else if (weight_type.isF16() || weight_type.isBF16()) {
        auto weight_data = weight_op.read<uint16_t>();
        auto weight_trans =
            std::make_shared<std::vector<uint16_t>>(weight_data->size(), 0);
        function_permute(weight_data->data(), weight_trans->data(),
                         weight_shape, {0, 2, 1, 3});
        std::vector<int64_t> weight_new_shape = {
            weight_shape[0], weight_shape[2], weight_shape[1], weight_shape[3]};
        rewriter.setInsertionPointAfter(op);
        auto type = RankedTensorType::get(weight_new_shape, weight_type);
        auto new_weight = top::WeightOp::create<uint16_t>(op, "transposed",
                                                          *weight_trans, type);
        op->setOperand(0, l_is_weight ? new_weight : trans_op.getInput());
        op->setOperand(1, r_is_weight ? new_weight : trans_op.getInput());
      } else {
        llvm_unreachable("Weight type error!");
      }

      op->setAttr("hdim_is_batch", rewriter.getBoolAttr(!hdim_is_batch));
      op->setAttr("left_transpose", rewriter.getBoolAttr(l_trans));
      op->setAttr("right_transpose", rewriter.getBoolAttr(r_trans));

      rewriter.eraseOp(trans_op);
      rewriter.eraseOp(weight_op);
    } else {
      return failure();
    }

    // 4. Modify matmul out shape and name
    auto mat_out = op->getResult(0);
    auto trans_type = mat_out.getType();
    auto out_shape = module::getShape(mat_out);
    std::vector<int64_t> new_out_shape(4, 0);
    new_out_shape[0] = out_shape[0];
    new_out_shape[1] = out_shape[2];
    new_out_shape[2] = out_shape[1];
    new_out_shape[3] = out_shape[3];
    module::setShape(mat_out, new_out_shape);
    auto ori_loc = op->getLoc();
    module::setLocSuffix(op, "hdim_is_batch");

    // 5. Add Transpose(0,2,1,3) to output
    rewriter.setInsertionPointAfter(op);
    std::vector<NamedAttribute> attrs;
    std::vector<int64_t> out_order = {0, 2, 1, 3};
    attrs.push_back(
        rewriter.getNamedAttr("order", rewriter.getI64ArrayAttr(out_order)));
    auto trans_op = rewriter.create<tpu::PermuteOp>(
        ori_loc, trans_type, ValueRange{mat_out, module::getNoneOp(op)}, attrs);
    rewriter.replaceAllUsesExcept(mat_out, trans_op->getResult(0), trans_op);
    return success();
  }
};

class MatMulLeftReusePattern : public OpRewritePattern<tpu::MatMulOp> {
public:
  using OpRewritePattern::OpRewritePattern;
  LogicalResult matchAndRewrite(tpu::MatMulOp op,
                                PatternRewriter &rewriter) const override {
    auto in_op = op.getInput().getDefiningOp();
    if (in_op->hasOneUse()) {
      op.setLeftReuse(0);
    } else {
      op.setLeftReuse(1);
    }
    return failure();
  }
};

/*
  Do:
    Reshape
            + MatMul -->>  MatMul
    Reshape

  When:
      Reshape (1,N,K) -> (1,1,N,K) or (1,N,K) -> (1,N,1,K)
*/
class MatMulRemoveReshapePattern : public OpRewritePattern<tpu::MatMulOp> {
public:
  using OpRewritePattern::OpRewritePattern;
  LogicalResult matchAndRewrite(tpu::MatMulOp op,
                                PatternRewriter &rewriter) const override {
    auto left_op =
        dyn_cast_or_null<tpu::ReshapeOp>(op.getInput().getDefiningOp());
    auto right_op =
        dyn_cast_or_null<tpu::ReshapeOp>(op.getRight().getDefiningOp());
    if (!(left_op && left_op->hasOneUse()))
      return failure();
    if (!(right_op && right_op->hasOneUse()))
      return failure();

    if (module::getShape(left_op.getInput()).size() !=
        module::getShape(right_op.getInput()).size())
      return failure();

    if (module::getShape(left_op.getInput()).size() <= 2) {
      return failure();
    }

    auto reshape_is_unsqueeze = [](tpu::ReshapeOp reshape_op) {
      std::vector<int64_t> in_shape = module::getShape(reshape_op.getInput());
      std::vector<int64_t> out_shape = module::getShape(reshape_op.getOutput());
      std::vector<int64_t> in_set;
      for (auto in : in_shape) {
        if (in != 1)
          in_set.emplace_back(in);
      }
      std::vector<int64_t> out_set;
      for (auto out : out_shape) {
        if (out != 1)
          out_set.emplace_back(out);
      }
      return (out_shape.size() > in_shape.size() && in_set == out_set);
    };

    if (!reshape_is_unsqueeze(left_op) || !reshape_is_unsqueeze(right_op))
      return failure();

    op.setOperand(0, left_op.getInput());
    op.setOperand(1, right_op.getInput());
    rewriter.eraseOp(left_op);
    rewriter.eraseOp(right_op);
    return success();
  }
};

// transform group conv to normal conv, when int8/f16/bf16 &&
// input_c<=ic_parallel && isBM1684XFamily()
class GroupConv2NormalConv : public OpRewritePattern<tpu::Conv2DOp> {
public:
  using OpRewritePattern::OpRewritePattern;
  LogicalResult matchAndRewrite(tpu::Conv2DOp op,
                                PatternRewriter &rewriter) const override {
    if (!(module::isBM1684XFamily() || module::isSG2260Family()) ||
        !module::isWeight(op.getFilter())) {
      return failure();
    }
    auto data_type = module::getStorageType(op.getFilter());
    if (!(data_type.isBF16() || data_type.isF16() || data_type.isInteger(8))) {
      return failure();
    }
    auto attrs = op.parseParam();
    if (attrs.groups == 1) {
      return failure();
    }
    int ic_parallel = BM168x::ic_num(data_type.getIntOrFloatBitWidth() / 8);
    if (attrs.ic > ic_parallel) {
      return failure();
    }

    if (data_type.isUnsignedInteger(8)) {
      updateFilter<uint8_t>(op, attrs);
    } else if (data_type.isInteger(8)) {
      updateFilter<int8_t>(op, attrs);
    } else {
      updateFilter<uint16_t>(op, attrs);
    }
    op.setGroup(1);
    return success();
  }

private:
  template <typename T>
  void updateFilter(tpu::Conv2DOp op, const conv_attr_t &p) const {
    int gic = p.ic / p.groups;
    int goc = p.oc / p.groups;
    int old_ic_num = gic * p.kh * p.kw;
    int new_ic_num = p.ic * p.kh * p.kw;
    auto filterOp = cast<top::WeightOp>(op.getFilter().getDefiningOp());
    auto filter_data = filterOp.read<T>();
    auto filter_size = filter_data->size();
    auto new_data = std::make_shared<std::vector<T>>(filter_size * p.groups,
                                                     op.getKernelZp());
    for (int i = 0; i < p.oc; i++) {
      auto begin = filter_data->begin() + old_ic_num * i;
      auto end = begin + old_ic_num;
      int group_idx = i / goc;
      auto to = new_data->begin() + new_ic_num * i + old_ic_num * group_idx;
      std::copy(begin, end, to);
    }
    auto new_type =
        module::getTypeLike(op.getFilter(), {p.oc, p.ic, p.kh, p.kw});
    auto new_filter =
        top::WeightOp::create(op, "filter_g2normal", *new_data, new_type);
    op->setOperand(1, new_filter);
  }
};

// reorder op when transpose is before mulconst/cast/softmax to optimize bert
// TODO: may be merged into PermuteReorderPattern
class PermuteAddWeightReorderPattern : public OpRewritePattern<tpu::PermuteOp> {
public:
  using OpRewritePattern::OpRewritePattern;
  LogicalResult matchAndRewrite(tpu::PermuteOp op,
                                PatternRewriter &rewriter) const override {
    //  if (module::isAsymmetric()) {
    //    return failure();
    //  }

    if (op->hasOneUse() == false) {
      return failure();
    }
    std::vector<int64_t> ps = {0, 2, 1, 3};
    auto order = module::getI64Array(op.getOrder());
    if (*order != ps) {
      return failure();
    }

    auto in_shape = module::getShape(op.getInput());
    auto out_shape = module::getShape(op.getOutput());
    auto nextOp = *op.getOutput().user_begin();
    if (nextOp->hasOneUse() == false) {
      return failure();
    }
    if (auto add_op = dyn_cast<tpu::AddOp>(nextOp)) {
      /**
       * weight        ->         permuted_weight   ->
       *               -> Add =>                    -> Add -> perm
       * input -> perm ->         input             ->
       *
       */
      auto inB = add_op.getInputs()[1];
      if (!module::isWeight(inB)) {
        return failure();
      }
      std::vector<int64_t> inB_shape = module::getShape(inB);
      std::vector<int64_t> new_inB_shape = {inB_shape[0], inB_shape[2],
                                            inB_shape[1], inB_shape[3]};
      auto newType = module::getTypeLike(inB, new_inB_shape);
      auto weight_op = inB.getDefiningOp<top::WeightOp>();
      auto weight_type = module::getElementType(weight_op.getOutput());
      if (weight_type.isF16() || weight_type.isBF16()) {
        auto weight_data = weight_op.read<uint16_t>();
        auto weight_tp =
            std::make_shared<std::vector<uint16_t>>(weight_data->size(), 0);
        function_permute(weight_data->data(), weight_tp->data(), inB_shape, ps);
        auto weight = tpu_mlir::top::WeightOp::create<uint16_t>(
            add_op, "transposed_add_weight", *weight_tp, newType);
        add_op.setOperand(1, weight);
      } else if (weight_type.isF32()) {
        auto weight_data = weight_op.read<float>();
        auto weight_tp =
            std::make_shared<std::vector<float>>(weight_data->size(), 0);
        function_permute(weight_data->data(), weight_tp->data(), inB_shape, ps);
        auto weight = tpu_mlir::top::WeightOp::create<float>(
            add_op, "transposed_add_weight", *weight_tp, newType);
        add_op.setOperand(1, weight);
      } else if (weight_type.isInteger(8)) {
        auto weight_data = weight_op.read<uint8_t>();
        auto weight_tp =
            std::make_shared<std::vector<uint8_t>>(weight_data->size(), 0);
        function_permute(weight_data->data(), weight_tp->data(), inB_shape, ps);
        auto weight = tpu_mlir::top::WeightOp::create<uint8_t>(
            add_op, "transposed_add_weight", *weight_tp, newType);
        add_op.setOperand(1, weight);
      }

      newType = RankedTensorType::get(
          in_shape, module::getElementType(add_op.getOutput()));
      add_op.getOutput().setType(newType);
      op.replaceAllUsesWith(op.getInput());
      rewriter.setInsertionPointAfter(add_op);
      newType = RankedTensorType::get(
          out_shape, module::getElementType(add_op.getOutput()));
      auto out_loc = add_op.getLoc(); // keep out location unchanged.
      module::setLocSuffix(add_op, "_trans");
      std::vector<NamedAttribute> attrs;
      attrs.push_back(
          rewriter.getNamedAttr("order", rewriter.getI64ArrayAttr(ps)));
      auto new_op = rewriter.create<tpu::PermuteOp>(
          out_loc, newType,
          ValueRange{add_op.getOutput(), module::getNoneOp(add_op)}, attrs);
      add_op.getOutput().replaceAllUsesExcept(new_op.getOutput(), {new_op});
      rewriter.eraseOp(op);
      return success();
    } else if (auto mul_op = dyn_cast<tpu::MulOp>(nextOp)) {
      auto inB = mul_op.getInputs()[1];
      if (!module::isWeight(inB)) {
        return failure();
      }
      auto inB_shape = module::getShape(inB);
      if (inB_shape[1] != 1) {
        return failure();
      }
      std::vector<int64_t> new_inB_shape = {inB_shape[0], inB_shape[2],
                                            inB_shape[1], inB_shape[3]};
      module::setShape(inB, new_inB_shape);
      Value mul_out = mul_op.getOutput();
      module::setShape(mul_out, in_shape);

      op.replaceAllUsesWith(op.getInput());
      rewriter.setInsertionPointAfter(mul_op);
      auto newType = module::getTypeLike(mul_out, out_shape);
      auto out_loc = mul_op.getLoc(); // keep out location unchanged.
      module::setLocSuffix(mul_op, "trans");
      std::vector<NamedAttribute> attrs;
      attrs.push_back(
          rewriter.getNamedAttr("order", rewriter.getI64ArrayAttr(ps)));
      auto new_op = rewriter.create<tpu::PermuteOp>(
          out_loc, newType, ValueRange{mul_out, module::getNoneOp(mul_op)},
          attrs);
      mul_out.replaceAllUsesExcept(new_op.getOutput(), {new_op});
      rewriter.eraseOp(op);
      return success();
    }

    return failure();
  }
};

/**
 * input0 + Permute \              => input0           \
 *                   => MaskedFill =>                   => MaskedFill + Permute
 * input1           /              => input1 + Permute /
 */
class MaskedFillPermuteMove : public OpRewritePattern<tpu::MaskedFillOp> {
public:
  using OpRewritePattern::OpRewritePattern;
  LogicalResult matchAndRewrite(tpu::MaskedFillOp op,
                                PatternRewriter &rewriter) const override {
    auto input_shape = module::getShape(op.getBrn());
    auto condition_shape = module::getShape(op.getCond());
    if (input_shape != condition_shape) {
      return failure();
    }
    auto op_name = module::getName(op.getOutput()).str();
    if (op_name.find("_masked_fill") != std::string::npos) {
      return failure();
    }
    auto none_op = module::getNoneOp(op);
    std::vector<bool> is_permute;
    assert(op->getNumOperands() == 2);
    tpu::PermuteOp permute_op;
    for (auto opd : op->getOperands()) {
      Operation *op_ = opd.getDefiningOp();
      if (isa<tpu::PermuteOp>(op_)) {
        is_permute.push_back(true);
        permute_op = dyn_cast<tpu::PermuteOp>(op_);
      } else {
        is_permute.push_back(false);
      }
    }
    if (is_permute[0] == is_permute[1]) {
      return failure();
    }
    auto permute_attr = permute_op->getAttrs();
    auto permute_order = *module::getI64Array(permute_op.getOrder());
    std::vector<int64_t> inv_order(permute_order.size());
    for (int i = 0; i < permute_order.size(); ++i) {
      inv_order[permute_order[i]] = i;
    }
    int need_permute = is_permute[0] ? 1 : 0;
    auto need_permute_op = op->getOperand(need_permute);

    auto type = permute_op.getInput().getType();
    auto name = module::getName(need_permute_op);
    std::vector<NamedAttribute> attrs;

    attrs.push_back(
        rewriter.getNamedAttr("order", rewriter.getI64ArrayAttr(inv_order)));

    int user_count = 0;
    for (auto j : need_permute_op.getUsers()) {
      if (isa<tpu::PermuteOp>(j)) {
        user_count++;
      }
    }
    auto loc = NameLoc::get(rewriter.getStringAttr(name.str() + "_permute" +
                                                   std::to_string(user_count)));
    auto new_permute_op = rewriter.create<tpu::PermuteOp>(
        loc, type, ValueRange{need_permute_op, none_op}, attrs);
    auto masked_fill_attrs = op->getAttrs();
    loc = NameLoc::get(
        rewriter.getStringAttr(module::getName(need_permute_op).str() +
                               "_masked_fill" + std::to_string(user_count)));
    Value cond, brn;
    if (is_permute[0]) {
      cond = permute_op.getInput();
      brn = new_permute_op.getOutput();
    } else {
      cond = new_permute_op.getOutput();
      brn = permute_op.getInput();
    }
    rewriter.setInsertionPointAfterValue(new_permute_op.getOutput());
    auto new_masked_fill_op = rewriter.create<tpu::MaskedFillOp>(
        loc, type, ValueRange{cond, brn}, masked_fill_attrs);
    rewriter.replaceAllUsesWith(permute_op, new_masked_fill_op.getOutput());
    rewriter.eraseOp(permute_op);
    rewriter.setInsertionPointAfterValue(new_masked_fill_op.getOutput());
    auto post_permute_op = rewriter.create<tpu::PermuteOp>(
        op.getLoc(), op.getOutput().getType(),
        ValueRange{new_masked_fill_op.getOutput(),
                   module::getNoneOp(new_masked_fill_op)},
        permute_attr);
    rewriter.replaceAllUsesWith(op.getOutput(), post_permute_op.getOutput());
    rewriter.eraseOp(op);
    return success();
  }
};

/**
 * permute \
 *          => Add => Add -> permute
 * permute /
 */
class MovePermuteAfterAdd : public OpRewritePattern<tpu::AddOp> {
public:
  using OpRewritePattern::OpRewritePattern;
  LogicalResult matchAndRewrite(tpu::AddOp op,
                                PatternRewriter &rewriter) const override {
    auto l_permute_op = op.getOperand(0).getDefiningOp<tpu::PermuteOp>();
    auto r_permute_op = op.getOperand(1).getDefiningOp<tpu::PermuteOp>();
    if (!l_permute_op || !r_permute_op)
      return failure();
    auto l_in_shape = module::getShape(l_permute_op.getInput()).vec();
    auto r_in_shape = module::getShape(r_permute_op.getInput()).vec();
    if (l_in_shape.size() != r_in_shape.size())
      return failure();
    auto l_permute_order = *module::getI64Array(l_permute_op.getOrder());
    auto r_permute_order = *module::getI64Array(r_permute_op.getOrder());
    if (l_permute_order != r_permute_order)
      return failure();
    auto loc = op.getLoc();
    op.setOperand(0, l_permute_op.getInput());
    op.setOperand(1, r_permute_op.getInput());
    auto output = op.getOutput();
    auto output_type = output.getType();
    std::vector<int64_t> new_shape;
    for (int i = 0; i < l_in_shape.size(); ++i) {
      new_shape.push_back(std::max(l_in_shape[i], r_in_shape[i]));
    }
    module::setShape(output, new_shape);
    module::setLocSuffix(op, "before_permute");

    if (l_permute_op.getOutput().getUsers().empty()) {
      rewriter.eraseOp(l_permute_op);
    }
    if (r_permute_op.getOutput().getUsers().empty()) {
      rewriter.eraseOp(r_permute_op);
    }

    rewriter.setInsertionPointAfterValue(output);
    std::vector<NamedAttribute> attrs;
    attrs.emplace_back(rewriter.getNamedAttr(
        "order", rewriter.getI64ArrayAttr(l_permute_order)));
    auto new_permute_op = rewriter.create<tpu::PermuteOp>(
        loc, output_type, ValueRange{output, module::getNoneOp(op)}, attrs);
    output.replaceAllUsesExcept(new_permute_op.getOutput(), new_permute_op);
    return success();
  }
};

/**
 * reshape \
 *          => Add => Add -> reshape
 * reshape /
 *
 * NOTE: may have performance problem, for example:
 *  reshape(* -> 1,64,1,1) \
 *                          => Add(1,64,1,1) => Add(1,1,1,64) -> reshape
 *  reshape(* -> 1,64,1,1) /
 *
 * Optimized pattern can not make full use of lanes.
 *
 */
class MoveReshapeAfterAdd : public OpRewritePattern<tpu::AddOp> {
public:
  using OpRewritePattern::OpRewritePattern;
  LogicalResult matchAndRewrite(tpu::AddOp op,
                                PatternRewriter &rewriter) const override {
    auto l_reshape_op = op.getOperand(0).getDefiningOp<tpu::ReshapeOp>();
    auto r_reshape_op = op.getOperand(1).getDefiningOp<tpu::ReshapeOp>();
    if (!l_reshape_op || !r_reshape_op)
      return failure();
    auto l_in_shape = module::getShape(l_reshape_op.getInput()).vec();
    auto r_in_shape = module::getShape(r_reshape_op.getInput()).vec();
    if (l_in_shape != r_in_shape)
      return failure();
    auto l_out_shape = module::getShape(l_reshape_op.getOutput()).vec();
    auto r_out_shape = module::getShape(r_reshape_op.getOutput()).vec();
    if (l_out_shape != r_out_shape)
      return failure();
    auto loc = op.getLoc();
    op.setOperand(0, l_reshape_op.getInput());
    op.setOperand(1, r_reshape_op.getInput());
    auto output = op.getOutput();
    module::setShape(output, l_in_shape);
    module::setLocSuffix(op, "before_reshape");

    rewriter.setInsertionPointAfterValue(output);
    auto reshape_type = module::getTypeLike(output, l_out_shape);
    auto new_reshape_op =
        rewriter.create<tpu::ReshapeOp>(loc, reshape_type, ValueRange{output});
    output.replaceAllUsesExcept(new_reshape_op.getOutput(), new_reshape_op);
    return success();
  }
};

// reorder op when reshapeOp is before matmul/mulconst/cast/softmax op to
// eliminate reshapeOp
// copied from lib/Dialect/Top/Transforms/ProcessorOptimize/OptimizeBM1684X.cpp
class TpuReshapeReorderPattern : public OpRewritePattern<tpu::ReshapeOp> {
public:
  using OpRewritePattern::OpRewritePattern;
  LogicalResult matchAndRewrite(tpu::ReshapeOp op,
                                PatternRewriter &rewriter) const override {
    auto output = op.getOutput();
    if (!output.hasOneUse()) {
      return failure();
    }
    auto next_op_ = *output.user_begin();

    if (auto next_op = dyn_cast<tpu::MatMulOp>(next_op_)) {
      // right is from Reshape too
      auto left = next_op.getInput();
      auto right = next_op.getRight();
      auto right_op_ = right.getDefiningOp();
      auto right_op = dyn_cast<tpu::ReshapeOp>(right_op_);
      if (op != left.getDefiningOp() || !right_op) {
        return failure();
      }
      // check left and right are both Reshape(n, c, h, w) --> (nxc, h, w)
      auto lshape_ = SmallVector<int64_t>(module::getShape(op.getInput()));
      auto lshape = module::getShape(left);
      if (!(lshape.size() == 3 && lshape_.size() == 4 &&
            lshape[0] == lshape_[0] * lshape_[1] && lshape[1] == lshape_[2] &&
            lshape[2] == lshape_[3])) {
        return failure();
      }
      auto rshape_ = module::getShape(right_op.getInput());
      auto rshape = SmallVector<int64_t>(module::getShape(right));
      if (!(rshape.size() == 3 && rshape_.size() == 4 &&
            rshape[0] == rshape_[0] * rshape_[1] && rshape[1] == rshape_[2] &&
            rshape[2] == rshape_[3])) {
        return failure();
      }
      if (lshape_[0] != rshape_[0] || lshape_[1] != rshape_[1]) {
        return failure();
      }

      // remove left and right ReshapeOp
      op.replaceAllUsesWith(op.getInput());
      right_op.replaceAllUsesWith(right_op.getInput());

      // Update MatMul output shape
      // and update loc to avoid comparing
      auto next_out = next_op.getOutput();
      auto ori_out_type = next_out.getType();
      auto oshape = module::getShape(next_out);
      std::vector<int64_t> new_oshape{lshape_[0], lshape_[1], oshape[1],
                                      oshape[2]};
      module::setShape(next_out, new_oshape);
      auto ori_loc = next_op.getLoc();
      module::setLocSuffix(next_op, "Reshape");

      // Add ReshapeOp after MatMul
      rewriter.setInsertionPointAfterValue(next_out);
      auto new_reshape_op = rewriter.create<tpu::ReshapeOp>(
          ori_loc, ori_out_type, ValueRange{next_out});
      next_out.replaceAllUsesExcept(new_reshape_op.getOutput(), new_reshape_op);
      rewriter.eraseOp(op);
      rewriter.eraseOp(right_op);
      return success();
    } else if (isa<tpu::MulConstOp, tpu::CastOp, tpu::SoftmaxOp>(next_op_)) {
      // check input is Reshape(n, c, h, w) --> (nxc, h, w)
      auto ishape = SmallVector<int64_t>(module::getShape(op.getInput()));
      auto next_ishape = module::getShape(op.getOutput());
      if (!(next_ishape.size() == 3 && ishape.size() == 4 &&
            next_ishape[0] == ishape[0] * ishape[1] &&
            next_ishape[1] == ishape[2] && next_ishape[2] == ishape[3])) {
        return failure();
      }
      // check next_op param
      if (auto next_op = dyn_cast<tpu::SoftmaxOp>(next_op_)) {
        int64_t axis = next_op.getAxis();
        if (axis != 2 || axis == -1) {
          return failure();
        }
      }

      // remove ReshapeOp
      op.replaceAllUsesWith(op.getInput());

      // update next_op output shape and modify loc name to avoid comparing
      auto next_out = next_op_->getResult(0);
      auto ori_out_type = next_out.getType();
      auto ori_loc = next_op_->getLoc();
      module::setShape(next_out, ishape);
      module::setLocSuffix(next_op_, "Reshape");

      // Add ReshapeOp after MulConst/Cast/Softmax
      rewriter.setInsertionPointAfterValue(next_out);
      auto new_reshape_op = rewriter.create<tpu::ReshapeOp>(
          ori_loc, ori_out_type, ValueRange{next_out});
      next_out.replaceAllUsesExcept(new_reshape_op.getOutput(), new_reshape_op);

      if (auto next_op = dyn_cast<tpu::SoftmaxOp>(next_op_)) {
        next_op->setAttr("axis", rewriter.getSI32IntegerAttr(3));
      }
      rewriter.eraseOp(op);
      return success();
    } else if (auto next_op = dyn_cast<tpu::ReshapeOp>(next_op_)) {
      auto ishape = module::getShape(op.getInput());
      auto next_oshape = module::getShape(next_op.getOutput());
      if (ishape != next_oshape) {
        return failure();
      }

      op.replaceAllUsesWith(op.getInput());
      next_op.replaceAllUsesWith(next_op.getInput());
      rewriter.eraseOp(op);
      return success();
    }

    return failure();
  }
};

// permute + permute or permute + reshape + permute
// copied from lib/Dialect/Top/Canonicalize/Permute.cpp (e41cc7c5)
struct PermuteFuse : public OpRewritePattern<tpu::PermuteOp> {
  using OpRewritePattern::OpRewritePattern;

  LogicalResult matchAndRewrite(tpu::PermuteOp op,
                                PatternRewriter &rewriter) const override {
    auto in = op.getInput();
    if (in.hasOneUse() == false) {
      return failure();
    }
    if (auto rop = dyn_cast<tpu::ReshapeOp>(in.getDefiningOp())) {
      in = rop.getInput();
      if (in.hasOneUse() == false) {
        return failure();
      }
    }
    auto permute_op = dyn_cast<tpu::PermuteOp>(in.getDefiningOp());
    if (!permute_op) {
      return failure();
    }
    // op order
    std::vector<int64_t> in0_shape = module::getShape(permute_op.getInput());
    auto in0_order = module::getI64Array(permute_op.getOrder());
    std::vector<int64_t> in1_shape = module::getShape(op.getInput());
    auto in1_order = module::getI64Array(op.getOrder());
    std::vector<int64_t> out1_shape = module::getShape(op.getOutput());
    std::vector<int64_t> in0_shape_fix;
    std::vector<int64_t> in0_order_fix;
    std::vector<int64_t> out0_shape_fix;
    std::vector<int64_t> in1_shape_fix;
    std::vector<int64_t> in1_order_fix;
    int to_dim;
    for (to_dim = 2; to_dim <= 5; to_dim++) {
      auto ret = permute_reset(in0_shape, *in0_order, in0_shape_fix,
                               in0_order_fix, to_dim);
      if (ret == false) {
        continue;
      }
      ret = permute_reset(in1_shape, *in1_order, in1_shape_fix, in1_order_fix,
                          to_dim);
      if (ret == false) {
        continue;
      }
      break;
    }
    if (to_dim > 5) {
      return failure();
    }
    for (auto o : in0_order_fix) {
      out0_shape_fix.push_back(in0_shape_fix[o]);
    }
    if (in1_shape_fix != out0_shape_fix) {
      return failure();
    }
    // test
    std::vector<int64_t> origin_data;
    for (int64_t i = 0; i < to_dim; i++) {
      origin_data.push_back(i);
    }
    std::vector<int64_t> result0_data;
    for (auto o : in0_order_fix) {
      result0_data.push_back(origin_data[o]);
    }
    std::vector<int64_t> result1_data;
    for (auto o : in1_order_fix) {
      result1_data.push_back(result0_data[o]);
    }
    if (result1_data != origin_data) {
      return failure();
    }
    // bingoo !
    if (out1_shape == in0_shape) {
      op.getOutput().replaceAllUsesWith(permute_op.getInput());
      rewriter.eraseOp(op);
      rewriter.eraseOp(permute_op);
    } else {
      auto loc = module::getLocLike(permute_op.getInput(), "Reshape");
      rewriter.setInsertionPoint(op);
      auto rs_op = rewriter.create<tpu::ReshapeOp>(
          loc, op.getOutput().getType(), ValueRange{permute_op.getInput()});
      op.getOutput().replaceAllUsesWith(rs_op.getOutput());
      rewriter.eraseOp(op);
    }
    return success();
  }
};

// Calculate `indices_coeff` for GatherElementsOp when axis != indices_dims - 1
//               / 1, i = axis
// axis_flag[i] =
//               \ 0, else
// input_stride[i] = input_shape[i-1] * ... * input_shape[0]
// indices_coeff[i0][i1]...[in-1] = i0 * input_stride[0] * axis_flag[i] + ... +
// in-1 * input_stride[n-1] * axis_flag[n-1]
struct GatherElementsPattern : public OpRewritePattern<tpu::GatherElementsOp> {
  using OpRewritePattern::OpRewritePattern;

  LogicalResult matchAndRewrite(tpu::GatherElementsOp op,
                                PatternRewriter &rewriter) const override {

    auto indices = op.getIndices();
    auto indices_shape = module::getShape(indices);
    auto indices_dims = indices_shape.size();
    auto axis = op.getAxis();
    if (axis == indices_dims - 1) {
      return failure();
    }
    if (!op.getIndicesCoeff().getType().isa<NoneType>()) {
      return failure();
    }
    auto input = op.getInput();
    auto input_shape = module::getShape(input);
    std::vector<Value> operands;
    operands.push_back(op.getInput());
    operands.push_back(indices);

    auto indice_type = module::getElementType(indices);
    auto type = RankedTensorType::get(indices_shape, indice_type);
    int indices_shape8[8] = {1, 1, 1, 1, 1, 1, 1, 1};
    int input_shape8[8] = {1, 1, 1, 1, 1, 1, 1, 1};

    for (int i = 0; i < indices_dims; ++i) {
      indices_shape8[i] = indices_shape[i];
      input_shape8[i] = input_shape[i];
    }
    std::vector<int> indices_coeff;

    int tmp = 0;
    // loop for 8 times
    for (int i0 = 0; i0 < indices_shape8[0]; ++i0) {
      int tmp0 = 0;
      tmp0 += axis == 0 ? 0 : i0;
      tmp0 *= input_shape8[1];
      for (int i1 = 0; i1 < indices_shape8[1]; ++i1) {
        int tmp1 = tmp0;
        tmp1 += axis == 1 ? 0 : i1;
        tmp1 *= input_shape8[2];
        for (int i2 = 0; i2 < indices_shape8[2]; ++i2) {
          int tmp2 = tmp1;
          tmp2 += axis == 2 ? 0 : i2;
          tmp2 *= input_shape8[3];
          for (int i3 = 0; i3 < indices_shape8[3]; ++i3) {
            int tmp3 = tmp2;
            tmp3 += axis == 3 ? 0 : i3;
            tmp3 *= input_shape8[4];
            for (int i4 = 0; i4 < indices_shape8[4]; ++i4) {
              int tmp4 = tmp3;
              tmp4 += axis == 4 ? 0 : i4;
              tmp4 *= input_shape8[5];
              for (int i5 = 0; i5 < indices_shape8[5]; ++i5) {
                int tmp5 = tmp4;
                tmp5 += axis == 5 ? 0 : i5;
                tmp5 *= input_shape8[6];
                for (int i6 = 0; i6 < indices_shape8[6]; ++i6) {
                  int tmp6 = tmp5;
                  tmp6 += axis == 6 ? 0 : i6;
                  tmp6 *= input_shape8[7];
                  for (int i7 = 0; i7 < indices_shape8[7]; ++i7) {
                    tmp++;
                    int tmp7 = tmp6;
                    tmp7 += i7;
                    indices_coeff.push_back(tmp7);
                    // llvm::outs() << tmp << " " << tmp7 << "\n";
                  }
                }
              }
            }
          }
        }
      }
    }

    auto indices_coeff_op =
        top::WeightOp::create(op, "indices_coeff", indices_coeff, type);
    operands.push_back(indices_coeff_op);

    operands.push_back(op.getBuffer());
    rewriter.setInsertionPointAfter(op);
    auto new_op = rewriter.create<tpu::GatherElementsOp>(
        op.getLoc(), op.getResult().getType(), operands, op->getAttrs());
    op.getOutput().replaceAllUsesWith(new_op.getOutput());
    rewriter.eraseOp(op);
    return success();
  }
};

struct ScatterElementsPattern
    : public OpRewritePattern<tpu::ScatterElementsOp> {
  using OpRewritePattern::OpRewritePattern;

  LogicalResult matchAndRewrite(tpu::ScatterElementsOp op,
                                PatternRewriter &rewriter) const override {

    auto indices = op.getIndices();
    auto indices_shape = module::getShape(indices);
    auto indices_dims = indices_shape.size();
    auto axis = op.getAxis();
    if (axis == indices_dims - 1) {
      return failure();
    }
    if (!op.getIndicesCoeff().getType().isa<NoneType>()) {
      return failure();
    }
    auto input = op.getInput();
    auto input_shape = module::getShape(input);
    std::vector<Value> operands;
    operands.push_back(op.getInput());
    operands.push_back(indices);
    operands.push_back(op.getUpdates());

    auto indice_type = module::getElementType(indices);
    auto type = RankedTensorType::get(indices_shape, indice_type);
    int indices_shape8[8] = {1, 1, 1, 1, 1, 1, 1, 1};
    int input_shape8[8] = {1, 1, 1, 1, 1, 1, 1, 1};

    for (int i = 0; i < indices_dims; ++i) {
      indices_shape8[i] = indices_shape[i];
      input_shape8[i] = input_shape[i];
    }
    std::vector<int> indices_coeff;

    int tmp = 0;
    // loop for 8 times
    for (int i0 = 0; i0 < indices_shape8[0]; ++i0) {
      int tmp0 = 0;
      tmp0 += axis == 0 ? 0 : i0;
      tmp0 *= input_shape8[1];
      for (int i1 = 0; i1 < indices_shape8[1]; ++i1) {
        int tmp1 = tmp0;
        tmp1 += axis == 1 ? 0 : i1;
        tmp1 *= input_shape8[2];
        for (int i2 = 0; i2 < indices_shape8[2]; ++i2) {
          int tmp2 = tmp1;
          tmp2 += axis == 2 ? 0 : i2;
          tmp2 *= input_shape8[3];
          for (int i3 = 0; i3 < indices_shape8[3]; ++i3) {
            int tmp3 = tmp2;
            tmp3 += axis == 3 ? 0 : i3;
            tmp3 *= input_shape8[4];
            for (int i4 = 0; i4 < indices_shape8[4]; ++i4) {
              int tmp4 = tmp3;
              tmp4 += axis == 4 ? 0 : i4;
              tmp4 *= input_shape8[5];
              for (int i5 = 0; i5 < indices_shape8[5]; ++i5) {
                int tmp5 = tmp4;
                tmp5 += axis == 5 ? 0 : i5;
                tmp5 *= input_shape8[6];
                for (int i6 = 0; i6 < indices_shape8[6]; ++i6) {
                  int tmp6 = tmp5;
                  tmp6 += axis == 6 ? 0 : i6;
                  tmp6 *= input_shape8[7];
                  for (int i7 = 0; i7 < indices_shape8[7]; ++i7) {
                    tmp++;
                    int tmp7 = tmp6;
                    tmp7 += i7;
                    indices_coeff.push_back(tmp7);
                    // llvm::outs() << tmp << " " << tmp7 << "\n";
                  }
                }
              }
            }
          }
        }
      }
    }

    auto indices_coeff_op =
        top::WeightOp::create(op, "indices_coeff", indices_coeff, type);
    operands.push_back(indices_coeff_op);

    operands.push_back(op.getBuffer());
    rewriter.setInsertionPointAfter(op);
    auto new_op = rewriter.create<tpu::ScatterElementsOp>(
        op.getLoc(), op.getResult().getType(), operands, op->getAttrs());
    op.getOutput().replaceAllUsesWith(new_op.getOutput());
    rewriter.eraseOp(op);
    return success();
  }
};

// permute + (mulconst) + add + cast + softmax + cast + permute
// -> add + cast + softmax + cast
struct PermuteFuseAddSoftmax : public OpRewritePattern<tpu::PermuteOp> {
  using OpRewritePattern::OpRewritePattern;

  LogicalResult matchAndRewrite(tpu::PermuteOp op,
                                PatternRewriter &rewriter) const override {
    auto in = op.getInput();
    auto out = op->getResult(0);
    if (in.hasOneUse() == false) {
      return failure();
    }
    if (!op->hasOneUse()) {
      return failure();
    }
    auto cast_bottom_op = dyn_cast<tpu::CastOp>(in.getDefiningOp());
    if (!cast_bottom_op) {
      return failure();
    }
    auto softmax_op =
        dyn_cast<tpu::SoftmaxOp>(cast_bottom_op->getOperand(0).getDefiningOp());
    if (!softmax_op) {
      return failure();
    }
    auto cast_top_op =
        dyn_cast<tpu::CastOp>(softmax_op->getOperand(0).getDefiningOp());
    if (!cast_top_op) {
      return failure();
    }
    auto add_op =
        dyn_cast<tpu::AddOp>(cast_top_op->getOperand(0).getDefiningOp());
    if (!add_op) {
      return failure();
    }
    auto mul_const_op =
        dyn_cast<tpu::MulConstOp>(add_op->getOperand(0).getDefiningOp());
    auto permute_op =
        dyn_cast<tpu::PermuteOp>(add_op->getOperand(0).getDefiningOp());
    if (mul_const_op) {
      permute_op =
          dyn_cast<tpu::PermuteOp>(mul_const_op->getOperand(0).getDefiningOp());
    }
    if (!permute_op) {
      return failure();
    }
    auto top_order = module::getI64Array(permute_op.getOrder());
    auto bottom_order = module::getI64Array(op.getOrder());
    if (false == (top_order->size() == 4 && top_order->at(0) == 0 &&
                  top_order->at(1) == 2 && top_order->at(2) == 1 &&
                  top_order->at(3) == 3)) {
      return failure();
    }
    if (false == (bottom_order->size() == 4 && bottom_order->at(0) == 0 &&
                  bottom_order->at(1) == 2 && bottom_order->at(2) == 1 &&
                  bottom_order->at(3) == 3)) {
      return failure();
    }
    // Define Param
    auto ori_shape = module::getShape(out);
    // MulConstOp
    if (mul_const_op) {
      module::setShape(mul_const_op->getOperand(0), ori_shape);
      module::setShape(mul_const_op->getResult(0), ori_shape);
    }
    // AddOp
    module::setShape(add_op->getOperand(0), ori_shape);
    module::setShape(add_op->getResult(0), ori_shape);
    // CastOp
    module::setShape(cast_top_op->getOperand(0), ori_shape);
    module::setShape(cast_top_op->getResult(0), ori_shape);
    // SoftmaxOp
    module::setShape(softmax_op->getOperand(0), ori_shape);
    module::setShape(softmax_op->getResult(0), ori_shape);
    // CastOp
    module::setShape(cast_bottom_op->getOperand(0), ori_shape);
    module::setShape(cast_bottom_op->getResult(0), ori_shape);

    // AddOp
    rewriter.setInsertionPoint(add_op);
    auto mask_shape = module::getShape(add_op->getOperand(1));
    auto mask_name = module::getName(add_op->getOperand(1)).str();
    if (mask_shape[1] != 1) {
      return failure();
    }
    auto new_mask_type = RankedTensorType::get(
        {mask_shape[0], mask_shape[2], mask_shape[1], mask_shape[3]},
        module::getElementType(out));
    if (mask_shape[1] == 1 && mask_shape[2] == 1) {
      // nothing to do
    } else {
      auto reshape_op = rewriter.create<tpu::ReshapeOp>(
          NameLoc::get(rewriter.getStringAttr(mask_name + "_reshape")),
          new_mask_type, add_op->getOperand(1));
      add_op->setOperand(1, reshape_op->getResult(0));
    }
    if (mul_const_op) {
      mul_const_op->setOperand(0, permute_op->getOperand(0));
    } else {
      add_op->setOperand(0, permute_op->getOperand(0));
    }
    rewriter.eraseOp(permute_op);

    // PermuteOp
    auto next_op = *op->getResult(0).user_begin();
    next_op->setOperand(0, op->getOperand(0));
    rewriter.eraseOp(op);
    return success();
  }
};

// permute + reshape -> reshape
struct PermuteReshapeFuse : public OpRewritePattern<tpu::PermuteOp> {
  using OpRewritePattern::OpRewritePattern;

  LogicalResult matchAndRewrite(tpu::PermuteOp op,
                                PatternRewriter &rewriter) const override {
    auto in = op.getInput();
    if (in.hasOneUse() == false) {
      return failure();
    }
    if (!op->hasOneUse()) {
      return failure();
    }
    auto reshape_op = dyn_cast<tpu::ReshapeOp>(*op->getResult(0).user_begin());
    if (!reshape_op) {
      return failure();
    }
    auto order = module::getI64Array(op.getOrder());
    if (false ==
        (order->size() == 4 && order->at(0) == 0 && order->at(1) == 2 &&
         order->at(2) == 1 && order->at(3) == 3)) {
      return failure();
    }
    auto input_shape = module::getShape(in);
    if (false == (input_shape[0] == 1 && input_shape[1] == 1)) {
      return failure();
    }
    // ReshapeOp
    module::setShape(reshape_op->getOperand(0), input_shape);
    reshape_op->setOperand(0, op->getOperand(0));
    rewriter.eraseOp(op);
    return success();
  }
};
} // namespace bm1684x

/**
 * A ---------------------------------\
 *                                     => MatMulHidmBatch => ...
 * B -- Reshape2 -- Tile -- Reshape1  /
 *
 * NOTE: This is typical for Group-Query-Attention(GQA) and B is Key or Value
 *
 */
class TileMatMulHdimBatchPattern : public OpRewritePattern<tpu::MatMulOp> {
public:
  using OpRewritePattern::OpRewritePattern;
  LogicalResult matchAndRewrite(tpu::MatMulOp op,
                                PatternRewriter &rewriter) const override {

    auto left = op.getInput();
    auto right = op.getRight();

    auto stype = module::getStorageType(left);
    if (stype.isF32() || stype.isInteger(8)) {
      return failure();
    }
    auto l_is_weight = module::isWeight(left);
    auto r_is_weight = module::isWeight(right);
    if (l_is_weight && r_is_weight) {
      return failure();
    }

    if (!l_is_weight && !r_is_weight) {
      auto r_reshape1_op = dyn_cast<tpu::ReshapeOp>(right.getDefiningOp());
      if (!(r_reshape1_op && r_reshape1_op->hasOneUse())) {
        return failure();
      }
      auto r_reshape1_input = r_reshape1_op.getInput();

      auto tile_op = dyn_cast<tpu::TileOp>(r_reshape1_input.getDefiningOp());
      if (!(tile_op && tile_op->hasOneUse())) {
        return failure();
      }
      auto tile_input = tile_op.getInput();

      auto r_reshape2_op = dyn_cast<tpu::ReshapeOp>(tile_input.getDefiningOp());
      if (!(r_reshape2_op && r_reshape2_op->hasOneUse())) {
        return failure();
      }
      auto r_reshape2_input = r_reshape2_op.getInput();
      auto shape = module::getShape(r_reshape2_input);
      // num_head of Key/Value must be 1 to do broadcast
      if (shape[2] != 1) {
        return failure();
      }
      auto hdim_is_batch = op.getHdimIsBatch();
      if (hdim_is_batch == false) {
        return failure();
      }

      r_reshape1_op.replaceAllUsesWith(r_reshape1_input);
      tile_op.replaceAllUsesWith(tile_input);
      r_reshape2_op.replaceAllUsesWith(r_reshape2_input);

      // op->setAttr("hdim_is_batch", rewriter.getBoolAttr(!hdim_is_batch));
      return success();
    }
    return failure();
  }
};

#if 0
/* for to reduce the data move, mark on the Redundancy SliceOp if match below pattern:
          /--->SliceOp
   reshape---->SliceOp
         \---->SliceOp
      */
class MarkRedundancySlicePattern : public OpRewritePattern<tpu::SliceOp> {
public:
  using OpRewritePattern::OpRewritePattern;
  LogicalResult matchAndRewrite(tpu::SliceOp op,
                                PatternRewriter &rewriter) const override {
    if (!isa<tpu::ReshapeOp>(op.getInput().getDefiningOp())) {
      return failure();
    }

    auto srcOp = op.getInput().getDefiningOp();
    for (Operation *user: srcOp->getUsers()) {
      if (!isa<tpu::SliceOp>(user))
        return failure();
    }
    //indicate don;t codegen later
    op->setAttr("discard", rewriter.getBoolAttr(true));
  }
};
#endif

#if 1
//  split the pattern if batch=1
class MatMulActiveMatMulPattern : public OpRewritePattern<tpu::MatMulOp> {
public:
  using OpRewritePattern::OpRewritePattern;
  LogicalResult matchAndRewrite(tpu::MatMulOp op,
                                PatternRewriter &rewriter) const override {

    auto left0 = op.getInput();
    auto right0 = op.getRight();
    auto stype = module::getStorageType(left0);
    auto mm0_left_shape = module::getShape(left0);
    if (!isa<Float16Type, BFloat16Type>(stype) ||
        !isa<top::WeightOp>(right0.getDefiningOp()) || mm0_left_shape[0] > 1) {
      return failure();
    }

    auto cast0 = dyn_cast<tpu::CastOp>(left0.getDefiningOp());
    if (!cast0) {
      return failure();
    }
    auto active0 = dyn_cast<tpu::ActiveOp>(cast0.getInput().getDefiningOp());
    if (!active0) {
      return failure();
    }
    auto cast1 = dyn_cast<tpu::CastOp>(active0.getInput().getDefiningOp());
    if (!cast1) {
      return failure();
    }
    auto mm1 = dyn_cast<tpu::MatMulOp>(cast1.getInput().getDefiningOp());
    if (!mm1) {
      return failure();
    }
    auto left1 = mm1.getInput();
    auto right1 = mm1.getRight();
    if (!isa<top::WeightOp>(right1.getDefiningOp())) {
      return failure();
    }
    if (!left1.hasOneUse()) {
      return failure();
    }

    // split the pattern
    std::vector<Value> operands;
    for (int i = 0; i < 2; ++i) {
      auto cur_out = left1;
      Operation *next_op = mm1.getOperation();
      auto suffix = std::to_string(i);
      next_op = tpu::cloneColParallelMatMul(rewriter, next_op, cur_out, 2, i);
      next_op = tpu::cloneCommonOp(rewriter, next_op, cur_out, suffix);
      next_op = tpu::cloneRowParallelMatMul(rewriter, next_op, cur_out, 2, i);
      operands.push_back(cur_out);
    }

    rewriter.setInsertionPointAfterValue(operands[0]);
    std::string suffix = std::string("add_");
    auto loc = module::getLocLike(operands[1], suffix);
    auto add = rewriter.create<tpu::AddOp>(
        loc, operands[0].getType(), mlir::ValueRange{operands[0], operands[1]});
    op.getOutput().replaceAllUsesWith(add.getOutput());
    return success();
  }
};
#endif

namespace tpu {
using namespace bm1684x;
void populateOptimizeBM1684XPatterns(RewritePatternSet *patterns) {
  auto ctx = patterns->getContext();
  patterns->add<LargePadConvPattern>(ctx, 9);
  patterns
      ->add<MatMulHdimBatchPattern, MatMulRemoveReshapePattern,
            MatMulLeftReusePattern, GroupConv2NormalConv, MovePermuteAfterAdd,
            MoveReshapeAfterAdd, TpuReshapeReorderPattern,
            PermuteAddWeightReorderPattern, MaskedFillPermuteMove, PermuteFuse,
            PermuteFuseAddSoftmax, patterns::FuseRepeatPattern<tpu::ReshapeOp>,
            PermuteReshapeFuse, GatherElementsPattern, ScatterElementsPattern,
            PermuteReorderPattern, PermutePadSwap, MatMulActiveMatMulPattern>(
          ctx, 8);
  patterns->add<TileMatMulHdimBatchPattern>(ctx, 7);
  patterns->add<SplitQuantizedMLPPattern, SplitMixedQuantizedMLPPattern>(ctx);
}
} // namespace tpu

} // namespace tpu_mlir
