// feed.hpp - Binance USD-M futures market-data feed (Boost.Beast, single I/O thread).
//
//  * Diff depth stream : wss://fstream.binance.com/public/ws/<symbol>@depth[@100ms|@500ms]
//  * Snapshot          : WebSocket API  wss://ws-fapi.binance.com/ws-fapi/v1  {"method":"depth"}
//
// One dedicated thread runs a single-threaded io_context (concurrency hint 1, so
// Asio skips its internal locking). Parsing, book maintenance and publishing all
// happen in that thread; the UI only ever sees complete frames through a
// wait-free triple buffer.
#pragma once
#include "core.hpp"

#include <atomic>
#include <chrono>
#include <cctype>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>

namespace dom {

namespace beast = boost::beast;
namespace net = boost::asio;
namespace ssl = boost::asio::ssl;
namespace websocket = beast::websocket;
using tcp = net::ip::tcp;

enum class FeedStatus : int { Stopped = 0, Connecting, Syncing, Live, Reconnecting };

// State shared between the network thread and the UI thread.
struct Shared {
  TripleBuffer<BookView> buf;
  std::atomic<int> status{static_cast<int>(FeedStatus::Stopped)};
  std::atomic<int64_t> resyncs{0};
  std::atomic<int64_t> reconnects{0};
  std::function<void()> wake;  // called after every publish (e.g. glfwPostEmptyEvent)

  void set_error(std::string e) { std::lock_guard<std::mutex> g(m_); err_ = std::move(e); }
  std::string error() { std::lock_guard<std::mutex> g(m_); return err_; }

 private:
  std::mutex m_;
  std::string err_;
};

// ------------------------------------------------------------ one TLS websocket
class WsClient : public std::enable_shared_from_this<WsClient> {
 public:
  using OpenFn = std::function<void()>;
  using MsgFn = std::function<void(std::string_view)>;
  using ErrFn = std::function<void(const std::string&)>;

  WsClient(net::io_context& ioc, ssl::context& ctx) : resolver_(ioc), ws_(ioc, ctx) {}

  void connect(std::string host, std::string target, OpenFn on_open, MsgFn on_msg, ErrFn on_err) {
    auto self = shared_from_this();  // keep alive even if a callback tears us down
    host_ = std::move(host);
    target_ = std::move(target);
    on_open_ = std::move(on_open);
    on_msg_ = std::move(on_msg);
    on_err_ = std::move(on_err);

    if (!SSL_set_tlsext_host_name(ws_.next_layer().native_handle(), host_.c_str())) {
      fail("could not set SNI host name");
      return;
    }
    ws_.next_layer().set_verify_callback(ssl::host_name_verification(host_));

    resolver_.async_resolve(host_, "443", [self](beast::error_code ec, tcp::resolver::results_type r) {
      if (self->closed_) return;
      if (ec) return self->fail("resolve: " + ec.message());
      auto& low = beast::get_lowest_layer(self->ws_);
      low.expires_after(std::chrono::seconds(10));
      low.async_connect(r, [self](beast::error_code ec2, tcp::resolver::results_type::endpoint_type) {
        self->on_connect(ec2);
      });
    });
  }

  void send(std::string msg) {
    out_ = std::move(msg);
    ws_.async_write(net::buffer(out_), [self = shared_from_this()](beast::error_code ec, std::size_t) {
      if (ec) self->fail("write: " + ec.message());
    });
  }

  // Abrupt close; pending handlers complete with operation_aborted and are ignored.
  void close() {
    closed_ = true;
    beast::get_lowest_layer(ws_).close();
  }

 private:
  void on_connect(beast::error_code ec) {
    if (closed_) return;
    if (ec) return fail("connect: " + ec.message());
    auto& low = beast::get_lowest_layer(ws_);
    beast::error_code ignored;
    low.socket().set_option(tcp::no_delay(true), ignored);  // never let Nagle hold a frame back
    low.expires_after(std::chrono::seconds(10));
    ws_.next_layer().async_handshake(ssl::stream_base::client,
                                     [self = shared_from_this()](beast::error_code e) { self->on_tls(e); });
  }

  void on_tls(beast::error_code ec) {
    if (closed_) return;
    if (ec) return fail("tls: " + ec.message());
    beast::get_lowest_layer(ws_).expires_never();
    // 10s handshake, 20s idle; the stream sends keep-alive pings at half the idle time.
    ws_.set_option(websocket::stream_base::timeout{std::chrono::seconds(10), std::chrono::seconds(20), true});
    ws_.set_option(websocket::stream_base::decorator([](websocket::request_type& req) {
      req.set(beast::http::field::user_agent, "dom-ladder/1.0");
    }));
    ws_.async_handshake(host_, target_,
                        [self = shared_from_this()](beast::error_code e) { self->on_ws(e); });
  }

