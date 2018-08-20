#pragma once

#include <boost/program_options.hpp>

#include "3rd_party/yaml-cpp/yaml.h"
#include "common/definitions.h"
#include "common/file_stream.h"
#include "common/logging.h"

// TODO: why are these needed by a config parser? Can they be removed for Linux
// as well?
#ifndef _WIN32
#include <sys/ioctl.h>
#include <unistd.h>
#endif

namespace marian {

enum struct ConfigMode {
  training,
  translating,
  rescoring,
};

// try to determine the width of the terminal
uint16_t guess_terminal_width(uint16_t max_width = 180);

class ConfigParser {
public:
  ConfigParser(int argc, char** argv, ConfigMode mode, bool validate = false)
      : mode_(mode),
        cmdline_options_("Allowed options", guess_terminal_width()) {
    parseOptions(argc, argv, validate);
  }

  void parseOptions(int argc, char** argv, bool validate);

  YAML::Node getConfig() const;
  std::vector<DeviceId> getDevices();

private:
  ConfigMode mode_;
  boost::program_options::options_description cmdline_options_;
  YAML::Node config_;

  bool has(const std::string& key) const;

  template <typename T>
  T get(const std::string& key) const {
    return config_[key].as<T>();
  }

  void addOptionsCommon(boost::program_options::options_description&);
  void addOptionsModel(boost::program_options::options_description&);
  void addOptionsTraining(boost::program_options::options_description&);
  void addOptionsRescore(boost::program_options::options_description&);
  void addOptionsValid(boost::program_options::options_description&);
  void addOptionsTranslate(boost::program_options::options_description&);

  void validateOptions() const;
  void validateDevices() const;
};
}  // namespace marian
