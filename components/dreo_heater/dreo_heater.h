#pragma once
#include "esphome.h"
#include "esphome/components/climate/climate.h"
#include "esphome/components/uart/uart.h"
#include "esphome/components/switch/switch.h"
#include "esphome/components/number/number.h"
#include <cmath>
#include <deque>
#include <vector>
#include <initializer_list>
#include <algorithm>
#include <cstring>
#include <numeric>
#include <iterator>

namespace esphome {
namespace dreo_heater {

const char *PRESET_H1 = "H1";
const char *PRESET_H2 = "H2";
const char *PRESET_H3 = "H3";

class DreoHeater : public climate::Climate, public uart::UARTDevice, public Component {
 public:
  DreoHeater(uart::UARTComponent *parent) : uart::UARTDevice(parent) {}

  bool debug_mode{false};
  void set_debug(bool enable) { this->debug_mode = enable; }

  switch_::Switch *sound_switch{nullptr};
  switch_::Switch *display_switch{nullptr};
  switch_::Switch *child_lock_switch{nullptr};
  switch_::Switch *window_switch{nullptr};
  switch_::Switch *temp_unit_switch{nullptr};

  number::Number *heat_level_number{nullptr};
  number::Number *timer_number{nullptr};
  number::Number *calibration_number{nullptr};

  void set_temp_unit_switch(switch_::Switch *s) { temp_unit_switch = s; }

  void set_temp_unit(bool is_fahrenheit) {
    // 0x16 type 04 length 01. 0x01=°C, 0x02=°F (common Tuya convention, user can flip if needed)
    send_tuya_dp(0x16, 0x04, 1, {(uint8_t)(is_fahrenheit ? 2 : 1)});
  }

  void setup() override {
      ESP_LOGI("dreo", "Dreo Climate Initialized");
      send_tuya_raw(0x00, {}); 
      delay(50);
      send_tuya_raw(0x03, {0x02, 0x05, 0x00});
      delay(50);
      send_tuya_raw(0x02, {}); 
  }

  climate::ClimateTraits traits() override {
    auto traits = climate::ClimateTraits();
    traits.set_feature_flags(
        climate::CLIMATE_SUPPORTS_CURRENT_TEMPERATURE | 
        climate::CLIMATE_SUPPORTS_ACTION
    );
    traits.set_supported_modes({
      climate::CLIMATE_MODE_OFF,
      climate::CLIMATE_MODE_HEAT,
      climate::CLIMATE_MODE_FAN_ONLY
    });
    traits.set_supported_presets({
        climate::CLIMATE_PRESET_NONE,
        climate::CLIMATE_PRESET_ECO
    });
    traits.set_supported_custom_presets({PRESET_H1, PRESET_H2, PRESET_H3});
    traits.set_visual_min_temperature(5.0);
    traits.set_visual_max_temperature(35.0);
    traits.set_visual_temperature_step(1.0); 
    return traits;
  }

  void control(const climate::ClimateCall &call) override {
    // Unchanged: see previous code for full block
    // ...
    if (call.get_target_temperature().has_value()) {
      float temp_c = *call.get_target_temperature();
      uint8_t temp_f = (uint8_t)(temp_c * 1.8f + 32.0f + 0.5f);
      if (temp_f < 41) temp_f = 41;
      if (temp_f > 95) temp_f = 95;
      set_temperature(temp_f);
      this->target_temperature = temp_c;
      this->publish_state();
    }
  }

  void loop() override {
    uint32_t now = millis();
    if (now - last_hb_ > 10000) {
        send_tuya_raw(0x00, {});
        last_hb_ = now;
    }
    bool data_received = false;
    uint8_t buf[128];
    while (available()) {
      int to_read = std::min((int)available(), 128);
      read_array(buf, to_read);
      rx_buf_.insert(rx_buf_.end(), buf, buf + to_read);
      data_received = true;
    }
    if (data_received) {
        if (rx_buf_.size() > 512) {
            rx_buf_.erase(rx_buf_.begin(), rx_buf_.begin() + (rx_buf_.size() - 256));
        }
        parse_rx();
    }
  }

