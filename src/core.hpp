// core.hpp - dependency-free core: fixed-point parsing, a tiny zero-alloc JSON
// scanner, the order book, the Binance diff-depth sync state machine and a
// lock-free triple buffer used to hand snapshots to the UI thread.
#pragma once
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <chrono>
#include <cstdint>
#include <numeric>
#include <string_view>
#include <vector>

namespace dom {

// ---------------------------------------------------------------- fixed point
constexpr int64_t kScale = 100'000'000;  // prices and quantities are int * 1e-8

struct Level {
  int64_t price;
  int64_t qty;
};

// "12345.67000000" -> 1234567000000.  No allocation, no locale, no floats.
inline bool parse_fixed(std::string_view s, int64_t& out) {
  size_t i = 0;
  const size_t n = s.size();
  if (n == 0) return false;
  bool neg = false;
  if (s[0] == '-') { neg = true; i = 1; }
  int64_t ip = 0, frac = 0;
  int digits = 0;
  bool any = false;
  for (; i < n && s[i] >= '0' && s[i] <= '9'; ++i) {
    ip = ip * 10 + (s[i] - '0');
    any = true;
    if (ip > 90'000'000'000LL) return false;  // overflow guard
  }
  if (i < n && s[i] == '.') {
    ++i;
    for (; i < n && s[i] >= '0' && s[i] <= '9'; ++i) {
      any = true;
      if (digits < 8) { frac = frac * 10 + (s[i] - '0'); ++digits; }
    }
  }
  if (!any || i != n) return false;
  while (digits < 8) { frac *= 10; ++digits; }
  const int64_t v = ip * kScale + frac;
  out = neg ? -v : v;
  return true;
}

// ----------------------------------------------------------- minimal JSON scan
class JsonCursor {
 public:
  explicit JsonCursor(std::string_view s) : p_(s.data()), end_(s.data() + s.size()) {}

  void ws() {
    while (p_ < end_ && (*p_ == ' ' || *p_ == '\n' || *p_ == '\t' || *p_ == '\r')) ++p_;
  }
  bool consume(char c) {
    ws();
    if (p_ < end_ && *p_ == c) { ++p_; return true; }
    return false;
  }
  // Returns the raw (still escaped) contents; Binance payloads never need unescaping.
  bool string(std::string_view& out) {
    ws();
    if (p_ >= end_ || *p_ != '"') return false;
    ++p_;
    const char* b = p_;
    while (p_ < end_ && *p_ != '"') {
      if (*p_ == '\\') { ++p_; if (p_ >= end_) return false; }
      ++p_;
    }
    if (p_ >= end_) return false;
    out = std::string_view(b, static_cast<size_t>(p_ - b));
    ++p_;
    return true;
  }
  // Accepts 123 or "123".
  bool integer(int64_t& out) {
    ws();
    bool quoted = false, neg = false;
    if (p_ < end_ && *p_ == '"') { quoted = true; ++p_; }
    if (p_ < end_ && *p_ == '-') { neg = true; ++p_; }
    if (p_ >= end_ || *p_ < '0' || *p_ > '9') return false;
    int64_t v = 0;
    while (p_ < end_ && *p_ >= '0' && *p_ <= '9') { v = v * 10 + (*p_ - '0'); ++p_; }
    if (quoted) { if (p_ >= end_ || *p_ != '"') return false; ++p_; }
    out = neg ? -v : v;
    return true;
  }
  bool skip_value() {
    ws();
    if (p_ >= end_) return false;
    const char c = *p_;
    if (c == '"') { std::string_view t; return string(t); }
    if (c == '{' || c == '[') {
      int depth = 0;
      while (p_ < end_) {
        const char d = *p_;
        if (d == '"') { std::string_view t; if (!string(t)) return false; continue; }
        if (d == '{' || d == '[') ++depth;
        else if (d == '}' || d == ']') { if (--depth == 0) { ++p_; return true; } }
        ++p_;
      }
      return false;
    }
    while (p_ < end_ && *p_ != ',' && *p_ != '}' && *p_ != ']' &&
           *p_ != ' ' && *p_ != '\n' && *p_ != '\r' && *p_ != '\t') ++p_;
    return true;
  }
  // on_key(key) must consume the value (parse or skip_value) and return success.
  template <class F> bool object(F&& on_key) {
    if (!consume('{')) return false;
    if (consume('}')) return true;
    for (;;) {
      std::string_view key;
      if (!string(key) || !consume(':') || !on_key(key)) return false;
      if (consume(',')) continue;
      return consume('}');
    }
  }
  template <class F> bool array(F&& on_elem) {
    if (!consume('[')) return false;
    if (consume(']')) return true;
    for (;;) {
      if (!on_elem()) return false;
      if (consume(',')) continue;
      return consume(']');
    }
  }
  // [["price","qty"], ...]
  bool levels(std::vector<Level>& out) {
    return array([&] {
      std::string_view a, b;
      Level l{};
      if (!consume('[') || !string(a) || !consume(',') || !string(b) || !consume(']')) return false;
      if (!parse_fixed(a, l.price) || !parse_fixed(b, l.qty)) return false;
      out.push_back(l);
      return true;
    });
  }

 private:
  const char* p_;
  const char* end_;
};

// ------------------------------------------------------------------- messages
struct DepthEvent {
  bool is_depth = false;
  int64_t E = 0, T = 0, U = 0, u = 0, pu = 0;
  std::vector<Level> bids, asks;
  void clear() {
    is_depth = false;
    E = T = U = u = pu = 0;
    bids.clear();
    asks.clear();
  }
};

struct Snapshot {
  int status = 0;
  int64_t lastUpdateId = 0, E = 0, T = 0;
  std::vector<Level> bids, asks;  // best first, as sent by the exchange
};

inline bool depth_field(JsonCursor& c, std::string_view k, DepthEvent& ev) {
  if (k == "e") {
    std::string_view v;
    if (!c.string(v)) return false;
    ev.is_depth = (v == "depthUpdate");
    return true;
  }
  if (k == "E") return c.integer(ev.E);
  if (k == "T") return c.integer(ev.T);
  if (k == "U") return c.integer(ev.U);
  if (k == "u") return c.integer(ev.u);
  if (k == "pu") return c.integer(ev.pu);
  if (k == "b") return c.levels(ev.bids);
  if (k == "a") return c.levels(ev.asks);
  if (k == "data") return c.object([&](std::string_view kk) { return depth_field(c, kk, ev); });
  return c.skip_value();
}

// Handles both the raw payload and the {"stream":..,"data":{..}} envelope.
inline bool parse_depth_event(std::string_view msg, DepthEvent& ev) {
  ev.clear();
  JsonCursor c(msg);
  return c.object([&](std::string_view k) { return depth_field(c, k, ev); });
}

inline bool parse_snapshot(std::string_view msg, Snapshot& s) {
  s = Snapshot{};
  JsonCursor c(msg);
  return c.object([&](std::string_view k) {
    if (k == "status") { int64_t v = 0; if (!c.integer(v)) return false; s.status = int(v); return true; }
    if (k == "result" || k == "data") {
      return c.object([&](std::string_view kk) {
        if (kk == "lastUpdateId") return c.integer(s.lastUpdateId);
        if (kk == "E") return c.integer(s.E);
        if (kk == "T") return c.integer(s.T);
        if (kk == "bids") return c.levels(s.bids);
        if (kk == "asks") return c.levels(s.asks);
        return c.skip_value();
      });
    }
    return c.skip_value();
  });
}

// ----------------------------------------------------------------- order book
constexpr size_t kViewLevels = 1024;  // levels per side published to the UI

struct BookView {
  uint32_t generation = 0;   // bumps when the feed (symbol) is restarted
  uint32_t nb = 0, na = 0;
  int64_t tick = 0;          // detected tick size (1e-8 units)
  int64_t seq = 0;           // publish counter
  int64_t event_time_ms = 0; // exchange E
  int64_t recv_wall_ms = 0;  // local wall clock when the frame was read
  int64_t recv_ns = 0;       // steady clock when the frame was read
  int64_t apply_ns = 0;      // parse + apply duration
  Level bids[kViewLevels];   // best (highest) first
  Level asks[kViewLevels];   // best (lowest) first
};

// Sorted flat vectors with the BEST level at the back: updates near the top of
// the book (the vast majority) touch the tail of the array only.
class OrderBook {
 public:
  void clear() { bids_.clear(); asks_.clear(); }

  void load(const Snapshot& s) {
    clear();
    bids_.reserve(s.bids.size() + 256);
    asks_.reserve(s.asks.size() + 256);
    for (auto it = s.bids.rbegin(); it != s.bids.rend(); ++it) if (it->qty > 0) bids_.push_back(*it);
    for (auto it = s.asks.rbegin(); it != s.asks.rend(); ++it) if (it->qty > 0) asks_.push_back(*it);
    std::sort(bids_.begin(), bids_.end(), bid_before);  // defensive; normally already sorted
    std::sort(asks_.begin(), asks_.end(), ask_before);
  }

  void apply(const std::vector<Level>& bids, const std::vector<Level>& asks) {
    for (const Level& l : bids) upsert(bids_, l, bid_before);
    for (const Level& l : asks) upsert(asks_, l, ask_before);
  }

  size_t bid_count() const { return bids_.size(); }
  size_t ask_count() const { return asks_.size(); }
  const Level* best_bid() const { return bids_.empty() ? nullptr : &bids_.back(); }
  const Level* best_ask() const { return asks_.empty() ? nullptr : &asks_.back(); }
  bool crossed() const { return !bids_.empty() && !asks_.empty() && bids_.back().price >= asks_.back().price; }

  int64_t detect_tick() const {
    int64_t g = 0;
    for (const Level& l : bids_) g = std::gcd(g, l.price);
    for (const Level& l : asks_) g = std::gcd(g, l.price);
    return g > 0 ? g : kScale / 100;
  }

  void fill_view(BookView& v) const {
    const size_t nb = std::min(bids_.size(), kViewLevels);
    const size_t na = std::min(asks_.size(), kViewLevels);
    for (size_t i = 0; i < nb; ++i) v.bids[i] = bids_[bids_.size() - 1 - i];
    for (size_t i = 0; i < na; ++i) v.asks[i] = asks_[asks_.size() - 1 - i];
    v.nb = static_cast<uint32_t>(nb);
    v.na = static_cast<uint32_t>(na);
  }

 private:
  static bool bid_before(const Level& a, const Level& b) { return a.price < b.price; }  // ascending
  static bool ask_before(const Level& a, const Level& b) { return a.price > b.price; }  // descending

  template <class Cmp>
  static void upsert(std::vector<Level>& v, const Level& l, Cmp before) {
    auto it = std::lower_bound(v.begin(), v.end(), l, before);
    const bool found = it != v.end() && it->price == l.price;
    if (l.qty == 0) { if (found) v.erase(it); }
    else if (found) it->qty = l.qty;
    else v.insert(it, l);
  }

  std::vector<Level> bids_;
  std::vector<Level> asks_;
};

// ------------------------------------------------- Binance futures book syncing
// Official procedure (USD-M futures):
//  1. open the stream and buffer events      2. fetch a depth snapshot
//  3. drop events with u < lastUpdateId
//  4. first applied event must satisfy U <= lastUpdateId <= u
//  5. afterwards every event's `pu` must equal the previous event's `u`,
//     otherwise start over from step 2.
class BookSync {
 public:
  enum class Result { Ignored, Applied, Resync };

  void load_snapshot(const Snapshot& s) {
    book_.load(s);
    snap_id_ = s.lastUpdateId;
    last_u_ = 0;
    live_ = false;
  }

  Result on_event(const DepthEvent& ev) {
    if (!live_) {
      if (ev.u < snap_id_) return Result::Ignored;
      if (ev.U <= snap_id_ && ev.u >= snap_id_) {
        book_.apply(ev.bids, ev.asks);
        last_u_ = ev.u;
        live_ = true;
        return Result::Applied;
      }
      return Result::Resync;  // ev.U > lastUpdateId: we missed something
    }
    if (ev.pu != last_u_) return Result::Resync;
    book_.apply(ev.bids, ev.asks);
    last_u_ = ev.u;
    return Result::Applied;
  }

  bool live() const { return live_; }
  OrderBook& book() { return book_; }
  const OrderBook& book() const { return book_; }

 private:
  OrderBook book_;
  int64_t snap_id_ = 0;
  int64_t last_u_ = 0;
  bool live_ = false;
};

// ---------------------------------------------------------------- triple buffer
// Single producer / single consumer, wait-free on both sides. The producer
// always overwrites the newest slot; the consumer always sees the latest frame.
template <class T>
class TripleBuffer {
 public:
  T& write_slot() { return buf_[write_]; }
  void publish() {
    const uint8_t old = middle_.exchange(static_cast<uint8_t>(write_ | kDirty), std::memory_order_acq_rel);
    write_ = old & 3;
  }
  // Consumer: returns true when a newer frame was swapped in.
  bool consume() {
    if (!(middle_.load(std::memory_order_acquire) & kDirty)) return false;
    const uint8_t old = middle_.exchange(read_, std::memory_order_acq_rel);
    read_ = old & 3;
    return true;
  }
  const T& read_slot() const { return buf_[read_]; }

 private:
  static constexpr uint8_t kDirty = 4;
  T buf_[3];
  uint8_t write_ = 0;
  uint8_t read_ = 2;
  std::atomic<uint8_t> middle_{1};
};

// ---------------------------------------------------------------------- clocks
inline int64_t now_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch()).count();
}
inline int64_t wall_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch()).count();
}

}  // namespace dom
