// main.cpp - minimalist old-school DOM / price ladder on Dear ImGui + GLFW + OpenGL3.
//
// Latency design
//   * network thread publishes complete frames into a wait-free triple buffer and
//     wakes the UI with glfwPostEmptyEvent(): the UI sleeps in glfwWaitEvents and
//     renders the moment a frame lands (no polling, no fixed frame cap, ~0% idle CPU)
//   * the ladder is drawn straight into the ImDrawList (no per-row widgets)
//   * vsync is OFF by default (toggle in the UI) so a new frame is never held back
//     until the next refresh
#include "core.hpp"
#include "feed.hpp"

#include <GLFW/glfw3.h>
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

using namespace dom;

namespace {

constexpr int kMults[] = {1, 2, 5, 10, 20, 50, 100, 200, 500, 1000};
constexpr const char* kMultLabels[] = {"x1", "x2", "x5", "x10", "x20", "x50", "x100", "x200", "x500", "x1000"};
constexpr const char* kSpeedLabels[] = {"100ms", "250ms", "500ms"};
constexpr const char* kSpeedSuffix[] = {"@100ms", "", "@500ms"};

struct UiState {
  char symbol[24] = "BTCUSDT";
  int speed_idx = 0;
  int group_idx = 0;
  bool auto_center = true;
  bool vsync = false;
  int jump = 0;  // pending request: 1 = centre on largest bid level, 2 = largest ask level
  float cluster_thr = 3.0f;  // highlight rows >= thr x median row size
  float scale = 1.0f;

  int64_t center = 0;
  uint32_t seen_generation = 0;
  uint32_t expected_generation = 0;

  // stats
  int64_t last_seq = 0;
  double last_stat_time = 0.0;
  int updates_in_window = 0;
  double upd_per_s = 0.0;
  double lat_us = 0.0, lat_ema_us = 0.0, lat_max_us = 0.0;

