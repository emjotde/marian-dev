#pragma once

#include <sstream>
#include <string>
#include "common/definitions.h"

#include "3rd_party/yaml-cpp/yaml.h"

#define YAML_REGISTER_TYPE(registered, type)                \
  namespace YAML {                                          \
  template <>                                               \
  struct convert<registered> {                              \
    static Node encode(const registered& rhs) {             \
      type value = static_cast<type>(rhs);                  \
      return Node(value);                                   \
    }                                                       \
    static bool decode(const Node& node, registered& rhs) { \
      type value = node.as<type>();                         \
      rhs = static_cast<registered>(value);                 \
      return true;                                          \
    }                                                       \
  };                                                        \
  }

namespace marian {

/**
 * Container for options stored as key-value pairs. Keys are unique strings.
 */
class Options {
protected:
  YAML::Node options_;

public:
  Options() {}

  Options clone() const {
    auto options = Options();
    options.options_ = YAML::Clone(options_);
    return options;
  }

  YAML::Node& getYaml() { return options_; }
  const YAML::Node& getYaml() const { return options_; }

  void parse(const std::string& yaml) {
    auto node = YAML::Load(yaml);
    for(auto it : node)
      options_[it.first.as<std::string>()] = YAML::Clone(it.second);
  }

  /**
   * @brief Splice options from a YAML node
   *
   * By default, only options with keys that do not already exist in options_ are extracted from
   * node. These options are cloned if overwirte is true.
   *
   * @param node a YAML node to transfer the options from
   * @param overwrite overwrite all options
   */
  void merge(YAML::Node& node, bool overwrite = false) {
    for(auto it : node)
      if(overwrite || !options_[it.first.as<std::string>()])
        options_[it.first.as<std::string>()] = YAML::Clone(it.second);
  }

  void merge(const YAML::Node& node, bool overwrite = false) { merge(node, overwrite); }
  void merge(Ptr<Options> options) { merge(options->getYaml()); }

  std::string str() {
    std::stringstream ss;
    ss << options_;
    return ss.str();
  }

  template <typename T>
  void set(const std::string& key, T value) {
    options_[key] = value;
  }

  template <typename T>
  T get(const std::string& key) {
    ABORT_IF(!has(key), "Required option '{}' has not been set", key);
    return options_[key].as<T>();
  }

  template <typename T>
  T get(const std::string& key, T defaultValue) {
    if(has(key))
      return options_[key].as<T>();
    else
      return defaultValue;
  }

  bool has(const std::string& key) const { return options_[key]; }
};

}  // namespace marian
