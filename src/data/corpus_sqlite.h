#pragma once

#include <fstream>
#include <iostream>
#include <random>

#include <boost/algorithm/string.hpp>
#include <boost/iterator/iterator_facade.hpp>

#include "common/config.h"
#include "common/definitions.h"
#include "common/file_stream.h"
#include "data/alignment.h"
#include "data/batch.h"
#include "data/corpus_base.h"
#include "data/dataset.h"
#include "data/vocab.h"

#include <SQLiteCpp/SQLiteCpp.h>
#include <SQLiteCpp/sqlite3/sqlite3.h>

static void SQLiteRandomSeed(sqlite3_context* context,
                             int argc,
                             sqlite3_value** argv) {
    const int result = std::rand();
    sqlite3_result_int(context, result);
}

namespace marian {
namespace data {

class CorpusSQLite : public CorpusBase {
private:
  UPtr<SQLite::Database> db_;
  UPtr<SQLite::Statement> select_;

  void fillSQLite();

public:
  CorpusSQLite(Ptr<Config> options, bool translate = false);

  CorpusSQLite(std::vector<std::string> paths,
               std::vector<Ptr<Vocab>> vocabs,
               Ptr<Config> options);

  sample next();

  void shuffle();

  void reset();

  iterator begin() { return iterator(this); }

  iterator end() { return iterator(); }

  std::vector<Ptr<Vocab>>& getVocabs() { return vocabs_; }

  batch_ptr toBatch(const std::vector<sample>& batchVector) {
    int batchSize = batchVector.size();

    std::vector<size_t> sentenceIds;

    std::vector<int> maxDims;
    for(auto& ex : batchVector) {
      if(maxDims.size() < ex.size())
        maxDims.resize(ex.size(), 0);
      for(size_t i = 0; i < ex.size(); ++i) {
        if(ex[i].size() > (size_t)maxDims[i])
          maxDims[i] = ex[i].size();
      }
      sentenceIds.push_back(ex.getId());
    }

    std::vector<Ptr<SubBatch>> subBatches;
    for(auto m : maxDims) {
      subBatches.emplace_back(New<SubBatch>(batchSize, m));
    }

    std::vector<size_t> words(maxDims.size(), 0);
    for(int i = 0; i < batchSize; ++i) {
      for(int j = 0; j < maxDims.size(); ++j) {
        for(int k = 0; k < batchVector[i][j].size(); ++k) {
          subBatches[j]->indices()[k * batchSize + i] = batchVector[i][j][k];
          subBatches[j]->mask()[k * batchSize + i] = 1.f;
          words[j]++;
        }
      }
    }

    for(size_t j = 0; j < maxDims.size(); ++j)
      subBatches[j]->setWords(words[j]);

    auto batch = batch_ptr(new batch_type(subBatches));
    batch->setSentenceIds(sentenceIds);

    if(options_->has("guided-alignment"))
      addAlignmentsToBatch(batch, batchVector);
    if(options_->has("data-weighting"))
      addWeightsToBatch(batch, batchVector);

    return batch;
  }

private:
  void createRandomFunction() {
    std::srand(Config::seed);
    sqlite3_create_function(db_->getHandle(),
                            "random_seed",
                            0,
                            SQLITE_UTF8,
                            NULL,
                            &SQLiteRandomSeed,
                            NULL,
                            NULL);
  }
};
}
}