  // scratch (kept to avoid per-frame allocation)
  std::vector<double> bid_q, ask_q, tmp;
  std::vector<float> bid_s, ask_s;
};

inline int64_t floor_div(int64_t a, int64_t b) {
  int64_t q = a / b;
  if ((a % b != 0) && ((a < 0) != (b < 0))) --q;
  return q;
}

int decimals_for(int64_t step) {
  for (int d = 0; d <= 8; ++d) {
    int64_t m = 1;
    for (int k = 0; k < 8 - d; ++k) m *= 10;
    if (step % m == 0) return d;
  }
  return 8;
}

void fmt_price(char* buf, size_t n, int64_t p, int dec) {
  const int64_t whole = p / kScale, frac = p % kScale;
  if (dec == 0) { std::snprintf(buf, n, "%lld", static_cast<long long>(whole)); return; }
  int64_t div = 1;
  for (int k = 0; k < 8 - dec; ++k) div *= 10;
  std::snprintf(buf, n, "%lld.%0*lld", static_cast<long long>(whole), dec, static_cast<long long>(frac / div));
}

void fmt_qty(char* buf, size_t n, double q) {
  if (q >= 1e6) std::snprintf(buf, n, "%.2fM", q / 1e6);
  else if (q >= 1e4) std::snprintf(buf, n, "%.1fK", q / 1e3);
  else if (q >= 100) std::snprintf(buf, n, "%.1f", q);
  else if (q >= 1) std::snprintf(buf, n, "%.2f", q);
  else std::snprintf(buf, n, "%.3f", q);
}

// Median size of a price bucket over the whole published depth of one side.
// This is the baseline "rest of the book" that clusters are measured against.
double side_median(const Level* lv, uint32_t n, int64_t group, std::vector<double>& tmp) {
  tmp.clear();
  if (n == 0) return 0.0;
  int64_t cur = floor_div(lv[0].price, group);
  double acc = 0.0;
  for (uint32_t i = 0; i < n; ++i) {
    const int64_t b = floor_div(lv[i].price, group);
    if (b != cur) { tmp.push_back(acc); cur = b; acc = 0.0; }
    acc += static_cast<double>(lv[i].qty) / kScale;
  }
  if (n < kViewLevels || tmp.empty()) tmp.push_back(acc);  // last bucket is partial when the view is truncated
  const size_t mid = tmp.size() / 2;
  std::nth_element(tmp.begin(), tmp.begin() + static_cast<std::ptrdiff_t>(mid), tmp.end());
  return tmp[mid];
}

// Bucket start price of the largest aggregated level (at the current grouping)
// on one side of the published depth.
int64_t largest_bucket(const Level* lv, uint32_t n, int64_t group) {
  int64_t best_b = floor_div(lv[0].price, group), cur = best_b, acc = 0, best_q = -1;
  for (uint32_t i = 0; i < n; ++i) {
    const int64_t b = floor_div(lv[i].price, group);
    if (b != cur) {
      if (acc > best_q) { best_q = acc; best_b = cur; }
      cur = b;
      acc = 0;
    }
    acc += lv[i].qty;
  }
  if (acc > best_q) best_b = cur;
  return best_b * group;
}

inline float smoothstep01(float t) {
  t = std::min(1.0f, std::max(0.0f, t));
  return t * t * (3.0f - 2.0f * t);
}

inline ImU32 col(int r, int g, int b, int a) { return IM_COL32(r, g, b, a); }
inline int mixc(int a, int b, float t) { return static_cast<int>(a + (b - a) * t); }

// Draws accent strips beside runs of "hot" rows (gaps of one row are bridged).
void draw_cluster_runs(ImDrawList* dl, const std::vector<float>& s, const std::vector<double>& q,
                       double thr_q, int rows, float x0, float x1, float y_top, float row_h, ImU32 color) {
  int i = 0;
  while (i < rows) {
    if (q[i] >= thr_q && s[i] > 0.0f) {
      int last = i;
      for (int j = i + 1; j < rows && j <= last + 2; ++j)
        if (q[j] >= thr_q && s[j] > 0.0f) last = j;
      dl->AddRectFilled(ImVec2(x0, y_top + row_h * i + 1.0f), ImVec2(x1, y_top + row_h * (last + 1) - 1.0f), color);
      i = last + 1;
    } else {
      ++i;
    }
  }
}

void draw_ladder(const BookView& v, UiState& ui) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 p0 = ImGui::GetCursorScreenPos();
  const ImVec2 avail = ImGui::GetContentRegionAvail();
  const float row_h = ImGui::GetTextLineHeight() + 3.0f * ui.scale;
  const float W = avail.x;
  const int rows = std::max(5, static_cast<int>((avail.y - row_h) / row_h));

  if (v.generation != ui.expected_generation || v.nb == 0 || v.na == 0) {
    ImGui::TextDisabled("waiting for order book...");
    return;
  }

  const int64_t group = v.tick * kMults[ui.group_idx];
  const int dec = decimals_for(group);
  const int64_t best_bid = v.bids[0].price, best_ask = v.asks[0].price;
  const int64_t mid_row = floor_div((best_bid + best_ask) / 2, group) * group;

  // ---- viewport: recentre on a new symbol, scroll with wheel, auto-follow
  if (ui.seen_generation != v.generation) {
    ui.seen_generation = v.generation;
    ui.center = mid_row;
    ui.auto_center = true;
  }
  ui.center = floor_div(ui.center, group) * group;
  if (ui.jump != 0) {  // jump to the biggest liquidity pool and stay there (auto-follow off)
    ui.center = largest_bucket(ui.jump == 1 ? v.bids : v.asks, ui.jump == 1 ? v.nb : v.na, group);
    ui.auto_center = false;
    ui.jump = 0;
  }
  if (ImGui::IsWindowHovered()) {
    const float wheel = ImGui::GetIO().MouseWheel;
    if (wheel != 0.0f) {
      ui.center += static_cast<int64_t>(std::lround(wheel * 3.0f)) * group;
      ui.auto_center = false;
    }
  }
  if (ui.auto_center) {
    const int64_t band = std::max(2, rows / 5);  // re-centre once the market leaves the middle 40%
    if (std::llabs(mid_row - ui.center) > band * group) ui.center = mid_row;
  }
  const int64_t top = ui.center + static_cast<int64_t>(rows / 2) * group;

