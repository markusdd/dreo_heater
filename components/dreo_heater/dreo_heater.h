#pragma once
#include "esphome.h"
#include "esphome/components/climate/climate.h"
#include "esphome/components/uart/uart.h"
#include "esphome/components/switch/switch.h"
#include "esphome/components/number/number.h"
#include <cmath>
#include <vector>
#include <initializer_list>
#include <algorithm>
#include <cstring>

namespace esphome {
namespace dreo_heater {

// Phase 1: Tuya DP IDs Mapping
enum class TuyaDP : uint8_t {
  POWER = 1,
  MODE = 2,
  HEAT_LEVEL = 3,
  TARGET_TEMP = 4,
  SOUND = 6,
  CURRENT_TEMP = 7,
  SCREEN_DISPLAY = 8, // Avoid Arduino DISPLAY macro
  TIMER = 9,
  CALIBRATION = 15,
  CHILD_LOCK = 16,
  TEMP_UNIT_ALIAS = 17, // 0x11
  HEATING_STATUS = 19,
  WINDOW_DETECTION = 20,
  TEMP_UNIT = 22, // 0x16
};

// Phase 1: Inline Math Helpers
inline float f_to_c(float f) { return (f - 32.0f) * 5.0f / 9.0f; }
inline uint8_t c_to_f(float c) { return (uint8_t)(c * 1.8f + 32.0f + 0.5f); }

class DreoHeater : public climate::Climate, public uart::UARTDevice, public Component {
 public:
  static constexpr const char* PRESET_H1 = "H1";
  static constexpr const char* PRESET_H2 = "H2";
  static constexpr const char* PRESET_H3 = "H3";

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

  void set_temp_unit(bool is_celsius) {
    uint8_t v = is_celsius ? 2 : 1;
    send_tuya_dp(TuyaDP::TEMP_UNIT, 0x04, &v, 1);
    if (unit_switch) unit_switch->publish_state(is_celsius);
  }
  void set_fahrenheit(bool is_fahrenheit) { set_temp_unit(!is_fahrenheit); }

  void setup() override {
      ESP_LOGI("dreo", "Dreo Climate Initialized");
      // Phase 2: Non-Blocking Initialization using set_timeout
      send_tuya_raw(0x00, nullptr, 0); 
      
      this->set_timeout("init_1", 50, [this]() {
          uint8_t init_data[] = {0x02, 0x05, 0x00};
          send_tuya_raw(0x03, init_data, sizeof(init_data));
      });
      
      this->set_timeout("init_2", 100, [this]() {
          send_tuya_raw(0x02, nullptr, 0); 
      });
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
            this->set_timeout("preset_mode", 50, [this, level, safe_ptr]() {
                set_mode(1); // Manual Mode
                this->set_timeout("preset_level", 50, [this, level, safe_ptr]() {
                    set_heat_level(level);
                    this->mode = climate::CLIMATE_MODE_HEAT;
                    this->set_custom_preset_(safe_ptr);
                    this->preset = climate::CLIMATE_PRESET_NONE; // Clear Standard Preset
                    this->publish_state();
                });
            });
            return; 
        }
    }

    // 2. Handle STANDARD Presets (Eco / None)
    if (call.get_preset().has_value()) {
        auto p = *call.get_preset();
        if (p == climate::CLIMATE_PRESET_ECO) {
            set_power(true);
            this->set_timeout("preset_eco", 50, [this]() {
                set_mode(2); // Eco Mode (Thermostat)
                this->mode = climate::CLIMATE_MODE_HEAT;
                this->preset = climate::CLIMATE_PRESET_ECO;
                this->set_custom_preset_(""); // Clear Custom Preset
                this->publish_state();
            });
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
        this->set_timeout("mode_change", 100, [this, mode]() {
            if (mode == climate::CLIMATE_MODE_HEAT) {
                set_mode(2); // Default to ECO
                this->preset = climate::CLIMATE_PRESET_ECO;
                this->set_custom_preset_("");
            } else if (mode == climate::CLIMATE_MODE_FAN_ONLY) {
                set_mode(3); 
                this->preset = climate::CLIMATE_PRESET_NONE;
                this->set_custom_preset_("");
            }
            this->mode = mode;
            this->publish_state();
        });
        return; // Return early, publish happens in timeout
      }
      this->publish_state();
    }

    // 4. Handle Temp Changes
    if (call.get_target_temperature().has_value()) {
      float temp_c = *call.get_target_temperature();
      set_temperature(temp_c);
      this->target_temperature = temp_c;
      this->publish_state();
    }
  }

