// Copyright 2022, DragonflyDB authors.  All rights reserved.
// See LICENSE for licensing terms.
//

#include <absl/strings/escaping.h>
#include <absl/strings/str_cat.h>

#include "base/logging.h"
#include "facade/command_id.h"
#include "facade/error.h"
#include "facade/resp_expr.h"
#include "strings/human_readable.h"

namespace facade {

using namespace std;

#define ADD(x) (x) += o.x

constexpr size_t kSizeConnStats = sizeof(ConnectionStats);

ConnectionStats& ConnectionStats::operator+=(const ConnectionStats& o) {
  // To break this code deliberately if we add/remove a field to this struct.
  static_assert(kSizeConnStats == 136u);

  ADD(read_buf_capacity);
  ADD(dispatch_queue_entries);
  ADD(dispatch_queue_bytes);
  ADD(dispatch_queue_subscriber_bytes);
  ADD(pipeline_cmd_cache_bytes);
  ADD(io_read_cnt);
  ADD(io_read_bytes);
  ADD(command_cnt_main);
  ADD(command_cnt_other);
  ADD(pipelined_cmd_cnt);
  ADD(pipelined_cmd_latency);
  ADD(conn_received_cnt);
  ADD(num_conns_main);
  ADD(num_conns_other);
  ADD(num_blocked_clients);
  ADD(num_migrations);
  ADD(num_recv_provided_calls);
  ADD(pipeline_throttle_count);

  return *this;
}

ReplyStats::ReplyStats(ReplyStats&& other) noexcept {
  *this = other;
}

ReplyStats& ReplyStats::operator+=(const ReplyStats& o) {
  static_assert(sizeof(ReplyStats) == 80u + kSanitizerOverhead);
  ADD(io_write_cnt);
  ADD(io_write_bytes);

  for (const auto& k_v : o.err_count) {
    err_count[k_v.first] += k_v.second;
  }

  ADD(script_error_count);

  send_stats += o.send_stats;
  squashing_current_reply_size.fetch_add(o.squashing_current_reply_size.load(memory_order_relaxed),
                                         memory_order_relaxed);
  return *this;
}

#undef ADD

ReplyStats& ReplyStats::operator=(const ReplyStats& o) {
  static_assert(sizeof(ReplyStats) == 80u + kSanitizerOverhead);

  if (this == &o) {
    return *this;
  }

  send_stats = o.send_stats;
  io_write_cnt = o.io_write_cnt;
  io_write_bytes = o.io_write_bytes;
  err_count = o.err_count;
  script_error_count = o.script_error_count;
  squashing_current_reply_size.store(o.squashing_current_reply_size.load(memory_order_relaxed),
                                     memory_order_relaxed);
  return *this;
}

string WrongNumArgsError(string_view cmd) {
  return absl::StrCat("wrong number of arguments for '", absl::AsciiStrToLower(cmd), "' command");
}

string InvalidExpireTime(string_view cmd) {
  return absl::StrCat("invalid expire time in '", absl::AsciiStrToLower(cmd), "' command");
}

string UnknownSubCmd(string_view subcmd, string_view cmd) {
  return absl::StrCat("Unknown subcommand or wrong number of arguments for '", subcmd, "'. Try ",
                      cmd, " HELP.");
}

string ConfigSetFailed(string_view config_name) {
  return absl::StrCat("CONFIG SET failed (possibly related to argument '", config_name, "').");
}

const char kSyntaxErr[] = "syntax error";
const char kWrongTypeErr[] = "-WRONGTYPE Operation against a key holding the wrong kind of value";
const char kWrongJsonTypeErr[] = "-WRONGTYPE wrong JSON type of path value";
const char kKeyNotFoundErr[] = "no such key";
const char kInvalidIntErr[] = "value is not an integer or out of range";
const char kInvalidFloatErr[] = "value is not a valid float";
const char kUintErr[] = "value is out of range, must be positive";
const char kIncrOverflow[] = "increment or decrement would overflow";
const char kDbIndOutOfRangeErr[] = "DB index is out of range";
const char kInvalidDbIndErr[] = "invalid DB index";
const char kScriptNotFound[] = "-NOSCRIPT No matching script. Please use EVAL.";
const char kAuthRejected[] = "-WRONGPASS invalid username-password pair or user is disabled.";
const char kExpiryOutOfRange[] = "expiry is out of range";
const char kIndexOutOfRange[] = "index out of range";
const char kOutOfMemory[] = "Out of memory";
const char kInvalidNumericResult[] = "result is not a number";
const char kClusterNotConfigured[] = "Cluster is not yet configured";
const char kLoadingErr[] = "-LOADING Dragonfly is loading the dataset in memory";
const char kUndeclaredKeyErr[] = "script tried accessing undeclared key";
const char kInvalidDumpValueErr[] = "DUMP payload version or checksum are wrong";
const char kInvalidJsonPathErr[] = "invalid JSON path";
const char kJsonParseError[] = "failed to parse JSON";
const char kCrossSlotError[] = "-CROSSSLOT Keys in request don't hash to the same slot";

const char kSyntaxErrType[] = "syntax_error";
const char kScriptErrType[] = "script_error";
const char kConfigErrType[] = "config_error";
const char kSearchErrType[] = "search_error";
const char kWrongTypeErrType[] = "wrong_type";
const char kRestrictDenied[] = "restrict_denied";

const char* RespExpr::TypeName(Type t) {
  switch (t) {
    case STRING:
      return "string";
    case INT64:
      return "int";
    case DOUBLE:
      return "double";
    case ARRAY:
      return "array";
    case NIL_ARRAY:
      return "nil-array";
    case NIL:
      return "nil";
    case ERROR:
      return "error";
  }
  ABSL_UNREACHABLE();
}

CommandId::CommandId(const char* name, uint32_t mask, int8_t arity, int8_t first_key,
                     int8_t last_key, uint32_t acl_categories)
    : name_(name),
      opt_mask_(mask),
      arity_(arity),
      first_key_(first_key),
      last_key_(last_key),
      acl_categories_(acl_categories) {
  if (name_ == "PUBLISH" || name_ == "SUBSCRIBE" || name_ == "UNSUBSCRIBE") {
    is_pub_sub_ = true;
  } else if (name_ == "PSUBSCRIBE" || name_ == "PUNSUBSCRIBE") {
    is_p_pub_sub_ = true;
  } else if (name_ == "SPUBLISH" || name_ == "SSUBSCRIBE" || name_ == "SUNSUBSCRIBE") {
    is_sharded_pub_sub_ = true;
  }
}

uint32_t CommandId::OptCount(uint32_t mask) {
  return absl::popcount(mask);
}

__thread FacadeStats* tl_facade_stats = nullptr;

static bool ParseHumanReadableBytes(std::string_view str, int64_t* num_bytes) {
  if (str.empty())
    return false;

  const char* cstr = str.data();
  bool neg = (*cstr == '-');
  if (neg) {
    cstr++;
  }
  char* end;
  double d = strtod(cstr, &end);

  if (end == cstr)  // did not succeed to advance
    return false;

  int64_t scale = 1;
  switch (*end) {
    // Considers just the first character after the number
    // so it matches: 1G, 1GB, 1GiB and 1Gigabytes
    // NB: an int64 can only go up to <8 EB.
    case 'E':
    case 'e':
      scale <<= 10;  // Fall through...
      ABSL_FALLTHROUGH_INTENDED;
    case 'P':
    case 'p':
      scale <<= 10;
      ABSL_FALLTHROUGH_INTENDED;
    case 'T':
    case 't':
      scale <<= 10;
      ABSL_FALLTHROUGH_INTENDED;
    case 'G':
    case 'g':
      scale <<= 10;
      ABSL_FALLTHROUGH_INTENDED;
    case 'M':
    case 'm':
      scale <<= 10;
      ABSL_FALLTHROUGH_INTENDED;
    case 'K':
    case 'k':
      scale <<= 10;
      ABSL_FALLTHROUGH_INTENDED;
    case 'B':
    case 'b':
    case '\0':
      break;  // To here.
    default:
      return false;
  }
  d *= scale;
  if (int64_t(d) > INT64_MAX || d < 0)
    return false;

  *num_bytes = static_cast<int64_t>(d + 0.5);
  if (neg) {
    *num_bytes = -*num_bytes;
  }
  return true;
}

bool AbslParseFlag(std::string_view in, MemoryBytesFlag* flag, std::string* err) {
  int64_t val;
  if (ParseHumanReadableBytes(in, &val) && val >= 0) {
    flag->value = val;
    return true;
  }

  *err = "Use human-readable format, eg.: 500MB, 1G, 1TB";
  return false;
}

std::string AbslUnparseFlag(const MemoryBytesFlag& flag) {
  return strings::HumanReadableNumBytes(flag.value);
}

}  // namespace facade

