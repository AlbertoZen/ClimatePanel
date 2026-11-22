#include "heating_controller.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace esphome::heating_controller {

using tessera::Font;
namespace ui = tessera::ui;

namespace {

constexpr uint8_t MAX_MODES = 7;
constexpr uint16_t HISTORY_POINTS = 24;
constexpr uint16_t MAX_HISTORY_SAMPLES = 4096;
constexpr uint16_t MAX_HISTORY_CHUNKS = 168;
constexpr uint8_t HISTORY_PERIOD_COUNT = 3;
constexpr uint32_t ONE_HOUR = 3600U;

void copy_text(char *target, size_t size, const char *source) {
  if (!size) return;
  if (!source) source = "";
  std::snprintf(target, size, "%s", source);
}

float number_of(JsonVariantConst value, float fallback = NAN) {
  if (value.isNull() || value.is<bool>()) return fallback;
  if (value.is<float>() || value.is<int>() || value.is<long>() || value.is<double>())
    return value.as<float>();
  if (value.is<const char *>()) {
    char *end = nullptr;
    const char *source = value.as<const char *>();
    const float result = std::strtof(source, &end);
    return end != source && std::isfinite(result) ? result : fallback;
  }
  return fallback;
}

bool valid_number(float value) { return std::isfinite(value); }

void format_number(char *buffer, size_t size, float value) {
  if (!valid_number(value)) copy_text(buffer, size, "--");
  else std::snprintf(buffer, size, "%.1f", value);
}

void utc_text(uint32_t epoch, char *buffer, size_t size) {
  const time_t stamp = static_cast<time_t>(epoch);
  struct tm utc {};
  gmtime_r(&stamp, &utc);
  std::strftime(buffer, size, "%Y-%m-%dT%H:%M:%SZ", &utc);
}

void title_case(const char *source, char *target, size_t size) {
  copy_text(target, size, source);
  if (target[0] >= 'a' && target[0] <= 'z') target[0] = static_cast<char>(target[0] - 'a' + 'A');
  for (size_t i = 1; target[i]; ++i) {
    if (target[i] == '_' || target[i] == '-') target[i] = ' ';
  }
}

uint32_t mode_icon(const char *mode) {
  if (!mode) return 0xF0393;  // thermostat
  if (std::strcmp(mode, "off") == 0) return 0xF0425;       // power
  if (std::strcmp(mode, "heat") == 0) return 0xF0238;      // fire
  if (std::strcmp(mode, "cool") == 0) return 0xF0717;      // snowflake
  if (std::strcmp(mode, "heat_cool") == 0) return 0xF1B17; // thermostat-auto
  if (std::strcmp(mode, "auto") == 0) return 0xF006A;      // autorenew
  if (std::strcmp(mode, "dry") == 0) return 0xF058E;       // water-percent
  if (std::strcmp(mode, "fan_only") == 0) return 0xF0210;  // fan
  return 0xF0393;                                           // thermostat
}

uint8_t history_activity(JsonObjectConst state) {
  const JsonObjectConst attributes = state["a"].as<JsonObjectConst>();
  const char *action = attributes["hvac_action"] | "";
  if (std::strcmp(action, "heating") == 0) return 1;
  if (std::strcmp(action, "cooling") == 0) return 2;
  if (std::strcmp(action, "fan") == 0 || std::strcmp(action, "drying") == 0) return 3;
  return 0;
}

struct Room {
  char entity[80] = {};
  char name[48] = {};
  char unit[8] = "°C";
  char mode[24] = {};
  char action[24] = {};
  char modes[MAX_MODES][24] = {};
  uint8_t mode_count = 0;
  float current = NAN;
  float target = NAN;
  float low = NAN;
  float high = NAN;
  float minimum = 5.0f;
  float maximum = 35.0f;
  float step = 0.5f;
  float humidity = NAN;
};

struct HistorySample {
  uint32_t time = 0;
  float temperature = NAN;
  float target = NAN;
  float low = NAN;
  float high = NAN;
  uint8_t activity = 0;  // 0 idle/off, 1 heating, 2 cooling, 3 fan/drying
};

}  // namespace

class HistoryCard : public tessera::Card {
 public:
  explicit HistoryCard(HeatingController *plugin) : plugin_(plugin) {}
  ~HistoryCard() override { plugin_->history_closed(this); }

