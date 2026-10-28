#include "../src/core.hpp"
#include <cassert>
#include <cstdio>
#include <string>
using namespace dom;

static int g_fail = 0;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #x); ++g_fail; } } while (0)

static DepthEvent ev(int64_t U, int64_t u, int64_t pu,
                     std::vector<Level> b, std::vector<Level> a) {
  DepthEvent e; e.is_depth = true; e.U = U; e.u = u; e.pu = pu; e.bids = std::move(b); e.asks = std::move(a);
  return e;
}
static Level L(double p, double q) { return {int64_t(p * 1e8 + 0.5), int64_t(q * 1e8 + 0.5)}; }

int main() {
  // fixed point
  int64_t v = 0;
  CHECK(parse_fixed("67000.10", v) && v == 6700010000000LL);
  CHECK(parse_fixed("0.00100000", v) && v == 100000);
  CHECK(parse_fixed("12", v) && v == 1200000000LL);
  CHECK(parse_fixed("0.123456789", v) && v == 12345678);
  CHECK(!parse_fixed("", v) && !parse_fixed("1.2x", v) && !parse_fixed(".", v));

  // depth event (raw)
  const char* raw = R"({"e":"depthUpdate","E":123456789,"T":123456788,"s":"BTCUSDT","U":157,"u":160,"pu":149,
    "b":[["67000.10","10.5"],["66999.90","0.000"]],"a":[["67000.20","3.2"]]})";
  DepthEvent e;
  CHECK(parse_depth_event(raw, e));
  CHECK(e.is_depth && e.E == 123456789 && e.U == 157 && e.u == 160 && e.pu == 149);
  CHECK(e.bids.size() == 2 && e.asks.size() == 1);
  CHECK(e.bids[0].price == 6700010000000LL && e.bids[0].qty == 1050000000LL && e.bids[1].qty == 0);

  // depth event inside a combined-stream envelope + non-depth message
  std::string env = std::string("{\"stream\":\"btcusdt@depth\",\"data\":") + raw + "}";
  CHECK(parse_depth_event(env, e) && e.is_depth && e.u == 160);
  CHECK(parse_depth_event(R"({"result":null,"id":1})", e) && !e.is_depth);
  CHECK(!parse_depth_event("{\"e\":", e));

  // snapshot (WebSocket API response)
  const char* snap = R"({"id":"s1","status":200,"result":{"lastUpdateId":1027024,"E":1589436922972,"T":1589436922959,
    "bids":[["4.00","431.0"],["3.90","2"]],"asks":[["4.10","12"],["4.20","7"]]},
    "rateLimits":[{"rateLimitType":"REQUEST_WEIGHT","interval":"MINUTE","intervalNum":1,"limit":2400,"count":20}]})";
  Snapshot s;
  CHECK(parse_snapshot(snap, s) && s.status == 200 && s.lastUpdateId == 1027024);
  CHECK(s.bids.size() == 2 && s.asks.size() == 2 && s.bids[0].price == 400000000LL);
  CHECK(parse_snapshot(R"({"id":"x","status":429,"error":{"code":-1003,"msg":"too many"}})", s) && s.status == 429);

  // book + sync state machine
  Snapshot sn; sn.lastUpdateId = 100;
  sn.bids = {L(10.0, 1), L(9.9, 2), L(9.8, 3)};
  sn.asks = {L(10.1, 1), L(10.2, 2), L(10.3, 3)};
  BookSync sync;
  sync.load_snapshot(sn);
  CHECK(sync.book().best_bid()->price == L(10.0, 0).price);
  CHECK(sync.book().best_ask()->price == L(10.1, 0).price);
  CHECK(sync.book().detect_tick() == L(0.1, 0).price);

  CHECK(sync.on_event(ev(90, 99, 89, {L(10.0, 9)}, {})) == BookSync::Result::Ignored);       // stale
  CHECK(sync.book().best_bid()->qty == L(0, 1).qty);
  CHECK(sync.on_event(ev(95, 105, 99, {L(10.0, 5), L(10.05, 4)}, {L(10.1, 0)})) == BookSync::Result::Applied);
  CHECK(sync.live());
  CHECK(sync.book().best_bid()->price == L(10.05, 0).price);                                // new best bid
  CHECK(sync.book().best_ask()->price == L(10.2, 0).price);                                 // best ask removed
  CHECK(sync.on_event(ev(106, 110, 105, {L(10.05, 0)}, {L(10.15, 1)})) == BookSync::Result::Applied);
  CHECK(sync.book().best_bid()->price == L(10.0, 0).price && sync.book().best_bid()->qty == L(0, 5).qty);
  CHECK(sync.book().best_ask()->price == L(10.15, 0).price);
  CHECK(!sync.book().crossed());
  CHECK(sync.on_event(ev(112, 115, 111, {}, {})) == BookSync::Result::Resync);               // pu gap

  BookSync gap; gap.load_snapshot(sn);
  CHECK(gap.on_event(ev(150, 160, 149, {}, {})) == BookSync::Result::Resync);                // U > lastUpdateId

  // view ordering
  BookView* view = new BookView();
  sync.book().fill_view(*view);
  CHECK(view->nb == 3 && view->na == 3);
  CHECK(view->bids[0].price > view->bids[1].price && view->asks[0].price < view->asks[1].price);
  delete view;

  // triple buffer
  TripleBuffer<int> tb;
  CHECK(!tb.consume());
  tb.write_slot() = 1; tb.publish();
  tb.write_slot() = 2; tb.publish();
  CHECK(tb.consume() && tb.read_slot() == 2);
  CHECK(!tb.consume());
  tb.write_slot() = 3; tb.publish();
  CHECK(tb.consume() && tb.read_slot() == 3);

  std::printf(g_fail ? "%d FAILED\n" : "all tests passed\n", g_fail);
  return g_fail ? 1 : 0;
}
