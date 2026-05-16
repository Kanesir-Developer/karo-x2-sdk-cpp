#include "ws_client.hpp"

#include <atomic>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>

#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/beast/core/buffers_to_string.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>

namespace karo::sdk::detail {

namespace asio  = boost::asio;
namespace ssl   = boost::asio::ssl;
namespace beast = boost::beast;
namespace ws    = boost::beast::websocket;
using tcp       = boost::asio::ip::tcp;
using WsStream  = ws::stream<ssl::stream<tcp::socket>>;

namespace {

void load_pem_certificate(ssl::context& ctx, const std::string& cert_pem)
{
  ctx.use_certificate_chain(asio::buffer(cert_pem));
}
void load_pem_private_key(ssl::context& ctx, const std::string& key_pem)
{
  ctx.use_private_key(asio::buffer(key_pem), ssl::context::pem);
}
void load_pem_ca(ssl::context& ctx, const std::string& ca_pem)
{
  ctx.add_certificate_authority(asio::buffer(ca_pem));
}

std::pair<std::string, std::string> split_endpoint(const std::string& ep)
{
  auto colon = ep.rfind(':');
  if (colon == std::string::npos) {
    throw SdkException(ErrorCode::TransportFailure,
      "endpoint missing port: " + ep);
  }
  return { ep.substr(0, colon), ep.substr(colon + 1) };
}

struct ConnectAttempt : std::enable_shared_from_this<ConnectAttempt> {
  asio::io_context&        ioc;
  ssl::context&            ssl_ctx;
  std::string              host;
  std::string              port;
  std::string              path;
  std::chrono::milliseconds timeout;

  std::unique_ptr<WsStream> stream;
  tcp::resolver            resolver;
  asio::steady_timer       deadline_timer;
  std::shared_ptr<std::promise<std::exception_ptr>> done;

  std::atomic<bool>        timed_out{false};
  std::atomic<bool>        completed{false};

  ConnectAttempt(asio::io_context& i, ssl::context& s,
                 std::string h, std::string p, std::string path_,
                 std::chrono::milliseconds to,
                 std::shared_ptr<std::promise<std::exception_ptr>> pr)
    : ioc(i), ssl_ctx(s), host(std::move(h)), port(std::move(p)),
      path(std::move(path_)), timeout(to), resolver(i),
      deadline_timer(i), done(std::move(pr)) {}

  void start() {
    auto self = shared_from_this();

    deadline_timer.expires_after(timeout);
    deadline_timer.async_wait([self](const boost::system::error_code& ec) {
      if (ec == asio::error::operation_aborted) return;

      self->timed_out.store(true);
      if (self->stream) {
        boost::system::error_code ec2;
        self->stream->next_layer().next_layer().close(ec2);
      }

    });

    resolver.async_resolve(host, port,
      [self](const boost::system::error_code& ec, tcp::resolver::results_type results) {
        if (self->completed.load()) return;
        if (ec) {
          self->finish(self->map_error(ec, "resolve"));
          return;
        }
        self->step_connect(std::move(results));
      });
  }

  void step_connect(tcp::resolver::results_type results) {
    auto self = shared_from_this();
    stream = std::make_unique<WsStream>(ioc, ssl_ctx);

    if (!SSL_set_tlsext_host_name(stream->next_layer().native_handle(),
                                   host.c_str())) {
      finish(std::make_exception_ptr(SdkException(
        ErrorCode::TransportFailure, "SSL_set_tlsext_host_name failed")));
      return;
    }
    asio::async_connect(stream->next_layer().next_layer(), results,
      [self](const boost::system::error_code& ec, const tcp::endpoint&) {
        if (self->completed.load()) return;
        if (ec) {
          self->finish(self->map_error(ec, "tcp connect"));
          return;
        }
        self->step_tls_handshake();
      });
  }

  void step_tls_handshake() {
    auto self = shared_from_this();
    stream->next_layer().async_handshake(ssl::stream_base::client,
      [self](const boost::system::error_code& ec) {
        if (self->completed.load()) return;
        if (ec) {
          self->finish(self->map_error(ec, "tls handshake"));
          return;
        }
        self->step_ws_handshake();
      });
  }

