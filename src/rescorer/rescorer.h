#pragma once

#include "marian.h"

#include "common/config.h"
#include "data/batch_generator.h"
#include "data/corpus.h"
#include "data/corpus_nbest.h"
#include "models/model_task.h"
#include "rescorer/score_collector.h"
#include "training/scheduler.h"
#include "training/validator.h"
#include "models/costs.h"

namespace marian {

using namespace data;

class Rescorer {
private:
  Ptr<models::ModelBase> builder_;

public:
  Rescorer(Ptr<Options> options)
      : builder_(models::from_options(options, models::usage::scoring)) {}

  void load(Ptr<ExpressionGraph> graph, const std::string& modelFile) {
    builder_->load(graph, modelFile);
  }

  Expr build(Ptr<ExpressionGraph> graph, Ptr<data::CorpusBatch> batch) {
    return builder_->build(graph, batch);
  }

  std::vector<data::SoftAlignment> getAlignment() {
    auto model = std::static_pointer_cast<models::Scorer>(builder_)->getModel();
    return std::static_pointer_cast<EncoderDecoderBase>(model)->getAlignment();
  }
};

template <class Model>
class Rescore : public ModelTask {
private:
  Ptr<Config> options_;
  Ptr<CorpusBase> corpus_;
  std::vector<Ptr<ExpressionGraph>> graphs_;
  std::vector<Ptr<Model>> models_;

public:
  Rescore(Ptr<Config> options)
      : options_(options),
        corpus_(
            options_->get<bool>("n-best")
                ? std::static_pointer_cast<CorpusBase>(
                      New<CorpusNBest>(options_))
                : std::static_pointer_cast<CorpusBase>(New<Corpus>(options_))) {
    ABORT_IF(options_->has("summary") && options_->get<float>("alignment", .0f),
             "Alignments can not be produced with summarized score");

    corpus_->prepare();

    auto devices = options_->getDevices();

    for(auto device : devices) {
      auto graph = New<ExpressionGraph>(true, options_->get<bool>("optimize"));
      graph->setDevice(device);
      graph->reserveWorkspaceMB(options_->get<size_t>("workspace"));
      graphs_.push_back(graph);
    }

    auto modelFile = options_->get<std::string>("model");

    Ptr<Options> temp = New<Options>();
    temp->merge(options);
    temp->set("inference", true);
    temp->set("cost-type", "ce-rescore");

    models_.resize(graphs_.size());
    ThreadPool pool(graphs_.size(), graphs_.size());
    for(size_t i = 0; i < graphs_.size(); ++i) {
      pool.enqueue(
          [=](int j) {
            models_[j] = New<Model>(temp);
            models_[j]->load(graphs_[j], modelFile);
          },
          i);
    }
  }

  void run() {
    LOG(info, "Scoring");

    auto batchGenerator = New<BatchGenerator<CorpusBase>>(corpus_, options_);
    batchGenerator->prepare(false);

    Ptr<ScoreCollector> output = options_->get<bool>("n-best")
                                     ? std::static_pointer_cast<ScoreCollector>(
                                           New<ScoreCollectorNBest>(options_))
                                     : New<ScoreCollector>();

    bool summarize = options_->has("summary");
    std::string summary
        = summarize ? options_->get<std::string>("summary") : "cross-entropy";

    float sumCost = 0;
    size_t sumWords = 0;
    size_t sumSamples = 0;

    size_t batchId = 0;

    std::mutex smutex;

    {
      ThreadPool pool(graphs_.size(), graphs_.size());

      while(*batchGenerator) {
        auto batch = batchGenerator->next();

        auto task = [=, &sumCost, &sumWords, &sumSamples, &smutex](int id) {
          thread_local Ptr<ExpressionGraph> graph;
          thread_local Ptr<Model> builder;

          if(!graph) {
            graph = graphs_[id % graphs_.size()];
            builder = models_[id % graphs_.size()];
          }

          auto costNode = builder->build(graph, batch);
          graph->forward();

          std::vector<float> scores;
          costNode->val()->get(scores);

          std::vector<std::vector<data::SoftAlignment>> aligns(batch->size());
          if(options_->get<float>("alignment", .0f)) {
            auto flatAligns = builder->getAlignment();

            // TODO: refactorize
            for(size_t b = 0; b < batch->size(); ++b) {
              for(size_t t = 0; t < flatAligns.size(); ++t) {

                size_t t_idx = b + (t * batch->size());
                if(batch->back()->mask()[t_idx] == 0)
                  continue;

                aligns[b].push_back({});
                for(size_t s = b; s < flatAligns[t].size(); s += batch->size()) {

                  size_t s_idx = s;
                  if(batch->front()->mask()[s_idx] == 0)
                    continue;

                  aligns[b][t].emplace_back(flatAligns[t][s]);
                }
              }
            }

            //for(size_t b = 0; b < aligns.size(); ++b) {
              //std::cerr << "ms.f= " << batch->front()->mask().size() << std::endl;
              //for(auto x : batch->front()->mask())
                //std::cerr << x << " ";
              //std::cerr << std::endl;
              //std::cerr << "ms.b= " << batch->back()->mask().size() << std::endl;
              //for(auto x : batch->back()->mask())
                //std::cerr << x << " ";

              //std::cerr << std::endl;
              //for(size_t i = 0; i < aligns[b].size(); ++i) {
                //std::cerr << "m=" << batch->back()->mask()[b + (i * batch->size())] << " ";
                //std::cerr << i << ": ";
                //for(size_t j = 0; j < aligns[b][i].size(); ++j) {
                  //std::cerr << aligns[b][i][j] << " ";
                  //std::cerr << "(" << batch->front()->mask()[b + (j * batch->size())] << ") ";
                //}
                //std::cerr << std::endl;
              //}
            //}
          }

          std::unique_lock<std::mutex> lock(smutex);
          for(auto s : scores)
            sumCost += s;
          sumWords += batch->back()->batchWords();
          sumSamples += batch->size();

          if(!summarize) {
            for(size_t i = 0; i < batch->size(); ++i) {
              output->Write(batch->getSentenceIds()[i], scores[i], aligns[i]);
            }
          }
        };

        pool.enqueue(task, batchId % graphs_.size());
        batchId++;
      }
    }

    if(summarize) {
      float cost = 0;
      if(summary == "perplexity")
        cost = std::exp(-(float)sumCost / (float)sumWords);
      else if(summary == "ce-sum")
        cost = -sumCost;
      else if(summary == "ce-mean-words")
        cost = -(float)sumCost / (float)sumWords;
      else
        cost = -sumCost / sumSamples;

      LOG(info, "Reporting {} summary", summary);
      std::cout << cost << std::endl;
    }
  }
};
}  // namespace marian
