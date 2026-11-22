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

constexpr uint8_t MAX_ROOMS = 4;
constexpr uint8_t MAX_MODES = 7;
constexpr uint8_t HISTORY_CHUNKS = 4;
constexpr uint8_t HISTORY_POINTS = 24;
constexpr uint16_t MAX_HISTORY_SAMPLES = 512;

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

struct Room {
  char entity[80] = {};
  char name[48] = {};
  char humidity_entity[80] = {};
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
  bool configured = false;
};

struct HistorySample {
  uint32_t time = 0;
  float temperature = NAN;
  float target = NAN;
  float low = NAN;
  float high = NAN;
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
    received_mask_ = 0;
    sample_count_ = 0;
    failed_ = false;
    plot_ready_ = false;
    plugin_->history_opened(this);

    title_ = ui::label(context.parent, Font::TITLE, theme::INK);
    ui::set_text(title_, plugin_->text("history_title"));
    lv_obj_set_pos(title_, ui::px(8), ui::px(4));
    lv_obj_set_width(title_, width_ - ui::px(16));

    room_legend_ = ui::label(context.parent, Font::BODY, theme::ACCENT);
    ui::set_text(room_legend_, plugin_->text("history_room_temperature"));
    lv_obj_set_pos(room_legend_, ui::px(8), ui::px(38));
    lv_obj_set_width(room_legend_, width_ / 2 - ui::px(12));

    target_legend_ = ui::label(context.parent, Font::BODY, theme::MUTED);
    ui::set_text(target_legend_, plugin_->text("history_target_temperature"));
    lv_obj_set_pos(target_legend_, width_ / 2, ui::px(38));
    lv_obj_set_width(target_legend_, width_ / 2 - ui::px(8));

    status_ = ui::label(context.parent, Font::BODY, theme::MUTED);
    lv_obj_set_width(status_, width_ - ui::px(20));
    lv_obj_set_style_text_align(status_, LV_TEXT_ALIGN_CENTER, 0);
    ui::set_text(status_, plugin_->text("history_loading"));
    lv_obj_set_pos(status_, ui::px(10), height_ / 2 - ui::line_height(Font::BODY) / 2);

