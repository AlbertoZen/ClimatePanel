#pragma once

#include "esphome/core/component.h"
#include "esphome/components/smart_display/plugin_api.h"

namespace esphome::heating_controller {

class HistoryCard;

class HeatingController : public Component, public tessera::Plugin {
 public:
  void setup() override;
  float get_setup_priority() const override { return setup_priority::DATA; }
  void on_message(JsonObjectConst message) override;
  void on_cards_closed() override;

  void history_opened(HistoryCard *card) { history_card_ = card; }
  void history_closed(HistoryCard *card);

 private:
  HistoryCard *history_card_ = nullptr;
};

}  // namespace esphome::heating_controller