  // ---- aggregate the visible rows
  ui.bid_q.assign(rows, 0.0);
  ui.ask_q.assign(rows, 0.0);
  for (uint32_t i = 0; i < v.nb; ++i) {
    const int64_t idx = (top - floor_div(v.bids[i].price, group) * group) / group;
    if (idx >= 0 && idx < rows) ui.bid_q[static_cast<size_t>(idx)] += static_cast<double>(v.bids[i].qty) / kScale;
  }
  for (uint32_t i = 0; i < v.na; ++i) {
    const int64_t idx = (top - floor_div(v.asks[i].price, group) * group) / group;
    if (idx >= 0 && idx < rows) ui.ask_q[static_cast<size_t>(idx)] += static_cast<double>(v.asks[i].qty) / kScale;
  }

  // ---- large-volume cluster detection (relative to each side's median bucket)
  const double med_b = std::max(side_median(v.bids, v.nb, group, ui.tmp), 1e-12);
  const double med_a = std::max(side_median(v.asks, v.na, group, ui.tmp), 1e-12);
  const float thr = ui.cluster_thr;
  const double thr_qb = med_b * thr, thr_qa = med_a * thr;
  ui.bid_s.assign(rows, 0.0f);
  ui.ask_s.assign(rows, 0.0f);
  double max_q = 1e-12;
  for (int i = 0; i < rows; ++i) {
    max_q = std::max(max_q, std::max(ui.bid_q[i], ui.ask_q[i]));
    ui.bid_s[i] = smoothstep01(static_cast<float>((ui.bid_q[i] / med_b - thr) / (1.5 * thr)));
    ui.ask_s[i] = smoothstep01(static_cast<float>((ui.ask_q[i] / med_a - thr) / (1.5 * thr)));
  }

  // ---- layout
  const float col_w = std::floor(W * 0.36f);
  const float x_bid0 = p0.x, x_px0 = p0.x + col_w, x_px1 = p0.x + W - col_w, x_ask1 = p0.x + W;
  const float y_top = p0.y + row_h;  // first row below the header

  const ImU32 c_grid = col(38, 38, 44, 255), c_dim = col(120, 120, 130, 255), c_text = col(205, 205, 210, 255);
  const ImU32 c_px_bg = col(22, 22, 27, 255);

  dl->AddText(ImVec2(x_bid0 + 4, p0.y + 1), c_dim, "BID");
  { const char* t = "PRICE"; dl->AddText(ImVec2((x_px0 + x_px1 - ImGui::CalcTextSize(t).x) * 0.5f, p0.y + 1), c_dim, t); }
  { const char* t = "ASK"; dl->AddText(ImVec2(x_ask1 - 4 - ImGui::CalcTextSize(t).x, p0.y + 1), c_dim, t); }

  dl->AddRectFilled(ImVec2(x_px0, y_top), ImVec2(x_px1, y_top + row_h * rows), c_px_bg);

  const int64_t bb_row = floor_div(best_bid, group) * group;
  const int64_t ba_row = floor_div(best_ask, group) * group;
  char buf[48];

