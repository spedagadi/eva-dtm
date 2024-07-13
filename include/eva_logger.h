#pragma once

#ifndef LOGGER_H_
#define LOGGER_H_

#include <sstream>
#include <iomanip>
#include "spdlog/sinks/daily_file_sink.h"
#include "spdlog/sinks/stdout_color_sinks.h"
#include "spdlog/spdlog.h"
#if defined(_WIN32) || (defined(__GNUC__) && (__GNUC__ >= 8))
#include <filesystem>
namespace filesystem = std::filesystem;
#else
#include <experimental/filesystem>
namespace filesystem = std::experimental::filesystem;
#endif

#define MAX_STR_LEN 256

enum class EnumLogMsgType {

  LOG_MSG_TYPE_INFO = 0,
  LOG_MSG_TYPE_ERROR = 1,
  LOG_MSG_TYPE_WARNING = 2,
  LOG_MSG_TYPE_DEBUG = 3,
  LOG_MSG_TYPE_SILENT = 4
};

class STATSLogger {
 private:
  std::shared_ptr<spdlog::logger> m_SpdLogger;
  std::shared_ptr<spdlog::logger> m_ConsoleLogger;
  std::string m_LogFilePath = "";
 public:
  STATSLogger(std::string component, std::string logFilePath,
              std::string cons_type) {
    spdlog::set_pattern("%+");
    spdlog::flush_every(std::chrono::seconds(3));
    m_SpdLogger =
        spdlog::daily_logger_mt(component, logFilePath.c_str(), 2, 30);
    m_ConsoleLogger = spdlog::stdout_color_mt(cons_type);
    m_LogFilePath = logFilePath;
  }
  ~STATSLogger() { spdlog::shutdown(); }

  std::string GetLogFilePath() { return m_LogFilePath; }

  template <typename... Args>
  void Log(EnumLogMsgType logType, const char *format, Args &&...args) {
    switch (logType) {
      case EnumLogMsgType::LOG_MSG_TYPE_INFO:
        info(format, std::forward<Args>(args)...);
        break;

      case EnumLogMsgType::LOG_MSG_TYPE_ERROR:
        error(format, std::forward<Args>(args)...);
        break;

      case EnumLogMsgType::LOG_MSG_TYPE_WARNING:
        warn(format, std::forward<Args>(args)...);
        break;

      case EnumLogMsgType::LOG_MSG_TYPE_DEBUG:
        debug(format, std::forward<Args>(args)...);
        break;

      case EnumLogMsgType::LOG_MSG_TYPE_SILENT:
        silent(format, std::forward<Args>(args)...);
        break;

    }
  }

  template <typename... Args>
  void debug(const char *format, Args &&...args) {
    try {
      m_ConsoleLogger->debug(format, std::forward<Args>(args)...);
      m_SpdLogger->debug(format, std::forward<Args>(args)...);
    } catch (std::exception &) {
    }
  }

  template <typename... Args>
  void info(const char *format, Args &&...args) {
    try {
      m_ConsoleLogger->info(format, std::forward<Args>(args)...);
      m_SpdLogger->info(format, std::forward<Args>(args)...);
    } catch (std::exception &) {
    }
  }

  template <typename... Args>
  void warn(const char *format, Args &&...args) {
    try {
      m_ConsoleLogger->warn(format, std::forward<Args>(args)...);
      m_SpdLogger->warn(format, std::forward<Args>(args)...);
    } catch (std::exception &) {
    }
  }

  template <typename... Args>
  void error(const char *format, Args &&...args) {
    try {
      m_ConsoleLogger->error(format, std::forward<Args>(args)...);
      m_SpdLogger->error(format, std::forward<Args>(args)...);
    } catch (std::exception &) {
    }
  }

  template <typename... Args>
  void silent(const char *format, Args &&...args) {
    try {
      //m_ConsoleLogger->error(format, std::forward<Args>(args)...);
      m_SpdLogger->info(format, std::forward<Args>(args)...);
    } catch (std::exception &) {
    }
  }
};

#if defined(_WIN32)
static std::string ExePath() {
  char buffer[MAX_PATH];
    GetModuleFileName(NULL, buffer, MAX_PATH);
  std::string::size_type pos = std::string(buffer).find_last_of("\\/");
  return std::string(buffer).substr(0, pos);
}
#else
#include <iostream>
static std::string ExePath() { return filesystem::current_path(); }
#endif

