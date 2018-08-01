#include "rnn/cells.h"

#include "graph/node_operators_binary.h"
#include "tensors/tensor_operators.h"

namespace marian {
namespace rnn {

struct GRUFastNodeOp : public NaryNodeOp {
  bool final_;

  GRUFastNodeOp(const std::vector<Expr>& nodes, bool final)
      : NaryNodeOp(nodes), final_(final) {}

  NodeOps forwardOps() {
    std::vector<Tensor> inputs;
    for(size_t i = 0; i < children_.size(); ++i)
      inputs.push_back(child(i)->val());

    return {NodeOp(GRUFastForward(val_, inputs, final_))};
  }

  NodeOps backwardOps() {
    std::vector<Tensor> inputs;
    std::vector<Tensor> outputs;
    for(auto child : children_) {
      inputs.push_back(child->val());
      if(child->trainable())
        outputs.push_back(child->grad());
      else
        outputs.push_back(nullptr);
    }

    return {NodeOp(GRUFastBackward(outputs, inputs, adj_, final_))};
  }

  // do not check if node is trainable
  virtual void runBackward(const NodeOps& ops) {
    for(auto&& op : ops)
      op();
  }

  const std::string type() { return "GRU-ops"; }

  const std::string color() { return "yellow"; }
};

Expr gruOps(const std::vector<Expr>& nodes, bool final) {
  return Expression<GRUFastNodeOp>(nodes, final);
}

/******************************************************************************/

struct LSTMCellNodeOp : public NaryNodeOp {
  LSTMCellNodeOp(const std::vector<Expr>& nodes) : NaryNodeOp(nodes) {}

  NodeOps forwardOps() {
    std::vector<Tensor> inputs;
    for(size_t i = 0; i < children_.size(); ++i)
      inputs.push_back(child(i)->val());

    return {NodeOp(LSTMCellForward(val_, inputs))};
  }

  NodeOps backwardOps() {
    std::vector<Tensor> inputs;
    std::vector<Tensor> outputs;
    for(auto child : children_) {
      inputs.push_back(child->val());
      if(child->trainable())
        outputs.push_back(child->grad());
      else
        outputs.push_back(nullptr);
    }

    return {NodeOp(LSTMCellBackward(outputs, inputs, adj_))};
  }

  // do not check if node is trainable
  virtual void runBackward(const NodeOps& ops) {
    for(auto&& op : ops)
      op();
  }

  const std::string type() { return "LSTM-cell-ops"; }

  const std::string color() { return "yellow"; }
};

struct LSTMOutputNodeOp : public NaryNodeOp {
  LSTMOutputNodeOp(const std::vector<Expr>& nodes) : NaryNodeOp(nodes) {}

  NodeOps forwardOps() {
    std::vector<Tensor> inputs;
    for(size_t i = 0; i < children_.size(); ++i)
      inputs.push_back(child(i)->val());

    return {NodeOp(LSTMOutputForward(val_, inputs))};
  }

  NodeOps backwardOps() {
    std::vector<Tensor> inputs;
    std::vector<Tensor> outputs;
    for(auto child : children_) {
      inputs.push_back(child->val());
      if(child->trainable())
        outputs.push_back(child->grad());
      else
        outputs.push_back(nullptr);
    }

    return {NodeOp(LSTMOutputBackward(outputs, inputs, adj_))};
  }

  // do not check if node is trainable
  virtual void runBackward(const NodeOps& ops) {
    for(auto&& op : ops)
      op();
  }

  const std::string type() { return "LSTM-output-ops"; }

  const std::string color() { return "yellow"; }
};

Expr lstmOpsC(const std::vector<Expr>& nodes) {
  return Expression<LSTMCellNodeOp>(nodes);
}

Expr lstmOpsO(const std::vector<Expr>& nodes) {
  return Expression<LSTMOutputNodeOp>(nodes);
}
}  // namespace rnn
}  // namespace marian