  for (int i = 0; i < rows; ++i) {
    const float y0 = y_top + row_h * i, y1 = y0 + row_h;
    const int64_t price = top - static_cast<int64_t>(i) * group;

    // best bid / ask markers in the price column
    if (price == bb_row && price == ba_row) dl->AddRectFilled(ImVec2(x_px0, y0), ImVec2(x_px1, y1), col(52, 52, 66, 255));
    else if (price == bb_row) dl->AddRectFilled(ImVec2(x_px0, y0), ImVec2(x_px1, y1), col(22, 70, 56, 255));
    else if (price == ba_row) dl->AddRectFilled(ImVec2(x_px0, y0), ImVec2(x_px1, y1), col(84, 32, 34, 255));

    dl->AddLine(ImVec2(x_bid0, y1), ImVec2(x_ask1, y1), c_grid);

    // bid side: bar grows left from the price column, number sits next to the price
    const double bq = ui.bid_q[i];
    if (bq > 0.0) {
      const float s = ui.bid_s[i];
      const float len = std::sqrt(static_cast<float>(bq / max_q)) * (col_w - 2.0f);
      dl->AddRectFilled(ImVec2(x_px0 - len, y0 + 1), ImVec2(x_px0, y1 - 1),
                        col(mixc(30, 70, s), mixc(150, 210, s), mixc(112, 160, s), static_cast<int>(70 + 120 * s)));
      fmt_qty(buf, sizeof buf, bq);
      const ImU32 tc = s > 0.0f ? col(mixc(205, 235, s), 255, mixc(210, 245, s), 255) : c_text;
      dl->AddText(ImVec2(x_px0 - 4 - ImGui::CalcTextSize(buf).x, y0 + 1.5f * ui.scale), tc, buf);
    }
    // ask side: mirrored
    const double aq = ui.ask_q[i];
    if (aq > 0.0) {
      const float s = ui.ask_s[i];
      const float len = std::sqrt(static_cast<float>(aq / max_q)) * (col_w - 2.0f);
      dl->AddRectFilled(ImVec2(x_px1, y0 + 1), ImVec2(x_px1 + len, y1 - 1),
                        col(mixc(176, 235, s), mixc(64, 110, s), mixc(66, 112, s), static_cast<int>(70 + 120 * s)));
      fmt_qty(buf, sizeof buf, aq);
      const ImU32 tc = s > 0.0f ? col(255, mixc(205, 232, s), mixc(205, 232, s), 255) : c_text;
      dl->AddText(ImVec2(x_px1 + 4, y0 + 1.5f * ui.scale), tc, buf);
    }

    fmt_price(buf, sizeof buf, price, dec);
    dl->AddText(ImVec2((x_px0 + x_px1 - ImGui::CalcTextSize(buf).x) * 0.5f, y0 + 1.5f * ui.scale), c_text, buf);
  }

  // thin accent strips on the outer edge of every cluster (bridged over 1-row gaps)
  const float strip = 2.0f * ui.scale;
  draw_cluster_runs(dl, ui.bid_s, ui.bid_q, thr_qb, rows, x_bid0, x_bid0 + strip, y_top, row_h, col(120, 255, 205, 150));
  draw_cluster_runs(dl, ui.ask_s, ui.ask_q, thr_qa, rows, x_ask1 - strip, x_ask1, y_top, row_h, col(255, 150, 150, 150));

  dl->AddLine(ImVec2(x_px0, y_top), ImVec2(x_px0, y_top + row_h * rows), c_grid);
  dl->AddLine(ImVec2(x_px1, y_top), ImVec2(x_px1, y_top + row_h * rows), c_grid);

  // tooltip: size relative to the rest of the book
  if (ImGui::IsWindowHovered()) {
    const ImVec2 m = ImGui::GetMousePos();
    const int r = static_cast<int>(std::floor((m.y - y_top) / row_h));
    if (r >= 0 && r < rows && m.x >= x_bid0 && m.x <= x_ask1) {
      const bool bid_side = m.x < (x_px0 + x_px1) * 0.5f;
      const double q = bid_side ? ui.bid_q[r] : ui.ask_q[r];
      if (q > 0.0) {
        fmt_price(buf, sizeof buf, top - static_cast<int64_t>(r) * group, dec);
        ImGui::SetTooltip("%s %s  %.4f  (%.1fx median)", bid_side ? "bid" : "ask", buf, q,
                          q / (bid_side ? med_b : med_a));
      }
    }
  }

  ImGui::Dummy(ImVec2(W, row_h * (rows + 1)));
}

void apply_style(float scale) {
  ImGui::StyleColorsClassic();
  ImGuiStyle& st = ImGui::GetStyle();
  st.WindowRounding = st.ChildRounding = st.FrameRounding = st.GrabRounding = st.PopupRounding = 0.0f;
  st.ScrollbarRounding = st.TabRounding = 0.0f;
  st.WindowBorderSize = 0.0f;
  st.FrameBorderSize = 1.0f;
  st.ItemSpacing = ImVec2(6, 3);
  st.WindowPadding = ImVec2(6, 6);
  ImVec4* c = st.Colors;
  c[ImGuiCol_WindowBg] = ImVec4(0.045f, 0.045f, 0.055f, 1.0f);
  c[ImGuiCol_ChildBg] = ImVec4(0.045f, 0.045f, 0.055f, 1.0f);
  c[ImGuiCol_FrameBg] = ImVec4(0.10f, 0.10f, 0.12f, 1.0f);
  c[ImGuiCol_Border] = ImVec4(0.22f, 0.22f, 0.27f, 1.0f);
  c[ImGuiCol_Button] = ImVec4(0.14f, 0.14f, 0.18f, 1.0f);
  c[ImGuiCol_ButtonHovered] = ImVec4(0.22f, 0.22f, 0.28f, 1.0f);
  c[ImGuiCol_CheckMark] = ImVec4(0.45f, 0.85f, 0.65f, 1.0f);
  c[ImGuiCol_SliderGrab] = ImVec4(0.45f, 0.85f, 0.65f, 1.0f);
  st.ScaleAllSizes(scale);
}

