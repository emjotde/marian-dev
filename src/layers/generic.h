#pragma once

#include "marian.h"

#include "layers/factory.h"

namespace marian {
namespace mlp {
enum struct act : int { linear, tanh, logit, ReLU, LeakyReLU, PReLU, swish };
}
}

YAML_REGISTER_TYPE(marian::mlp::act, int)

namespace marian {
namespace mlp {

class Layer {
protected:
  Ptr<ExpressionGraph> graph_;
  Ptr<Options> options_;

public:
  Layer(Ptr<ExpressionGraph> graph, Ptr<Options> options)
      : graph_(graph), options_(options) {}

  template <typename T>
  T opt(const std::string key) {
    return options_->get<T>(key);
  }

  template <typename T>
  T opt(const std::string key, T defaultValue) {
    return options_->get<T>(key, defaultValue);
  }

  virtual Expr apply(const std::vector<Expr>&) = 0;
  virtual Expr apply(Expr) = 0;
};

class Dense : public Layer {
private:
  std::vector<Expr> params_;
  std::map<std::string, Expr> tiedParams_;

public:
  Dense(Ptr<ExpressionGraph> graph, Ptr<Options> options)
      : Layer(graph, options) {}

  void tie_transposed(const std::string& param, const std::string& tied) {
    tiedParams_[param] = graph_->get(tied);
  }

  Expr apply(const std::vector<Expr>& inputs) {
    ABORT_IF(inputs.empty(), "No inputs");

    if(inputs.size() == 1)
      return apply(inputs[0]);

    auto name = opt<std::string>("prefix");
    auto dim = opt<int>("dim");

    auto layerNorm = opt<bool>("layer-normalization", false);
    auto nematusNorm = opt<bool>("nematus-normalization", false);
    auto activation = (act)opt<int>("activation", (int)act::linear);

    auto g = graph_;

    params_ = {};
    std::vector<Expr> outputs;
    size_t i = 0;
    for(auto&& in : inputs) {
      Expr W;
      bool transposeW = false;
      std::string nameW = "W" + std::to_string(i);
      if(tiedParams_.count(nameW)) {
        W = tiedParams_[nameW];
        transposeW = true;
      } else {
        W = g->param(
            name + "_" + nameW, {in->shape()[-1], dim}, inits::glorot_uniform);
      }

      Expr b;
      std::string nameB = "b" + std::to_string(i);
      if(tiedParams_.count(nameB))
        b = tiedParams_[nameB];
      else
        b = g->param(name + "_" + nameB, {1, dim}, inits::zeros);

      params_.push_back(W);
      params_.push_back(b);

      if(layerNorm) {
        if(nematusNorm) {
          auto ln_s = g->param(name + "_ln_s" + std::to_string(i),
                               {1, dim},
                               inits::from_value(1.f));
          auto ln_b = g->param(
              name + "_ln_b" + std::to_string(i), {1, dim}, inits::zeros);

          outputs.push_back(layer_norm(
              affine(in, W, b, false, transposeW), ln_s, ln_b, NEMATUS_LN_EPS));
        } else {
          auto gamma = g->param(name + "_gamma" + std::to_string(i),
                                {1, dim},
                                inits::from_value(1.0));

          params_.push_back(gamma);
          outputs.push_back(
              layer_norm(dot(in, W, false, transposeW), gamma, b));
        }

      } else {
        outputs.push_back(affine(in, W, b, false, transposeW));
      }
      i++;
    }

    switch(activation) {
      case act::linear: return plus(outputs);
      case act::tanh: return tanh(outputs);
      case act::logit: return logit(outputs);
      case act::ReLU: return relu(outputs);
      case act::LeakyReLU: return leakyrelu(outputs);
      case act::PReLU: return prelu(outputs);
      case act::swish: return swish(outputs);
      default: return plus(outputs);
    }
  };