  void step_ws_handshake() {
    auto self = shared_from_this();
    stream->set_option(ws::stream_base::decorator(
      [](ws::request_type& req) {
        req.set(beast::http::field::user_agent, "karo-sdk-cpp/2.0");
      }));
    stream->binary(true);
    std::string host_port = host + ":" + port;
    stream->async_handshake(host_port, path,
      [self](const boost::system::error_code& ec) {
        if (self->completed.load()) return;
        if (ec) {
          self->finish(self->map_error(ec, "ws handshake"));
          return;
        }
        self->finish(nullptr);
      });
  }

  void finish(std::exception_ptr eptr) {
    if (completed.exchange(true)) return;
    boost::system::error_code dec;
    deadline_timer.cancel();
    done->set_value(eptr);
  }

  std::exception_ptr map_error(const boost::system::error_code& ec,
                                const std::string& step) {
    if (timed_out.load() || ec == asio::error::operation_aborted) {
      return std::make_exception_ptr(SdkException(
        ErrorCode::Timeout, "connect timeout (" + step + ")"));
    }
    auto code = ErrorCode::TransportFailure;
    return std::make_exception_ptr(SdkException(
      code, step + ": " + ec.message()));
  }
};

}

struct WsClient::Impl : std::enable_shared_from_this<Impl> {
  Config                       cfg;
  asio::io_context             ioc;
  ssl::context                 ssl_ctx{ssl::context::tlsv12_client};
  std::unique_ptr<WsStream>    stream;
  beast::flat_buffer           read_buf;
  std::thread                  io_thread;
  std::atomic<bool>            open{false};
  std::atomic<bool>            closing{false};
  std::mutex                   mu;
  MessageHandler               on_message;
  CloseHandler                 on_close;

  struct PendingWrite {
    std::shared_ptr<std::vector<uint8_t>> buf;

    std::shared_ptr<std::promise<std::exception_ptr>> done;
    std::shared_ptr<std::atomic<bool>> timed_out;
    std::shared_ptr<std::atomic<bool>> completed;
    std::shared_ptr<asio::steady_timer> deadline;
  };
  std::deque<PendingWrite>     write_queue;
  bool                         write_in_flight = false;

  std::optional<asio::executor_work_guard<asio::io_context::executor_type>> work;

  explicit Impl(Config c) : cfg(std::move(c))
  {
    if (cfg.insecure_skip_verify) {
      ssl_ctx.set_verify_mode(ssl::verify_none);
    } else {
      ssl_ctx.set_verify_mode(ssl::verify_peer);
      if (!cfg.ca_pem.empty()) {
        try {
          load_pem_ca(ssl_ctx, cfg.ca_pem);
        } catch (const std::exception& e) {
          throw SdkException(ErrorCode::AuthFailed,
            std::string("invalid ca_pem: ") + e.what());
        }
      } else {
        ssl_ctx.set_default_verify_paths();
      }
    }
    if (!cfg.cert_pem.empty()) {
      try {
        load_pem_certificate(ssl_ctx, cfg.cert_pem);
        load_pem_private_key(ssl_ctx, cfg.key_pem);
      } catch (const std::exception& e) {
        throw SdkException(ErrorCode::AuthFailed,
          std::string("invalid cert/key pem: ") + e.what());
      }
    }

    work.emplace(asio::make_work_guard(ioc));
    io_thread = std::thread([this] {
      try { ioc.run(); } catch (...) { }
    });
  }

  ~Impl()
  {

    if (io_thread.joinable() &&
        std::this_thread::get_id() == io_thread.get_id()) {
      std::terminate();
    }

    closing.store(true);
    open.store(false);
    work.reset();
    if (stream) {
      boost::system::error_code ec;
      stream->next_layer().next_layer().close(ec);
    }
    if (!ioc.stopped()) ioc.stop();
    if (io_thread.joinable()) io_thread.join();
  }

