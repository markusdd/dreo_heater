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

// Custom Presets (Static Constants)
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
  
  // Backward compatible and alias for unit switch
  switch_::Switch *temp_unit_switch{nullptr};
  switch_::Switch *unit_switch{nullptr};

  number::Number *heat_level_number{nullptr};
  number::Number *timer_number{nullptr};
  number::Number *calibration_number{nullptr};

  void set_temp_unit_switch(switch_::Switch *s) { temp_unit_switch = s; unit_switch = s; }
  void set_unit_switch(switch_::Switch *s) { unit_switch = s; temp_unit_switch = s; }

  void set_temp_unit(bool is_fahrenheit) {
    // 0x16 type 04 length 01. 0x01=°C, 0x02=°F (common Tuya convention, user can flip if needed)
    send_tuya_dp(0x16, 0x04, 1, {(uint8_t)(is_fahrenheit ? 2 : 1)});
  }
  void set_fahrenheit(bool is_fahrenheit) { set_temp_unit(is_fahrenheit); }

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
    
    // MODES: HEAT and FAN_ONLY
    traits.set_supported_modes({
      climate::CLIMATE_MODE_OFF,
      climate::CLIMATE_MODE_HEAT,
      climate::CLIMATE_MODE_FAN_ONLY
    });
    
    // STANDARD PRESETS: Enable Official ECO here
    traits.set_supported_presets({
        climate::CLIMATE_PRESET_NONE,
        climate::CLIMATE_PRESET_ECO
    });

    // CUSTOM PRESETS: Only H1-H3 here
    traits.set_supported_custom_presets({
        PRESET_H1,
        PRESET_H2,
        PRESET_H3
    });

    traits.set_visual_min_temperature(5.0);
    traits.set_visual_max_temperature(35.0);
    traits.set_visual_temperature_step(1.0); 
    return traits;
  }

  void control(const climate::ClimateCall &call) override {
    // 1. Handle CUSTOM Presets (H1/H2/H3)
    if (!call.get_custom_preset().empty()) {
        auto preset_ref = call.get_custom_preset(); 
        const char* safe_ptr = nullptr;
        int level = 0;
        
        if (preset_ref == PRESET_H1) { level = 1; safe_ptr = PRESET_H1; }
        else if (preset_ref == PRESET_H2) { level = 2; safe_ptr = PRESET_H2; }
        else if (preset_ref == PRESET_H3) { level = 3; safe_ptr = PRESET_H3; }

        if (level > 0) {
            set_power(true);
            delay(50);
            set_mode(1); // Manual Mode
            delay(50);
            set_heat_level(level);
            
            this->mode = climate::CLIMATE_MODE_HEAT;
            this->set_custom_preset_(safe_ptr);
            this->preset = climate::CLIMATE_PRESET_NONE; // Clear Standard Preset
            this->publish_state();
            return; 
        }
    }

    // 2. Handle STANDARD Presets (Eco / None)
    if (call.get_preset().has_value()) {
        auto p = *call.get_preset();
        if (p == climate::CLIMATE_PRESET_ECO) {
            set_power(true);
            delay(50);
            set_mode(2); // Eco Mode (Thermostat)
            
            this->mode = climate::CLIMATE_MODE_HEAT;
            this->preset = climate::CLIMATE_PRESET_ECO;
            this->set_custom_preset_(""); // Clear Custom Preset
            this->publish_state();
            return;
        }
        else if (p == climate::CLIMATE_PRESET_NONE) {
            this->preset = climate::CLIMATE_PRESET_NONE;
            this->set_custom_preset_("");
            this->publish_state();
        }
    }

    // 3. Handle Mode Changes
    if (call.get_mode().has_value()) {
      climate::ClimateMode mode = *call.get_mode();
      
      if (mode == climate::CLIMATE_MODE_OFF) {
        set_power(false);
        this->mode = climate::CLIMATE_MODE_OFF;
        this->action = climate::CLIMATE_ACTION_OFF;
        this->preset = climate::CLIMATE_PRESET_NONE;
        this->set_custom_preset_("");
      } else {
        set_power(true);
        delay(100); 

        if (mode == climate::CLIMATE_MODE_HEAT) {
            // Default to ECO when turning on Heat
            set_mode(2); 
            this->preset = climate::CLIMATE_PRESET_ECO;
            this->set_custom_preset_("");
            
        } else if (mode == climate::CLIMATE_MODE_FAN_ONLY) {
            set_mode(3); 
            this->preset = climate::CLIMATE_PRESET_NONE;
            this->set_custom_preset_("");
        }
        this->mode = mode;
      }
      this->publish_state();
    }

    // 4. Handle Temp Changes
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
              case 1: { // Power
                  bool is_on = (val != 0);
                  if (!is_on) {
                      this->mode = climate::CLIMATE_MODE_OFF;
                      this->action = climate::CLIMATE_ACTION_OFF;
                      this->set_custom_preset_("");
                      this->preset = climate::CLIMATE_PRESET_NONE;
                  } else if (this->mode == climate::CLIMATE_MODE_OFF) {
                      this->mode = climate::CLIMATE_MODE_HEAT;
                      this->preset = climate::CLIMATE_PRESET_ECO;
                  }
                  changed = true;
                  break;
              }

              case 2: // Mode
                  if (this->mode != climate::CLIMATE_MODE_OFF) {
                      if (val == 2) { // Eco
                          this->mode = climate::CLIMATE_MODE_HEAT;
                          this->preset = climate::CLIMATE_PRESET_ECO;
                          this->set_custom_preset_("");
                      }
                      else if (val == 3) { // Fan
                          this->mode = climate::CLIMATE_MODE_FAN_ONLY;
                          this->preset = climate::CLIMATE_PRESET_NONE;
                          this->set_custom_preset_("");
                      }
                      else if (val == 1) { // Manual
                          this->mode = climate::CLIMATE_MODE_HEAT;
                          this->preset = climate::CLIMATE_PRESET_NONE;
                      }
                      changed = true;
                  }
                  break;

              case 3: // Heat Level
                  if (heat_level_number) heat_level_number->publish_state(val);

                  if (this->mode == climate::CLIMATE_MODE_HEAT && this->preset != climate::CLIMATE_PRESET_ECO) {
                      if (val == 1) this->set_custom_preset_(PRESET_H1);
                      else if (val == 2) this->set_custom_preset_(PRESET_H2);
                      else if (val == 3) this->set_custom_preset_(PRESET_H3);
                      changed = true;
                  }
                  break;

              case 4: { // Target Temp
                  float temp_f = (float)val;
                  this->target_temperature = (temp_f - 32.0f) * 5.0f / 9.0f;
                  changed = true;
                  break;
              }
              case 7: { // Current Temp
                  float temp_f = (float)val;
                  this->current_temperature = (temp_f - 32.0f) * 5.0f / 9.0f;
                  changed = true;
                  break;
              }

              case 19: // Heating Status
                  if (this->mode != climate::CLIMATE_MODE_OFF) {
                      if (val == 1) this->action = climate::CLIMATE_ACTION_HEATING;
                      else {
                          if (this->mode == climate::CLIMATE_MODE_FAN_ONLY) this->action = climate::CLIMATE_ACTION_FAN;
                          else this->action = climate::CLIMATE_ACTION_IDLE;
                      }
                      changed = true;
                  }
                  break;

              case 6: if (sound_switch) sound_switch->publish_state(val == 0); break;
              case 8: if (display_switch) display_switch->publish_state(val != 0); break;
              case 9: if (timer_number) timer_number->publish_state(val); break;
              case 15: if (calibration_number) calibration_number->publish_state((int)val); break;
              case 16: if (child_lock_switch) child_lock_switch->publish_state(val != 0); break;
              case 20: if (window_switch) window_switch->publish_state(val != 0); break;
              case 22: // 0x16 temp unit
                if (unit_switch) unit_switch->publish_state(val == 2); // 2=Fahrenheit, 1=Celsius
                break;
              default: break;
          }
          
          it += (5 + dp_len);
      }
      
      if (changed) this->publish_state();
  }

  // --- SENDERS ---
  void set_power(bool on) { send_tuya_dp(0x01, 0x01, 1, {(uint8_t)(on ? 1 : 0)}); }
  void set_mode(int mode) { send_tuya_dp(0x02, 0x01, 1, {(uint8_t)mode}); }
  void set_temperature(uint8_t temp_f) { send_tuya_dp(0x04, 0x01, 1, {temp_f}); }
  void set_heat_level(int level) { send_tuya_dp(0x03, 0x01, 1, {(uint8_t)level}); }
  
  void set_sound(bool on) { send_tuya_dp(0x06, 0x01, 1, {(uint8_t)(on ? 0 : 1)}); } 
  void set_display(bool on) { send_tuya_dp(0x08, 0x01, 1, {(uint8_t)(on ? 1 : 0)}); }
  void set_child_lock(bool on) { send_tuya_dp(0x10, 0x01, 1, {(uint8_t)(on ? 1 : 0)}); }
  void set_window_mode(bool on) { send_tuya_dp(0x14, 0x01, 1, {(uint8_t)(on ? 1 : 0)}); }
  
  void set_timer(int minutes) {
    uint32_t val_u32 = (uint32_t)minutes;
    uint8_t data[9];
    data[0] = 0x09;
    data[1] = 0x01;
    data[2] = 0x02;
    data[3] = 0x00;
    data[4] = 0x04;
    data[5] = (val_u32 >> 24) & 0xFF;
    data[6] = (val_u32 >> 16) & 0xFF;
    data[7] = (val_u32 >> 8) & 0xFF;
    data[8] = (val_u32 >> 0) & 0xFF;
    send_tuya_raw(0x06, data, 9);
  }

  void set_calibration(int offset) {
    uint32_t val_u32 = (uint32_t)offset;
    uint8_t data[9];
    data[0] = 0x0F;
    data[1] = 0x01;
    data[2] = 0x02;
    data[3] = 0x00;
    data[4] = 0x04;
    data[5] = (val_u32 >> 24) & 0xFF;
    data[6] = (val_u32 >> 16) & 0xFF;
    data[7] = (val_u32 >> 8) & 0xFF;
    data[8] = (val_u32 >> 0) & 0xFF;
    send_tuya_raw(0x06, data, 9);
  }

  template <typename T>
  void send_tuya_dp_impl(uint8_t dp, uint8_t type, uint8_t len, const T &val) {
    uint8_t data[64];
    size_t idx = 0;
    if (idx < sizeof(data)) data[idx++] = dp;
    if (idx < sizeof(data)) data[idx++] = type;
    if (idx < sizeof(data)) data[idx++] = len;
    if (idx < sizeof(data)) data[idx++] = 0x00;
    if (len >= 1 && idx < sizeof(data)) data[idx++] = 0x01;
    for (auto b : val) {
        if (idx < sizeof(data)) data[idx++] = b;
    }
    send_tuya_raw(0x06, data, idx);
  }

  void send_tuya_dp(uint8_t dp, uint8_t type, uint8_t len, std::initializer_list<uint8_t> val) {
      send_tuya_dp_impl(dp, type, len, val);
  }

  void send_tuya_dp(uint8_t dp, uint8_t type, uint8_t len, const std::vector<uint8_t> &val) {
      send_tuya_dp_impl(dp, type, len, val);
  }

  void send_tuya_raw(uint8_t cmd, const std::vector<uint8_t> &data) {
    send_tuya_raw(cmd, data.data(), data.size());
  }

  void send_tuya_raw(uint8_t cmd, std::initializer_list<uint8_t> data) {
    send_tuya_raw(cmd, data.begin(), data.size());
  }

  void send_tuya_raw(uint8_t cmd, const uint8_t* payload, size_t len) {
    uint8_t pkt[128]; 
    size_t payload_len = len;
    size_t packet_len = 8 + payload_len + 1;

    if (packet_len > sizeof(pkt)) {
        ESP_LOGE("dreo", "Packet too large for stack buffer");
        return;
    }

    pkt[0] = 0x55;
    pkt[1] = 0xAA;
    pkt[2] = 0x00; // Ver
    pkt[3] = seq_++; // Seq
    pkt[4] = cmd; // Cmd
    pkt[5] = 0x00; // Len High
    pkt[6] = 0x00; // Len Low
    pkt[7] = (uint8_t)payload_len; // Data Size

    uint32_t sum = pkt[3] + pkt[4] + pkt[7];

    if (payload_len > 0) {
        for (size_t i = 0; i < payload_len; i++) {
            uint8_t b = payload[i];
            pkt[8 + i] = b;
            sum += b;
        }
    }

    pkt[packet_len - 1] = (uint8_t)((sum - 1) & 0xFF);
    this->write_array(pkt, packet_len);
  }
  
 protected:
  std::deque<uint8_t> rx_buf_;
  uint32_t last_hb_{0};
  uint8_t seq_{0x00};
};

} // namespace dreo_heater
} // namespace esphome
