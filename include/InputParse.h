#pragma once

#ifndef INPUT_PARSE_H
#define INPUT_PARSE_H

#include <vector>
#include <string>
#include <algorithm>
#include <map>
//#include "eva_logger.h"

class InputParser {
public:
  InputParser(int& argc, char** argv) {
    for (int i = 1; i < argc; ++i)
      m_tokens.push_back(std::string(argv[i]));
  }
  const std::string& getCmdOption(const std::string& option) const {
    std::vector<std::string>::const_iterator itr;
    itr = std::find(m_tokens.begin(), m_tokens.end(), option);
    if (itr != m_tokens.end() && ++itr != m_tokens.end()) {
      return *itr;
    }
    static const std::string empty_string("");
    return empty_string;
  }
  bool cmdOptionExists(const std::string& option) const {
    return std::find(m_tokens.begin(), m_tokens.end(), option) !=
      m_tokens.end();
  }

private:
  std::vector<std::string> m_tokens;
};

static bool ReadConfigParameter(std::map<std::string, std::string> &kvp,
                         const char* key, std::string &value_str,
                         /*STATSLogger *logger,*/ std::string &func_name) {

  auto param_itr = kvp.find(key);
  if (param_itr == kvp.end()) {

    //logger->Log(EnumLogMsgType::LOG_MSG_TYPE_ERROR, "{0} ERROR - failed to "
    //                        "find {1} in parameters. Ensure [{2}] key is set",
    //            func_name, key, key);
    return false;
  } else
    value_str = param_itr->second;
  return true;
}

#endif