  void loop() override {
    uint32_t now = millis();
    if (now - last_hb_ > 10000) {
        send_tuya_raw(0x00, nullptr, 0);
        last_hb_ = now;
    }

    // Phase 4: Buffer Optimization
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
            rx_buf_.erase(rx_buf_.begin(), rx_buf_.end() - 256);
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
              rx_buf_.erase(rx_buf_.begin(), it);

              if (rx_buf_.size() < 2) return;

              if (rx_buf_[1] != 0xAA) {
                  rx_buf_.erase(rx_buf_.begin());
                  continue;
              }
          }
          uint8_t len_h = rx_buf_[6];
          uint8_t len_l = rx_buf_[7];
          uint16_t payload_len = (len_h << 8) | len_l;
          uint16_t packet_len = 8 + payload_len + 1; 
          
          if (rx_buf_.size() < packet_len) return; 
          
          uint8_t received_sum = rx_buf_[packet_len - 1];
          uint32_t calc_sum = 0;
          for (size_t i = 2; i < packet_len - 1; i++) {
              calc_sum += rx_buf_[i];
          }
          uint8_t expected_sum = (uint8_t)((calc_sum - 1) & 0xFF);
          
          if (received_sum == expected_sum) {
              uint8_t cmd = rx_buf_[4];
              if (cmd == 0x07 || cmd == 0x08) process_status(8, payload_len);
              rx_buf_.erase(rx_buf_.begin(), rx_buf_.begin() + packet_len);
          } else {
              rx_buf_.erase(rx_buf_.begin());
          }
      }
  }
  
  void process_status(size_t start_idx, size_t len) {
      size_t idx = start_idx;
      size_t end_idx = start_idx + len;
      bool changed = false;

      while (idx + 5 <= end_idx) {
          uint8_t dp_id = rx_buf_[idx];
          uint16_t dp_len = (rx_buf_[idx + 3] << 8) | rx_buf_[idx + 4];
          if (idx + 5 + dp_len > end_idx) break;
          
          uint32_t val = 0;
          size_t val_idx = idx + 5;

          if (dp_len == 1) {
              val = rx_buf_[val_idx];
          } else if (dp_len == 4) {
              val = ((uint32_t)rx_buf_[val_idx] << 24) |
                    ((uint32_t)rx_buf_[val_idx+1] << 16) |
                    ((uint32_t)rx_buf_[val_idx+2] << 8) |
                    ((uint32_t)rx_buf_[val_idx+3]);
          } else {
              for(int i=0; i<dp_len; i++) {
                  val = (val << 8) + rx_buf_[val_idx+i];
              }
          }
          
          if (this->debug_mode) {
              ESP_LOGD("dreo", "DP ID: %d Len: %d Val: %d", dp_id, dp_len, val);
          }

          switch (static_cast<TuyaDP>(dp_id)) {
              case TuyaDP::POWER: { 
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

              case TuyaDP::MODE:
                  if (this->mode != climate::CLIMATE_MODE_OFF) {
                      if (val == 2) { 
                          this->mode = climate::CLIMATE_MODE_HEAT;
                          this->preset = climate::CLIMATE_PRESET_ECO;
                          this->set_custom_preset_("");
                      }
                      else if (val == 3) { 
                          this->mode = climate::CLIMATE_MODE_FAN_ONLY;
                          this->preset = climate::CLIMATE_PRESET_NONE;
                          this->set_custom_preset_("");
                      }
                      else if (val == 1) { 
                          this->mode = climate::CLIMATE_MODE_HEAT;
                          this->preset = climate::CLIMATE_PRESET_NONE;
                      }
                      changed = true;
                  }
                  break;

              case TuyaDP::HEAT_LEVEL:
                  if (heat_level_number) heat_level_number->publish_state(val);

                  if (this->mode == climate::CLIMATE_MODE_HEAT && this->preset != climate::CLIMATE_PRESET_ECO) {
                      if (val == 1) this->set_custom_preset_(PRESET_H1);
                      else if (val == 2) this->set_custom_preset_(PRESET_H2);
                      else if (val == 3) this->set_custom_preset_(PRESET_H3);
                      changed = true;
                  }
                  break;

              case TuyaDP::TARGET_TEMP: { 
                  float val_f = (float)val;
                  bool is_celsius = (unit_switch && unit_switch->state);
                  if (is_celsius) {
                      this->target_temperature = val_f;
                  } else {
                      this->target_temperature = f_to_c(val_f);
                  }
                  changed = true;
                  break;
              }
              case TuyaDP::CURRENT_TEMP: { 
                  float val_f = (float)val;
                  bool is_celsius = (unit_switch && unit_switch->state);
                  if (is_celsius) {
                      this->current_temperature = val_f;
                  } else {
                      this->current_temperature = f_to_c(val_f);
                  }
                  changed = true;
                  break;
              }

              case TuyaDP::HEATING_STATUS:
                  if (this->mode != climate::CLIMATE_MODE_OFF) {
                      if (val == 1) this->action = climate::CLIMATE_ACTION_HEATING;
                      else {
                          if (this->mode == climate::CLIMATE_MODE_FAN_ONLY) this->action = climate::CLIMATE_ACTION_FAN;
                          else this->action = climate::CLIMATE_ACTION_IDLE;
                      }
                      changed = true;
                  }
                  break;

              case TuyaDP::SOUND: if (sound_switch) sound_switch->publish_state(val == 0); break;
              case TuyaDP::SCREEN_DISPLAY: if (display_switch) display_switch->publish_state(val != 0); break;
              case TuyaDP::TIMER: if (timer_number) timer_number->publish_state(val); break;
              case TuyaDP::CALIBRATION: if (calibration_number) calibration_number->publish_state((int)val); break;
              case TuyaDP::CHILD_LOCK: if (child_lock_switch) child_lock_switch->publish_state(val != 0); break;
              case TuyaDP::WINDOW_DETECTION: if (window_switch) window_switch->publish_state(val != 0); break;
              case TuyaDP::TEMP_UNIT:
                if (unit_switch) unit_switch->publish_state(val == 2);
                break;
              default: break;
          }
          
          idx += (5 + dp_len);
      }
      
      if (changed) this->publish_state();
  }

  // Phase 3: Protocol Strictness
  void send_tuya_dp(TuyaDP dp, uint8_t type, const uint8_t* payload, size_t len) {
      uint8_t data[64];
      size_t idx = 0;
      data[idx++] = static_cast<uint8_t>(dp);
      data[idx++] = 0x01; // Protocol byte
      data[idx++] = type; // Type
      data[idx++] = (len >> 8) & 0xFF; // Len High
      data[idx++] = len & 0xFF;        // Len Low
      
      for (size_t i = 0; i < len && idx < sizeof(data); i++) {
          data[idx++] = payload[i];
      }
      send_tuya_raw(0x06, data, idx);
  }

  void set_power(bool on) { uint8_t v = on ? 1 : 0; send_tuya_dp(TuyaDP::POWER, 0x01, &v, 1); }
  void set_mode(int mode) { uint8_t v = mode; send_tuya_dp(TuyaDP::MODE, 0x01, &v, 1); }
  void set_temperature(float temp_c) {
    bool is_celsius = (unit_switch && unit_switch->state);
    uint8_t v;
    if (is_celsius) {
        v = (uint8_t)(temp_c + 0.5f);
    } else {
        uint8_t temp_f = c_to_f(temp_c);
        if (temp_f < 41) temp_f = 41;
        if (temp_f > 95) temp_f = 95;
        v = temp_f;
    }
    send_tuya_dp(TuyaDP::TARGET_TEMP, 0x01, &v, 1);
  }
  void set_heat_level(int level) { uint8_t v = level; send_tuya_dp(TuyaDP::HEAT_LEVEL, 0x01, &v, 1); }
  
  void set_sound(bool on) { uint8_t v = on ? 0 : 1; send_tuya_dp(TuyaDP::SOUND, 0x01, &v, 1); } 
  void set_display(bool on) { uint8_t v = on ? 1 : 0; send_tuya_dp(TuyaDP::SCREEN_DISPLAY, 0x01, &v, 1); }
  void set_child_lock(bool on) { uint8_t v = on ? 1 : 0; send_tuya_dp(TuyaDP::CHILD_LOCK, 0x01, &v, 1); }
  void set_window_mode(bool on) { uint8_t v = on ? 1 : 0; send_tuya_dp(TuyaDP::WINDOW_DETECTION, 0x01, &v, 1); }
  
  void set_timer(int minutes) {
      uint32_t val = minutes;
      uint8_t v[4] = { (uint8_t)(val >> 24), (uint8_t)(val >> 16), (uint8_t)(val >> 8), (uint8_t)(val) };
      send_tuya_dp(TuyaDP::TIMER, 0x02, v, 4);
  }

  void set_calibration(int offset) {
      uint32_t val = offset;
      uint8_t v[4] = { (uint8_t)(val >> 24), (uint8_t)(val >> 16), (uint8_t)(val >> 8), (uint8_t)(val) };
      send_tuya_dp(TuyaDP::CALIBRATION, 0x02, v, 4);
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
    pkt[6] = 0x00; 
    pkt[7] = (uint8_t)payload_len;

    uint32_t sum = pkt[3] + pkt[4] + pkt[7];

    if (payload_len > 0 && payload != nullptr) {
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
  std::vector<uint8_t> rx_buf_;
  uint32_t last_hb_{0};
  uint8_t seq_{0x00};
};

} // namespace dreo_heater
} // namespace esphome