namespace std {

using facade::ArgS;

ostream& operator<<(ostream& os, facade::CmdArgList ras) {
  os << "[";
  if (!ras.empty()) {
    for (size_t i = 0; i < ras.size() - 1; ++i) {
      os << absl::CHexEscape(ArgS(ras, i)) << ",";
    }
    os << absl::CHexEscape(ArgS(ras, ras.size() - 1));
  }
  os << "]";

  return os;
}

ostream& operator<<(ostream& os, const facade::RespExpr& e) {
  using facade::RespExpr;
  using facade::ToSV;

  switch (e.type) {
    case RespExpr::INT64:
      os << "i" << get<int64_t>(e.u);
      break;
    case RespExpr::DOUBLE:
      os << "d" << get<double>(e.u);
      break;
    case RespExpr::STRING:
      os << "'" << ToSV(get<RespExpr::Buffer>(e.u)) << "'";
      break;
    case RespExpr::NIL:
      os << "nil";
      break;
    case RespExpr::NIL_ARRAY:
      os << "[]";
      break;
    case RespExpr::ARRAY:
      os << facade::RespSpan{*get<RespExpr::Vec*>(e.u)};
      break;
    case RespExpr::ERROR:
      os << "e(" << ToSV(get<RespExpr::Buffer>(e.u)) << ")";
      break;
  }

  return os;
}

ostream& operator<<(ostream& os, facade::RespSpan ras) {
  os << "[";
  if (!ras.empty()) {
    for (size_t i = 0; i < ras.size() - 1; ++i) {
      os << ras[i] << ",";
    }
    os << ras.back();
  }
  os << "]";

  return os;
}

ostream& operator<<(ostream& os, facade::Protocol p) {
  switch (p) {
    case facade::Protocol::REDIS:
      os << "REDIS";
      break;
    case facade::Protocol::MEMCACHE:
      os << "MEMCACHE";
      break;
  }

  return os;
}

}  // namespace std