static void FormatDuration(std::chrono::milliseconds ms, int &out_hours,
                           int &out_mins, int &out_secs, int &out_msecs) {
  using namespace std::chrono;
  auto secs = std::chrono::duration_cast<seconds>(ms);
  ms -= std::chrono::duration_cast<milliseconds>(secs);
  auto mins = std::chrono::duration_cast<minutes>(secs);
  secs -= std::chrono::duration_cast<seconds>(mins);
  auto hour = std::chrono::duration_cast<hours>(mins);
  mins -= std::chrono::duration_cast<minutes>(hour);

  out_hours = (int)hour.count();
  out_mins = (int)mins.count();
  out_secs = (int)secs.count();
  out_msecs = (int)ms.count();

  // std::stringstream ss;
  // ss << hour.count() << " Hours : " << mins.count()
  //   << " Minutes : " << secs.count() << " Seconds : " << ms.count()
  //   << " Milliseconds";
  // return ss.str();
}

static void ReplaceStringInPlace(std::string &subject,
                                 const std::string &search,
                                 const std::string &replace) {
  size_t pos = 0;
  while ((pos = subject.find(search, pos)) != std::string::npos) {
    subject.replace(pos, search.length(), replace);
    pos += replace.length();
  }
}

static std::vector<std::string> split_string(std::string s,
                                             std::string delimiter) {
  size_t pos_start = 0, pos_end, delim_len = delimiter.length();
  std::string token;
  std::vector<std::string> res;

  while ((pos_end = s.find(delimiter, pos_start)) != std::string::npos) {
    token = s.substr(pos_start, pos_end - pos_start);
    pos_start = pos_end + delim_len;
    res.push_back(token);
  }

  res.push_back(s.substr(pos_start));
  return res;
}

static void GetTeamSampleFolder(std::string &s3_path, std::string &out_folder) {
  out_folder = "";
  std::string strip_path = "s3://ucv-pipeline-data/Production/TeamSamples/";
  std::string trunc_path = s3_path;
  ReplaceStringInPlace(trunc_path, strip_path, "");
  std::vector<std::string> str_list = split_string(trunc_path, "/");
  if (str_list.size() == 4) {
    std::string sample_type = str_list[0];
    std::string team_type = str_list[1];
    std::string det_folder = str_list[2];
    std::string sample_image = str_list[3];
    ReplaceStringInPlace(sample_image, ".bmp", "");
    out_folder = ExePath() + "/league_samples/" + sample_type + "/" +
                 team_type + "/normal/" + sample_image + "/";
  } else if (str_list.size() == 3) {
    std::string team_type = str_list[0];
    std::string det_folder = str_list[1];
    std::string sample_image = str_list[2];
    ReplaceStringInPlace(sample_image, ".bmp", "");
    out_folder = ExePath() + "/league_samples/" + team_type + "/normal/" +
                 sample_image + "/";
  }
}

static std::string GetCurrentDateTimeChrono() {
  auto now = std::chrono::system_clock::now();
  auto in_time_t = std::chrono::system_clock::to_time_t(now);

  std::stringstream ss;
  ss << std::put_time(std::localtime(&in_time_t), "%Y-%m-%d %X");
  return ss.str();
}

static void GetUTCDateTime(std::string &date_time, int64_t &date_time_ms) {
  std::chrono::time_point time_now = std::chrono::system_clock::now();

  std::time_t now = std::chrono::system_clock::to_time_t(time_now);

  date_time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    time_now.time_since_epoch())
                    .count();

  char buf[100] = {0};
  std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", std::localtime(&now));
  date_time = std::string(buf);
}

static void GetUTCDate(std::string &date_time) {
  std::time_t now =
      std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());

  char buf[100] = {0};
  std::strftime(buf, sizeof(buf), "%Y-%m-%d", std::localtime(&now));
  date_time = std::string(buf);
}


#if !defined(WIN32)
#include <openssl/md5.h>
#include <iomanip>
#include <sstream>
#include <boost/iostreams/device/mapped_file.hpp>
static void MD5ForFile(const std::string &path, std::string &md5_str, STATSLogger *logger) {
  try {
    unsigned char result[MD5_DIGEST_LENGTH];
    boost::iostreams::mapped_file_source src(path);
    MD5((unsigned char *)src.data(), src.size(), result);
    std::ostringstream sout;
    sout << std::hex << std::setfill('0');
    for (auto c : result) sout << std::setw(2) << (int)c;
    md5_str = sout.str();
  } catch (std::exception &ex) {
    logger->Log(EnumLogMsgType::LOG_MSG_TYPE_ERROR, "MD5ForFile() ERROR exception occurred");
    int64_t time_ms = 0;
    GetUTCDateTime(md5_str, time_ms);
  }
}
#endif

#include <boost/date_time/gregorian/gregorian.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>
#include <boost/chrono.hpp>

static void ConvertEpochTimeToStr(int64_t time_ms, std::string &out_str) {
  boost::posix_time::ptime now =
      boost::posix_time::from_time_t(time_ms / 1000) +
      boost::posix_time::milliseconds(time_ms % 1000);
  out_str = boost::posix_time::to_simple_string(now);
}

#endif