  void Connect()
  {
    auto [host, port] = split_endpoint(cfg.endpoint);
    auto done = std::make_shared<std::promise<std::exception_ptr>>();

    auto attempt = std::make_shared<ConnectAttempt>(
      ioc, ssl_ctx, host, port, cfg.path, cfg.connect_timeout, done);

    asio::post(ioc, [attempt] {
      attempt->start();
    });

    auto fut = done->get_future();

    auto grace_wait = cfg.connect_timeout + std::chrono::seconds(1);
    if (fut.wait_for(grace_wait) != std::future_status::ready) {

      throw SdkException(ErrorCode::Timeout,
        "connect timeout (internal: chain did not signal)");
    }

    auto eptr = fut.get();
    if (eptr) {
      try { std::rethrow_exception(eptr); }
      catch (const SdkException&) { throw; }
      catch (const std::exception& e) {
        throw SdkException(ErrorCode::TransportFailure,
          std::string("connect: ") + e.what());
      }
    }

    stream = std::move(attempt->stream);
    open.store(true);
    StartRead();
  }

  void StartRead()
  {
    asio::post(ioc, [this] { DoRead(); });
  }

  void DoRead()
  {
    if (!stream || !open.load()) return;
    stream->async_read(read_buf,
      [this](beast::error_code ec, std::size_t n) {
        if (ec) {
          auto reason = (ec == ws::error::closed || ec == asio::error::eof)
            ? ErrorCode::Cancelled
            : ErrorCode::TransportFailure;
          OnDisconnect(reason, ec.message());
          return;
        }
        auto data = beast::buffers_to_string(read_buf.cdata());
        read_buf.consume(n);
        std::vector<uint8_t> bytes(data.begin(), data.end());
        MessageHandler cb;
        {
          std::lock_guard<std::mutex> lock(mu);
          cb = on_message;
        }
        if (cb) {
          try { cb(std::move(bytes)); } catch (...) { }
        }
        DoRead();
      });
  }

  void EnqueueWrite(PendingWrite pw) {
    if (closing.load() || !open.load() || !stream) {

      if (pw.completed && !pw.completed->exchange(true)) {
        if (pw.deadline) pw.deadline->cancel();
        if (pw.done) {
          pw.done->set_value(std::make_exception_ptr(SdkException(
            ErrorCode::TransportFailure, "ws closing or not open")));
        }
      }
      return;
    }
    write_queue.push_back(std::move(pw));
    if (!write_in_flight) {
      ProcessWriteQueue();
    }
  }

  void ProcessWriteQueue() {
    if (write_queue.empty()) {
      write_in_flight = false;
      return;
    }
    if (!stream || !open.load()) {

      while (!write_queue.empty()) {
        auto& pw = write_queue.front();
        if (pw.completed && !pw.completed->exchange(true)) {
          if (pw.deadline) pw.deadline->cancel();
          if (pw.done) {
            pw.done->set_value(std::make_exception_ptr(SdkException(
              ErrorCode::TransportFailure, "ws not open")));
          }
        }
        write_queue.pop_front();
      }
      write_in_flight = false;
      return;
    }
    write_in_flight = true;

    auto& pw = write_queue.front();
    auto buf = pw.buf;
    auto done = pw.done;
    auto timed_out = pw.timed_out;
    auto completed = pw.completed;
    auto deadline = pw.deadline;

    stream->async_write(asio::buffer(*buf),
      [this, done, timed_out, completed, deadline]
      (const boost::system::error_code& ec, std::size_t) {

        if (!write_queue.empty()) write_queue.pop_front();
        if (completed && !completed->exchange(true)) {
          if (deadline) deadline->cancel();
          if (done) {
            if (timed_out && timed_out->load()) {
              done->set_value(std::make_exception_ptr(SdkException(
                ErrorCode::Timeout, "send timeout")));
            } else if (ec) {
              done->set_value(std::make_exception_ptr(SdkException(
                ErrorCode::TransportFailure, "send: " + ec.message())));
            } else {
              done->set_value(nullptr);
            }
          } else if (ec && !(timed_out && timed_out->load())) {

            OnDisconnect(ErrorCode::TransportFailure, ec.message());
          }
        }

        ProcessWriteQueue();
      });
  }

