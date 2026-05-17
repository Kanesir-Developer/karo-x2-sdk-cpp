#include "karo/sdk/diagnostics.hpp"

#include <cstdint>
#include <sstream>
#include <string>

namespace karo::sdk {

namespace {

std::string escape_json_string(const std::string& in) {
  std::string out;
  out.reserve(in.size() + 2);
  for (char c : in) {
    switch (c) {
      case '"':  out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b";  break;
      case '\f': out += "\\f";  break;
      case '\n': out += "\\n";  break;
      case '\r': out += "\\r";  break;
      case '\t': out += "\\t";  break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {

          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x",
                        static_cast<unsigned int>(static_cast<unsigned char>(c)));
          out += buf;
        } else {
          out += c;
        }
    }
  }
  return out;
}

void append_quoted(std::ostringstream& o, const std::string& s) {
  o << '"' << escape_json_string(s) << '"';
}

void append_kv_string(std::ostringstream& o, const char* key, const std::string& v) {
  o << '"' << key << "\":";
  append_quoted(o, v);
}

}

std::string to_json(const Diagnostics& d) {
  std::ostringstream o;
  o << '{';

  o << "\"connection\":{";
  append_kv_string(o, "state", to_string(d.connection.state));
  o << ",\"last_state_change_ms\":" << d.connection.last_state_change_ms
    << ",\"reconnect_count\":" << d.connection.reconnect_count;
  if (d.connection.last_error_code != ErrorCode::Ok) {
    o << ",\"last_error\":{";
    append_kv_string(o, "code", to_string(d.connection.last_error_code));
    o << ",";
    append_kv_string(o, "message", d.connection.last_error_message);
    o << '}';
  } else {
    o << ",\"last_error\":null";
  }
  o << ",";
  append_kv_string(o, "server_protocol_version", d.connection.server_protocol_version);
  o << '}';

  o << ",\"transport\":{"
    << "\"rtt_p50_ms\":" << d.transport.rtt_p50_ms
    << ",\"rtt_p99_ms\":" << d.transport.rtt_p99_ms
    << ",\"bytes_sent\":" << d.transport.bytes_sent
    << ",\"bytes_received\":" << d.transport.bytes_received
    << ",\"last_ping_age_ms\":" << d.transport.last_ping_age_ms
    << '}';

  o << ",\"subscriptions\":[";
  for (size_t i = 0; i < d.subscriptions.size(); ++i) {
    if (i > 0) o << ',';
    const auto& s = d.subscriptions[i];
    o << '{';
    append_kv_string(o, "topic", s.topic);
    o << ",";
    append_kv_string(o, "state", to_string(s.state));
    o << ",\"configured_hz\":" << s.configured_hz
      << ",\"received_hz_1min\":" << s.received_hz_1min
      << ",\"drop_count\":" << s.drop_count
      << ",\"last_seq\":" << s.last_seq
      << ",\"last_message_age_ms\":" << s.last_message_age_ms
      << '}';
  }
  o << ']';

  o << ",\"rpcs\":[";
  for (size_t i = 0; i < d.rpcs.size(); ++i) {
    if (i > 0) o << ',';
    const auto& r = d.rpcs[i];
    o << '{';
    append_kv_string(o, "name", r.name);
    o << ",\"count\":" << r.count
      << ",\"error_count\":" << r.error_count
      << ",\"p99_latency_ms\":" << r.p99_latency_ms
      << '}';
  }
  o << ']';

  o << '}';
  return o.str();
}

}

