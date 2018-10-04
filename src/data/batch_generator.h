#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <queue>

#include "common/config.h"
#include "data/batch_stats.h"
#include "data/rng_engine.h"
#include "training/training_state.h"

namespace marian {
namespace data {

template <class DataSet>
class BatchGenerator : public RNGEngine {
public:
  typedef typename DataSet::batch_ptr BatchPtr;

  typedef typename DataSet::sample sample;
  typedef std::vector<sample> samples;

protected:
  Ptr<DataSet> data_;
  Ptr<Config> options_;
  bool restored_{false};
  bool shuffle_;

private:
  Ptr<BatchStats> stats_;

  int batchSize_{1};

  typename DataSet::iterator current_;
  bool newlyPrepared_{true};

  size_t maxiBatchSize_;
  std::deque<BatchPtr> bufferedBatches_;
  BatchPtr currentBatch_;

  mutable std::mutex loadMutex_;
  mutable std::condition_variable loadCondition_;
  bool loadingSamples_{false};
  bool hadData_{false};

  void fillBatches(bool shuffle = true) {
    typedef typename sample::value_type Item;
    auto itemCmp
        = [](const Item& sa, const Item& sb) { return sa.size() < sb.size(); };

    auto cmpSrc = [itemCmp](const sample& a, const sample& b) {
      return std::lexicographical_compare(
          a.begin(), a.end(), b.begin(), b.end(), itemCmp);
    };

    auto cmpTrg = [itemCmp](const sample& a, const sample& b) {
      return std::lexicographical_compare(
          a.rbegin(), a.rend(), b.rbegin(), b.rend(), itemCmp);
    };

    auto cmpNone = [](const sample& a, const sample& b) { return &a < &b; };

    typedef std::function<bool(const sample&, const sample&)> cmp_type;
    typedef std::priority_queue<sample, samples, cmp_type> sample_queue;

    std::unique_ptr<sample_queue> maxiBatch;

    if(options_->has("maxi-batch-sort")) {
      if(options_->get<std::string>("maxi-batch-sort") == "src")
        maxiBatch.reset(new sample_queue(cmpSrc));
      else if(options_->get<std::string>("maxi-batch-sort") == "none")
        maxiBatch.reset(new sample_queue(cmpNone));
      else
        maxiBatch.reset(new sample_queue(cmpTrg));
    } else {
      maxiBatch.reset(new sample_queue(cmpNone));
    }

    size_t maxBatchSize = options_->get<int>("mini-batch");
    size_t maxSize = maxBatchSize * options_->get<int>("maxi-batch");

    // LOG(info, "Preloading batches");

    // consume data from corpus into maxi-batch (single sentences)
    // sorted into specified order (due to queue)
    if(newlyPrepared_) {
      current_ = data_->begin();
      newlyPrepared_ = false;
    } else {
      if(current_ != data_->end())
        ++current_;
    }
    size_t sets = 0;
    while(current_ != data_->end() && maxiBatch->size() < maxSize) {
      maxiBatch->push(*current_);
      
      // if(maxiBatch->size() % 100000 == 0)
      //   LOG(info, "Preloaded samples: {}", maxiBatch->size());

      sets = current_->size();
      // do not consume more than required for the maxi batch as this causes
      // that line-by-line translation is delayed by one sentence
      bool last = maxiBatch->size() == maxSize;
      if(!last)
        ++current_;
    }

    // LOG(info, "Turning samples into batches");

    samples batchVector;
    int currentWords = 0;
    std::vector<size_t> lengths(sets, 0);

    std::vector<BatchPtr> tempBatches;

    // while there are sentences in the queue
    while(!maxiBatch->empty()) {
      // push item onto batch
      batchVector.push_back(maxiBatch->top());
      currentWords += (int)batchVector.back()[0].size();
      maxiBatch->pop();

      // Batch size based on sentences
      bool makeBatch = batchVector.size() == maxBatchSize;

      // Batch size based on words
      if(options_->has("mini-batch-words")) {
        int mbWords = options_->get<int>("mini-batch-words");
        if(mbWords > 0)
          makeBatch = currentWords > mbWords;
      }

      if(options_->has("mini-batch-fit")) {
        // Dynamic batching
        if(stats_) {
          for(size_t i = 0; i < sets; ++i)
            if(batchVector.back()[i].size() > lengths[i])
              lengths[i] = batchVector.back()[i].size();

          maxBatchSize = stats_->getBatchSize(lengths);

          if(batchVector.size() > maxBatchSize) {
            maxiBatch->push(batchVector.back());
            batchVector.pop_back();
            makeBatch = true;
          } else {
            makeBatch = batchVector.size() == maxBatchSize;
          }
        }
      }

      // if batch has desired size create a real batch
      if(makeBatch) {
        tempBatches.push_back(data_->toBatch(batchVector));

        // prepare for next batch
        batchVector.clear();
        currentWords = 0;
        lengths.clear();
        lengths.resize(sets, 0);
      }
    }

    // turn rest into batch
    if(!batchVector.empty())
      tempBatches.push_back(data_->toBatch(batchVector));

    if(shuffle) {
      // shuffle the batches
      std::shuffle(tempBatches.begin(), tempBatches.end(), eng_);
    }

    // Wait here until batch buffer is empty;   
    // LOG(info, "Waiting for buffer to be empty");
    std::unique_lock<std::mutex> lock(loadMutex_);
    loadCondition_.wait(
        lock, [this] { return bufferedBatches_.empty(); });
  
    // put batches onto queue
    // exclusive lock
    // LOG(info, "Dumping batches to buffer");
    for(const auto& batch : tempBatches)
      bufferedBatches_.push_back(batch);
    // LOG(info, "Done dumping batches");
    
    loadingSamples_ = false;
    hadData_ = tempBatches.size() > 0;

    // Buffer is full now, everyone else can carry on
    loadCondition_.notify_all();
  }

public:
  BatchGenerator(Ptr<DataSet> data,
                 Ptr<Config> options,
                 Ptr<BatchStats> stats = nullptr)
      : data_(data), options_(options), stats_(stats) {}

