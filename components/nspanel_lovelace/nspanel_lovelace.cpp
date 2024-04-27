#include "nspanel_lovelace.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <stdio.h>
#include <stdint.h>
#include <string>
#include <time.h>
#include <vector>
#include <utility>
// #include <driver/gpio.h>
#include <esp_heap_caps.h>
// #include <esp32/rom/rtc.h>
#include <esp_system.h>
// #include <regex>
#include "esphome/core/application.h"
#include "esphome/core/helpers.h"
#include "esphome/core/util.h"
#include "esphome/components/json/json_util.h"

#include "config.h"
#include "cards.h"
#include "card_items.h"
#include "pages.h"
#include "page_item_visitor.h"
#include "page_visitor.h"

namespace esphome {
namespace nspanel_lovelace {

// Use PSRAM for ArduinoJson (if available, otherwise use normal malloc)
// see: https://arduinojson.org/v6/how-to/use-external-ram-on-esp32/#how-to-use-the-psram-with-arduinojson
struct SpiRamAllocator {
  void* allocate(size_t size) {
   if (psram_available())
     return heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
   else
     return malloc(size);
  }

  void deallocate(void* pointer) {
    if (psram_available())
      heap_caps_free(pointer);
    else
      return free(pointer);
  }

  void* reallocate(void* ptr, size_t new_size) {
    if (psram_available())
      return heap_caps_realloc(ptr, new_size, MALLOC_CAP_SPIRAM);
    else
      return realloc(ptr, new_size);
  }
};
using SpiRamJsonDocument = BasicJsonDocument<SpiRamAllocator>;

static const char *const TAG = "nspanel_lovelace";

NSPanelLovelace::NSPanelLovelace() {
  command_buffer_.reserve(1024);
}

bool NSPanelLovelace::restore_state_() {
  NSPanelRestoreState recovered{};
  this->pref_ = global_preferences->make_preference<NSPanelRestoreState>(/*this->get_object_id_hash() ^ */RESTORE_STATE_VERSION);
  bool restored = this->pref_.load(&recovered);
  if (restored) {
    this->display_active_dim_ = recovered.display_active_dim_;
    this->display_inactive_dim_ = recovered.display_inactive_dim_;
  }
  return restored;
}
bool NSPanelLovelace::save_state_() {
  NSPanelRestoreState state{};
  state.display_active_dim_ = this->display_active_dim_;
  state.display_inactive_dim_ = this->display_inactive_dim_;
  return this->pref_.save(&state);
}

void NSPanelLovelace::setup() {
  this->restore_state_();

#ifdef USE_TIME
  this->setup_time_();
#endif
  // The display isn't reset when ESP is reset (on ota update etc.)
  // so we need to simulate the display 'startup' instead
  // see: https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/system/misc_system_api.html#_CPPv418esp_reset_reason_t
  auto reason = esp_reset_reason();
  if (reason == esp_reset_reason_t::ESP_RST_SW ||
      reason == esp_reset_reason_t::ESP_RST_DEEPSLEEP/* ||
      reason == esp_reset_reason_t::ESP_RST_USB*/) {
// #ifdef USE_ESP_IDF
//     gpio_set_level(GPIO_NUM_4, !gpio_get_level(GPIO_NUM_4));
//     delay(200);
//     gpio_set_level(GPIO_NUM_4, !gpio_get_level(GPIO_NUM_4));
// #else
//     digitalWrite(GPIO4, !digitalRead(GPIO4));
//     delay(200);
//     digitalWrite(GPIO4, !digitalRead(GPIO4));
// #endif
    this->set_display_dim();
    this->render_page_(render_page_option::screensaver);
  }

  if (!this->weather_entity_id_.empty()) {
    // state provides the information for the icon
    this->subscribe_homeassistant_state(
        &NSPanelLovelace::on_weather_state_update_, this->weather_entity_id_);
    this->subscribe_homeassistant_state(
        &NSPanelLovelace::on_weather_temperature_update_,
        this->weather_entity_id_, ha_attr_type::temperature);
    this->subscribe_homeassistant_state(
        &NSPanelLovelace::on_weather_temperature_unit_update_,
        this->weather_entity_id_, ha_attr_type::temperature_unit);
    // note: no longer available in HA 2024.4+
    // this->subscribe_homeassistant_state(
    //     &NSPanelLovelace::on_weather_forecast_update_, this->weather_entity_id_,
    //     "forecast");

    // todo: call the weather.get_forecasts service periodically (~1/hr?) 
  }
  
  for (auto &entity : this->entities_) {
    auto &entity_id = entity->get_entity_id();
    ESP_LOGV(TAG, "Adding subscriptions for entity '%s'", entity_id.c_str());
    bool add_state_subscription = false;
    if (entity->is_type(entity_type::light)) {
      add_state_subscription = true;
      // selectively subscribe to light attributes based on the supported color modes
      this->subscribe_homeassistant_state(
          &NSPanelLovelace::on_entity_attr_supported_color_modes_update_, 
          entity_id, ha_attr_type::supported_color_modes);
      // need to subscribe to brightness to know if brightness is supported
      this->subscribe_homeassistant_state(
          &NSPanelLovelace::on_entity_attr_brightness_update_, 
          entity_id, ha_attr_type::brightness);
    }
    else if (entity->is_type(entity_type::switch_) ||
        entity->is_type(entity_type::input_boolean) ||
        entity->is_type(entity_type::automation) ||
        entity->is_type(entity_type::fan) ||
        entity->is_type(entity_type::timer)) {
      add_state_subscription = true;
    }
    // icons and unit_of_measurement based on state and device_class
    else if (entity->is_type(entity_type::sensor) ||
        entity->is_type(entity_type::binary_sensor)) {
      add_state_subscription = true;
      // if (!entity->is_icon_value_overridden()) {
        this->subscribe_homeassistant_state(
            &NSPanelLovelace::on_entity_attr_device_class_update_, 
            entity_id, ha_attr_type::device_class);
      // }
      this->subscribe_homeassistant_state(
          &NSPanelLovelace::on_entity_attr_unit_of_measurement_update_, 
          entity_id, ha_attr_type::unit_of_measurement);
    }
    else if (entity->is_type(entity_type::cover)) {
      add_state_subscription = true;
      // supported_features, current_position, device_class
      this->subscribe_homeassistant_state(
          &NSPanelLovelace::on_entity_attr_device_class_update_, 
          entity_id, ha_attr_type::device_class);
      this->subscribe_homeassistant_state(
          &NSPanelLovelace::on_entity_attr_supported_features_update_, 
          entity_id, ha_attr_type::supported_features);
      this->subscribe_homeassistant_state(
          &NSPanelLovelace::on_entity_attr_current_position_update_, 
          entity_id, ha_attr_type::current_position);
    }
    else if (entity->is_type(entity_type::alarm_control_panel)) {
      add_state_subscription = true;
      this->subscribe_homeassistant_state(
        &NSPanelLovelace::on_entity_attr_code_arm_required_update_,
        entity_id, ha_attr_type::code_arm_required);
    }
    else if (entity->is_type(entity_type::timer)) {
      add_state_subscription = true;
      this->subscribe_homeassistant_state(
        &NSPanelLovelace::on_entity_attr_editable_update_,
        entity_id, ha_attr_type::editable);
      this->subscribe_homeassistant_state(
        &NSPanelLovelace::on_entity_attr_duration_update_,
        entity_id, ha_attr_type::duration);
      this->subscribe_homeassistant_state(
        &NSPanelLovelace::on_entity_attr_remaining_update_,
        entity_id, ha_attr_type::remaining);
      this->subscribe_homeassistant_state(
        &NSPanelLovelace::on_entity_attr_finishes_at_update_,
        entity_id, ha_attr_type::finishes_at);
    }

    if (add_state_subscription) {
      this->subscribe_homeassistant_state(
        &NSPanelLovelace::on_entity_state_update_,
        entity_id);
    }
  }
}

void NSPanelLovelace::loop() {
#ifdef USE_NSPANEL_TFT_UPLOAD
  if (this->is_updating_ || this->reparse_mode_) {
    return;
  }
#endif

  // Monitor for commands arriving from the screen over UART
  uint8_t d;
  while (this->available()) {
    this->read_byte(&d);
    this->buffer_.push_back(d);
    if (!this->process_data_()) {
      ESP_LOGW(TAG, "Unparsed data: 0x%02x", d);
      this->buffer_.clear();
    }
  }

  if (this->force_current_page_update_) {
    this->force_current_page_update_ = false;
    ESP_LOGD(TAG, "Render HA update");
    if (this->popup_page_current_uuid_.empty()) {
      this->render_item_update_(this->current_page_);
    } else {
      this->render_popup_page_update_(this->cached_page_item_);
    }
  }

  // Throttle command processing to avoid flooding the display with commands
  if ((millis() - this->command_last_sent_) > COMMAND_COOLDOWN) {
    this->process_display_command_queue_();
  }
}

std::shared_ptr<Entity> NSPanelLovelace::create_entity(const std::string &entity_id) {
  for (auto &e : this->entities_) {
    if (entity_id == e->get_entity_id()) return e;
  }
  auto entity = std::make_shared<Entity>(entity_id);
  this->entities_.push_back(entity);
  return entity;
}

void NSPanelLovelace::on_page_item_added_callback(const std::shared_ptr<PageItem> &item) {
  bool found = false;
  auto &item_uuid = item->get_uuid();

  if (page_item_cast<StatefulPageItem>(item.get())) {
    for (auto &item : this->stateful_page_items_) {
      if (item->get_uuid() == item_uuid) {
        found = true;
        break;
      }
    }
    if (!found) {
      auto& stateful_item = (const std::shared_ptr<StatefulPageItem>&)item;
      this->stateful_page_items_.push_back(stateful_item);
      ESP_LOGV(TAG, "Adding stateful item uuid.%s %s", 
        item_uuid.c_str(),
        stateful_item->get_entity_id().c_str());
    }
  }
}

void NSPanelLovelace::set_display_timeout(uint16_t timeout) {
  this->command_buffer_
    .assign("timeout").append(1, SEPARATOR)
    .append(esphome::to_string(timeout));
  this->send_buffered_command_();
}

void NSPanelLovelace::set_display_active_dim(uint8_t active) {
  this->set_display_dim(UINT8_MAX, active);
}
void NSPanelLovelace::set_display_inactive_dim(uint8_t inactive) {
  this->set_display_dim(inactive);
}
void NSPanelLovelace::set_display_dim(uint8_t inactive, uint8_t active) {
  bool save_state = false;

  if (active != UINT8_MAX) {
    if (active > 100) {
      active = 100;
    }
    save_state = this->display_active_dim_ != active;
    this->display_active_dim_ = active;
  }

  if (inactive != UINT8_MAX) {
    if (inactive >= active) {
      // inactive must be less than active otherwise the Nextion display breaks
      inactive = active - 1;
    }
    save_state = save_state || this->display_inactive_dim_ != inactive;
    this->display_inactive_dim_ = inactive;
  }

  if (save_state) this->save_state_();
  
  this->command_buffer_
    .assign("dimmode").append(1, SEPARATOR)
    // brightness when inactive (after timeout reached)
    .append(esphome::to_string(this->display_inactive_dim_)).append(1, SEPARATOR)
    // brightness when active (when buttons pressed)
    .append(esphome::to_string(this->display_active_dim_)).append(1, SEPARATOR)
    // background colour when active (not screensaver background, defaults to ha-dark)
    .append(esphome::to_string(6371));
  
  this->send_buffered_command_();
}

void NSPanelLovelace::set_day_of_week_override(DayOfWeekMap::dow dow, const std::array<const char *, 2> &value) {
  assert(dow < 7);
  switch(dow) {
  case DayOfWeekMap::dow::sunday:
    this->day_of_week_map_.set_sunday(value);
    break;
  case DayOfWeekMap::dow::monday:
    this->day_of_week_map_.set_monday(value);
    break;
  case DayOfWeekMap::dow::tuesday:
    this->day_of_week_map_.set_tuesday(value);
    break;
  case DayOfWeekMap::dow::wednesday:
    this->day_of_week_map_.set_wednesday(value);
    break;
  case DayOfWeekMap::dow::thursday:
    this->day_of_week_map_.set_thursday(value);
    break;
  case DayOfWeekMap::dow::friday:
    this->day_of_week_map_.set_friday(value);
    break;
  case DayOfWeekMap::dow::saturday:
    this->day_of_week_map_.set_saturday(value);
    break;
  }
}

bool NSPanelLovelace::process_data_() {
  uint32_t at = this->buffer_.size() - 1;
  auto *data = &this->buffer_[0];
  uint8_t new_byte = data[at];

  // Byte 0: HEADER1 (always 0x55)
  if (at == 0)
    return new_byte == 0x55;
  // Byte 1: HEADER2 (always 0xBB)
  if (at == 1)
    return new_byte == 0xBB;

  // Byte 3 & 4 - length (little endian)
  if (at == 2 || at == 3) {
    return true;
  }
  uint16_t length = encode_uint16(data[3], data[2]);

  // Wait until all data comes in
  if (at - 4 < length) {
    //    ESP_LOGD(TAG, "Message (%d/%d): 0x%02x", at - 3, length, new_byte);
    return true;
  }

  // Last two bytes: CRC; return after first one
  if (at == 4 + length) {
    return true;
  }

  uint16_t crc16 = encode_uint16(data[4 + length + 1], data[4 + length]);
  uint16_t calculated_crc16 = esphome::crc16(data, 4 + length);

  if (crc16 != calculated_crc16) {
    ESP_LOGW(TAG, "Received invalid message checksum %02X!=%02X", crc16, calculated_crc16);
    return false;
  }

  const uint8_t *message_data = data + 4;
  std::string message(message_data, message_data + length);

  this->process_command_(message);
  this->buffer_.clear();
  return true;
}

void NSPanelLovelace::process_command_(const std::string &message) {
  ESP_LOGD(TAG, "Screen CMD: %s", message.c_str());

  std::vector<std::string> tokens;
  tokens.reserve(5);
  split_str(',', message, tokens);
  if (tokens.size() < 2 || tokens.at(0) != "event") { return; }

  // note: from luibackend/mqtt.py
  if (tokens.at(1) == action_type::buttonPress2) {
    if (tokens.size() == 5) {
      this->process_button_press_(tokens.at(2), tokens.at(3), tokens.at(4));
    } else if (tokens.size() == 4) {
      this->process_button_press_(tokens.at(2), tokens.at(3));
    }
  } else if (tokens.at(1) == action_type::pageOpenDetail) {
    this->render_popup_page_(tokens.at(3));
  } else if (tokens.at(1) == action_type::sleepReached) {
    //std::string page = tokens.at(2);

    // todo: temporary, render default page instead
    this->render_page_(render_page_option::screensaver);
  } else if (tokens.at(1) == action_type::startup) {
    // restore dimmode state
    this->set_display_dim();
    this->render_page_(render_page_option::screensaver);
#ifdef USE_TIME
    // If the TFT is reset then the time needs reconfiguring
    if (this->time_configured_) {
      this->update_datetime();
    }
#endif
  }

  this->incoming_msg_callback_.call(message);
}

void NSPanelLovelace::render_page_(size_t index) {
  if (index > this->pages_.size() - 1) return;
  this->current_page_index_ = index;
  this->current_page_ = this->pages_.at(index).get();
  this->force_current_page_update_ = false;
  this->render_current_page_();
}

void NSPanelLovelace::render_page_(render_page_option d) {
  uint8_t start_page_index = 1;
  if (d == render_page_option::default_page) {
    // todo: fetch default page from config
    this->current_page_index_ = start_page_index;
  } if (d == render_page_option::screensaver) {
    this->current_page_index_ = this->screensaver_ == nullptr ? start_page_index : 0;
  } else if (d == render_page_option::next) {
    if (this->current_page_index_ == this->pages_.size() - 1)
      this->current_page_index_ = start_page_index;
    else 
      ++this->current_page_index_;
  } else if (d == render_page_option::prev) {
    if (this->current_page_index_ <= start_page_index)
      this->current_page_index_ = this->pages_.size() - 1;
    else
      --this->current_page_index_;
  } else if (d == render_page_option::down) {
    // todo?
  }
  this->current_page_ = this->pages_.at(this->current_page_index_).get();
  this->force_current_page_update_ = false;
  this->render_current_page_();
}

void NSPanelLovelace::render_current_page_() {
  if (this->current_page_ == nullptr)
    this->render_page_(render_page_option::default_page);

  this->command_buffer_.assign("pageType")
      .append(1, SEPARATOR)
      .append(this->current_page_->get_type());
  this->send_buffered_command_();
  this->popup_page_current_uuid_.clear();

  this->set_display_timeout(this->current_page_->get_sleep_timeout());
  
  this->render_item_update_(this->current_page_);
}

void NSPanelLovelace::render_item_update_(Page *page) {
  page->render(this->command_buffer_);
  this->send_buffered_command_();

  if (page->is_type(page_type::screensaver) && this->screensaver_ != nullptr) {
    if (this->screensaver_->should_render_status_update()) {
      this->screensaver_->render_status_update(this->command_buffer_);
      this->send_buffered_command_();
    }
  }
}

void NSPanelLovelace::render_popup_page_(const std::string &internal_id) {
  if (this->current_page_ == nullptr) return;
  this->render_popup_page_update_(internal_id);
  this->set_display_timeout(10);
}

void NSPanelLovelace::render_popup_page_update_(const std::string &internal_id) {
  if (this->current_page_ == nullptr) return;
  auto uuid = internal_id.substr(5);

  if (this->cached_page_item_ == nullptr || this->cached_page_item_->get_uuid() != uuid) {
    // Only search for items in the current page to reduce processing time
    for (auto &item : this->current_page_->get_items()) {
      if (item->get_uuid() != uuid) continue;
      if (auto page_item = page_item_cast<StatefulPageItem>(item.get())) {
        this->cached_page_item_ = page_item;
        break;
      } else {
        return;
      }
    }
  }
  
  this->popup_page_current_uuid_ = uuid;
  this->render_popup_page_update_(this->cached_page_item_);
}

void NSPanelLovelace::render_popup_page_update_(StatefulPageItem *item) {
  if (item == nullptr) return;

  if (item->is_type(entity_type::light)) {
    this->render_light_detail_update_(item);
  } else if (item->is_type(entity_type::timer)) {
    this->render_timer_detail_update_(item);
  } else {
    return;
  }

  this->send_buffered_command_();
}

// entityUpdateDetail~{entity_id}~~{icon_color}~{switch_val}~{brightness}~{color_temp}~{color}~{color_translation}~{color_temp_translation}~{brightness_translation}~{effect_supported}
void NSPanelLovelace::render_light_detail_update_(StatefulPageItem *item) {
  if (item == nullptr) return;

  auto entity = item->get_entity();
  auto &supported_modes = entity->get_attribute(ha_attr_type::supported_color_modes);
  bool enable_color_wheel = entity->get_state() == generic_type::on &&
      (contains_value(supported_modes, ha_attr_color_mode::xy) || 
      contains_value(supported_modes, ha_attr_color_mode::hs) ||
      contains_value(supported_modes, ha_attr_color_mode::rgb) ||
      contains_value(supported_modes, ha_attr_color_mode::rgbw));

  std::string color_mode = entity->get_attribute(ha_attr_type::color_mode);
  std::string color_temp = generic_type::disable;
  if (contains_value(supported_modes, ha_attr_color_mode::color_temp)) {
    if (color_mode == ha_attr_color_mode::color_temp) {
      color_temp = entity->get_attribute(ha_attr_type::color_temp, generic_type::disable);
    } else {
      color_temp = generic_type::unknown;
    }
  } else {
    color_temp = generic_type::disable;
  }

  this->command_buffer_
      // entityUpdateDetail~
      .assign("entityUpdateDetail").append(1, SEPARATOR)
      // entity_id~~
      .append("uuid.").append(item->get_uuid()).append(2, SEPARATOR)
      // icon_color~
      .append(item->get_icon_color_str()).append(1, SEPARATOR)
      // switch_val~
      .append(std::to_string(entity->get_state() == generic_type::on ? 1 : 0)).append(1, SEPARATOR)
      // brightness~ (0-100)
      .append(entity->get_attribute(ha_attr_type::brightness, generic_type::disable)).append(1, SEPARATOR)
      // todo: color_temp~ (color temperature value or 'disable')
      .append(color_temp).append(1, SEPARATOR)
      // todo: color~ ('enable' or 'disable')
      .append(enable_color_wheel ? generic_type::enable : generic_type::disable).append(1, SEPARATOR)
      // color_translation~
      .append("Colour").append(1, SEPARATOR)
      // color_temp_translation~
      .append("Colour temperature").append(1, SEPARATOR)
      // brightness_translation~
      .append("Brightness").append(1, SEPARATOR)
      // effect_supported ('enable' or 'disable')
      // todo: requires parsing 'enabled_features' attribute
      .append(generic_type::disable);
}

// entityUpdateDetail~{entity_id}~~{icon_color}~{entity_id}~{min_remaining}~{sec_remaining}~{editable}~{action1}~{action2}~{action3}~{label1}~{label2}~{label3}
void NSPanelLovelace::render_timer_detail_update_(StatefulPageItem *item) {
  if (item == nullptr) return;

  auto &state = item->get_state();
  bool render = false;
  uint16_t min_remaining = 0, sec_remaining = 0;
  bool idle = state == "paused" || state == "idle";

  if (idle) {
    std::string time_remaining_str;
    if (state == "paused") {
      time_remaining_str = item->get_attribute(ha_attr_type::remaining);
    } else {
      time_remaining_str = item->get_attribute(ha_attr_type::duration);
    }
    if (!time_remaining_str.empty()) {
      std::vector<std::string> time_parts;
      split_str(':', time_remaining_str, time_parts);
      if (time_parts.size() == 3) {
        min_remaining = (stoi(time_parts[0]) * 60) + stoi(time_parts[1]);
        sec_remaining = stoi(time_parts[2]);
        render = true;
      }
    }
  }
  // active
  else {
    auto &finishes_at = item->get_attribute(ha_attr_type::finishes_at);
    if (!finishes_at.empty()) {
      tm t{};
      if (iso8601_to_tm(finishes_at.c_str(), t)) {
        // uint8_t hr = t.tm_hour;
        ESPTime now = this->time_id_.value()->now();
        if (now.is_valid()) {
          uint32_t seconds = static_cast<uint16_t>(
            difftime(now.timestamp, mktime(&t)));
          if (seconds >= UINT16_MAX) seconds = UINT16_MAX;
          min_remaining = seconds / 60;
          sec_remaining = seconds % 60;
          render = true;
        }
      }
    }
  }

  if (!render) {
    this->render_current_page_();
    return;
  }

  this->set_display_timeout(30);

  this->command_buffer_
    // entityUpdateDetail~
    .assign("entityUpdateDetail").append(1, SEPARATOR)
    // entity_id~~
    .append("uuid.").append(item->get_uuid()).append(2, SEPARATOR)
    // icon_color~
    .append(item->get_icon_color_str()).append(1, SEPARATOR)
    // entity_id~~
    .append("uuid.").append(item->get_uuid()).append(1, SEPARATOR)
    // min_remaining~
    .append(std::to_string(min_remaining)).append(1, SEPARATOR)
    // sec_remaining~
    .append(std::to_string(sec_remaining)).append(1, SEPARATOR)
    // editable~
    .append((idle && 
      item->get_attribute(ha_attr_type::editable) == generic_type::on)
        ? "1" : "0")
    .append(1, SEPARATOR)
    // action1~
    .append(idle ? "" : "pause").append(1, SEPARATOR)
    // action2~
    .append(idle ? "start" : "cancel").append(1, SEPARATOR)
    // action3~
    .append(idle ? "" : "finish").append(1, SEPARATOR)
    // label1~
    .append(idle ? "" : "Pause").append(1, SEPARATOR)
    // label2~
    .append(idle ? "Start" : "Cancel").append(1, SEPARATOR)
    // label3
    .append(idle ? "" : "Finish");
}

void NSPanelLovelace::dump_config() {
  ESP_LOGCONFIG(TAG, "NSPanelLovelace:");
  ESP_LOGCONFIG(TAG, "\tVersion: %s", NSPANEL_LOVELACE_BUILD_VERSION);
  // ESP_LOGCONFIG(TAG, "\tRAM: %u %u %u",
  //   psram_used(),
  //   heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
  //   heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
  ESP_LOGCONFIG(TAG, "\tState: pages:%u,stateful_items:%u,entities:%u",
      this->pages_.size(),
      this->stateful_page_items_.size(),
      this->entities_.size());
}

void NSPanelLovelace::send_nextion_command_(const std::string &command) {
  ESP_LOGD(TAG, "Sending: %s", command.c_str());
  this->write_str(command.c_str());
  const uint8_t to_send[3] = {0xFF, 0xFF, 0xFF};
  this->write_array(to_send, sizeof(to_send));
}

void NSPanelLovelace::process_display_command_queue_() {
#ifdef USE_NSPANEL_TFT_UPLOAD
  // don't execute custom commands when the screen is updating - UI updates could spoil the upload
  if (this->is_updating_) return;
#endif
  // nothing to process
  if (this->command_buffer_.empty() && this->command_queue_.empty()) return;

  // Store the command for later processing so the function can return quickly
  if (!this->command_buffer_.empty()) {
    this->command_queue_.push(this->command_buffer_);
    ESP_LOGVV(TAG, "Command queued (size: %u)", this->command_queue_.size());
    this->command_buffer_.clear();
    return;
  } else if (!this->command_queue_.empty()) {
    // todo: can we use std::move? it changes the capacity of the buffer
    this->command_buffer_.assign(this->command_queue_.front());
    this->command_queue_.pop();
    ESP_LOGVV(TAG, "Command un-queued (size: %u)", this->command_queue_.size());
  }

  ESP_LOGD(TAG, "Sending: %s", this->command_buffer_.c_str());
  // todo: break this in to chunks and use write_array() in loop() instead (similar to command_queue)
  std::vector<uint8_t> data;
  data.reserve(this->command_buffer_.length() + 6);
  data.assign({
    0x55, 0xBB, 
    static_cast<uint8_t>(command_buffer_.length() & 0xFF),
    static_cast<uint8_t>((command_buffer_.length() >> 8) & 0xFF)
  });
  data.insert(data.end(), this->command_buffer_.begin(), this->command_buffer_.end());
  auto crc = esphome::crc16(data.data(), data.size());
  data.push_back(static_cast<uint8_t>(crc & 0xFF));
  data.push_back(static_cast<uint8_t>((crc >> 8) & 0xFF));

  App.feed_wdt();
  this->write_array(data);
  
  this->command_buffer_.clear();
  this->command_last_sent_ = millis();
}

void NSPanelLovelace::send_buffered_command_() {
  if (this->command_buffer_.empty()) return;
  this->process_display_command_queue_();
}

void NSPanelLovelace::send_display_command(const char *command) {
  this->command_buffer_.assign(command);
  this->send_buffered_command_();
}

#ifdef USE_NSPANEL_TFT_UPLOAD
uint16_t NSPanelLovelace::recv_ret_string_(std::string &response, uint32_t timeout, bool recv_flag) {
  uint16_t ret;
  uint8_t c = 0;
  uint8_t nr_of_ff_bytes = 0;
  uint64_t start;
  bool exit_flag = false;
  bool ff_flag = false;

  start = millis();

  while ((timeout == 0 && this->available()) || millis() - start <= timeout) {
    if (!this->available()) {
      App.feed_wdt();
      continue;
    }

    this->read_byte(&c);
    if (c == 0xFF) {
      nr_of_ff_bytes++;
    } else {
      nr_of_ff_bytes = 0;
      ff_flag = false;
    }

    if (nr_of_ff_bytes >= 3)
      ff_flag = true;

    response += (char)c;
    if (recv_flag) {
      if (response.find(0x05) != std::string::npos) {
        exit_flag = true;
      }
    }
    App.feed_wdt();
    delay(2);

    if (exit_flag || ff_flag) {
      break;
    }
  }

  if (ff_flag)
    response = response.substr(0, response.length() - 3); // Remove last 3 0xFF

  ret = response.length();
  return ret;
}

void NSPanelLovelace::start_reparse_mode_() {
  this->send_nextion_command_("DRAKJHSUYDGBNCJHGJKSHBDN");
  this->send_nextion_command_("recmod=0");
  this->send_nextion_command_("recmod=0");
  this->send_nextion_command_("connect");
  reparse_mode_ = true;
}

void NSPanelLovelace::exit_reparse_mode_() {
  this->send_nextion_command_("recmod=1");
  reparse_mode_ = false;
}
#endif // USE_NSPANEL_TFT_UPLOAD

void NSPanelLovelace::init_display_(int baud_rate) {
  // hopefully on NSPanel it should always be an ESP32ArduinoUARTComponent instance
#ifdef USE_ESP_IDF
  auto *uart = reinterpret_cast<uart::IDFUARTComponent*>(this->parent_);
#else
  auto *uart = reinterpret_cast<uart::ESP32ArduinoUARTComponent*>(this->parent_);
#endif
  uart->set_baud_rate(baud_rate);
  uart->setup();
}

#ifdef USE_TIME
// see: https://esphome.io/components/time/#strftime
void NSPanelLovelace::update_datetime(const datetime_mode mode, const char *date_format, const char *time_format) {
  ESPTime now = this->time_id_.value()->now();

  if (!now.is_valid()) {
    ESP_LOGW(TAG, "esphome time invalid, using default time");
    now = esphome::ESPTime::from_epoch_utc(0);
  }

  // ESP_LOGV(TAG, "datetime update %u,%u %u,%u", now.hour, this->now_hour_, now.minute, this->now_minute_);
  this->now_hour_ = now.hour;
  this->now_minute_ = now.minute;

  if ((mode & datetime_mode::date) == datetime_mode::date) {
    std::string datefmt(date_format);
    // todo: fetch from config before using default value
    if (datefmt.empty())
      datefmt = this->date_format_;
    
    DayOfWeekMap::dow_mode dow_mode = DayOfWeekMap::dow_mode::none;
    if (datefmt.find("%A", 0) != std::string::npos) {
      dow_mode = (DayOfWeekMap::dow_mode)(dow_mode |
                  DayOfWeekMap::dow_mode::long_dow);
    }
    if (datefmt.find("%a", 0) != std::string::npos ||
        datefmt.find("%c", 0) != std::string::npos || 
        datefmt.find("%h", 0) != std::string::npos) {
      dow_mode = (DayOfWeekMap::dow_mode)(dow_mode |
                  DayOfWeekMap::dow_mode::short_dow);
    }

    auto timestr = now.strftime(datefmt);
    this->command_buffer_
      .assign("date").append(1, SEPARATOR)
      .append(this->day_of_week_map_.replace(timestr, dow_mode));
    this->send_buffered_command_();
  }

  if ((mode & datetime_mode::time) == datetime_mode::time) {
    std::string timefmt(time_format);
    // todo: fetch from config before using default value
    if (timefmt.empty())
      timefmt = this->time_format_;
    
    this->command_buffer_
      .assign("time").append(1, SEPARATOR)
      .append(now.strftime(timefmt));
    this->send_buffered_command_();
  }
}

void NSPanelLovelace::setup_time_() {
  if (this->time_id_.has_value()) {
    this->time_id_.value()->add_on_time_sync_callback([this] {
      this->update_datetime(datetime_mode::both);
    });
    // Configure a callback to check the time every 1 second
    this->set_interval("check_time", 1000, [this] {
      this->check_time_();
    });
    this->time_configured_ = true;
  } else {
    ESP_LOGW(TAG, "time_id not configured, default time displayed");
  }
}

void NSPanelLovelace::check_time_() {
  if (!this->time_id_.has_value() ||
      !this->time_id_.value()->now().is_valid())
    return;

  // todo: only check if the current page is screensaver?
  ESPTime now = this->time_id_.value()->now();
  // update the date once an hour to account for daylight saving etc.
  if (now.hour != this->now_hour_) {
    this->update_datetime(datetime_mode::both);
  }
  // update the time every minute
  else if (now.minute != this->now_minute_) {
    this->update_datetime(datetime_mode::time);
  }
}

#endif

size_t NSPanelLovelace::find_page_index_by_uuid_(const std::string &uuid) const {
  size_t index = 0;
  for (auto &p : this->pages_) {
    if (p->get_uuid() == uuid)
      return index;
    ++index;
  }
  return SIZE_MAX;
}

const std::string &NSPanelLovelace::try_replace_uuid_with_entity_id_(
    const std::string &uuid_or_entity_id) {
  // not a uuid if it does not begin with the uuid prefix
  // (navigation uuids are dealt with separately)
  if (!starts_with(uuid_or_entity_id, entity_type::uuid))
    return uuid_or_entity_id;

  auto uuid = uuid_or_entity_id.substr(5);
  auto item = this->get_page_item_(uuid);
  if (item == nullptr) return uuid_or_entity_id;
  
  return item->get_entity_id();
}

void NSPanelLovelace::process_button_press_(
    std::string &internal_id, 
    const std::string &button_type, 
    const std::string &value,
    bool called_from_timeout) {
  if (button_type.empty()) return;
  
  // Throttle and filter processing of spammy actions to avoid command flooding
  if (!called_from_timeout) {
    if (internal_id == this->button_press_uuid_ && 
        button_type == this->button_press_type_) {
      this->button_press_value_ = value;
      if (this->button_press_timeout_set_) return;
      this->set_timeout("btnpr", 200, [this]() {
        this->button_press_timeout_set_ = false;
        ESP_LOGD(TAG, "Button press delayed: %s,%s,%s", 
            this->button_press_uuid_.c_str(), this->button_press_type_.c_str(), 
            this->button_press_value_.c_str());
        this->process_button_press_(
            this->button_press_uuid_, this->button_press_type_, 
            this->button_press_value_, true);
      });
      this->button_press_timeout_set_ = true;
      return;
    } else if (this->button_press_timeout_set_) {
      this->cancel_timeout("btnpr");
      this->button_press_timeout_set_ = false;
    }
    this->button_press_uuid_ = internal_id;
    this->button_press_type_ = button_type;
  }

  auto entity_type = get_entity_type(internal_id);
  std::string& entity_id = internal_id;
  
  if (entity_type == entity_type::uuid) {
    entity_id = this->try_replace_uuid_with_entity_id_(internal_id);
    ESP_LOGV(TAG, "Lookup %s -> %s", internal_id.c_str(), entity_id.c_str());
    entity_type = get_entity_type(entity_id);
    if (entity_type == nullptr) return;
  }

  // Screen tapped when on the screensaver, show the default card or use the first card in the config.
  if (internal_id == page_type::screensaver && button_type == button_type::bExit) {
    // todo: make a note of last used card
    //
    // config.get("screensaver.defaultCard")
    // use defaultCard if defaultCard not null

    // _previous_card.clear();
    // _current_card = action_type::screensaver;
    // render_card(_current_card);

    // todo: temporary for testing
    this->render_page_(render_page_option::default_page);
    return;
  }

  if (button_type == button_type::sleepReached) {
    // todo
    // make a note of last used card then render screensaver
    // _previous_card = _current_card;
    // _current_card = action_type::screensaver;
    // render_page_(_current_card);
    this->render_page_(render_page_option::screensaver);
    return;
  }

  if (button_type == button_type::bExit) {
    this->render_current_page_();
    return;
  }

  if (button_type == button_type::onOff) {
    if (!value.empty()) {
      this->call_ha_service_(
        entity_type, 
        value == "1" ? ha_action_type::turn_on : ha_action_type::turn_off, 
        entity_id);
    }
  } else if (button_type == button_type::numberSet) {
    // todo
  }
  // cover and shutter cards
  else if (button_type == button_type::up) {
    this->call_ha_service_(
      entity_type, ha_action_type::open_cover, entity_id);
  } else if (button_type == button_type::stop) {
    this->call_ha_service_(
      entity_type, ha_action_type::stop_cover, entity_id);
  } else if (button_type == button_type::down) {
    this->call_ha_service_(
      entity_type, ha_action_type::close_cover, entity_id);
  } else if (button_type == button_type::positionSlider) {
    // todo
  } else if (button_type == button_type::tiltOpen) {
    // todo
  } else if (button_type == button_type::tiltStop) {
    // todo
  } else if (button_type == button_type::tiltClose) {
    // todo
  } else if (button_type == button_type::tiltSlider) {
    // todo
  } else if (button_type == button_type::button) {
    if (entity_type == entity_type::navigate ||
        entity_type == entity_type::navigate_uuid) {
      auto uuid = internal_id.substr(strlen(entity_type) + 1);
      this->render_page_(this->find_page_index_by_uuid_(uuid));
    } else if (
        entity_type == entity_type::scene ||
        entity_type == entity_type::script) {
      this->call_ha_service_(
          entity_type, ha_action_type::turn_on,
          entity_id);
    } else if (
        entity_type == entity_type::light ||
        entity_type == entity_type::switch_ ||
        entity_type == entity_type::input_boolean ||
        entity_type == entity_type::automation ||
        entity_type == entity_type::fan) {
      this->call_ha_service_(
          entity_type, ha_action_type::toggle,
          entity_id);
    } else if (
        entity_type == entity_type::button ||
        entity_type == entity_type::input_button) {
      this->call_ha_service_(
          entity_type, ha_action_type::press,
          entity_id);
    }
  }
  // media cards
  else if (button_type == button_type::mediaNext) {
    // todo
  } else if (button_type == button_type::mediaBack) {
    // todo
  } else if (button_type == button_type::mediaPause) {
    // todo
  } else if (button_type == button_type::mediaOnOff) {
    // todo
  } else if (button_type == button_type::mediaShuffle) {
    // todo
  } else if (button_type == button_type::volumeSlider) {
    // todo
  } else if (button_type == button_type::speakerSel) {
    // todo
  }
  // light cards
  else if (button_type == button_type::brightnessSlider) {
    if (value.empty()) return;
    this->call_ha_service_(
      entity_type, 
      ha_action_type::turn_on, 
      {{
        {(const char*)ha_attr_type::entity_id, entity_id},
        // scale 0-100 to ha brightness range
        {(const char*)ha_attr_type::brightness, std::to_string(
          static_cast<int>(
            scale_value(std::stoi(value), {0, 100}, {0, 255})
          )).c_str()}
      }});
  } else if (button_type == button_type::colorTempSlider) {
    if (value.empty()) return;
    auto entity = get_entity_(entity_id);
    if (entity == nullptr) return;
    auto &minstr = entity->get_attribute(ha_attr_type::min_mireds);
    auto &maxstr = entity->get_attribute(ha_attr_type::max_mireds);
    uint16_t min_mireds = minstr.empty() ? 153 : std::stoi(minstr);
    uint16_t max_mireds = maxstr.empty() ? 500 : std::stoi(maxstr);
    if (min_mireds >= max_mireds) {
      ESP_LOGW(TAG, "min/max mired range invalid %i>=%i", min_mireds, max_mireds);
      min_mireds = 153;
      max_mireds = 500;
    }
    
    this->call_ha_service_(
      entity_type, 
      ha_action_type::turn_on, 
      {{
        {(const char*)ha_attr_type::entity_id, entity_id},
        // scale 0-100 from slider to color range of the light
        {(const char*)ha_attr_type::color_temp, std::to_string(
          static_cast<int>(
            scale_value(std::stoi(value), {0, 100},
            {static_cast<double>(min_mireds), static_cast<double>(max_mireds)})
          )).c_str()}
      }});
  } else if (button_type == button_type::colorWheel) {
    if (value.empty()) return;

    std::vector<std::string> xy_tokens;
    split_str('|', value, xy_tokens);
    if (xy_tokens.size() != 3) return;

    std::string rgb_str = to_string(
        xy_to_rgb(
          std::stod(xy_tokens[0]),
          std::stod(xy_tokens[1]),
          std::stod(xy_tokens[2])
        ), ',', '[', ']');

    this->call_ha_service_(
      entity_type, 
      ha_action_type::turn_on, 
      {{
        {(const char*)ha_attr_type::entity_id, entity_id}
      }},
      {{
        {(const char*)ha_attr_type::rgb_color, rgb_str.c_str()}
      }});
  } else if (
    button_type == button_type::armHome ||
    button_type == button_type::armAway ||
    button_type == button_type::armNight ||
    button_type == button_type::armVacation ||
    button_type == button_type::disarm)
  {
    auto action = std::string("alarm_").append(button_type);
    if (value.empty()) {
      this->call_ha_service_(entity_type, action.c_str(), entity_id);
    } else {
      this->call_ha_service_(
        entity_type, action.c_str(), 
        {{
          {(const char*)ha_attr_type::entity_id, entity_id},
          {(const char*)ha_attr_type::code, value.c_str()}
        }});
    }
  } else if (starts_with(button_type, entity_type::timer)) {
    std::string service(button_type);
    service[5] = '.';
    this->call_ha_service_(service, entity_id);
  }
}

StatefulPageItem* NSPanelLovelace::get_page_item_(const std::string &uuid) {
  // use cached version if possible
  if (this->cached_page_item_ != nullptr && 
      this->cached_page_item_->get_uuid() == uuid) 
    return this->cached_page_item_;

  for (auto& item : this->stateful_page_items_) {
    if (item->get_uuid() != uuid) continue;
    return this->cached_page_item_ = item.get();
  }
  return this->cached_page_item_ = nullptr;
}

Entity* NSPanelLovelace::get_entity_(const std::string &entity_id) {
  // use cached version if possible
  if (this->cached_entity_ != nullptr && 
      this->cached_entity_->get_entity_id() == entity_id) 
    return this->cached_entity_;

  for (auto& entity : this->entities_) {
    if (entity->get_entity_id() != entity_id) continue;
    return this->cached_entity_ = entity.get();
  }
  return this->cached_entity_ = nullptr;
}

void NSPanelLovelace::call_ha_service_(
    const std::string &service, const std::string &entity_id) {
  this->call_ha_service_(service, {{ha_attr_type::entity_id, entity_id}}, {});
}

void NSPanelLovelace::call_ha_service_(
    const char *entity_type, const char *action, const std::string &entity_id) {
  this->call_ha_service_(entity_type, action, {{ha_attr_type::entity_id, entity_id}});
}

void NSPanelLovelace::call_ha_service_(
    const char *entity_type, const char *action,
    const std::map<std::string, std::string> &data) {
  // this->call_homeassistant_service(service, data);
  this->call_ha_service_(entity_type, action, data, {});
}

void NSPanelLovelace::call_ha_service_(
    const char *entity_type, const char *action,
    const std::map<std::string, std::string> &data,
    const std::map<std::string, std::string> &data_template) {
  this->call_ha_service_(
    std::string(entity_type).append(1, '.').append(action),
    data, data_template);
}

void NSPanelLovelace::call_ha_service_(
    const std::string &service,
    const std::map<std::string, std::string> &data,
    const std::map<std::string, std::string> &data_template) {
  api::HomeassistantServiceResponse resp;
  resp.service = service;

  auto it = data.find(ha_attr_type::entity_id);
  if (it == data.end())
    ESP_LOGD(TAG, "Call HA: %s -> %s", resp.service.c_str(), it->second.c_str());
  else
    ESP_LOGD(TAG, "Call HA: %s", resp.service.c_str());

  for (auto &it : data) {
    api::HomeassistantServiceMap kv;
    kv.key = it.first;
    kv.value = it.second;
    resp.data.push_back(kv);
  }
  for (auto &it : data_template) {
    api::HomeassistantServiceMap kv;
    kv.key = it.first;
    kv.value = it.second;
    resp.data_template.push_back(kv);
  }

  api::global_api_server->send_homeassistant_service_call(resp);
}

void NSPanelLovelace::on_entity_state_update_(std::string entity_id, std::string state) {
  this->on_entity_attribute_update_(entity_id, ha_attr_type::state, state);
}
void NSPanelLovelace::on_entity_attr_unit_of_measurement_update_(std::string entity_id, std::string unit_of_measurement) {
  this->on_entity_attribute_update_(entity_id, ha_attr_type::unit_of_measurement, unit_of_measurement);
}
void NSPanelLovelace::on_entity_attr_device_class_update_(std::string entity_id, std::string device_class) {
  this->on_entity_attribute_update_(entity_id, ha_attr_type::device_class, device_class);
}
void NSPanelLovelace::on_entity_attr_supported_features_update_(std::string entity_id, std::string supported_features) {
  this->on_entity_attribute_update_(entity_id, ha_attr_type::supported_features, supported_features);
}
void NSPanelLovelace::on_entity_attr_supported_color_modes_update_(std::string entity_id, std::string supported_color_modes) {
  this->on_entity_attribute_update_(entity_id, ha_attr_type::supported_color_modes, supported_color_modes);

  if (supported_color_modes.empty()) return;
  if (supported_color_modes.length() == 1 && 
      contains_value(supported_color_modes, ha_attr_color_mode::onoff)) {
    return;
  }

  this->subscribe_homeassistant_state(
      &NSPanelLovelace::on_entity_attr_color_mode_update_, entity_id, ha_attr_type::color_mode);

  if (contains_value(supported_color_modes, ha_attr_color_mode::color_temp)) {
    this->subscribe_homeassistant_state(
        &NSPanelLovelace::on_entity_attr_min_mireds_update_, entity_id, ha_attr_type::min_mireds);
    this->subscribe_homeassistant_state(
        &NSPanelLovelace::on_entity_attr_max_mireds_update_, entity_id, ha_attr_type::max_mireds);
    this->subscribe_homeassistant_state(
        &NSPanelLovelace::on_entity_attr_color_temp_update_, entity_id, ha_attr_type::color_temp);
  }
}
void NSPanelLovelace::on_entity_attr_color_mode_update_(std::string entity_id, std::string color_mode) {
  this->on_entity_attribute_update_(entity_id, ha_attr_type::color_mode, color_mode);
}
void NSPanelLovelace::on_entity_attr_brightness_update_(std::string entity_id, std::string brightness) {
  this->on_entity_attribute_update_(entity_id, ha_attr_type::brightness, brightness);
}
void NSPanelLovelace::on_entity_attr_color_temp_update_(std::string entity_id, std::string color_temp) {
  this->on_entity_attribute_update_(entity_id, ha_attr_type::color_temp, color_temp);
}
void NSPanelLovelace::on_entity_attr_min_mireds_update_(std::string entity_id, std::string min_mireds) {
  this->on_entity_attribute_update_(entity_id, ha_attr_type::min_mireds, min_mireds);
}
void NSPanelLovelace::on_entity_attr_max_mireds_update_(std::string entity_id, std::string max_mireds) {
  this->on_entity_attribute_update_(entity_id, ha_attr_type::max_mireds, max_mireds);
}
void NSPanelLovelace::on_entity_attr_code_arm_required_update_(std::string entity_id, std::string code_required) {
  this->on_entity_attribute_update_(entity_id, ha_attr_type::code_arm_required, code_required);
}
void NSPanelLovelace::on_entity_attr_current_position_update_(std::string entity_id, std::string current_position) {
  this->on_entity_attribute_update_(entity_id, ha_attr_type::current_position, current_position);
}
void NSPanelLovelace::on_entity_attr_editable_update_(std::string entity_id, std::string editable) {
  this->on_entity_attribute_update_(entity_id, ha_attr_type::editable, editable);
}
void NSPanelLovelace::on_entity_attr_duration_update_(std::string entity_id, std::string duration) {
  this->on_entity_attribute_update_(entity_id, ha_attr_type::duration, duration);
}
void NSPanelLovelace::on_entity_attr_remaining_update_(std::string entity_id, std::string remaining) {
  this->on_entity_attribute_update_(entity_id, ha_attr_type::remaining, remaining);
}
void NSPanelLovelace::on_entity_attr_finishes_at_update_(std::string entity_id, std::string finishes_at) {
  this->on_entity_attribute_update_(entity_id, ha_attr_type::finishes_at, finishes_at);
}
void NSPanelLovelace::on_entity_attribute_update_(const std::string &entity_id, const char *attr, const std::string &attr_value) {
  auto entity = this->get_entity_(entity_id);
  if (entity == nullptr) return;

  if (attr == ha_attr_type::state) {
    entity->set_state(attr_value);
  } else {
    entity->set_attribute(attr, attr_value);
  }

  ESP_LOGD(TAG, "HA update: %s %s='%s'",
    entity_id.c_str(), attr, attr_value.c_str());

  // if (this->force_current_page_update_) return;

  // If there are lots of entity attributes that update within a short time
  // then this will queue lots of commands unnecessarily.
  // This re-schedules updates every time one happens within a 200ms period.
  this->set_timeout(entity_id, 200, [this, entity_id] () {
    if (this->force_current_page_update_) return;
    if (this->current_page_ == nullptr) return;

    if (this->screensaver_ != nullptr && 
        this->current_page_->is_type(page_type::screensaver)) {
      force_current_page_update_ = 
        this->screensaver_->should_render_status_update(entity_id);
      return;
    }

    // re-render only if the entity is on the currently active card
    // todo: this doesnt account for popup pages
    for (auto &item : this->current_page_->get_items()) {
      auto stateful_item = page_item_cast<StatefulPageItem>(item.get());
      if (stateful_item == nullptr) return;
      
      if (stateful_item->get_entity_id() == entity_id) {
        force_current_page_update_ = true;
        return;
      }
    }
    
    // todo: implement popup page checks too
    // if (this->popup_page_current_uuid_ == item->get_uuid()) {
    //   this->render_popup_page_update_(item);
    // } else if (this->popup_page_current_uuid_.empty()) {
    //   this->render_item_update_(this->current_page_);
    // }
  });
}

void NSPanelLovelace::send_weather_update_command_() {
  if (this->current_page_ != this->screensaver_)
    return;
  this->screensaver_->render(this->command_buffer_);
  this->send_buffered_command_();
}

void NSPanelLovelace::on_weather_state_update_(std::string entity_id, std::string state) {
  if (this->screensaver_ == nullptr) return;
  auto item = this->screensaver_->get_item<WeatherItem>(0);
  if (item == nullptr) return;
  item->set_icon_by_weather_condition(state);
  this->send_weather_update_command_();
}

void NSPanelLovelace::on_weather_temperature_update_(std::string entity_id, std::string temperature) {
  if (this->screensaver_ == nullptr) return;
  auto item = this->screensaver_->get_item<WeatherItem>(0);
  if (item == nullptr) return;
  item->set_value(std::move(temperature));
  this->send_weather_update_command_();
}

void NSPanelLovelace::on_weather_temperature_unit_update_(std::string entity_id, std::string temperature_unit) {
  if (this->screensaver_ == nullptr) return;
  WeatherItem::temperature_unit = std::move(temperature_unit);
  this->screensaver_->set_items_render_invalid();
  this->send_weather_update_command_();
}

// todo: apply this technique https://arduinojson.org/v6/how-to/deserialize-a-very-large-document/#deserialization-in-chunks
void NSPanelLovelace::on_weather_forecast_update_(const std::string &forecast_json) {
  if (this->screensaver_ == nullptr) return;
  // todo: check if we are on the screensaver otherwise don't update
  // todo: implement color updates: "color~background~tTime~timeAMPM~tDate~tMainText~tForecast1~tForecast2~tForecast3~tForecast4~tForecast1Val~tForecast2Val~tForecast3Val~tForecast4Val~bar~tMainTextAlt2~tTimeAdd"

  ArduinoJson::StaticJsonDocument<200> filter;
  filter[0]["datetime"] = true;
  filter[0]["condition"] = true;
  filter[0]["temperature"] = true;

  if (filter.overflowed()) {
    ESP_LOGW(TAG, "Weather unparsable: filter overflowed");
    return;
  }
  
  // Note: Unfortunately the json received is nearly 6KB!
  //       We filter the variables to consume less but it is still a lot,
  //       so we need to allocate an appropriate amount of memory to read it.
  SpiRamJsonDocument doc(psram_available() ? 7680 : 6144);
  ArduinoJson::DeserializationError error = ArduinoJson::deserializeJson(
    doc, (char *)forecast_json.data(), DeserializationOption::Filter(filter));
  App.feed_wdt();

  if (error || doc.overflowed()) {
    ESP_LOGW(TAG, "Weather unparsable: %s", error ? error.c_str() : "doc overflow");
    return;
  }

  this->command_buffer_.clear();

  char buff[16] = {};
  uint8_t index = 1, item_count = this->screensaver_->get_items().size();

  for (const ArduinoJson::JsonObject &item : doc.as<ArduinoJson::JsonArray>()) {
    // can only display the first 4 items (minus 1 for the current weather)
    if (index == item_count)
      break;

    auto weatherItem = this->screensaver_->get_item<WeatherItem>(index);
    if (weatherItem == nullptr)
      continue;

    weatherItem->set_icon_by_weather_condition(
        item["condition"].as<std::string>());

    // icon displayName
    // todo: import temperature symbol from config
    tm t{};
    // Parse date e.g. 2023-08-22T21:00:00+00:00
    if (!iso8601_to_tm(item["datetime"], t)) {
      ESP_LOGW(TAG, "Weather 'datetime' unparsable: %s", item["datetime"].as<const char *>());
      // return;
      t = { 
        // second, minute, hour
        0,0,0,
        // monthday, month, year
        0,0,100,
        // weekday, yearday, isdst
        0,0,0
      };
    }

    if (this->weather_forecast_type_ == weather_forcast_type::hourly) {
      // ESPTime now; now.strftime(datefmt);
      strftime(buff, sizeof(buff), this->time_format_.c_str(), &t);
      weatherItem->set_display_name(buff);
    } else {
      weatherItem->set_display_name(this->day_of_week_map_.at(t.tm_wday).at(0));
    }
    
    snprintf(buff, sizeof(buff), "%.1f", item["temperature"].as<float>());
    weatherItem->set_value(buff);

    ++index;
  }
  this->send_weather_update_command_();
}

} // namespace nspanel_lovelace
} // namespace esphome