  void parse_rx() {
    while (rx_buf_.size() >= 9) {
      if (rx_buf_[0] != 0x55 || rx_buf_[1] != 0xAA) {
        auto it = std::find(rx_buf_.begin(), rx_buf_.end(), 0x55);
        if (it == rx_buf_.end()) {
          rx_buf_.clear();
          return;
        }
        if (it != rx_buf_.begin()) {
          rx_buf_.erase(rx_buf_.begin(), it);
        }
        if (rx_buf_.size() < 2) return;
        if (rx_buf_[1] != 0xAA) {
          rx_buf_.pop_front();
          continue;
        }
      }
      uint8_t len_h = rx_buf_[6];
      uint8_t len_l = rx_buf_[7];
      uint16_t payload_len = (len_h << 8) | len_l;
      uint16_t packet_len = 8 + payload_len + 1; 
      if (rx_buf_.size() < packet_len) return; 
      uint8_t received_sum = rx_buf_[packet_len - 1];
      uint32_t calc_sum = std::accumulate(rx_buf_.begin() + 2, rx_buf_.begin() + packet_len - 1, 0);
      uint8_t expected_sum = (uint8_t)((calc_sum - 1) & 0xFF);
      if (received_sum == expected_sum) {
        uint8_t cmd = rx_buf_[4];
        if (cmd == 0x07 || cmd == 0x08) process_status(8, payload_len);
        rx_buf_.erase(rx_buf_.begin(), rx_buf_.begin() + packet_len);
      } else {
        rx_buf_.pop_front();
      }
    }
  }

  void process_status(int start_idx, int len) {
    auto it = rx_buf_.begin() + start_idx;
    auto end_it = it + len;
    bool changed = false;
    while (it + 5 <= end_it) {
      uint8_t dp_id = *it;
      uint8_t dp_len = *(it + 4);
      if (it + 5 + dp_len > end_it) break;
      uint32_t val = 0;
      auto val_it = it + 5;
      if (dp_len == 1) {
        val = *val_it;
      } else if (dp_len == 4) {
        auto it2 = val_it;
        val = ((uint32_t)*it2++ << 24);
        val |= ((uint32_t)*it2++ << 16);
        val |= ((uint32_t)*it2++ << 8);
        val |= ((uint32_t)*it2);
      } else {
        auto it2 = val_it;
        for(int i=0; i<dp_len; i++) {
          val = (val << 8) + *it2++;
        }
      }
      if (this->debug_mode) {
        ESP_LOGD("dreo", "DP ID: %d Len: %d Val: %d", dp_id, dp_len, val);
      }
      switch (dp_id) {
        case 1: { // ... (other code unchanged)
        // ...
        }
        // ... keep all other cases unchanged ...
        case 22: // 0x16 temp unit
          if (temp_unit_switch) temp_unit_switch->publish_state(val == 2); // 2=Fahrenheit, 1=Celsius
          break;
        default: break;
      }
      it += (5 + dp_len);
    }
    if (changed) this->publish_state();
  }

  // --- SENDERS (unchanged except above additions)
  void set_power(bool on) { send_tuya_dp(0x01, 0x01, 1, {(uint8_t)(on ? 1 : 0)}); }
  void set_mode(int mode) { send_tuya_dp(0x02, 0x01, 1, {(uint8_t)mode}); }
  void set_temperature(uint8_t temp_f) { send_tuya_dp(0x04, 0x01, 1, {temp_f}); }
  void set_heat_level(int level) { send_tuya_dp(0x03, 0x01, 1, {(uint8_t)level}); }
  void set_sound(bool on) { send_tuya_dp(0x06, 0x01, 1, {(uint8_t)(on ? 0 : 1)}); } 
  void set_display(bool on) { send_tuya_dp(0x08, 0x01, 1, {(uint8_t)(on ? 1 : 0)}); }
  void set_child_lock(bool on) { send_tuya_dp(0x10, 0x01, 1, {(uint8_t)(on ? 1 : 0)}); }
  void set_window_mode(bool on) { send_tuya_dp(0x14, 0x01, 1, {(uint8_t)(on ? 1 : 0)}); }
  void set_timer(int minutes) { /* ... unchanged ... */ }
  void set_calibration(int offset) { /* ... unchanged ... */ }
  template <typename T>
  void send_tuya_dp_impl(uint8_t dp, uint8_t type, uint8_t len, const T &val) { /* ... unchanged ... */ }
  void send_tuya_dp(uint8_t dp, uint8_t type, uint8_t len, std::initializer_list<uint8_t> val) { send_tuya_dp_impl(dp, type, len, val); }
  void send_tuya_dp(uint8_t dp, uint8_t type, uint8_t len, const std::vector<uint8_t> &val) { send_tuya_dp_impl(dp, type, len, val); }
  void send_tuya_raw(uint8_t cmd, const std::vector<uint8_t> &data) { send_tuya_raw(cmd, data.data(), data.size()); }
  void send_tuya_raw(uint8_t cmd, std::initializer_list<uint8_t> data) { send_tuya_raw(cmd, data.begin(), data.size()); }
  void send_tuya_raw(uint8_t cmd, const uint8_t* payload, size_t len) { /* ... unchanged ... */ }

 protected:
  std::deque<uint8_t> rx_buf_;
  uint32_t last_hb_{0};
  uint8_t seq_{0x00};
};

} // namespace dreo_heater
} // namespace esphome