  void open(const tessera::CardContext &context) override {
    copy_text(entity_, sizeof(entity_), context.entity);
    width_ = context.width;
    height_ = context.height;
    plugin_->history_opened(this);

    title_ = ui::label(context.parent, Font::TITLE, theme::INK);
    ui::set_text(title_, plugin_->text("history_title"));
    lv_obj_set_pos(title_, ui::px(8), ui::px(3));
    lv_obj_set_width(title_, width_ - ui::px(16));

    for (uint8_t i = 0; i < HISTORY_PERIOD_COUNT; ++i) {
      period_background_[i] = ui::block(context.parent, theme::BUTTON);
      period_label_[i] = ui::label(context.parent, Font::BODY, theme::INK);
      lv_obj_set_style_radius(period_background_[i], ui::px(8), 0);
      lv_obj_add_flag(period_background_[i], LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(period_label_[i], LV_OBJ_FLAG_HIDDEN);
    }
    period_y_ = ui::px(34);
    period_height_ = ui::px(32);
    period_width_ = ui::px(64);
    period_gap_ = ui::px(10);
    period_start_x_ = (width_ - (HISTORY_PERIOD_COUNT * period_width_ +
                      (HISTORY_PERIOD_COUNT - 1) * period_gap_)) / 2;

    room_legend_ = ui::label(context.parent, Font::BODY, theme::ACCENT);
    ui::set_text(room_legend_, plugin_->text("history_room_temperature"));
    lv_obj_set_pos(room_legend_, ui::px(8), ui::px(73));
    lv_obj_set_width(room_legend_, ui::px(130));

    target_legend_ = ui::label(context.parent, Font::BODY, theme::MUTED);
    ui::set_text(target_legend_, plugin_->text("history_target_temperature"));
    lv_obj_set_pos(target_legend_, ui::px(140), ui::px(73));
    lv_obj_set_width(target_legend_, ui::px(132));

    heat_legend_ = ui::label(context.parent, Font::BODY, theme::MARK_AMBER);
    ui::set_text(heat_legend_, plugin_->text("history_heat_legend"));
    lv_obj_set_pos(heat_legend_, ui::px(286), ui::px(73));
    lv_obj_set_width(heat_legend_, ui::px(76));

    cool_legend_ = ui::label(context.parent, Font::BODY, theme::MARK_BLUE);
    ui::set_text(cool_legend_, plugin_->text("history_cool_legend"));
    lv_obj_set_pos(cool_legend_, ui::px(365), ui::px(73));
    lv_obj_set_width(cool_legend_, ui::px(68));

    status_ = ui::label(context.parent, Font::BODY, theme::MUTED);
    lv_obj_set_width(status_, width_ - ui::px(20));
    lv_obj_set_style_text_align(status_, LV_TEXT_ALIGN_CENTER, 0);
    ui::set_text(status_, plugin_->text("history_loading"));
    lv_obj_set_pos(status_, ui::px(10), height_ / 2 - ui::line_height(Font::BODY) / 2);

    build_plot(context.parent);
    paint_periods();
    start_history(24);
  }

  void on_tap() override {
    const tessera::Point point = tessera::tap_point();
    if (point.x < 0 || point.y < period_y_ || point.y > period_y_ + period_height_) return;

    for (uint8_t i = 0; i < HISTORY_PERIOD_COUNT; ++i) {
      const int x = period_start_x_ + i * (period_width_ + period_gap_);
      if (point.x >= x && point.x <= x + period_width_) {
        const uint16_t selected = i == 0 ? 1 : i == 1 ? 24 : 168;
        if (selected != period_hours_) start_history(selected);
        return;
      }
    }
  }

  void on_message(JsonObjectConst message) {
    const uint32_t request_id = message["re"] | 0u;
    if (!active_request_id_ || request_id != active_request_id_) return;
    active_request_id_ = 0;

    if (!(message["ok"] | false)) {
      failed_ = true;
    } else {
      // Recorder must include attribute-only changes: hvac_action and temperatures
      // often change while the climate entity's main state (heat/off) stays the same.
      const JsonObjectConst result = message["result"].as<JsonObjectConst>();
      const JsonArrayConst states = result[entity_].as<JsonArrayConst>();
      for (JsonVariantConst item : states) {
        const JsonObjectConst state = item.as<JsonObjectConst>();
        const uint32_t timestamp = static_cast<uint32_t>(number_of(state["lu"], 0));
        if (!timestamp || sample_count_ >= MAX_HISTORY_SAMPLES) continue;

        const JsonObjectConst attributes = state["a"].as<JsonObjectConst>();
        HistorySample sample;
        sample.time = timestamp;
        sample.temperature = number_of(attributes["current_temperature"]);
        sample.target = number_of(attributes["temperature"]);
        sample.low = number_of(attributes["target_temp_low"]);
        sample.high = number_of(attributes["target_temp_high"]);
        sample.activity = history_activity(state);
        samples_[sample_count_++] = sample;
      }
    }

    send_attempts_ = 0;
    ++completed_chunks_;
    if (completed_chunks_ >= expected_chunks_) {
      render_plot();
    } else {
      send_next_chunk();
    }
    update_status();
  }

  void on_tick(uint32_t) override {
    // If the host cannot accept a request yet, retry the same hour next second.
    if (expected_chunks_ && completed_chunks_ < expected_chunks_ && !active_request_id_)
      send_next_chunk();
  }

  void on_theme() override {
    if (title_) ui::set_color(title_, theme::INK);
    if (room_legend_) ui::set_color(room_legend_, theme::ACCENT);
    if (target_legend_) ui::set_color(target_legend_, theme::MUTED);
    if (heat_legend_) ui::set_color(heat_legend_, theme::MARK_AMBER);
    if (cool_legend_) ui::set_color(cool_legend_, theme::MARK_BLUE);
    if (status_) ui::set_color(status_, theme::MUTED);
    paint_periods();
    for (lv_obj_t *label : axis_labels_) if (label) ui::set_color(label, theme::SUBTLE);
    if (axis_start_) ui::set_color(axis_start_, theme::SUBTLE);
    if (axis_mid_) ui::set_color(axis_mid_, theme::SUBTLE);
    if (axis_now_) ui::set_color(axis_now_, theme::SUBTLE);
    for (lv_obj_t *line : grid_lines_) {
      if (line) lv_obj_set_style_line_color(line, ui::color(theme::GRID), 0);
    }
    if (room_line_) lv_obj_set_style_line_color(room_line_, ui::color(theme::ACCENT), 0);
    if (target_line_) lv_obj_set_style_line_color(target_line_, ui::color(theme::MUTED), 0);
    if (activity_background_ && sample_count_) draw_activity_bands();
  }

 private:
  void paint_periods() {
    static const char *keys[HISTORY_PERIOD_COUNT] = {
      "history_period_1h", "history_period_24h", "history_period_1w"
    };
    if (!period_background_[0]) return;
    for (uint8_t i = 0; i < HISTORY_PERIOD_COUNT; ++i) {
      const int x = period_start_x_ + i * (period_width_ + period_gap_);
      const bool selected = (i == 0 && period_hours_ == 1) ||
                            (i == 1 && period_hours_ == 24) ||
                            (i == 2 && period_hours_ == 168);
      lv_obj_set_pos(period_background_[i], x, period_y_);
      lv_obj_set_size(period_background_[i], period_width_, period_height_);
      lv_obj_set_pos(period_label_[i], x, period_y_ + ui::px(6));
      lv_obj_set_width(period_label_[i], period_width_);
      lv_obj_set_style_text_align(period_label_[i], LV_TEXT_ALIGN_CENTER, 0);
      lv_obj_set_style_bg_color(period_background_[i],
                                ui::color(selected ? theme::ACCENT : theme::BUTTON), 0);
      ui::set_color(period_label_[i], selected ? theme::ON_ACCENT : theme::INK);
      ui::set_text(period_label_[i], plugin_->text(keys[i]));
      lv_obj_clear_flag(period_background_[i], LV_OBJ_FLAG_HIDDEN);
      lv_obj_clear_flag(period_label_[i], LV_OBJ_FLAG_HIDDEN);
    }
  }

  void build_plot(lv_obj_t *parent) {
    const int left = ui::px(42);
    const int right = ui::px(12);
    plot_x_ = left;
    plot_y_ = ui::px(105);
    plot_w_ = std::max(20, width_ - left - right);
    plot_h_ = std::max(40, height_ - plot_y_ - ui::px(52));

    activity_background_ = lv_obj_create(parent);
    lv_obj_remove_style_all(activity_background_);
    lv_obj_set_pos(activity_background_, plot_x_, plot_y_);
    lv_obj_set_size(activity_background_, plot_w_, plot_h_);
    lv_obj_set_style_bg_opa(activity_background_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(activity_background_, 0, 0);
    lv_obj_set_style_pad_all(activity_background_, 0, 0);
    lv_obj_remove_flag(activity_background_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(activity_background_, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < 5; ++i) {
      grid_points_[i][0] = {0, static_cast<lv_value_precise_t>(i * plot_h_ / 4)};
      grid_points_[i][1] = {static_cast<lv_value_precise_t>(plot_w_),
                            static_cast<lv_value_precise_t>(i * plot_h_ / 4)};
      grid_lines_[i] = lv_line_create(parent);
      lv_obj_remove_flag(grid_lines_[i], LV_OBJ_FLAG_CLICKABLE);
      lv_obj_set_style_line_width(grid_lines_[i], 1, 0);
      lv_obj_set_style_line_color(grid_lines_[i], ui::color(theme::GRID), 0);
      lv_line_set_points(grid_lines_[i], grid_points_[i], 2);
      lv_obj_set_pos(grid_lines_[i], plot_x_, plot_y_);
    }

    room_line_ = lv_line_create(parent);
    lv_obj_remove_flag(room_line_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_line_width(room_line_, ui::px(3), 0);
    lv_obj_set_style_line_rounded(room_line_, true, 0);
    lv_obj_set_style_line_color(room_line_, ui::color(theme::ACCENT), 0);
    lv_obj_set_pos(room_line_, plot_x_, plot_y_);

    target_line_ = lv_line_create(parent);
    lv_obj_remove_flag(target_line_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_line_width(target_line_, ui::px(2), 0);
    lv_obj_set_style_line_rounded(target_line_, true, 0);
    lv_obj_set_style_line_color(target_line_, ui::color(theme::MUTED), 0);
    lv_obj_set_pos(target_line_, plot_x_, plot_y_);

    for (int i = 0; i < 5; ++i) {
      axis_labels_[i] = ui::label(parent, Font::BODY, theme::SUBTLE);
      lv_obj_set_pos(axis_labels_[i], 0,
                     plot_y_ + i * plot_h_ / 4 - ui::line_height(Font::BODY) / 2);
      lv_obj_set_width(axis_labels_[i], left - ui::px(8));
      lv_obj_set_style_text_align(axis_labels_[i], LV_TEXT_ALIGN_RIGHT, 0);
    }

    axis_start_ = ui::label(parent, Font::BODY, theme::SUBTLE);
    axis_mid_ = ui::label(parent, Font::BODY, theme::SUBTLE);
    axis_now_ = ui::label(parent, Font::BODY, theme::SUBTLE);
    lv_obj_set_pos(axis_start_, plot_x_, plot_y_ + plot_h_ + ui::px(8));
    lv_obj_set_pos(axis_mid_, plot_x_ + plot_w_ / 2 - ui::px(24), plot_y_ + plot_h_ + ui::px(8));
    lv_obj_set_pos(axis_now_, plot_x_ + plot_w_ - ui::px(30), plot_y_ + plot_h_ + ui::px(8));
    lv_obj_set_width(axis_start_, ui::px(74));
    lv_obj_set_width(axis_mid_, ui::px(74));
    lv_obj_set_width(axis_now_, ui::px(60));
    ui::set_text(axis_now_, plugin_->text("history_now"));
    update_axis_labels();
  }

  void update_axis_labels() {
    if (!axis_start_ || !axis_mid_) return;
    if (period_hours_ == 1) {
      ui::set_text(axis_start_, plugin_->text("history_1h_ago"));
      ui::set_text(axis_mid_, plugin_->text("history_30m_ago"));
    } else if (period_hours_ == 24) {
      ui::set_text(axis_start_, plugin_->text("history_24h_ago"));
      ui::set_text(axis_mid_, plugin_->text("history_12h_ago"));
    } else {
      ui::set_text(axis_start_, plugin_->text("history_7d_ago"));
      ui::set_text(axis_mid_, plugin_->text("history_3_5d_ago"));
    }
  }

  void start_history(uint16_t hours) {
    period_hours_ = hours;
    paint_periods();
    update_axis_labels();
    active_request_id_ = 0;  // Ignore late replies from the range we just left.
    next_chunk_ = 0;
    send_attempts_ = 0;
    sample_count_ = 0;
    completed_chunks_ = 0;
    failed_ = false;
    plot_ready_ = false;
    history_end_ = tessera::epoch();
    history_start_ = history_end_ - static_cast<uint32_t>(period_hours_) * ONE_HOUR;
    expected_chunks_ = std::min<uint16_t>(period_hours_, MAX_HISTORY_CHUNKS);
    if (activity_background_) lv_obj_clean(activity_background_);
    if (room_line_) lv_obj_add_flag(room_line_, LV_OBJ_FLAG_HIDDEN);
    if (target_line_) lv_obj_add_flag(target_line_, LV_OBJ_FLAG_HIDDEN);
    if (status_) {
      ui::set_text(status_, plugin_->text("history_loading"));
      lv_obj_clear_flag(status_, LV_OBJ_FLAG_HIDDEN);
    }
    if (!history_end_ || entity_[0] == '\0') {
      failed_ = true;
      completed_chunks_ = expected_chunks_;
      update_status();
      return;
    }
    send_next_chunk();
  }

  void send_next_chunk() {
    if (active_request_id_ || completed_chunks_ >= expected_chunks_ || next_chunk_ >= expected_chunks_)
      return;

    const uint32_t chunk_start = history_start_ + static_cast<uint32_t>(next_chunk_) * ONE_HOUR;
    const uint32_t chunk_end = std::min(history_end_, chunk_start + ONE_HOUR);
    char start_text[32], end_text[32];
    utc_text(chunk_start, start_text, sizeof(start_text));
    utc_text(chunk_end, end_text, sizeof(end_text));

    JsonDocument request;
    request["ask"] = "history/history_during_period";
    JsonObject data = request["data"].to<JsonObject>();
    data["start_time"] = start_text;
    data["end_time"] = end_text;
    data["entity_ids"].to<JsonArray>().add(entity_);
    data["include_start_time_state"] = true;
    data["significant_changes_only"] = false;
    data["minimal_response"] = true;
    data["no_attributes"] = false;

    const uint32_t request_id = tessera::send(plugin_, request.as<JsonObjectConst>());
    if (!request_id) {
      // A full request queue must not silently skip an hour of the week.
      // Retry on the next card tick; after three failed sends, mark this hour
      // unavailable and keep the rest of the history moving.
      if (++send_attempts_ >= 3) {
        failed_ = true;
        send_attempts_ = 0;
        ++next_chunk_;
        ++completed_chunks_;
        if (completed_chunks_ >= expected_chunks_) render_plot();
      }
      update_status();
      return;
    }
    active_request_id_ = request_id;
    ++next_chunk_;
    send_attempts_ = 0;
    update_status();
  }

  void update_status() {
    if (!status_) return;
    if (completed_chunks_ < expected_chunks_) {
      ui::set_text(status_, plugin_->text("history_loading"));
      lv_obj_clear_flag(status_, LV_OBJ_FLAG_HIDDEN);
    } else if (!plot_ready_ || sample_count_ < 2) {
      ui::set_text(status_, plugin_->text(failed_ ? "history_unavailable" : "history_no_data"));
      lv_obj_clear_flag(status_, LV_OBJ_FLAG_HIDDEN);
    } else {
      ui::set_text(status_, failed_ ? plugin_->text("history_partial") : "");
      if (failed_) lv_obj_clear_flag(status_, LV_OBJ_FLAG_HIDDEN);
      else lv_obj_add_flag(status_, LV_OBJ_FLAG_HIDDEN);
    }
  }

  void draw_activity_bands() {
    if (!activity_background_) return;
    lv_obj_clean(activity_background_);
    if (sample_count_ < 2) return;

    const uint32_t finish = history_end_;
    const uint32_t start = history_start_;
    const uint32_t span = std::max<uint32_t>(1, finish - start);

    uint16_t i = 0;
    while (i < sample_count_) {
      const uint8_t action = samples_[i].activity;
      uint16_t next = i + 1;
      while (next < sample_count_ && samples_[next].activity == action) ++next;
      if (action == 1 || action == 2) {
        const uint32_t band_start = std::max(start, samples_[i].time);
        const uint32_t band_end = next < sample_count_ ? std::min(finish, samples_[next].time) : finish;
        if (band_end > band_start) {
          const int raw_x1 = static_cast<int>((static_cast<uint64_t>(band_start - start) * plot_w_) / span);
          const int raw_x2 = static_cast<int>((static_cast<uint64_t>(band_end - start) * plot_w_) / span);
          const int x1 = std::clamp(raw_x1, 0, plot_w_);
          const int x2 = std::clamp(raw_x2, 0, plot_w_);
          const int band_width = x2 - x1;
          if (band_width <= 0) {
            i = next;
            continue;
          }
          lv_obj_t *band = lv_obj_create(activity_background_);
          lv_obj_remove_style_all(band);
          lv_obj_set_pos(band, x1, 0);
          lv_obj_set_size(band, band_width, plot_h_);
          lv_obj_set_style_bg_color(band, ui::color(action == 1 ? theme::MARK_AMBER : theme::MARK_BLUE), 0);
          lv_obj_set_style_bg_opa(band, LV_OPA_20, 0);
          lv_obj_set_style_border_width(band, 0, 0);
          lv_obj_set_style_pad_all(band, 0, 0);
          lv_obj_clear_flag(band, LV_OBJ_FLAG_CLICKABLE);
          lv_obj_clear_flag(band, LV_OBJ_FLAG_SCROLLABLE);
        }
      }
      i = next;
    }
  }

  void render_plot() {
    plot_ready_ = false;
    if (sample_count_ < 2) {
      draw_activity_bands();
      update_status();
      return;
    }

    std::sort(samples_, samples_ + sample_count_, [](const HistorySample &a, const HistorySample &b) {
      return a.time < b.time;
    });
    uint16_t unique_count = 0;
    for (uint16_t i = 0; i < sample_count_; ++i) {
      if (unique_count && samples_[i].time == samples_[unique_count - 1].time)
        samples_[unique_count - 1] = samples_[i];
      else
        samples_[unique_count++] = samples_[i];
    }
    sample_count_ = unique_count;
    draw_activity_bands();

    const uint32_t finish = history_end_;
    const uint32_t start = history_start_;
    const uint32_t span = std::max<uint32_t>(1, finish - start);
    float values_min = 100000.0f;
    float values_max = -100000.0f;
    uint8_t room_count = 0, target_count = 0;

    for (uint16_t i = 0; i < HISTORY_POINTS; ++i) {
      const uint32_t at = start + static_cast<uint32_t>((static_cast<uint64_t>(span) * i) / (HISTORY_POINTS - 1));
      const HistorySample *sample = nullptr;
      for (uint16_t j = 0; j < sample_count_; ++j) {
        if (samples_[j].time <= at) sample = &samples_[j];
        else break;
      }
      if (!sample) continue;

      const float room_value = sample->temperature;
      float target_value = sample->target;
      if (!valid_number(target_value) && valid_number(sample->low) && valid_number(sample->high))
        target_value = (sample->low + sample->high) / 2.0f;

      if (valid_number(room_value)) {
        room_values_[room_count] = {
          static_cast<lv_value_precise_t>(i * (plot_w_ - 1) / (HISTORY_POINTS - 1)), 0
        };
        room_raw_[room_count] = room_value;
        values_min = std::min(values_min, room_value);
        values_max = std::max(values_max, room_value);
        ++room_count;
      }
      if (valid_number(target_value)) {
        target_values_[target_count] = {
          static_cast<lv_value_precise_t>(i * (plot_w_ - 1) / (HISTORY_POINTS - 1)), 0
        };
        target_raw_[target_count] = target_value;
        values_min = std::min(values_min, target_value);
        values_max = std::max(values_max, target_value);
        ++target_count;
      }
    }

    if (room_count < 2 && target_count < 2) {
      update_status();
      return;
    }
    plot_ready_ = true;

    const float padding = std::max(0.5f, (values_max - values_min) * 0.1f);
    values_min -= padding;
    values_max += padding;
    const float range = std::max(0.1f, values_max - values_min);

    for (int i = 0; i < room_count; ++i)
      room_values_[i].y = static_cast<lv_value_precise_t>(
        plot_h_ - 1 - (room_raw_[i] - values_min) * (plot_h_ - 1) / range
      );
    for (int i = 0; i < target_count; ++i)
      target_values_[i].y = static_cast<lv_value_precise_t>(
        plot_h_ - 1 - (target_raw_[i] - values_min) * (plot_h_ - 1) / range
      );

    if (room_count >= 2) {
      lv_line_set_points(room_line_, room_values_, room_count);
      lv_obj_clear_flag(room_line_, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(room_line_, LV_OBJ_FLAG_HIDDEN);
    }
    if (target_count >= 2) {
      lv_line_set_points(target_line_, target_values_, target_count);
      lv_obj_clear_flag(target_line_, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(target_line_, LV_OBJ_FLAG_HIDDEN);
    }

    for (int i = 0; i < 5; ++i) {
      char text[12];
      const float value = values_max - range * i / 4.0f;
      std::snprintf(text, sizeof(text), "%.1f", value);
      ui::set_text(axis_labels_[i], text);
    }
    update_status();
  }

  HeatingController *plugin_;
  char entity_[80] = {};
  uint32_t active_request_id_ = 0;
  uint32_t history_start_ = 0, history_end_ = 0;
  uint16_t expected_chunks_ = 0;
  uint16_t next_chunk_ = 0;
  uint16_t completed_chunks_ = 0;
  uint8_t send_attempts_ = 0;
  uint16_t period_hours_ = 24;
  bool failed_ = false;
  bool plot_ready_ = false;
  int width_ = 0, height_ = 0;
  int plot_x_ = 0, plot_y_ = 0, plot_w_ = 0, plot_h_ = 0;
  int period_y_ = 0, period_height_ = 0, period_width_ = 0, period_gap_ = 0, period_start_x_ = 0;
  uint16_t sample_count_ = 0;
  HistorySample samples_[MAX_HISTORY_SAMPLES] = {};
  lv_obj_t *title_ = nullptr;
  lv_obj_t *period_background_[HISTORY_PERIOD_COUNT] = {};
  lv_obj_t *period_label_[HISTORY_PERIOD_COUNT] = {};
  lv_obj_t *room_legend_ = nullptr;
  lv_obj_t *target_legend_ = nullptr;
  lv_obj_t *heat_legend_ = nullptr;
  lv_obj_t *cool_legend_ = nullptr;
  lv_obj_t *status_ = nullptr;
  lv_obj_t *activity_background_ = nullptr;
  lv_obj_t *grid_lines_[5] = {};
  lv_point_precise_t grid_points_[5][2] = {};
  lv_obj_t *room_line_ = nullptr;
  lv_obj_t *target_line_ = nullptr;
  lv_point_precise_t room_values_[HISTORY_POINTS] = {};
  lv_point_precise_t target_values_[HISTORY_POINTS] = {};
  float room_raw_[HISTORY_POINTS] = {};
  float target_raw_[HISTORY_POINTS] = {};
  lv_obj_t *axis_labels_[5] = {};
  lv_obj_t *axis_start_ = nullptr;
  lv_obj_t *axis_mid_ = nullptr;
  lv_obj_t *axis_now_ = nullptr;
};

class HeatingTile : public tessera::Tile {
 public:
  explicit HeatingTile(HeatingController *plugin) : plugin_(plugin) {}

  void create(const tessera::TileContext &context) override {
    width_ = context.width;
    height_ = context.height;
    tile_ = context.tile;
    copy_text(room_.entity, sizeof(room_.entity), context.entity);
    copy_text(room_.name, sizeof(room_.name), context.name);
    copy_text(humidity_entity_, sizeof(humidity_entity_), context.options["humidity_sensor"] | "");
    copy_text(outside_entity_, sizeof(outside_entity_), context.options["outside_temperature"] | "");
    show_dot_ = context.options["show_current_temp_dot"] | false;
    create_ui(context.parent);
    paint();
  }

  void on_state(JsonObjectConst data) override {
    read_climate(data, room_);
    const JsonObjectConst entities = data["entities"].as<JsonObjectConst>();

    if (humidity_entity_[0]) {
      const JsonObjectConst humidity = entities["humidity_sensor"];
      room_.humidity = number_of(humidity["state"]);
    } else {
      room_.humidity = NAN;
    }

    if (outside_entity_[0]) {
      const JsonObjectConst outside = entities["outside_temperature"];
      outside_temperature_ = number_of(outside["state"]);
      if (!valid_number(outside_temperature_))
        outside_temperature_ = number_of(outside["attributes"]["temperature"]);
    } else {
      outside_temperature_ = NAN;
    }

    if (pending_valid_) {
      const float actual = dual_edit_high_ ? room_.high : dual_edit_low_ ? room_.low : room_.target;
      if (valid_number(actual) && std::fabs(actual - pending_target_) <= std::max(0.1f, room_.step / 2.0f)) {
        pending_valid_ = false;
        pending_sent_ = false;
      } else if (pending_sent_ && millis() - pending_sent_at_ > 8000) {
        pending_valid_ = false;
        pending_sent_ = false;
      }
    }
    paint();
  }

  void on_tick(uint32_t) override {
    if (pending_valid_) {
      if (!pending_sent_ && millis() - last_adjustment_at_ >= 700) {
        send_setpoint();
      } else if (pending_sent_ && millis() - pending_sent_at_ > 8000) {
        pending_valid_ = false;
        pending_sent_ = false;
        paint();
      }
    }
  }

  void on_theme() override { paint(); }

  void on_tap() override {
    const tessera::Point point = tessera::tap_point();
    if (point.x < 0 || point.y < 0 || room_.entity[0] == '\0') return;

    // The two large keys are the only setpoint controls. A tap elsewhere inside the
    // dial opens history, leaving the face itself free of an awkward drag target.
    if (point.y >= stepper_y_ && point.y <= stepper_y_ + stepper_h_) {
      if (point.x >= minus_x_ && point.x <= minus_x_ + stepper_h_) {
        adjust_setpoint(-1.0f);
        return;
      }
      if (point.x >= plus_x_ && point.x <= plus_x_ + stepper_h_) {
        adjust_setpoint(1.0f);
        return;
      }
    }

    if (dual_available_ && point.y >= dual_y_ && point.y <= dual_y_ + ui::px(32)) {
      dual_edit_low_ = point.x < width_ / 2;
      dual_edit_high_ = point.x >= width_ / 2;
      paint();
      return;
    }

    if (room_.mode_count && point.y >= mode_y_ && point.y <= mode_y_ + mode_height_) {
      const int relative = point.x - mode_start_x_;
      if (relative >= 0) {
        const int stride = mode_slot_width_ + mode_gap_;
        const int index = relative / stride;
        const int within = relative % stride;
        if (within < mode_slot_width_) select_mode(index);
      }
      return;
    }

    const int dx = point.x - centre_x_;
    const int dy = point.y - centre_y_;
    const int radius = diameter_ / 2;
    if (dx * dx + dy * dy <= radius * radius)
      tessera::open_card(plugin_->plugin_id(), "history", room_.entity, tile_);
  }

 private:
  void read_climate(JsonObjectConst entity, Room &room) {
    if (entity.isNull()) return;
    const JsonObjectConst attributes = entity["attributes"].as<JsonObjectConst>();
    if (!room.name[0]) copy_text(room.name, sizeof(room.name), entity["name"] | "");
    copy_text(room.mode, sizeof(room.mode), entity["state"] | "");
    copy_text(room.action, sizeof(room.action), attributes["hvac_action"] | "");
    copy_text(room.unit, sizeof(room.unit), attributes["temperature_unit"] | "°C");
    room.current = number_of(attributes["current_temperature"]);
    room.target = number_of(attributes["temperature"]);
    room.low = number_of(attributes["target_temp_low"]);
    room.high = number_of(attributes["target_temp_high"]);
    room.minimum = number_of(attributes["min_temp"], 5.0f);
    room.maximum = number_of(attributes["max_temp"], 35.0f);
    room.step = number_of(attributes["target_temp_step"], 0.5f);
    if (!valid_number(room.step) || room.step <= 0.0f) room.step = 0.5f;
    room.mode_count = 0;
    const JsonArrayConst modes = attributes["hvac_modes"].as<JsonArrayConst>();
    for (JsonVariantConst mode : modes) {
      if (room.mode_count >= MAX_MODES) break;
      copy_text(room.modes[room.mode_count], sizeof(room.modes[0]), mode.as<const char *>());
      ++room.mode_count;
    }
  }

  void create_ui(lv_obj_t *parent) {
    // Tessera-style heading: icon and the name chosen in the Layout editor.
    header_h_ = ui::px(36);
    header_icon_ = ui::label(parent, Font::ICON_SMALL, theme::ROW_ICON);
    ui::set_text(header_icon_, ui::icon(0xF0393));  // thermostat
    lv_obj_set_pos(header_icon_, ui::px(12), ui::px(5));
    lv_obj_set_width(header_icon_, ui::px(24));

    header_ = ui::label(parent, Font::TITLE, theme::INK);
    lv_obj_set_pos(header_, ui::px(40), ui::px(4));
    lv_obj_set_width(header_, width_ - ui::px(52));
    lv_obj_set_style_text_align(header_, LV_TEXT_ALIGN_LEFT, 0);

    // Leave a proper finger-width gutter so the +/- keys do not sit over the arc.
    mode_y_ = height_ - ui::px(48);
    mode_height_ = ui::px(40);
    mode_slot_width_ = ui::px(42);
    mode_gap_ = ui::px(6);

    const int key_size = ui::px(52);
    const int side_gap = ui::px(8);
    const int environment_y = mode_y_ - ui::px(34);
    const int dial_top = header_h_ + ui::px(6);
    const int dial_bottom = environment_y - ui::px(20);
    const int available_height = std::max(ui::px(120), dial_bottom - dial_top);
    const int available_width = std::max(ui::px(120), width_ - 2 * (key_size + side_gap));
    diameter_ = std::max(ui::px(120), std::min(available_width, available_height));
    arc_x_ = (width_ - diameter_) / 2;
    arc_y_ = dial_top + (available_height - diameter_) / 2;
    centre_x_ = width_ / 2;
    centre_y_ = arc_y_ + diameter_ / 2;

    arc_ = lv_arc_create(parent);
    lv_obj_remove_style_all(arc_);
    lv_obj_remove_flag(arc_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(arc_, diameter_, diameter_);
    lv_obj_set_pos(arc_, arc_x_, arc_y_);
    lv_arc_set_rotation(arc_, 135);
    lv_arc_set_bg_angles(arc_, 0, 270);
    lv_arc_set_range(arc_, 0, 1000);
    lv_arc_set_value(arc_, 500);
    lv_obj_set_style_arc_width(arc_, ui::px(14), LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc_, ui::px(14), LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(arc_, true, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(arc_, true, LV_PART_INDICATOR);

    target_whole_ = ui::label(parent, Font::DIGITS, theme::INK);
    target_unit_ = ui::label(parent, Font::BODY_LARGE, theme::MUTED);
    lv_obj_set_width(target_unit_, ui::px(42));
    target_decimal_ = ui::label(parent, Font::BODY_LARGE, theme::INK);
    lv_obj_set_width(target_decimal_, ui::px(44));

    current_ = ui::label(parent, Font::BODY_LARGE, theme::INK);
    lv_obj_set_width(current_, width_);
    lv_obj_set_style_text_align(current_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(current_, 0,
                   centre_y_ + ui::line_height(Font::DIGITS) / 2 + ui::px(8));

    status_ = ui::label(parent, Font::BODY, theme::MUTED);
    lv_obj_set_width(status_, width_);
    lv_obj_set_style_text_align(status_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(status_, 0,
                   centre_y_ + ui::line_height(Font::DIGITS) / 2 + ui::px(34));

    // The two controls are outside the dial track, leaving the arc visually unobstructed.
    stepper_h_ = key_size;
    minus_x_ = side_gap;
    plus_x_ = width_ - side_gap - key_size;
    stepper_y_ = centre_y_ - key_size / 2;

    minus_ = ui::block(parent, theme::STEPPER_KEY);
    plus_ = ui::block(parent, theme::STEPPER_KEY);
    lv_obj_set_size(minus_, key_size, key_size);
    lv_obj_set_size(plus_, key_size, key_size);
    lv_obj_set_style_radius(minus_, key_size / 2, 0);
    lv_obj_set_style_radius(plus_, key_size / 2, 0);
    lv_obj_set_pos(minus_, minus_x_, stepper_y_);
    lv_obj_set_pos(plus_, plus_x_, stepper_y_);

    minus_label_ = ui::label(parent, Font::ICON, theme::INK);
    plus_label_ = ui::label(parent, Font::ICON, theme::INK);
    ui::set_text(minus_label_, ui::icon(0xF0374));
    ui::set_text(plus_label_, ui::icon(0xF0415));
    lv_obj_set_width(minus_label_, key_size);
    lv_obj_set_width(plus_label_, key_size);
    lv_obj_set_style_text_align(minus_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_align(plus_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(minus_label_, minus_x_, stepper_y_ + (key_size - ui::line_height(Font::ICON)) / 2);
    lv_obj_set_pos(plus_label_, plus_x_, stepper_y_ + (key_size - ui::line_height(Font::ICON)) / 2);

    dot_ = ui::block(parent, theme::ACCENT);
    lv_obj_set_size(dot_, ui::px(12), ui::px(12));
    lv_obj_set_style_radius(dot_, ui::px(6), 0);
    lv_obj_add_flag(dot_, LV_OBJ_FLAG_HIDDEN);

    dual_low_ = ui::label(parent, Font::BODY, theme::MUTED);
    dual_high_ = ui::label(parent, Font::BODY, theme::MUTED);
    lv_obj_set_width(dual_low_, width_ / 2);
    lv_obj_set_width(dual_high_, width_ / 2);
    lv_obj_set_style_text_align(dual_low_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_align(dual_high_, LV_TEXT_ALIGN_CENTER, 0);
    dual_y_ = centre_y_ + ui::line_height(Font::DIGITS) / 2 + ui::px(58);
    lv_obj_set_pos(dual_low_, 0, dual_y_);
    lv_obj_set_pos(dual_high_, width_ / 2, dual_y_);

    environment_ = ui::label(parent, Font::BODY, theme::MUTED);
    lv_obj_set_width(environment_, width_);
    lv_obj_set_style_text_align(environment_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(environment_, 0, environment_y);

    for (int i = 0; i < MAX_MODES; ++i) {
      mode_background_[i] = ui::block(parent, theme::BUTTON);
      mode_label_[i] = ui::label(parent, Font::ICON, theme::INK);
      lv_obj_set_style_radius(mode_background_[i], ui::px(10), 0);
      lv_obj_add_flag(mode_background_[i], LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(mode_label_[i], LV_OBJ_FLAG_HIDDEN);
    }
  }

  void adjust_setpoint(float direction) {
    const bool dual = valid_number(room_.low) && valid_number(room_.high);
    if (dual && !dual_edit_low_ && !dual_edit_high_) dual_edit_low_ = true;
    float current = dual_edit_high_ ? room_.high : dual_edit_low_ ? room_.low : room_.target;
    if (!valid_number(current)) current = valid_number(room_.target) ? room_.target : 20.0f;
    float next = current + direction * room_.step;
    next = std::max(room_.minimum, std::min(room_.maximum, next));
    next = std::round(next / room_.step) * room_.step;
    pending_target_ = next;
    pending_valid_ = true;
    pending_sent_ = false;
    last_adjustment_at_ = millis();
    paint();
  }

  void send_setpoint() {
    if (!room_.entity[0]) {
      pending_valid_ = false;
      paint();
      return;
    }
    JsonDocument fields;
    if (valid_number(room_.low) && valid_number(room_.high)) {
      const float low = dual_edit_high_ ? room_.low : (dual_edit_low_ ? pending_target_ : room_.low);
      const float high = dual_edit_high_ ? pending_target_ : room_.high;
      fields["target_temp_low"] = low;
      fields["target_temp_high"] = high;
    } else {
      fields["temperature"] = pending_target_;
    }
    if (!tessera::action("climate.set_temperature", room_.entity, fields.as<JsonVariantConst>())) {
      pending_valid_ = false;
      pending_sent_ = false;
      paint();
      return;
    }
    pending_sent_ = true;
    pending_sent_at_ = millis();
  }

  void select_mode(int index) {
    if (index < 0 || index >= room_.mode_count) return;
    JsonDocument fields;
    fields["hvac_mode"] = room_.modes[index];
    tessera::action("climate.set_hvac_mode", room_.entity, fields.as<JsonVariantConst>());
  }

  void paint() {
    if (!header_) return;
    ui::set_color(header_, theme::INK);
    ui::set_color(header_icon_, theme::ROW_ICON);
    ui::set_color(target_whole_, theme::INK);
    ui::set_color(target_unit_, theme::MUTED);
    ui::set_color(target_decimal_, theme::INK);
    ui::set_color(current_, theme::INK);
    ui::set_color(status_, theme::MUTED);
    ui::set_color(minus_label_, theme::INK);
    ui::set_color(plus_label_, theme::INK);
    ui::set_color(environment_, theme::MUTED);
    lv_obj_set_style_bg_color(minus_, ui::color(theme::STEPPER_KEY), 0);
    lv_obj_set_style_bg_color(plus_, ui::color(theme::STEPPER_KEY), 0);
    lv_obj_set_style_arc_color(arc_, ui::color(theme::TRACK), LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc_, ui::color(theme::ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(dot_, ui::color(theme::ACCENT), 0);

    if (!room_.entity[0]) {
      ui::set_text(header_, plugin_->text("choose_climate"));
      ui::set_text(target_whole_, "--");
      ui::set_text(target_decimal_, "");
      ui::set_text(current_, "");
      ui::set_text(status_, plugin_->text("choose_climate_hint"));
      return;
    }

    ui::set_text(header_, room_.name[0] ? room_.name : plugin_->text("climate_room"));

    const bool has_dual = valid_number(room_.low) && valid_number(room_.high);
    const float base_target = has_dual ? (dual_edit_high_ ? room_.high : room_.low) : room_.target;
    const float shown_target = pending_valid_ ? pending_target_ : base_target;
    char whole[16] = "--", decimal[8] = "";
    if (valid_number(shown_target)) {
      const int tenths = static_cast<int>(std::lround(shown_target * 10.0f));
      std::snprintf(whole, sizeof(whole), "%d", tenths / 10);
      std::snprintf(decimal, sizeof(decimal), ".%d", std::abs(tenths % 10));
    }
    ui::set_text(target_whole_, whole);
    ui::set_text(target_decimal_, decimal);
    ui::set_text(target_unit_, room_.unit);
    const int whole_width = std::max(ui::px(1), ui::text_width(whole, Font::DIGITS));
    const int suffix_width = ui::px(44);
    const int suffix_gap = ui::px(5);
    const int group_width = whole_width + suffix_gap + suffix_width;
    lv_obj_set_width(target_whole_, whole_width + ui::px(2));
    lv_obj_set_style_text_align(target_whole_, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_pos(target_whole_, centre_x_ - group_width / 2,
                   centre_y_ - ui::line_height(Font::DIGITS) / 2 - ui::px(18));
    lv_obj_update_layout(target_whole_);
    lv_obj_align_to(target_unit_, target_whole_, LV_ALIGN_OUT_RIGHT_TOP, suffix_gap, ui::px(10));
    lv_obj_align_to(target_decimal_, target_whole_, LV_ALIGN_OUT_RIGHT_BOTTOM, suffix_gap, -ui::px(16));

    char current_text[64];
    if (valid_number(room_.current)) {
      char number[20];
      format_number(number, sizeof(number), room_.current);
      const std::string current_label = plugin_->text("current_temperature");
      std::snprintf(current_text, sizeof(current_text), "%s %s", current_label.c_str(), number);
    } else {
      copy_text(current_text, sizeof(current_text), plugin_->text("current_unavailable"));
    }
    ui::set_text(current_, current_text);

    char state_text[80], action_words[24];
    title_case(room_.action[0] ? room_.action : room_.mode, action_words, sizeof(action_words));
    if (valid_number(room_.humidity))
      std::snprintf(state_text, sizeof(state_text), "%s  ·  %.0f%%", action_words, room_.humidity);
    else
      std::snprintf(state_text, sizeof(state_text), "%s", action_words);
    ui::set_text(status_, state_text);

    dual_available_ = valid_number(room_.low) && valid_number(room_.high);
    if (dual_available_) {
      char low_text[20], high_text[20], left[32], right[32];
      const float shown_low = pending_valid_ && dual_edit_low_ ? pending_target_ : room_.low;
      const float shown_high = pending_valid_ && dual_edit_high_ ? pending_target_ : room_.high;
      format_number(low_text, sizeof(low_text), shown_low);
      format_number(high_text, sizeof(high_text), shown_high);
      const std::string heating_label = plugin_->text("heating");
      const std::string cooling_label = plugin_->text("cooling");
      std::snprintf(left, sizeof(left), "%s %s°", heating_label.c_str(), low_text);
      std::snprintf(right, sizeof(right), "%s %s°", cooling_label.c_str(), high_text);
      ui::set_text(dual_low_, left);
      ui::set_text(dual_high_, right);
      lv_obj_clear_flag(dual_low_, LV_OBJ_FLAG_HIDDEN);
      lv_obj_clear_flag(dual_high_, LV_OBJ_FLAG_HIDDEN);
      ui::set_color(dual_low_, dual_edit_low_ ? theme::ACCENT : theme::MUTED);
      ui::set_color(dual_high_, dual_edit_high_ ? theme::ACCENT : theme::MUTED);
    } else {
      lv_obj_add_flag(dual_low_, LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(dual_high_, LV_OBJ_FLAG_HIDDEN);
      dual_edit_low_ = dual_edit_high_ = false;
    }

    float value = shown_target;
    if (!valid_number(value)) value = 20.0f;
    const float fraction = (value - room_.minimum) / std::max(0.1f, room_.maximum - room_.minimum);
    lv_arc_set_value(arc_, static_cast<int>(std::round(
      std::max(0.0f, std::min(1.0f, fraction)) * 1000.0f
    )));

    if (show_dot_ && valid_number(room_.current)) {
      const float position = (room_.current - room_.minimum) /
                             std::max(0.1f, room_.maximum - room_.minimum);
      const float angle = (135.0f + std::max(0.0f, std::min(1.0f, position)) * 270.0f) *
                          0.0174532925f;
      const int radius = diameter_ / 2 - ui::px(8);
      lv_obj_set_pos(dot_,
                     centre_x_ + static_cast<int>(std::cos(angle) * radius) - ui::px(6),
                     centre_y_ + static_cast<int>(std::sin(angle) * radius) - ui::px(6));
      lv_obj_clear_flag(dot_, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(dot_, LV_OBJ_FLAG_HIDDEN);
    }

    if (valid_number(outside_temperature_)) {
      char outside_text[40];
      const std::string outside_label = plugin_->text("outside_short");
      std::snprintf(outside_text, sizeof(outside_text), "%s %.1f°", outside_label.c_str(),
                    outside_temperature_);
      ui::set_text(environment_, outside_text);
    } else {
      ui::set_text(environment_, "");
    }

    const uint8_t modes = room_.mode_count;
    if (modes > 0) {
      mode_start_x_ = (width_ - (modes * mode_slot_width_ + (modes - 1) * mode_gap_)) / 2;
    }
    for (int i = 0; i < MAX_MODES; ++i) {
      if (i < modes) {
        const int x = mode_start_x_ + i * (mode_slot_width_ + mode_gap_);
        lv_obj_set_pos(mode_background_[i], x, mode_y_);
        lv_obj_set_size(mode_background_[i], mode_slot_width_, mode_height_);
        lv_obj_set_pos(mode_label_[i], x,
                       mode_y_ + (mode_height_ - ui::line_height(Font::ICON)) / 2);
        lv_obj_set_width(mode_label_[i], mode_slot_width_);
        lv_obj_set_style_text_align(mode_label_[i], LV_TEXT_ALIGN_CENTER, 0);
        ui::set_text(mode_label_[i], ui::icon(mode_icon(room_.modes[i])));
        const bool selected = std::strcmp(room_.mode, room_.modes[i]) == 0;
        lv_obj_set_style_bg_color(mode_background_[i],
                                  ui::color(selected ? theme::ACCENT : theme::BUTTON), 0);
        ui::set_color(mode_label_[i], selected ? theme::ON_ACCENT : theme::INK);
        lv_obj_clear_flag(mode_background_[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(mode_label_[i], LV_OBJ_FLAG_HIDDEN);
      } else {
        lv_obj_add_flag(mode_background_[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(mode_label_[i], LV_OBJ_FLAG_HIDDEN);
      }
    }
  }

  HeatingController *plugin_;
  Room room_ = {};
  char humidity_entity_[80] = {};
  char outside_entity_[80] = {};
  float outside_temperature_ = NAN;
  int width_ = 0, height_ = 0, tile_ = -1, diameter_ = 0;
  int header_h_ = 0, arc_x_ = 0, arc_y_ = 0, centre_x_ = 0, centre_y_ = 0;
  int minus_x_ = 0, plus_x_ = 0, stepper_y_ = 0, stepper_h_ = 0;
  int mode_y_ = 0, mode_height_ = 0, mode_slot_width_ = 0, mode_gap_ = 0, mode_start_x_ = 0;
  int dual_y_ = 0;
  bool show_dot_ = false, dual_available_ = false;
  bool dual_edit_low_ = false, dual_edit_high_ = false;
  bool pending_valid_ = false, pending_sent_ = false;
  float pending_target_ = NAN;
  uint32_t last_adjustment_at_ = 0, pending_sent_at_ = 0;
  lv_obj_t *header_ = nullptr, *header_icon_ = nullptr, *arc_ = nullptr;
  lv_obj_t *target_whole_ = nullptr, *target_unit_ = nullptr, *target_decimal_ = nullptr;
  lv_obj_t *current_ = nullptr, *status_ = nullptr;
  lv_obj_t *minus_ = nullptr, *plus_ = nullptr, *minus_label_ = nullptr, *plus_label_ = nullptr, *dot_ = nullptr;
  lv_obj_t *dual_low_ = nullptr, *dual_high_ = nullptr, *environment_ = nullptr;
  lv_obj_t *mode_background_[MAX_MODES] = {}, *mode_label_[MAX_MODES] = {};
};

void HeatingController::setup() {
  add_tile("heating", [this]() { return new HeatingTile(this); });
  add_card("history", [this]() { return new HistoryCard(this); }, true);
}

void HeatingController::history_closed(HistoryCard *card) {
  if (history_card_ == card) history_card_ = nullptr;
}

void HeatingController::on_message(JsonObjectConst message) {
  if (history_card_) history_card_->on_message(message);
}

void HeatingController::on_cards_closed() {
  history_card_ = nullptr;
}

}  // namespace esphome::heating_controller
