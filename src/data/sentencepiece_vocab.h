#pragma once

#ifdef USE_SENTENCEPIECE

#include "data/vocab_impl.h"

#include "sentencepiece/src/sentencepiece_processor.h"

#include "3rd_party/exception.h"
#include "3rd_party/yaml-cpp/yaml.h"
#include "common/logging.h"
#include "common/regex.h"
#include "common/utils.h"
#include "common/filesystem.h"

#include <algorithm>
#include <iostream>

namespace marian {

class SentencePieceVocab : public VocabImpl {
private:
  UPtr<sentencepiece::SentencePieceProcessor> spm_;
  float alpha_{0};

public:
  static Ptr<VocabImpl> tryToLoad(const std::string& /*vocabPath*/) {
    return nullptr;
  }

  virtual int loadOrCreate(const std::string& vocabPath,
                           const std::string& textPath,
                           int max = 0) override;

  virtual int load(const std::string& vocabPath, int max = 0) override;

  virtual Word operator[](const std::string& word) const override;

  virtual const std::string& operator[](Word id) const override;

  virtual Words encode(const std::string& line,
                       bool addEOS = true,
                       bool inference = false) const;

  virtual std::string decode(const Words& sentence,
                             bool ignoreEOS = true) const;

  virtual size_t size() const;

  virtual Word getEosId() const override { return (Word)spm_->eos_id(); }
  virtual Word getUnkId() const override { return (Word)spm_->unk_id(); }

  void create(const std::string& /*vocabPath*/, const std::string& /*trainPath*/) {
    ABORT("[data] Training of SentencePieceVocabulary not supported yet");
  }

  void create(io::InputFileStream& /*trainStrm*/,
              io::OutputFileStream& /*vocabStrm*/,
              size_t /*maxSize*/) {
    ABORT("[data] Training of SentencePieceVocabulary not supported yet");
  }

  void createFake() {
    ABORT("[data] Fake SentencePieceVocabulary not supported");
  }
};

Word SentencePieceVocab::operator[](const std::string& token) const {
  return (Word)spm_->PieceToId(token);
}

const std::string& SentencePieceVocab::operator[](Word id) const {
  ABORT_IF(id >= size(), "Unknown word id: ", id);
  return spm_->IdToPiece(id);
}

Words SentencePieceVocab::encode(const std::string& line, bool addEOS, bool inference) const {
  std::vector<int> spmIds;
  if(inference || alpha_ == 0)
    spm_->Encode(line, &spmIds);
  else
    spm_->SampleEncode(line, -1, alpha_, &spmIds);

  Words words(spmIds.begin(), spmIds.end());

  if(addEOS)
    words.push_back(getEosId());
  return words;
}

std::string SentencePieceVocab::decode(const Words& sentence, bool ignoreEOS) const {
  std::string line;
  // convert vector of Word to vector of int
  std::vector<int> spmSentence(sentence.begin(), sentence.end());
  spm_->Decode(spmSentence, &line);
  return line;
}

size_t SentencePieceVocab::size() const {
  return spm_->GetPieceSize();
}

int SentencePieceVocab::loadOrCreate(const std::string& vocabPath,
                                     const std::string& trainPath,
                                     int max) {
  if(vocabPath.empty()) {
    if(filesystem::exists(trainPath + ".spm")) {
      return load(trainPath + ".spm", max);
    }

    // @TODO: make this work, currently it will abort on purpose
    create(trainPath + ".spm", trainPath);
    return load(trainPath + ".spm", max);
  } else {
    if(!filesystem::exists(vocabPath))
      // @TODO: make this work, currently it will abort on purpose
      create(vocabPath, trainPath);
    return load(vocabPath, max);
  }
}

int SentencePieceVocab::load(const std::string& vocabPath, int /*max*/) {
  LOG(info, "[data] Loading SentencePiece vocabulary from file {}", vocabPath);

  ABORT_IF(!filesystem::exists(vocabPath),
           "SentencePiece vocabulary file {} does not exits",
           vocabPath);

  spm_.reset(new sentencepiece::SentencePieceProcessor());
  const auto status = spm_->Load(vocabPath);

  ABORT_IF(!status.ok(),
           "SentencePiece error: {}",
           status.ToString());

  return spm_->GetPieceSize();
}

}

#endif