  Expr apply(Expr input) {
    auto g = graph_;

    auto name = options_->get<std::string>("prefix");
    auto dim = options_->get<int>("dim");

    auto layerNorm = options_->get<bool>("layer-normalization", false);
    auto nematusNorm = opt<bool>("nematus-normalization", false);
    auto activation = (act)options_->get<int>("activation", (int)act::linear);

    Expr W;
    bool transposeW = false;
    std::string nameW = "W";
    if(tiedParams_.count(nameW)) {
      transposeW = true;
      W = tiedParams_[nameW];
    } else {
      W = g->param(
          name + "_" + nameW, {input->shape()[-1], dim}, inits::glorot_uniform);
    }
    Expr b;
    std::string nameB = "b";
    if(tiedParams_.count(nameB))
      b = tiedParams_[nameB];
    else
      b = g->param(name + "_" + nameB, {1, dim}, inits::zeros);

    params_ = {W, b};

    Expr out;
    if(layerNorm) {
      if(nematusNorm) {
        auto ln_s = g->param(name + "_ln_s", {1, dim}, inits::from_value(1.f));
        auto ln_b = g->param(name + "_ln_b", {1, dim}, inits::zeros);

        out = layer_norm(
            affine(input, W, b, false, transposeW), ln_s, ln_b, NEMATUS_LN_EPS);
      } else {
        auto gamma
            = g->param(name + "_gamma", {1, dim}, inits::from_value(1.0));

        params_.push_back(gamma);
        out = layer_norm(dot(input, W, false, transposeW), gamma, b);
      }
    } else {
      out = affine(input, W, b, false, transposeW);
    }

    switch(activation) {
      case act::linear: return out;
      case act::tanh: return tanh(out);
      case act::logit: return logit(out);
      case act::ReLU: return relu(out);
      case act::LeakyReLU: return leakyrelu(out);
      case act::PReLU: return prelu(out);
      case act::swish: return swish(out);
      default: return out;
    }
  }
};

}  // namespace mlp

struct EmbeddingFactory : public Factory {
  EmbeddingFactory(Ptr<ExpressionGraph> graph) : Factory(graph) {}

  Expr construct() {
    std::string name = opt<std::string>("prefix");
    int dimVoc = opt<int>("dimVocab");
    int dimEmb = opt<int>("dimEmb");

    bool fixed = opt<bool>("fixed", false);

    NodeInitializer initFunc = inits::glorot_uniform;
    if(options_->has("embFile")) {
      std::string file = opt<std::string>("embFile");
      if(!file.empty()) {
        bool norm = opt<bool>("normalization", false);
        initFunc = inits::from_word2vec(file, dimVoc, dimEmb, norm);
      }
    }

    return graph_->param(name, {dimVoc, dimEmb}, initFunc, fixed);
  }
};

typedef Accumulator<EmbeddingFactory> embedding;

static inline Expr Cost(Expr logits,
                        Expr indices,
                        Expr mask,
                        std::string costType = "cross-entropy",
                        float smoothing = 0,
                        Expr weights = nullptr) {
  using namespace keywords;

  auto ce = cross_entropy(logits, indices);

  if(weights)
    ce = weights * ce;

  if(smoothing > 0) {
    // @TODO: add this to CE kernels instead
    auto ceq = mean(logsoftmax(logits), axis = -1);
    ce = (1 - smoothing) * ce - smoothing * ceq;
  }

  if(mask)
    ce = ce * mask;

  auto costSum = sum(ce, axis = -3);

  Expr cost;
  // axes:
  //  - time axis (words): -3
  //  - batch axis (sentences): -2
  if(costType == "ce-mean"
     || costType
            == "cross-entropy") {  // sum over words; average over sentences
    cost = mean(costSum, axis = -2);
  } else if(costType == "ce-mean-words") {  // average over target tokens
    cost = sum(costSum, axis = -2) / sum(sum(mask, axis = -3), axis = -2);
  } else if(costType == "ce-sum") {  // sum over target tokens
    cost = sum(costSum, axis = -2);
  } else if(costType == "perplexity") {  // ==exp('ce-mean-words')
    cost = exp(sum(costSum, axis = -2) / sum(sum(mask, axis = -3), axis = -2));
  } else if(costType == "ce-rescore") {  // sum over words, keep batch axis
    cost = -costSum;
  } else {  // same as ce-mean
    cost = mean(costSum, axis = -2);
  }

  return cost;
}
}