  BatchPtr next() {
    // Start preloading batches and inform that loading is happening.
    // Detach the loading process so it's not blocking batch processing.
    {
      std::unique_lock<std::mutex> lock(loadMutex_);
      if(!loadingSamples_ && hadData_) {
        loadingSamples_ = true;
        std::thread([this]() { 
          fillBatches(shuffle_); 
        }).detach();
      }
    }
    
    // If there are no batches, but loading is happening,
    // wait for loading to finish. 
    {
      std::unique_lock<std::mutex> lock(loadMutex_);
      loadCondition_.wait(lock, [this] { 
        return !(loadingSamples_ && bufferedBatches_.empty()); 
      });
    }

    std::unique_lock<std::mutex> lock(loadMutex_);
    // Try to consume a batch
    if(bufferedBatches_.empty()) {
      // There was no batch in the buffer despite preloading -> end of epoch
      return nullptr;
    }
    else {
      auto batch = bufferedBatches_.front();
      bufferedBatches_.pop_front();

      // if(bufferedBatches_.size() % 100 == 0)
      //   LOG(info, "Buffered batches left {}", bufferedBatches_.size());

      // If there are no batches left, notify everyone who's waiting.
      // This will either dump buffered batches on the current queue,
      // or switch to the next epoch in the next call.
      if(bufferedBatches_.empty()) {
        // LOG(info, "Empty batches, notifying");
        loadCondition_.notify_all();
      }

      return batch;
    }
  }

  std::vector<BatchPtr> nextN(size_t num) {
    std::vector<BatchPtr> batches;
    for(int i = 0; i < num && *this; ++i)
      batches.push_back(next());
    return batches;
  }

  void prepare(bool shuffle = true) {
    if(shuffle)
      data_->shuffle();
    else
      data_->reset();
    newlyPrepared_ = true;
    
    // @TODO: solve this better, maybe use options
    shuffle_ = shuffle;

    fillBatches(shuffle);
  }

  bool restore(Ptr<TrainingState> state, bool shuffle) {
    if(state->epochs == 1 && state->batchesEpoch == 0)
      return false;

    LOG(info,
        "[data] Restoring the corpus state to epoch {}, batch {}",
        state->epochs,
        state->batches);

    if(state->epochs > 1) {
      data_->restore(state);
      setRNGState(state->seedBatch);
    }

    prepare(shuffle);
    for(size_t i = 0; i < state->batchesEpoch; ++i)
      next();

    return true;
  }
};

class CorpusBatchGenerator : public BatchGenerator<CorpusBase>,
                             public TrainingObserver {
public:
  CorpusBatchGenerator(Ptr<CorpusBase> data,
                       Ptr<Config> options,
                       Ptr<BatchStats> stats = nullptr)
      : BatchGenerator(data, options, stats) {}

  void actAfterEpoch(TrainingState& state) override {
    state.seedBatch = getRNGState();
    state.seedCorpus = data_->getRNGState();
  }
};
}  // namespace data
}  // namespace marian