  void Send(std::vector<uint8_t> bytes)
  {
    if (!open.load()) {
      throw SdkException(ErrorCode::TransportFailure, "ws not open");
    }
    auto buf = std::make_shared<std::vector<uint8_t>>(std::move(bytes));
    auto done = std::make_shared<std::promise<std::exception_ptr>>();
    auto timed_out = std::make_shared<std::atomic<bool>>(false);
    auto completed = std::make_shared<std::atomic<bool>>(false);

    auto deadline = std::make_shared<asio::steady_timer>(ioc);
    deadline->expires_after(cfg.write_timeout);
    deadline->async_wait([this, timed_out, completed]
                         (const boost::system::error_code& ec) {
      if (ec == asio::error::operation_aborted) return;
      if (completed->load()) return;
      timed_out->store(true);
      if (stream) {
        boost::system::error_code ec2;
        stream->next_layer().next_layer().close(ec2);
      }
    });

    PendingWrite pw{buf, done, timed_out, completed, deadline};
    asio::post(ioc, [this, pw = std::move(pw)]() mutable {
      EnqueueWrite(std::move(pw));
    });

    auto fut = done->get_future();
    auto grace = cfg.write_timeout + std::chrono::seconds(1);
    if (fut.wait_for(grace) != std::future_status::ready) {
      throw SdkException(ErrorCode::Timeout,
        "send timeout (internal: write chain did not signal)");
    }
    auto eptr = fut.get();
    if (eptr) {
      try { std::rethrow_exception(eptr); }
      catch (const SdkException&) { throw; }
      catch (const std::exception& e) {
        throw SdkException(ErrorCode::TransportFailure,
          std::string("send: ") + e.what());
      }
    }
  }

  void OnDisconnect(ErrorCode reason, const std::string& msg)
  {
    bool was_open = open.exchange(false);
    if (!was_open) return;
    CloseHandler cb;
    {
      std::lock_guard<std::mutex> lock(mu);
      cb = on_close;
    }
    if (cb) {
      try { cb(reason, msg); } catch (...) { }
    }
  }

  void Close()
  {
    if (closing.exchange(true)) return;
    if (!open.load()) return;

    asio::post(ioc, [this] {
      if (!stream || !open.load()) return;
      if (write_in_flight) {

        StartDrainAndClose();
      } else {
        DoAsyncClose();
      }
    });
  }

  void StartDrainAndClose() {

    if (write_queue.empty() && !write_in_flight) {
      DoAsyncClose();
      return;
    }

    auto timer = std::make_shared<asio::steady_timer>(ioc);
    timer->expires_after(std::chrono::milliseconds(10));
    timer->async_wait([this, timer](const boost::system::error_code& ec) {
      if (ec) return;
      StartDrainAndClose();
    });
  }

  void DoAsyncClose() {
    if (!stream || !open.load()) return;
    stream->async_close(ws::close_code::normal,
      [this](const boost::system::error_code&) {
        open.store(false);
      });
  }

  bool Post(std::vector<uint8_t> bytes) noexcept
  {
    if (!open.load() || closing.load()) return false;
    auto buf = std::make_shared<std::vector<uint8_t>>(std::move(bytes));
    auto completed = std::make_shared<std::atomic<bool>>(false);
    PendingWrite pw{buf, nullptr, nullptr, completed, nullptr};
    try {
      asio::post(ioc, [this, pw = std::move(pw)]() mutable {
        EnqueueWrite(std::move(pw));
      });
      return true;
    } catch (...) {
      return false;
    }
  }
};

WsClient::WsClient(Config cfg) : impl_(std::make_unique<Impl>(std::move(cfg))) {}
WsClient::~WsClient() = default;

void WsClient::SetMessageHandler(MessageHandler cb)
{
  std::lock_guard<std::mutex> lock(impl_->mu);
  impl_->on_message = std::move(cb);
}
void WsClient::SetCloseHandler(CloseHandler cb)
{
  std::lock_guard<std::mutex> lock(impl_->mu);
  impl_->on_close = std::move(cb);
}
void WsClient::Connect()                        { impl_->Connect(); }
void WsClient::Send(std::vector<uint8_t> bytes) { impl_->Send(std::move(bytes)); }
bool WsClient::Post(std::vector<uint8_t> bytes) noexcept {
  return impl_->Post(std::move(bytes));
}
void WsClient::Close()                          { impl_->Close(); }
bool WsClient::IsOpen() const noexcept          { return impl_->open.load(); }

}

