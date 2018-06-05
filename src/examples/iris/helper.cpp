#include <algorithm>
#include <fstream>
#include <iostream>
#include <map>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "common/config.h"

// Constants for Iris example
const int NUM_FEATURES = 4;
const int NUM_LABELS = 3;

void readIrisData(const std::string fileName,
                  std::vector<float>& features,
                  std::vector<float>& labels) {
  std::map<std::string, int> CLASSES
      = {{"Iris-setosa", 0}, {"Iris-versicolor", 1}, {"Iris-virginica", 2}};

  std::ifstream in(fileName);
  if(!in.is_open()) {
    std::cerr << "Iris dataset not found: " << fileName << std::endl;
  }
  std::string line;
  std::string value;
  while(GetLine(in, line)) {
    std::stringstream ss(line);
    int i = 0;
    while(GetLine(ss, value, ',')) {
      if(++i == 5)
        labels.emplace_back(CLASSES[value]);
      else
        features.emplace_back(std::stof(value));
    }
  }
}

void shuffleData(std::vector<float>& features, std::vector<float>& labels) {
  // Create a list of indeces 0...K
  std::vector<int> indeces;
  indeces.reserve(labels.size());
  for(int i = 0; i < labels.size(); ++i)
    indeces.push_back(i);

  // Shuffle indeces
  static std::mt19937 urng(marian::Config::seed);
  std::shuffle(indeces.begin(), indeces.end(), urng);

  std::vector<float> featuresTemp;
  featuresTemp.reserve(features.size());
  std::vector<float> labelsTemp;
  labelsTemp.reserve(labels.size());

  // Get shuffled features and labels
  for(auto i = 0; i < indeces.size(); ++i) {
    auto idx = indeces[i];
    labelsTemp.push_back(labels[idx]);
    featuresTemp.insert(featuresTemp.end(),
                        features.begin() + (idx * NUM_FEATURES),
                        features.begin() + ((idx + 1) * NUM_FEATURES));
  }

  features = featuresTemp;
  labels = labelsTemp;
}

float calculateAccuracy(const std::vector<float> probs,
                        const std::vector<float> labels) {
  size_t numCorrect = 0;
  for(size_t i = 0; i < probs.size(); i += NUM_LABELS) {
    auto pred = std::distance(
        probs.begin() + i,
        std::max_element(probs.begin() + i, probs.begin() + i + NUM_LABELS));
    if(pred == labels[i / NUM_LABELS])
      ++numCorrect;
  }
  return numCorrect / float(labels.size());
}