  void on_ws(beast::error_code ec) {
    if (closed_) return;
    if (ec) return fail("websocket handshake: " + ec.message());
    ws_.text(true);
    do_read();
    if (on_open_) on_open_();
  }

  void do_read() {
    ws_.async_read(buf_, [self = shared_from_this()](beast::error_code ec, std::size_t) { self->on_read(ec); });
  }

  void on_read(beast::error_code ec) {
    if (closed_) return;
    if (ec) return fail("read: " + ec.message());
    const auto d = buf_.data();  // contiguous for flat_buffer
    if (on_msg_) on_msg_(std::string_view(static_cast<const char*>(d.data()), d.size()));
    buf_.consume(buf_.size());
    if (!closed_) do_read();
  }

  void fail(const std::string& m) {
    if (closed_) return;
    closed_ = true;
    beast::get_lowest_layer(ws_).close();
    if (on_err_) on_err_(m);
  }

  tcp::resolver resolver_;
  websocket::stream<beast::ssl_stream<beast::tcp_stream>> ws_;
  beast::flat_buffer buf_;
  std::string host_, target_, out_;
  OpenFn on_open_;
  MsgFn on_msg_;
  ErrFn on_err_;
  bool closed_ = false;
};

// ------------------------------------------------------------------------ feed
class Feed {
 public:
  // speed_suffix: "" (250ms default), "@100ms" or "@500ms"
  Feed(Shared& shared, const std::string& symbol, std::string speed_suffix, uint32_t generation)
      : shared_(shared), gen_(generation), speed_(std::move(speed_suffix)) {
    for (char c : symbol) {
      upper_.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
      lower_.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    ctx_.set_default_verify_paths();
    ctx_.set_verify_mode(ssl::verify_peer);
    snapshot_req_ = R"({"id":"snap","method":"depth","params":{"symbol":")" + upper_ + R"(","limit":1000}})";
    set_status(FeedStatus::Connecting);
    thr_ = std::thread([this] {
      net::post(ioc_, [this] { start_session(); });
      ioc_.run();
    });
  }

  ~Feed() {
    ioc_.stop();
    if (thr_.joinable()) thr_.join();
    stream_.reset();
    api_.reset();
  }
  Feed(const Feed&) = delete;
  Feed& operator=(const Feed&) = delete;

 private:
  enum class Phase { WaitingSnapshot, Active };
  using ms = std::chrono::milliseconds;

  void set_status(FeedStatus s) {
    shared_.status.store(static_cast<int>(s), std::memory_order_relaxed);
    if (shared_.wake) shared_.wake();
  }

  void teardown() {
    ++epoch_;
    timer_.cancel();
    snap_timer_.cancel();
    if (stream_) { stream_->close(); stream_.reset(); }
    if (api_) { api_->close(); api_.reset(); }
  }

  void restart(const std::string& why) {
    teardown();
    shared_.set_error(why);
    shared_.reconnects.fetch_add(1, std::memory_order_relaxed);
    set_status(FeedStatus::Reconnecting);
    const uint64_t ep = epoch_;
    timer_.expires_after(backoff_);
    timer_.async_wait([this, ep](beast::error_code ec) {
      if (ec || ep != epoch_) return;
      start_session();
    });
    backoff_ = std::min(backoff_ * 2, ms(8000));
  }

  void start_session() {
    sync_ = BookSync{};
    pending_.clear();
    phase_ = Phase::WaitingSnapshot;
    set_status(FeedStatus::Connecting);
    stream_ = std::make_shared<WsClient>(ioc_, ctx_);
    stream_->connect(
        "fstream.binance.com", "/public/ws/" + lower_ + "@depth" + speed_,
        [this] {                       // stream open -> fetch the snapshot while events buffer
          set_status(FeedStatus::Syncing);
          request_snapshot();
        },
        [this](std::string_view m) { on_stream_message(m); },
        [this](const std::string& e) { restart("stream: " + e); });
  }

  void request_snapshot() {
    if (api_) { api_->close(); api_.reset(); }
    last_snapshot_req_ = std::chrono::steady_clock::now();
    phase_ = Phase::WaitingSnapshot;
    api_ = std::make_shared<WsClient>(ioc_, ctx_);
    api_->connect(
        "ws-fapi.binance.com", "/ws-fapi/v1",
        [this] { api_->send(snapshot_req_); },
        [this](std::string_view m) { on_snapshot_message(m); },
        [this](const std::string& e) { restart("snapshot: " + e); });
  }

  // Resync = fetch a fresh snapshot, but never more than once per second.
  void schedule_resync() {
    shared_.resyncs.fetch_add(1, std::memory_order_relaxed);
    phase_ = Phase::WaitingSnapshot;
    set_status(FeedStatus::Syncing);
    const auto since = std::chrono::steady_clock::now() - last_snapshot_req_;
    auto delay = std::chrono::duration_cast<ms>(std::chrono::seconds(1) - since);
    if (delay < ms(0)) delay = ms(0);
    const uint64_t ep = epoch_;
    snap_timer_.expires_after(delay);
    snap_timer_.async_wait([this, ep](beast::error_code ec) {
      if (ec || ep != epoch_) return;
      request_snapshot();
    });
  }

  void on_stream_message(std::string_view m) {
    const int64_t t_recv = now_ns();
    const int64_t wall = wall_ms();
    if (!parse_depth_event(m, ev_) || !ev_.is_depth) return;

    if (phase_ == Phase::WaitingSnapshot) {
      pending_.push_back(ev_);
      if (pending_.size() > 50000) restart("event buffer overflow while waiting for snapshot");
      return;
    }
    switch (sync_.on_event(ev_)) {
      case BookSync::Result::Applied:
        if (sync_.book().crossed()) {            // should never happen; cheap insurance
          pending_.clear();
          schedule_resync();
          return;
        }
        publish(t_recv, wall, ev_.E);
        if (sync_.live() && shared_.status.load(std::memory_order_relaxed) != static_cast<int>(FeedStatus::Live)) {
          backoff_ = ms(500);
          set_status(FeedStatus::Live);
        }
        break;
      case BookSync::Result::Ignored:
        break;
      case BookSync::Result::Resync:
        pending_.clear();
        pending_.push_back(ev_);
        schedule_resync();
        break;
    }
  }

  void on_snapshot_message(std::string_view m) {
    if (!parse_snapshot(m, snap_)) { restart("malformed snapshot response"); return; }
    if (snap_.status != 200) { restart("snapshot request failed, status " + std::to_string(snap_.status)); return; }
    if (api_) { api_->close(); api_.reset(); }

    sync_.load_snapshot(snap_);
    tick_ = sync_.book().detect_tick();
    phase_ = Phase::Active;

    bool resync = false;
    for (const DepthEvent& e : pending_) {
      if (sync_.on_event(e) == BookSync::Result::Resync) { resync = true; break; }
    }
    pending_.clear();
    if (resync) { schedule_resync(); return; }

    publish(now_ns(), wall_ms(), snap_.E);
    if (sync_.live()) {
      backoff_ = ms(500);
      set_status(FeedStatus::Live);
    }
  }

  void publish(int64_t t_recv, int64_t wall, int64_t event_time) {
    BookView& v = shared_.buf.write_slot();
    sync_.book().fill_view(v);
    v.generation = gen_;
    v.tick = tick_;
    v.seq = ++seq_;
    v.event_time_ms = event_time;
    v.recv_wall_ms = wall;
    v.recv_ns = t_recv;
    v.apply_ns = now_ns() - t_recv;  // parse + apply + copy
    shared_.buf.publish();
    if (shared_.wake) shared_.wake();
  }

  Shared& shared_;
  uint32_t gen_;
  std::string speed_, upper_, lower_, snapshot_req_;

  net::io_context ioc_{1};  // concurrency hint 1: lock-free scheduler fast path
  ssl::context ctx_{ssl::context::tls_client};
  net::steady_timer timer_{ioc_};
  net::steady_timer snap_timer_{ioc_};
  std::shared_ptr<WsClient> stream_, api_;

  BookSync sync_;
  Snapshot snap_;
  DepthEvent ev_;
  std::vector<DepthEvent> pending_;
  Phase phase_ = Phase::WaitingSnapshot;
  int64_t tick_ = kScale / 100;
  int64_t seq_ = 0;
  uint64_t epoch_ = 0;
  ms backoff_{500};
  std::chrono::steady_clock::time_point last_snapshot_req_{};

  std::thread thr_;  // must be last: starts running in the constructor body
};

}  // namespace dom
