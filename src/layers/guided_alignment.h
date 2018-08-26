#pragma once

#include "marian.h"

namespace marian {

static inline Expr guidedAlignmentCost(Ptr<ExpressionGraph> graph,
                                       Ptr<data::CorpusBatch> batch,
                                       Ptr<Options> options,
                                       Expr att) {
  using namespace keywords;

  int dimBatch = att->shape()[-2];
  int dimSrc = att->shape()[-3];
  int dimTrg = att->shape()[-1];

  //debug(att, "Attention");
  
  auto aln = graph->constant(att->shape(),
                             inits::from_vector(batch->getGuidedAlignment()));

  //debug(aln, "Alignment");

  std::string guidedCostType
      = options->get<std::string>("guided-alignment-cost");

  Expr alnCost;
  float eps = 1e-6;
  if(guidedCostType == "mse") {
    alnCost = sum(flatten(square(att - aln))) / (2 * dimBatch);
  } else if(guidedCostType == "mult") {
    alnCost = -log(sum(flatten(att * aln)) + eps) / dimBatch;
  } else if(guidedCostType == "ce") {
    alnCost = -sum(flatten(aln * log(att + eps))) / dimBatch;
  } else {
    ABORT("Unknown alignment cost type");
  }

  float guidedScalar = options->get<float>("guided-alignment-weight");
  return guidedScalar * alnCost;
}
}  // namespace marian