const char* status_text(int s) {
  switch (static_cast<FeedStatus>(s)) {
    case FeedStatus::Connecting: return "connecting";
    case FeedStatus::Syncing: return "syncing";
    case FeedStatus::Live: return "LIVE";
    case FeedStatus::Reconnecting: return "reconnecting";
    default: return "stopped";
  }
}

void glfw_error(int code, const char* msg) { std::fprintf(stderr, "GLFW error %d: %s\n", code, msg); }

}  // namespace

int main() {
  glfwSetErrorCallback(glfw_error);
  if (!glfwInit()) return 1;
#if defined(__APPLE__)
  const char* glsl = "#version 150";
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
  glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#else
  const char* glsl = "#version 330";
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#endif
  GLFWwindow* window = glfwCreateWindow(460, 860, "DOM - Binance USD-M Futures", nullptr, nullptr);
  if (!window) { glfwTerminate(); return 1; }
  glfwMakeContextCurrent(window);

  UiState ui;
  float sx = 1.0f, sy = 1.0f;
  glfwGetWindowContentScale(window, &sx, &sy);
  ui.scale = std::max(1.0f, sx);

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  ImFontConfig fc;  // the built-in ProggyClean bitmap font: the old-school look, crisp at 13px multiples
  fc.SizePixels = 13.0f * ui.scale;
  fc.OversampleH = fc.OversampleV = 1;
  fc.PixelSnapH = true;
  io.Fonts->AddFontDefault(&fc);
  apply_style(ui.scale);
  ImGui_ImplGlfw_InitForOpenGL(window, true);
  ImGui_ImplOpenGL3_Init(glsl);

  auto shared = std::make_unique<Shared>();
  shared->wake = [] { glfwPostEmptyEvent(); };  // thread-safe per GLFW docs
  std::unique_ptr<Feed> feed;
  uint32_t generation = 0;
  auto start_feed = [&] {
    feed.reset();  // joins the old network thread
    ui.expected_generation = ++generation;
    feed = std::make_unique<Feed>(*shared, ui.symbol, kSpeedSuffix[ui.speed_idx], generation);
  };
  start_feed();

  glfwSwapInterval(ui.vsync ? 1 : 0);
  bool applied_vsync = ui.vsync;

  while (!glfwWindowShouldClose(window)) {
    glfwWaitEventsTimeout(0.05);  // wakes instantly on input or on a published frame
    const bool fresh = shared->buf.consume();
    const BookView& v = shared->buf.read_slot();

    if (ui.vsync != applied_vsync) { glfwSwapInterval(ui.vsync ? 1 : 0); applied_vsync = ui.vsync; }

    // updates/s
    if (fresh) ++ui.updates_in_window;
    const double now_s = glfwGetTime();
    if (now_s - ui.last_stat_time >= 1.0) {
      ui.upd_per_s = ui.updates_in_window / (now_s - ui.last_stat_time);
      ui.updates_in_window = 0;
      ui.last_stat_time = now_s;
    }

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("##root", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);

    // ---- controls row 1: symbol, speed, status
    bool restart = false;
    ImGui::SetNextItemWidth(110.0f * ui.scale);
    if (ImGui::InputText("##sym", ui.symbol, sizeof ui.symbol,
                         ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CharsUppercase |
                             ImGuiInputTextFlags_CharsNoBlank))
      restart = true;
    ImGui::SameLine();
    if (ImGui::Button("Go")) restart = true;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(70.0f * ui.scale);
    if (ImGui::Combo("##speed", &ui.speed_idx, kSpeedLabels, IM_ARRAYSIZE(kSpeedLabels))) restart = true;
    ImGui::SameLine();
    {
      const int st = shared->status.load(std::memory_order_relaxed);
      const ImVec4 colr = st == static_cast<int>(FeedStatus::Live)
                              ? ImVec4(0.35f, 0.85f, 0.55f, 1.0f)
                              : (st == static_cast<int>(FeedStatus::Reconnecting) ? ImVec4(0.95f, 0.35f, 0.35f, 1.0f)
                                                                                  : ImVec4(0.95f, 0.75f, 0.30f, 1.0f));
      ImGui::TextColored(colr, "%s", status_text(st));
      if (st == static_cast<int>(FeedStatus::Reconnecting) && ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", shared->error().c_str());
    }
    if (restart && ui.symbol[0] != '\0') start_feed();

    // ---- controls row 2: grouping, centering, cluster threshold, vsync
    ImGui::SetNextItemWidth(70.0f * ui.scale);
    ImGui::Combo("##group", &ui.group_idx, kMultLabels, IM_ARRAYSIZE(kMultLabels));
    ImGui::SameLine();
    ImGui::Checkbox("auto", &ui.auto_center);
    ImGui::SameLine();
    if (ImGui::Button("Center") || (!io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_C, false))) {
      ui.auto_center = true;
      ui.seen_generation = 0;  // forces a recentre on the next draw
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80.0f * ui.scale);
    ImGui::SliderFloat("##thr", &ui.cluster_thr, 1.5f, 10.0f, "cl>%.1fx");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("highlight levels at least this many times the median level size");
    ImGui::SameLine();
    ImGui::Checkbox("vsync", &ui.vsync);

    // ---- controls row 3: jump to the largest liquidity pool on each side
    if (ImGui::Button("Max Bid [B]") || (!io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_B, false))) ui.jump = 1;
    ImGui::SameLine();
    if (ImGui::Button("Max Ask [A]") || (!io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_A, false))) ui.jump = 2;
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("centre on the largest level at the current grouping (within the 1024 published levels);\n"
                        "auto-follow turns off - press Center / C to follow the market again");

    // ---- ladder
    const float footer_h = ImGui::GetTextLineHeightWithSpacing() * 2.0f + 4.0f * ui.scale;
    ImGui::BeginChild("##ladder", ImVec2(0, -footer_h), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    draw_ladder(v, ui);
    ImGui::EndChild();

    // ---- footer: spread + latency
    if (v.generation == ui.expected_generation && v.nb && v.na) {
      char sp[32];
      fmt_price(sp, sizeof sp, v.asks[0].price - v.bids[0].price, decimals_for(v.tick));
      ImGui::TextDisabled("spread %s | tick %.8g | seq %lld | %.0f upd/s", sp, double(v.tick) / kScale,
                          static_cast<long long>(v.seq), ui.upd_per_s);
      ImGui::TextDisabled("feed ~%lld ms | recv>draw %.0f us (avg %.0f, max %.0f) | parse+apply %.0f us | resync %lld",
                          static_cast<long long>(v.recv_wall_ms - v.event_time_ms), ui.lat_us, ui.lat_ema_us,
                          ui.lat_max_us, v.apply_ns / 1000.0,
                          static_cast<long long>(shared->resyncs.load(std::memory_order_relaxed)));
    }
    ImGui::End();

    ImGui::Render();
    int fbw = 0, fbh = 0;
    glfwGetFramebufferSize(window, &fbw, &fbh);
    glViewport(0, 0, fbw, fbh);
    glClearColor(0.045f, 0.045f, 0.055f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    if (fresh && v.generation == ui.expected_generation) {  // network read -> draw calls submitted
      ui.lat_us = static_cast<double>(now_ns() - v.recv_ns) / 1000.0;
      ui.lat_ema_us = ui.lat_ema_us == 0.0 ? ui.lat_us : ui.lat_ema_us * 0.95 + ui.lat_us * 0.05;
      ui.lat_max_us = std::max(ui.lat_max_us * 0.999, ui.lat_us);
    }
    glfwSwapBuffers(window);
  }

  feed.reset();
  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplGlfw_Shutdown();
  ImGui::DestroyContext();
  glfwDestroyWindow(window);
  glfwTerminate();
  return 0;
}