    build_plot(context.parent);
    request_history();
  }

  void on_message(JsonObjectConst message) {
    const uint32_t request_id = message["re"] | 0u;
    int chunk = -1;
    for (int i = 0; i < HISTORY_CHUNKS; ++i) {
      if (request_ids_[i] && request_ids_[i] == request_id) {
        chunk = i;
        break;
      }
    }
    if (chunk < 0) return;

    if (!(message["ok"] | false)) {
      failed_ = true;
      received_mask_ |= static_cast<uint8_t>(1U << chunk);
      update_status();
      return;
    }

    const JsonObjectConst result = message["result"].as<JsonObjectConst>();
    const JsonArrayConst times = result["times"].as<JsonArrayConst>();
    const JsonArrayConst temperatures = result["temperatures"].as<JsonArrayConst>();
    const JsonArrayConst targets = result["targets"].as<JsonArrayConst>();
    const JsonArrayConst lows = result["target_lows"].as<JsonArrayConst>();
    const JsonArrayConst highs = result["target_highs"].as<JsonArrayConst>();
    size_t count = times.size();
    count = std::min(count, temperatures.size());
    count = std::min(count, targets.size());
    count = std::min(count, lows.size());
    count = std::min(count, highs.size());

    for (size_t i = 0; i < count && sample_count_ < MAX_HISTORY_SAMPLES; ++i) {
      const uint32_t timestamp = static_cast<uint32_t>(number_of(times[i], 0));
      if (!timestamp) continue;
      HistorySample sample;
      sample.time = timestamp;
      sample.temperature = number_of(temperatures[i]);
      sample.target = number_of(targets[i]);
      sample.low = number_of(lows[i]);
      sample.high = number_of(highs[i]);
      samples_[sample_count_++] = sample;
    }

    received_mask_ |= static_cast<uint8_t>(1U << chunk);
    if (received_mask_ == 0x0F) render_plot();
    update_status();
  }

  void on_theme() override {
    if (title_) ui::set_color(title_, theme::INK);
    if (room_legend_) ui::set_color(room_legend_, theme::ACCENT);
    if (target_legend_) ui::set_color(target_legend_, theme::MUTED);
    if (status_) ui::set_color(status_, theme::MUTED);
    for (lv_obj_t *label : axis_labels_) if (label) ui::set_color(label, theme::SUBTLE);
    if (axis_start_) ui::set_color(axis_start_, theme::SUBTLE);
    if (axis_mid_) ui::set_color(axis_mid_, theme::SUBTLE);
    if (axis_now_) ui::set_color(axis_now_, theme::SUBTLE);
    for (lv_obj_t *line : grid_lines_) {
      if (line) lv_obj_set_style_line_color(line, ui::color(theme::GRID), 0);
    }
    if (room_line_) lv_obj_set_style_line_color(room_line_, ui::color(theme::ACCENT), 0);
    if (target_line_) lv_obj_set_style_line_color(target_line_, ui::color(theme::MUTED), 0);
  }

 private:
  void build_plot(lv_obj_t *parent) {
    const int left = ui::px(42);
    const int right = ui::px(12);
    plot_x_ = left;
    plot_y_ = ui::px(82);
    plot_w_ = std::max(20, width_ - left - right);
    plot_h_ = std::max(40, height_ - plot_y_ - ui::px(52));

    for (int i = 0; i < 5; ++i) {
      grid_points_[i][0] = {0, static_cast<lv_value_precise_t>(i * plot_h_ / 4)};
      grid_points_[i][1] = {static_cast<lv_value_precise_t>(plot_w_), static_cast<lv_value_precise_t>(i * plot_h_ / 4)};
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
      lv_obj_set_pos(axis_labels_[i], 0, plot_y_ + i * plot_h_ / 4 - ui::line_height(Font::BODY) / 2);
      lv_obj_set_width(axis_labels_[i], left - ui::px(8));
      lv_obj_set_style_text_align(axis_labels_[i], LV_TEXT_ALIGN_RIGHT, 0);
    }

    axis_start_ = ui::label(parent, Font::BODY, theme::SUBTLE);
    ui::set_text(axis_start_, plugin_->text("history_24h_ago"));
    lv_obj_set_pos(axis_start_, plot_x_, plot_y_ + plot_h_ + ui::px(8));

    axis_mid_ = ui::label(parent, Font::BODY, theme::SUBTLE);
    ui::set_text(axis_mid_, plugin_->text("history_12h_ago"));
    lv_obj_set_pos(axis_mid_, plot_x_ + plot_w_ / 2 - ui::px(24), plot_y_ + plot_h_ + ui::px(8));

    axis_now_ = ui::label(parent, Font::BODY, theme::SUBTLE);
    ui::set_text(axis_now_, plugin_->text("history_now"));
    lv_obj_set_pos(axis_now_, plot_x_ + plot_w_ - ui::px(30), plot_y_ + plot_h_ + ui::px(8));
  }

  void request_history() {
    const uint32_t now = tessera::epoch();
    if (!now || entity_[0] == '\0') {
      failed_ = true;
      received_mask_ = 0x0F;
      update_status();
      return;
    }
    const uint32_t start = now - 24U * 3600U;
    for (int i = 0; i < HISTORY_CHUNKS; ++i) {
      const uint32_t chunk_start = start + static_cast<uint32_t>(i) * 6U * 3600U;
      const uint32_t chunk_end = (i == HISTORY_CHUNKS - 1) ? now : chunk_start + 6U * 3600U;
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
      data["significant_changes_only"] = true;
      data["minimal_response"] = false;
      data["no_attributes"] = false;
      request_ids_[i] = tessera::send(plugin_, request.as<JsonObjectConst>());
      if (!request_ids_[i]) {
        failed_ = true;
        received_mask_ |= static_cast<uint8_t>(1U << i);
      }
    }
    if (received_mask_ == 0x0F) update_status();
  }

  void update_status() {
    if (!status_) return;
    if (received_mask_ != 0x0F) {
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

  void render_plot() {
    plot_ready_ = false;
    if (sample_count_ < 2) {
      update_status();
      return;
    }
    std::sort(samples_, samples_ + sample_count_, [](const HistorySample &a, const HistorySample &b) {
      return a.time < b.time;
    });

    const uint32_t finish = tessera::epoch();
    const uint32_t start = finish - 24U * 3600U;
    const uint32_t span = std::max<uint32_t>(1, finish - start);
    float values_min = 100000.0f;
    float values_max = -100000.0f;
    uint8_t room_count = 0, target_count = 0;

    for (int i = 0; i < HISTORY_POINTS; ++i) {
      const uint32_t at = start + static_cast<uint32_t>((static_cast<uint64_t>(span) * i) / (HISTORY_POINTS - 1));
      const HistorySample *sample = nullptr;
      for (int j = 0; j < sample_count_; ++j) {
        if (samples_[j].time <= at) sample = &samples_[j];
        else break;
      }
      if (!sample) continue;
      const float room_value = sample->temperature;
      float target_value = sample->target;
      if (!valid_number(target_value) && valid_number(sample->low) && valid_number(sample->high))
        target_value = (sample->low + sample->high) / 2.0f;

      if (valid_number(room_value)) {
        room_values_[room_count] = {static_cast<lv_value_precise_t>(i * (plot_w_ - 1) / (HISTORY_POINTS - 1)), 0};
        room_raw_[room_count] = room_value;
        values_min = std::min(values_min, room_value);
        values_max = std::max(values_max, room_value);
        ++room_count;
      }
      if (valid_number(target_value)) {
        target_values_[target_count] = {static_cast<lv_value_precise_t>(i * (plot_w_ - 1) / (HISTORY_POINTS - 1)), 0};
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
      room_values_[i].y = static_cast<lv_value_precise_t>(plot_h_ - 1 - (room_raw_[i] - values_min) * (plot_h_ - 1) / range);
    for (int i = 0; i < target_count; ++i)
      target_values_[i].y = static_cast<lv_value_precise_t>(plot_h_ - 1 - (target_raw_[i] - values_min) * (plot_h_ - 1) / range);

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
  uint32_t request_ids_[HISTORY_CHUNKS] = {};
  uint8_t received_mask_ = 0;
  bool failed_ = false;
  bool plot_ready_ = false;
  int width_ = 0, height_ = 0;
  int plot_x_ = 0, plot_y_ = 0, plot_w_ = 0, plot_h_ = 0;
  uint16_t sample_count_ = 0;
  HistorySample samples_[MAX_HISTORY_SAMPLES] = {};
  lv_obj_t *title_ = nullptr;
  lv_obj_t *room_legend_ = nullptr;
  lv_obj_t *target_legend_ = nullptr;
  lv_obj_t *status_ = nullptr;
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
    show_dot_ = context.options["show_current_temp_dot"] | true;

    for (int i = 0; i < MAX_ROOMS; ++i) {
      char climate_key[20], humidity_key[20];
      std::snprintf(climate_key, sizeof(climate_key), "climate_%d", i + 1);
      std::snprintf(humidity_key, sizeof(humidity_key), "humidity_%d", i + 1);
      copy_text(rooms_[i].entity, sizeof(rooms_[i].entity), context.options[climate_key] | "");
      copy_text(rooms_[i].humidity_entity, sizeof(rooms_[i].humidity_entity), context.options[humidity_key] | "");
      rooms_[i].configured = rooms_[i].entity[0] != '\0';
      char default_name[16];
      std::snprintf(default_name, sizeof(default_name), "Room %d", i + 1);
      copy_text(rooms_[i].name, sizeof(rooms_[i].name), default_name);
      if (rooms_[i].configured) {
        if (room_count_ == 0) active_room_ = i;
        ++room_count_;
      }
    }
    copy_text(outside_entity_, sizeof(outside_entity_), context.options["outside_temperature"] | "");
    create_ui(context.parent);
    paint();
  }

  void on_state(JsonObjectConst data) override {
    const JsonObjectConst entities = data["entities"].as<JsonObjectConst>();
    for (int i = 0; i < MAX_ROOMS; ++i) {
      if (!rooms_[i].configured) continue;
      read_climate(entities[climate_option(i)], rooms_[i]);
      if (rooms_[i].humidity_entity[0]) {
        const JsonObjectConst humidity = entities[humidity_option(i)];
        rooms_[i].humidity = number_of(humidity["state"]);
      } else {
        rooms_[i].humidity = NAN;
      }
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
      const Room &room = rooms_[active_room_];
      const float actual = dual_edit_high_ ? room.high : dual_edit_low_ ? room.low : room.target;
      if (valid_number(actual) && std::fabs(actual - pending_target_) <= std::max(0.1f, room.step / 2.0f)) {
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
    if (point.x < 0 || point.y < 0 || room_count_ == 0) return;

    if (point.y < header_h_) {
      if (point.x < width_ / 3) select_room(-1);
      else if (point.x > width_ * 2 / 3) select_room(1);
      return;
    }

    const int middle = width_ / 2;
    if (dual_available_ && point.y >= dual_y_ && point.y <= dual_y_ + ui::px(36)) {
      dual_edit_low_ = point.x < middle;
      dual_edit_high_ = point.x >= middle;
      paint();
      return;
    }

    if (point.y >= stepper_y_ && point.y <= stepper_y_ + stepper_h_) {
      adjust_setpoint(point.x < middle ? -1.0f : 1.0f);
      return;
    }

    const uint8_t modes = rooms_[active_room_].mode_count;
    if (modes && point.y >= mode_y_ && point.y < history_y_) {
      const int cell = width_ / modes;
      const int index = std::min<int>(modes - 1, point.x / std::max(1, cell));
      select_mode(index);
      return;
    }

    if (point.y >= history_y_) {
      tessera::open_card(plugin_->plugin_id(), "history", rooms_[active_room_].entity, tile_);
    }
  }

 private:
  static const char *climate_option(int index) {
    static const char *keys[] = {"climate_1", "climate_2", "climate_3", "climate_4"};
    return keys[index];
  }
  static const char *humidity_option(int index) {
    static const char *keys[] = {"humidity_1", "humidity_2", "humidity_3", "humidity_4"};
    return keys[index];
  }

  void read_climate(JsonObjectConst entity, Room &room) {
    if (entity.isNull()) return;
    const JsonObjectConst attributes = entity["attributes"].as<JsonObjectConst>();
    copy_text(room.name, sizeof(room.name), entity["name"] | room.name);
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
    header_h_ = ui::px(42);
    header_ = ui::label(parent, Font::TITLE, theme::INK);
    lv_obj_set_pos(header_, ui::px(40), ui::px(4));
    lv_obj_set_width(header_, width_ - ui::px(80));
    lv_obj_set_style_text_align(header_, LV_TEXT_ALIGN_CENTER, 0);

    previous_ = ui::label(parent, Font::TITLE, theme::INK);
    ui::set_text(previous_, "<");
    lv_obj_set_pos(previous_, ui::px(8), ui::px(4));
    lv_obj_set_width(previous_, ui::px(28));

    next_ = ui::label(parent, Font::TITLE, theme::INK);
    ui::set_text(next_, ">");
    lv_obj_set_pos(next_, width_ - ui::px(32), ui::px(4));
    lv_obj_set_width(next_, ui::px(28));

    diameter_ = std::max(ui::px(120), std::min(width_ * 56 / 100, height_ * 52 / 100));
    const int arc_x = (width_ - diameter_) / 2;
    const int arc_y = header_h_ + ui::px(8);
    const int centre_y = arc_y + diameter_ / 2;
    arc_ = lv_arc_create(parent);
    lv_obj_remove_style_all(arc_);
    lv_obj_remove_flag(arc_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(arc_, diameter_, diameter_);
    lv_obj_set_pos(arc_, arc_x, arc_y);
    lv_arc_set_rotation(arc_, 135);
    lv_arc_set_bg_angles(arc_, 0, 270);
    lv_arc_set_range(arc_, 0, 1000);
    lv_arc_set_value(arc_, 500);
    lv_obj_set_style_arc_width(arc_, ui::px(14), LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc_, ui::px(14), LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(arc_, true, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(arc_, true, LV_PART_INDICATOR);

    target_ = ui::label(parent, Font::DIGITS, theme::INK);
    lv_obj_set_width(target_, diameter_ - ui::px(40));
    lv_obj_set_style_text_align(target_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(target_, arc_x + ui::px(20), centre_y - ui::line_height(Font::DIGITS) / 2);

    unit_ = ui::label(parent, Font::BODY_LARGE, theme::MUTED);
    lv_obj_set_width(unit_, diameter_ - ui::px(30));
    lv_obj_set_style_text_align(unit_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(unit_, arc_x + ui::px(15), centre_y + ui::line_height(Font::DIGITS) / 2 - ui::px(2));

    current_ = ui::label(parent, Font::BODY_LARGE, theme::INK);
    lv_obj_set_width(current_, width_);
    lv_obj_set_style_text_align(current_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(current_, 0, arc_y + diameter_ + ui::px(28));

    status_ = ui::label(parent, Font::BODY, theme::MUTED);
    lv_obj_set_width(status_, width_);
    lv_obj_set_style_text_align(status_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(status_, ui::px(6), arc_y + diameter_ + ui::px(51));

    const int key_size = ui::px(48);
    minus_ = ui::block(parent, theme::BUTTON);
    plus_ = ui::block(parent, theme::BUTTON);
    lv_obj_set_size(minus_, key_size, key_size);
    lv_obj_set_size(plus_, key_size, key_size);
    lv_obj_set_style_radius(minus_, key_size / 2, 0);
    lv_obj_set_style_radius(plus_, key_size / 2, 0);
    lv_obj_set_pos(minus_, std::max(0, arc_x - key_size - ui::px(8)), centre_y - key_size / 2);
    lv_obj_set_pos(plus_, std::min(width_ - key_size, arc_x + diameter_ + ui::px(8)), centre_y - key_size / 2);
    minus_label_ = ui::label(parent, Font::HEADLINE, theme::INK);
    plus_label_ = ui::label(parent, Font::HEADLINE, theme::INK);
    ui::set_text(minus_label_, "−");
    ui::set_text(plus_label_, "+");
    lv_obj_set_width(minus_label_, key_size);
    lv_obj_set_width(plus_label_, key_size);
    lv_obj_set_style_text_align(minus_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_align(plus_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(minus_label_, std::max(0, arc_x - key_size - ui::px(8)), centre_y - ui::line_height(Font::HEADLINE) / 2);
    lv_obj_set_pos(plus_label_, std::min(width_ - key_size, arc_x + diameter_ + ui::px(8)), centre_y - ui::line_height(Font::HEADLINE) / 2);

    dot_ = ui::block(parent, theme::ACCENT);
    lv_obj_set_size(dot_, ui::px(12), ui::px(12));
    lv_obj_set_style_radius(dot_, ui::px(6), 0);
    lv_obj_add_flag(dot_, LV_OBJ_FLAG_HIDDEN);

    dual_low_ = ui::label(parent, Font::BODY, theme::MUTED);
    dual_high_ = ui::label(parent, Font::BODY, theme::MUTED);
    lv_obj_set_width(dual_low_, width_ / 2);
    lv_obj_set_width(dual_high_, width_ / 2);
    lv_obj_set_pos(dual_low_, 0, arc_y + diameter_ + ui::px(4));
    lv_obj_set_pos(dual_high_, width_ / 2, arc_y + diameter_ + ui::px(4));
    lv_obj_set_style_text_align(dual_low_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_align(dual_high_, LV_TEXT_ALIGN_CENTER, 0);
    dual_y_ = arc_y + diameter_ + ui::px(4);

    environment_ = ui::label(parent, Font::BODY, theme::MUTED);
    lv_obj_set_width(environment_, width_);
    lv_obj_set_style_text_align(environment_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(environment_, 0, arc_y + diameter_ + ui::px(75));

    mode_y_ = height_ - ui::px(70);
    mode_count_ = ui::label(parent, Font::BODY, theme::MUTED);
    lv_obj_set_width(mode_count_, width_);
    lv_obj_set_style_text_align(mode_count_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(mode_count_, 0, mode_y_ - ui::line_height(Font::BODY) - ui::px(3));

    for (int i = 0; i < MAX_MODES; ++i) {
      mode_background_[i] = ui::block(parent, theme::BUTTON);
      mode_label_[i] = ui::label(parent, Font::BODY, theme::INK);
      lv_obj_set_style_radius(mode_background_[i], ui::px(8), 0);
      lv_obj_add_flag(mode_background_[i], LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(mode_label_[i], LV_OBJ_FLAG_HIDDEN);
    }

    history_ = ui::label(parent, Font::BODY, theme::ACCENT);
    ui::set_text(history_, plugin_->text("history_open"));
    lv_obj_set_width(history_, width_);
    lv_obj_set_style_text_align(history_, LV_TEXT_ALIGN_CENTER, 0);
    history_y_ = height_ - ui::px(28);
    lv_obj_set_pos(history_, 0, history_y_);

    stepper_y_ = centre_y - key_size / 2;
    stepper_h_ = key_size;
  }

  void select_room(int direction) {
    if (room_count_ < 2) return;
    int candidate = active_room_;
    for (int attempt = 0; attempt < MAX_ROOMS; ++attempt) {
      candidate += direction;
      if (candidate < 0) candidate = MAX_ROOMS - 1;
      if (candidate >= MAX_ROOMS) candidate = 0;
      if (rooms_[candidate].configured) {
        active_room_ = candidate;
        pending_valid_ = false;
        pending_sent_ = false;
        dual_edit_low_ = false;
        dual_edit_high_ = false;
        paint();
        return;
      }
    }
  }

  void adjust_setpoint(float direction) {
    Room &room = rooms_[active_room_];
    const bool dual = valid_number(room.low) && valid_number(room.high);
    if (dual && !dual_edit_low_ && !dual_edit_high_) dual_edit_low_ = true;
    float current = dual_edit_low_ ? room.low : dual_edit_high_ ? room.high : room.target;
    if (!valid_number(current)) current = valid_number(room.target) ? room.target : 20.0f;
    float next = current + direction * room.step;
    next = std::max(room.minimum, std::min(room.maximum, next));
    next = std::round(next / room.step) * room.step;
    pending_target_ = next;
    pending_valid_ = true;
    pending_sent_ = false;
    last_adjustment_at_ = millis();
    paint();
  }

  void send_setpoint() {
    Room &room = rooms_[active_room_];
    if (!room.entity[0]) {
      pending_valid_ = false;
      paint();
      return;
    }
    JsonDocument fields;
    if (valid_number(room.low) && valid_number(room.high)) {
      const float low = dual_edit_high_ ? room.low : (dual_edit_low_ ? pending_target_ : room.low);
      const float high = dual_edit_high_ ? pending_target_ : (dual_edit_low_ ? room.high : room.high);
      fields["target_temp_low"] = low;
      fields["target_temp_high"] = high;
    } else {
      fields["temperature"] = pending_target_;
    }
    if (!tessera::action("climate.set_temperature", room.entity, fields.as<JsonVariantConst>())) {
      pending_valid_ = false;
      pending_sent_ = false;
      paint();
      return;
    }
    pending_sent_ = true;
    pending_sent_at_ = millis();
  }

  void select_mode(int index) {
    Room &room = rooms_[active_room_];
    if (index < 0 || index >= room.mode_count) return;
    JsonDocument fields;
    fields["hvac_mode"] = room.modes[index];
    tessera::action("climate.set_hvac_mode", room.entity, fields.as<JsonVariantConst>());
  }

  void paint() {
    if (!header_) return;
    ui::set_color(header_, theme::INK);
    ui::set_color(previous_, theme::INK);
    ui::set_color(next_, theme::INK);
    ui::set_color(target_, theme::INK);
    ui::set_color(unit_, theme::MUTED);
    ui::set_color(current_, theme::INK);
    ui::set_color(status_, theme::MUTED);
    ui::set_color(minus_label_, theme::INK);
    ui::set_color(plus_label_, theme::INK);
    ui::set_color(environment_, theme::MUTED);
    ui::set_color(mode_count_, theme::MUTED);
    ui::set_color(history_, theme::ACCENT);
    lv_obj_set_style_bg_color(minus_, ui::color(theme::BUTTON), 0);
    lv_obj_set_style_bg_color(plus_, ui::color(theme::BUTTON), 0);
    lv_obj_set_style_arc_color(arc_, ui::color(theme::TRACK), LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc_, ui::color(theme::ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(dot_, ui::color(theme::ACCENT), 0);
    if (!room_count_) {
      ui::set_text(header_, plugin_->text("choose_rooms"));
      ui::set_text(target_, "--");
      ui::set_text(current_, "");
      ui::set_text(status_, plugin_->text("choose_rooms_hint"));
      return;
    }
    Room &room = rooms_[active_room_];
    ui::set_text(header_, room.name[0] ? room.name : plugin_->text("room"));

    const bool has_dual = valid_number(room.low) && valid_number(room.high);
    const float base_target = has_dual ? (dual_edit_high_ ? room.high : room.low) : room.target;
    const float shown_target = pending_valid_ ? pending_target_ : base_target;
    char target_text[24];
    format_number(target_text, sizeof(target_text), shown_target);
    ui::set_text(target_, target_text);
    ui::set_text(unit_, room.unit);

    char current_text[64];
    if (valid_number(room.current)) {
      char number[20];
      format_number(number, sizeof(number), room.current);
      const std::string current_label = plugin_->text("current_temperature");
      std::snprintf(current_text, sizeof(current_text), "%s %s", current_label.c_str(), number);
    } else {
      copy_text(current_text, sizeof(current_text), plugin_->text("current_unavailable"));
    }
    ui::set_text(current_, current_text);

    char state_text[80], action_words[24];
    title_case(room.action[0] ? room.action : room.mode, action_words, sizeof(action_words));
    if (valid_number(room.humidity))
      std::snprintf(state_text, sizeof(state_text), "%s  ·  %.0f%%", action_words, room.humidity);
    else
      std::snprintf(state_text, sizeof(state_text), "%s", action_words);
    ui::set_text(status_, state_text);

    dual_available_ = valid_number(room.low) && valid_number(room.high);
    if (dual_available_) {
      char low_text[20], high_text[20], left[32], right[32];
      const float shown_low = pending_valid_ && dual_edit_low_ ? pending_target_ : room.low;
      const float shown_high = pending_valid_ && dual_edit_high_ ? pending_target_ : room.high;
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
    const float fraction = (value - room.minimum) / std::max(0.1f, room.maximum - room.minimum);
    lv_arc_set_value(arc_, static_cast<int>(std::round(std::max(0.0f, std::min(1.0f, fraction)) * 1000.0f)));
    if (show_dot_ && valid_number(room.current)) {
      const float position = (room.current - room.minimum) / std::max(0.1f, room.maximum - room.minimum);
      const float angle = (135.0f + std::max(0.0f, std::min(1.0f, position)) * 270.0f) * 0.0174532925f;
      const int radius = diameter_ / 2 - ui::px(8);
      const int centre_x = width_ / 2;
      const int centre_y = header_h_ + ui::px(8) + diameter_ / 2;
      lv_obj_set_pos(dot_, centre_x + static_cast<int>(std::cos(angle) * radius) - ui::px(6),
                     centre_y + static_cast<int>(std::sin(angle) * radius) - ui::px(6));
      lv_obj_clear_flag(dot_, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(dot_, LV_OBJ_FLAG_HIDDEN);
    }

    if (valid_number(outside_temperature_)) {
      char outside_text[40];
      const std::string outside_label = plugin_->text("outside_short");
      std::snprintf(outside_text, sizeof(outside_text), "%s %.1f°", outside_label.c_str(), outside_temperature_);
      ui::set_text(environment_, outside_text);
    } else {
      ui::set_text(environment_, "");
    }

    const uint8_t modes = room.mode_count;
    for (int i = 0; i < MAX_MODES; ++i) {
      if (i < modes) {
        const int x = i * width_ / modes;
        const int next_x = (i + 1) * width_ / modes;
        const int w = next_x - x - ui::px(4);
        lv_obj_set_pos(mode_background_[i], x + ui::px(2), mode_y_);
        lv_obj_set_size(mode_background_[i], w, ui::px(34));
        lv_obj_set_pos(mode_label_[i], x + ui::px(2), mode_y_ + ui::px(8));
        lv_obj_set_width(mode_label_[i], w);
        lv_obj_set_style_text_align(mode_label_[i], LV_TEXT_ALIGN_CENTER, 0);
        char mode_text[24];
        title_case(room.modes[i], mode_text, sizeof(mode_text));
        ui::set_text(mode_label_[i], mode_text);
        const bool selected = std::strcmp(room.mode, room.modes[i]) == 0;
        lv_obj_set_style_bg_color(mode_background_[i], ui::color(selected ? theme::ACCENT : theme::BUTTON), 0);
        ui::set_color(mode_label_[i], selected ? theme::ON_ACCENT : theme::INK);
        lv_obj_clear_flag(mode_background_[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(mode_label_[i], LV_OBJ_FLAG_HIDDEN);
      } else {
        lv_obj_add_flag(mode_background_[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(mode_label_[i], LV_OBJ_FLAG_HIDDEN);
      }
    }
    if (room_count_ > 1) {
      uint8_t order = 0;
      for (int i = 0; i <= active_room_; ++i) if (rooms_[i].configured) ++order;
      char count_text[16];
      std::snprintf(count_text, sizeof(count_text), "%u / %u", order, room_count_);
      ui::set_text(mode_count_, count_text);
    } else {
      ui::set_text(mode_count_, "");
    }
  }

  HeatingController *plugin_;
  Room rooms_[MAX_ROOMS] = {};
  char outside_entity_[80] = {};
  float outside_temperature_ = NAN;
  uint8_t room_count_ = 0;
  uint8_t active_room_ = 0;
  int width_ = 0, height_ = 0, tile_ = -1, diameter_ = 0;
  int header_h_ = 0, stepper_y_ = 0, stepper_h_ = 0, mode_y_ = 0, history_y_ = 0, dual_y_ = 0;
  bool show_dot_ = true, dual_available_ = false;
  bool dual_edit_low_ = false, dual_edit_high_ = false;
  bool pending_valid_ = false, pending_sent_ = false;
  float pending_target_ = NAN;
  uint32_t last_adjustment_at_ = 0, pending_sent_at_ = 0;
  lv_obj_t *header_ = nullptr, *previous_ = nullptr, *next_ = nullptr;
  lv_obj_t *arc_ = nullptr, *target_ = nullptr, *unit_ = nullptr, *current_ = nullptr, *status_ = nullptr;
  lv_obj_t *minus_ = nullptr, *plus_ = nullptr, *minus_label_ = nullptr, *plus_label_ = nullptr, *dot_ = nullptr;
  lv_obj_t *dual_low_ = nullptr, *dual_high_ = nullptr, *environment_ = nullptr;
  lv_obj_t *mode_count_ = nullptr, *history_ = nullptr;
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
