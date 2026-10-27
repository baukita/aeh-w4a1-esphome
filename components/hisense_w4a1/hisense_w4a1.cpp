#include "hisense_w4a1.h"

#include <algorithm>
#include <array>
#include <cmath>

#include "esphome/core/log.h"

#include "frame.h"

namespace esphome {
namespace hisense_w4a1 {

static const char *const TAG = "hisense_w4a1";

// The AC answers a status request in about 220 ms.
static const uint32_t REPLY_TIMEOUT_MS = 1000;
// Quiet time on the bus between one transaction and the next.
static const uint32_t MIN_GAP_MS = 200;
// Read the status this long after the last queued command, to confirm what
// the AC actually applied.
static const uint32_t CONFIRM_DELAY_MS = 1500;
// The link sensor turns off after this long without a status reply.
static const uint32_t LINK_TIMEOUT_MS = 30000;
static const size_t MAX_FRAME_LEN = 200;
// Status data starts after the 16-byte header; field offsets count from here.
static const size_t STATUS_DATA_START = 16;

// mode_status values
static const uint8_t MODE_FAN = 0;
static const uint8_t MODE_HEAT = 1;
static const uint8_t MODE_COOL = 2;
static const uint8_t MODE_DRY = 3;
// SMART on the remote: the AC picks heat, fan, dry or cool and its own setpoint.
static const uint8_t MODE_SMART = 5;

// wind_status values. Auto is 1 in cool, fan-only and dry, and 0 in heat and SMART.
static const uint8_t WIND_AUTO_HEAT = 0;
static const uint8_t WIND_AUTO = 1;
static const uint8_t WIND_QUIET = 2;

// The remote's five fan speeds, lowest first, are custom fan modes. They can't
// be called low, medium and high: ESPHome turns those names into its standard
// fan modes.
static const char *const FAN_SPEEDS[] = {"Speed 1", "Speed 2", "Speed 3", "Speed 4", "Speed 5"};
static const uint8_t FAN_SPEED_WIND[] = {10, 12, 14, 16, 18};
static const Command *const FAN_SPEED_COMMANDS[] = {&CMD_SPEED_LOW, &CMD_SPEED_2, &CMD_SPEED_MED, &CMD_SPEED_4,
                                                    &CMD_SPEED_MAX};
static const size_t NUM_FAN_SPEEDS = sizeof(FAN_SPEED_WIND);

// Sleep levels 2-4 are custom presets; level 1 is the standard "sleep" preset.
static const char *const SLEEP_PRESETS[] = {"Sleep 2", "Sleep 3", "Sleep 4"};
// The remote's 8° HEAT: heat mode with an 8 °C setpoint. The AC has no flag for it.
static const char *const HEAT_8_PRESET = "8° Heat";
static const uint8_t HEAT_8_TEMP = 8;

// Where settings sit in an update frame (docs/frames.md, "Update frame
// layout"). Each setting is its value followed by an "apply" bit;
// the AC leaves a setting alone when its apply bit is 0.
static const size_t BYTE_FAN = 16;       // value * 2 + 1
static const size_t BYTE_SLEEP = 17;     // level * 2 + 1, so 0x01 ends sleep
static const size_t BYTE_SETPOINT = 19;  // °C * 2 + 1
static const size_t BYTE_PRESETS = 33;   // low power, eco, turbo, dual frequency: two bits each
static const size_t BYTE_QUIET = 35;
static const uint8_t PRESET_ECO_ON = 0x30;
static const uint8_t PRESET_ECO_OFF = 0x10;
static const uint8_t PRESET_TURBO_ON = 0x0C;
static const uint8_t PRESET_TURBO_OFF = 0x04;

using Body = std::array<uint8_t, CMD_LEN>;

static Body body_of(const Command &cmd) {
  Body body;
  std::copy(cmd.data, cmd.data + CMD_LEN, body.begin());
  return body;
}

static uint8_t setpoint_byte(uint8_t temp) { return temp * 2 + 1; }

/// Reads a field from the status data. `offset` is 1-based, as in pyaehw4a1's table.
static uint32_t get_bits(const uint8_t *data, size_t len, uint16_t offset, uint8_t length) {
  uint32_t value = 0;
  for (uint8_t i = 0; i < length; i++) {
    const uint16_t bit = offset - 1 + i;
    if (bit / 8 >= len)
      return 0;
    value = (value << 1) | ((data[bit / 8] >> (7 - bit % 8)) & 1);
  }
  return value;
}

/// The pyaehw4a1 frame for a mode, or nullptr for SMART, which uses CMD_SMART.
static const Command *mode_command(uint8_t mode) {
  switch (mode) {
    case MODE_FAN:
      return &CMD_MODE_FAN;
    case MODE_HEAT:
      return &CMD_MODE_HEAT;
    case MODE_COOL:
      return &CMD_MODE_COOL;
    case MODE_DRY:
      return &CMD_MODE_DRY;
    default:
      return nullptr;
  }
}

static bool known_mode(uint8_t mode) { return mode <= MODE_DRY || mode == MODE_SMART; }

static bool is_auto_wind(uint8_t wind) { return wind == WIND_AUTO || wind == WIND_AUTO_HEAT; }

/// Index into FAN_SPEEDS, or -1 if `wind` isn't one of the five speeds.
static int fan_speed_index(uint8_t wind) {
  for (size_t i = 0; i < NUM_FAN_SPEEDS; i++) {
    if (FAN_SPEED_WIND[i] == wind)
      return i;
  }
  return -1;
}

static bool known_wind(uint8_t wind) {
  return is_auto_wind(wind) || wind == WIND_QUIET || fan_speed_index(wind) >= 0;
}

/// The fan frame that gives `wind`; auto for values it doesn't know.
static const Command &fan_command(uint8_t wind) {
  if (wind == WIND_QUIET)
    return CMD_SPEED_MUTE;
  const int speed = fan_speed_index(wind);
  return speed >= 0 ? *FAN_SPEED_COMMANDS[speed] : CMD_SPEED_AUTO;
}

/// Copies the fan setting (speed and quiet) from a fan frame into `body`.
static void set_fan(Body &body, const Command &fan) {
  body[BYTE_FAN] = fan.data[BYTE_FAN];
  body[BYTE_QUIET] = fan.data[BYTE_QUIET];
}

static bool is_heat_8(const AcStatus &s) {
  return s.on && s.mode == MODE_HEAT && !s.fahrenheit && s.set_temp < MIN_TEMP_C;
}

void HisenseDisplaySwitch::write_state(bool state) { this->parent_->set_display(state); }

HisenseClimate::HisenseClimate() {
  this->set_supported_custom_fan_modes(FAN_SPEEDS);
  this->set_supported_custom_presets({SLEEP_PRESETS[0], SLEEP_PRESETS[1], SLEEP_PRESETS[2], HEAT_8_PRESET});
}

void HisenseClimate::setup() {
  if (this->flow_control_pin_ != nullptr) {
    this->flow_control_pin_->setup();
    this->flow_control_pin_->digital_write(false);
  }
  this->rx_.reserve(MAX_FRAME_LEN);
  this->setpoints_pref_ = global_preferences->make_preference<ModeSetpoints>(fnv1_hash("hisense_w4a1_setpoints"), true);
  if (!this->setpoints_pref_.load(&this->setpoints_))
    this->setpoints_ = {};
  this->mode = climate::CLIMATE_MODE_OFF;
  this->poll_wanted_ = true;
}

void HisenseClimate::update() { this->poll_wanted_ = true; }

void HisenseClimate::dump_config() {
  ESP_LOGCONFIG(TAG, "Hisense AC (AEH-W4A1 protocol over RS-485):");
  LOG_PIN("  Flow control pin: ", this->flow_control_pin_);
  LOG_UPDATE_INTERVAL(this);
  ESP_LOGCONFIG(TAG, "  Remembered setpoints (0 = not yet): fan %u, heat %u, cool %u, dry %u", this->setpoints_.temp[0],
                this->setpoints_.temp[1], this->setpoints_.temp[2], this->setpoints_.temp[3]);
}

climate::ClimateTraits HisenseClimate::traits() {
  auto traits = climate::ClimateTraits();
  traits.add_feature_flags(climate::CLIMATE_SUPPORTS_CURRENT_TEMPERATURE);
  traits.set_supported_modes({climate::CLIMATE_MODE_OFF, climate::CLIMATE_MODE_HEAT, climate::CLIMATE_MODE_COOL,
                              climate::CLIMATE_MODE_DRY, climate::CLIMATE_MODE_FAN_ONLY, climate::CLIMATE_MODE_AUTO});
  traits.set_supported_fan_modes({climate::CLIMATE_FAN_AUTO, climate::CLIMATE_FAN_QUIET});
  traits.set_supported_swing_modes({climate::CLIMATE_SWING_OFF, climate::CLIMATE_SWING_VERTICAL,
                                    climate::CLIMATE_SWING_HORIZONTAL, climate::CLIMATE_SWING_BOTH});
  traits.set_supported_presets({climate::CLIMATE_PRESET_NONE, climate::CLIMATE_PRESET_ECO,
                                climate::CLIMATE_PRESET_BOOST, climate::CLIMATE_PRESET_SLEEP});
  traits.set_visual_min_temperature(MIN_TEMP_C);
  traits.set_visual_max_temperature(MAX_TEMP_C);
  traits.set_visual_target_temperature_step(1.0f);
  traits.set_visual_current_temperature_step(1.0f);
  return traits;
}

void HisenseClimate::loop() {
  uint8_t byte;
  while (this->available() > 0 && this->read_byte(&byte))
    this->feed_(byte);

  const uint32_t now = millis();
  switch (this->phase_) {
    case Phase::SENDING:
      if (static_cast<int32_t>(now - this->tx_done_at_) >= 0) {
        // The bytes should be out by now; flush() only waits for the last few.
        (void) this->flush();
        if (this->flow_control_pin_ != nullptr)
          this->flow_control_pin_->digital_write(false);
        this->phase_ = Phase::WAITING;
        this->reply_deadline_ = millis() + REPLY_TIMEOUT_MS;
      }
      break;
    case Phase::WAITING:
      if (static_cast<int32_t>(now - this->reply_deadline_) >= 0) {
        if (this->in_flight_is_status_) {
          this->missed_polls_++;
          ESP_LOGW(TAG, "No status reply from the AC (%u in a row)", this->missed_polls_);
        } else {
          // Never re-sent: the display commands, for one, get no reply but still work.
          ESP_LOGD(TAG, "No reply to %s; the next status read will confirm it", this->in_flight_name_);
        }
        this->finish_transaction_(now);
      }
      break;
    case Phase::IDLE:
      if (now - this->idle_since_ >= MIN_GAP_MS)
        this->start_next_(now);
      break;
  }
  this->update_link_(now);
}

void HisenseClimate::start_next_(uint32_t now) {
  if (!this->pending_.empty()) {
    const Outgoing out = std::move(this->pending_.front());
    this->pending_.pop_front();
    ESP_LOGD(TAG, "Sending %s", out.name);
    this->in_flight_is_status_ = false;
    this->in_flight_name_ = out.name;
    this->send_(out.wire.data(), out.wire.size(), now);
    return;
  }
  if (this->poll_wanted_ && static_cast<int32_t>(now - this->poll_not_before_) >= 0) {
    this->poll_wanted_ = false;
    this->in_flight_is_status_ = true;
    this->in_flight_name_ = "status request";
    this->send_(STATUS_REQUEST, sizeof(STATUS_REQUEST), now);
  }
}

void HisenseClimate::send_(const uint8_t *data, size_t len, uint32_t now) {
  this->in_frame_ = false;
  this->escape_ = false;
  if (this->flow_control_pin_ != nullptr)
    this->flow_control_pin_->digital_write(true);
  this->write_array(data, len);
  // 10 bits per byte on the wire (start, 8 data, stop), plus a little margin.
  this->tx_done_at_ = now + (len * 10000UL) / this->parent_->get_baud_rate() + 2;
  this->phase_ = Phase::SENDING;
}

void HisenseClimate::finish_transaction_(uint32_t now) {
  const bool was_command = !this->in_flight_is_status_;
  this->phase_ = Phase::IDLE;
  this->idle_since_ = now;
  this->in_flight_is_status_ = false;
  this->in_flight_name_ = nullptr;
  if (was_command && this->pending_.empty()) {
    this->poll_wanted_ = true;
    this->poll_not_before_ = now + CONFIRM_DELAY_MS;
  }
}

void HisenseClimate::feed_(uint8_t byte) {
  if (!this->in_frame_) {
    if (this->prev_byte_ == 0xF4 && byte == 0xF5) {
      this->rx_.assign({0xF4, 0xF5});
      this->in_frame_ = true;
      this->escape_ = false;
    }
    this->prev_byte_ = byte;
    return;
  }
  if (this->escape_) {
    this->escape_ = false;
    if (byte == 0xF4) {
      // F4 F4 is a literal F4.
      this->rx_.push_back(0xF4);
    } else if (byte == 0xFB) {
      // F4 FB ends the frame.
      this->rx_.push_back(0xF4);
      this->rx_.push_back(0xFB);
      this->in_frame_ = false;
      this->prev_byte_ = 0;
      this->check_frame_();
      return;
    } else if (byte == 0xF5) {
      // A new frame started before this one ended; drop the partial one.
      this->rx_.assign({0xF4, 0xF5});
    } else {
      // An unescaped F4 followed by something else: keep both bytes.
      this->rx_.push_back(0xF4);
      this->rx_.push_back(byte);
    }
  } else if (byte == 0xF4) {
    this->escape_ = true;
  } else {
    this->rx_.push_back(byte);
  }
  if (this->rx_.size() > MAX_FRAME_LEN) {
    ESP_LOGW(TAG, "Frame longer than %u bytes, dropping it", (unsigned) MAX_FRAME_LEN);
    this->in_frame_ = false;
    this->prev_byte_ = 0;
  }
}

void HisenseClimate::check_frame_() {
  const auto &f = this->rx_;
  const size_t n = f.size();
  if (n < 9 || n != static_cast<size_t>(f[4]) + 9) {
    ESP_LOGD(TAG, "Dropping a frame with the wrong length (%u bytes): %s", (unsigned) n,
             format_hex_pretty(f).c_str());
    return;
  }
  uint32_t sum = 0;
  for (size_t i = 2; i < n - 4; i++)
    sum += f[i];
  if ((sum & 0xFFFF) != ((static_cast<uint32_t>(f[n - 4]) << 8) | f[n - 3])) {
    ESP_LOGD(TAG, "Dropping a frame with a bad checksum: %s", format_hex_pretty(f).c_str());
    return;
  }
  // Byte 2 is 0x01 on frames from the AC and 0x00 on our own requests.
  if (f[2] != 0x01)
    return;

  const bool is_status = f[13] == 0x66 && f[14] == 0x00;
  if (is_status) {
    this->parse_status_(f.data() + STATUS_DATA_START, n - STATUS_DATA_START - 4);
  } else {
    ESP_LOGD(TAG, "Frame type %u/%u from the AC", f[13], f[14]);
  }
  // A status read is only answered by a status frame; a command by any frame.
  if (this->phase_ == Phase::WAITING && (is_status || !this->in_flight_is_status_))
    this->finish_transaction_(millis());
}

void HisenseClimate::parse_status_(const uint8_t *p, size_t len) {
  AcStatus s;
  s.wind = get_bits(p, len, 1, 8);
  s.sleep = get_bits(p, len, 9, 7);
  s.mode = get_bits(p, len, 17, 4);
  s.on = get_bits(p, len, 21, 1);
  s.set_temp = get_bits(p, len, 25, 8);
  s.room_temp = get_bits(p, len, 33, 8);
  s.fahrenheit = get_bits(p, len, 87, 1);
  s.swing_vertical = get_bits(p, len, 153, 1);
  s.swing_horizontal = get_bits(p, len, 154, 1);
  s.eco = get_bits(p, len, 158, 1);
  s.turbo = get_bits(p, len, 159, 1);
  s.display = get_bits(p, len, 169, 1);

  if (this->missed_polls_ > 0)
    ESP_LOGI(TAG, "AC answering again after %u missed status reads", this->missed_polls_);
  this->missed_polls_ = 0;
  this->last_status_at_ = millis();
  const bool first = !this->have_status_;
  // Warn when an unknown value first shows up, not on every poll.
  if (!known_mode(s.mode) && (first || s.mode != this->status_.mode))
    ESP_LOGW(TAG, "Unknown mode %u from the AC", s.mode);
  if (!known_wind(s.wind) && (first || s.wind != this->status_.wind))
    ESP_LOGW(TAG, "Unknown fan value %u from the AC, showing it as auto", s.wind);
  this->status_ = s;
  this->have_status_ = true;

  ESP_LOGV(TAG, "Status: on=%d mode=%u set=%u room=%u wind=%u sleep=%u swing=%d/%d eco=%d turbo=%d display=%d", s.on,
           s.mode, s.set_temp, s.room_temp, s.wind, s.sleep, s.swing_vertical, s.swing_horizontal, s.eco, s.turbo,
           s.display);
  if (first)
    ESP_LOGI(TAG, "First status from the AC: %s, mode %u, set to %u, room %u", s.on ? "on" : "off", s.mode,
             s.set_temp, s.room_temp);

  // While commands are queued or in flight, keep showing the state they lead
  // to; the confirming read after the last one publishes the real state.
  const bool command_in_flight = this->phase_ != Phase::IDLE && !this->in_flight_is_status_;
  if (this->pending_.empty() && !command_in_flight) {
    this->remember_(s);
    this->publish_from_(s, first);
  }
}

void HisenseClimate::remember_(const AcStatus &s) {
  if (!s.on || s.fahrenheit)
    return;
  if (!s.turbo && !s.eco && s.sleep == 0 && !is_heat_8(s)) {
    this->plain_ = s;
    this->have_plain_ = true;
  }
  // Turbo sets its own setpoint, and 8° Heat is below the normal range.
  if (s.mode <= MODE_DRY && !s.turbo && s.set_temp >= MIN_TEMP_C && s.set_temp <= MAX_TEMP_C &&
      this->setpoints_.temp[s.mode] != s.set_temp) {
    this->setpoints_.temp[s.mode] = s.set_temp;
    this->setpoints_pref_.save(&this->setpoints_);
  }
}

uint8_t HisenseClimate::setpoint_for_(uint8_t mode) const {
  if (mode <= MODE_DRY) {
    const uint8_t temp = this->setpoints_.temp[mode];
    if (temp >= MIN_TEMP_C && temp <= MAX_TEMP_C)
      return temp;
  }
  // Not known yet: keep the current setpoint, or else use the mode frame's own.
  const uint8_t current = this->status_.set_temp;
  if (current >= MIN_TEMP_C && current <= MAX_TEMP_C)
    return current;
  const Command *cmd = mode_command(mode);
  return cmd != nullptr ? (cmd->data[BYTE_SETPOINT] - 1) / 2 : 0;
}

void HisenseClimate::publish_from_(const AcStatus &s, bool force) {
  bool changed = force;

  climate::ClimateMode mode = climate::CLIMATE_MODE_OFF;
  if (s.on) {
    switch (s.mode) {
      case MODE_HEAT:
        mode = climate::CLIMATE_MODE_HEAT;
        break;
      case MODE_COOL:
        mode = climate::CLIMATE_MODE_COOL;
        break;
      case MODE_DRY:
        mode = climate::CLIMATE_MODE_DRY;
        break;
      case MODE_FAN:
        mode = climate::CLIMATE_MODE_FAN_ONLY;
        break;
      case MODE_SMART:
        mode = climate::CLIMATE_MODE_AUTO;
        break;
      default:
        // Already warned about in parse_status_().
        break;
    }
  }
  if (this->mode != mode) {
    this->mode = mode;
    changed = true;
  }

  float target = s.set_temp;
  float current = s.room_temp;
  if (s.fahrenheit) {
    target = (target - 32.0f) * 5.0f / 9.0f;
    current = (current - 32.0f) * 5.0f / 9.0f;
  }
  if (this->target_temperature != target) {
    this->target_temperature = target;
    changed = true;
  }
  if (this->current_temperature != current) {
    this->current_temperature = current;
    changed = true;
  }

  const int speed = fan_speed_index(s.wind);
  if (speed >= 0) {
    changed |= this->set_custom_fan_mode_(FAN_SPEEDS[speed]);
  } else if (s.wind == WIND_QUIET) {
    changed |= this->set_fan_mode_(climate::CLIMATE_FAN_QUIET);
  } else {
    changed |= this->set_fan_mode_(climate::CLIMATE_FAN_AUTO);
  }

  climate::ClimateSwingMode swing = climate::CLIMATE_SWING_OFF;
  if (s.swing_vertical && s.swing_horizontal) {
    swing = climate::CLIMATE_SWING_BOTH;
  } else if (s.swing_vertical) {
    swing = climate::CLIMATE_SWING_VERTICAL;
  } else if (s.swing_horizontal) {
    swing = climate::CLIMATE_SWING_HORIZONTAL;
  }
  if (this->swing_mode != swing) {
    this->swing_mode = swing;
    changed = true;
  }

  if (s.turbo) {
    changed |= this->set_preset_(climate::CLIMATE_PRESET_BOOST);
  } else if (s.eco) {
    changed |= this->set_preset_(climate::CLIMATE_PRESET_ECO);
  } else if (s.sleep == 1) {
    changed |= this->set_preset_(climate::CLIMATE_PRESET_SLEEP);
  } else if (s.sleep >= 2 && s.sleep <= 4) {
    changed |= this->set_custom_preset_(SLEEP_PRESETS[s.sleep - 2]);
  } else if (is_heat_8(s)) {
    changed |= this->set_custom_preset_(HEAT_8_PRESET);
  } else {
    changed |= this->set_preset_(climate::CLIMATE_PRESET_NONE);
  }

  if (changed)
    this->publish_state();

  if (this->display_switch_ != nullptr && (!this->display_published_ || this->display_switch_->state != s.display)) {
    this->display_switch_->publish_state(s.display);
    this->display_published_ = true;
  }
}

void HisenseClimate::update_link_(uint32_t now) {
  if (this->link_sensor_ == nullptr)
    return;
  const bool ok = this->have_status_ && now - this->last_status_at_ < LINK_TIMEOUT_MS;
  if (!this->link_published_ || this->link_sensor_->state != ok) {
    this->link_sensor_->publish_state(ok);
    this->link_published_ = true;
  }
}

void HisenseClimate::enqueue_(const Command &cmd) { this->enqueue_(cmd.name, cmd.data, cmd.len); }

void HisenseClimate::enqueue_(const char *name, const uint8_t *body, size_t len) {
  Outgoing out{name, {}};
  encode_frame(body, len, out.wire);
  this->pending_.push_back(std::move(out));
}

void HisenseClimate::set_display(bool on) {
  this->enqueue_(on ? CMD_DISPLAY_ON : CMD_DISPLAY_OFF);
  if (this->display_switch_ != nullptr)
    this->display_switch_->publish_state(on);
}

void HisenseClimate::control(const climate::ClimateCall &call) {
  if (!this->have_status_) {
    ESP_LOGW(TAG, "No status from the AC yet, ignoring the change");
    this->publish_state();
    return;
  }
  // `next` is what the AC should report once the queued commands have run.
  // Commands are only queued for real changes, since each one makes the AC beep.
  AcStatus next = this->status_;
  const size_t queued_before = this->pending_.size();

  uint8_t want_temp = 0;
  if (call.get_target_temperature().has_value())
    want_temp = std::clamp<int>(static_cast<int>(lroundf(*call.get_target_temperature())), MIN_TEMP_C, MAX_TEMP_C);

  if (call.get_mode().has_value())
    this->control_mode_(*call.get_mode(), want_temp, next);
  if (want_temp != 0 && want_temp != next.set_temp)
    this->control_setpoint_(want_temp, next);
  if (call.get_fan_mode().has_value() || call.has_custom_fan_mode())
    this->control_fan_(call, next);
  if (call.get_swing_mode().has_value())
    this->control_swing_(*call.get_swing_mode(), next);
  if (call.get_preset().has_value() || call.has_custom_preset())
    this->control_preset_(call, next);

  if (this->pending_.size() == queued_before) {
    // Nothing to send; put the frontend back to the AC's real state.
    this->publish_from_(this->status_, true);
    return;
  }
  this->publish_from_(next, true);
}

void HisenseClimate::control_mode_(climate::ClimateMode mode, uint8_t want_temp, AcStatus &next) {
  if (mode == climate::CLIMATE_MODE_OFF) {
    if (next.on) {
      // The off frame also ends turbo, eco and sleep.
      this->enqueue_(CMD_OFF);
      next.on = false;
      next.turbo = false;
      next.eco = false;
      next.sleep = 0;
    }
    return;
  }

  if (mode == climate::CLIMATE_MODE_AUTO) {
    if (next.mode != MODE_SMART) {
      this->enqueue_(CMD_SMART);
      next.mode = MODE_SMART;
    }
  } else {
    uint8_t ac_mode = next.mode;
    switch (mode) {
      case climate::CLIMATE_MODE_HEAT:
        ac_mode = MODE_HEAT;
        break;
      case climate::CLIMATE_MODE_COOL:
        ac_mode = MODE_COOL;
        break;
      case climate::CLIMATE_MODE_DRY:
        ac_mode = MODE_DRY;
        break;
      case climate::CLIMATE_MODE_FAN_ONLY:
        ac_mode = MODE_FAN;
        break;
      default:
        ESP_LOGW(TAG, "Mode %s isn't supported", LOG_STR_ARG(climate::climate_mode_to_string(mode)));
        break;
    }
    if (ac_mode != next.mode) {
      // pyaehw4a1's mode frames carry a fixed setpoint (heat 23 °C, cool 26 °C,
      // dry and fan 25 °C), which the AC takes. Like the remote, send the
      // setpoint last used in that mode instead.
      const Command &cmd = *mode_command(ac_mode);
      Body body = body_of(cmd);
      const uint8_t temp = want_temp != 0 ? want_temp : this->setpoint_for_(ac_mode);
      if (temp != 0) {
        body[BYTE_SETPOINT] = setpoint_byte(temp);
        next.set_temp = temp;
      }
      this->enqueue_(cmd.name, body.data(), body.size());
      // Mode frames also end sleep and turbo, and all but fan-only set auto fan.
      next.mode = ac_mode;
      next.sleep = 0;
      next.turbo = false;
      if (ac_mode != MODE_FAN)
        next.wind = WIND_AUTO;
    }
  }
  if (!next.on) {
    this->enqueue_(CMD_ON);
    next.on = true;
  }
}

void HisenseClimate::control_setpoint_(uint8_t temp, AcStatus &next) {
  if (!next.on) {
    ESP_LOGW(TAG, "The AC is off; not sending the temperature");
  } else if (next.mode == MODE_SMART) {
    ESP_LOGW(TAG, "In SMART mode the AC picks its own setpoint; changing it isn't supported yet");
  } else if (next.fahrenheit) {
    ESP_LOGW(TAG, "The AC is set to Fahrenheit, which this component can't send");
  } else if (next.turbo) {
    // Changing the setpoint ends boost, in one frame that also puts the fan back.
    Body body = body_of(CMD_NOOP);
    body[BYTE_PRESETS] = PRESET_TURBO_OFF;
    body[BYTE_SETPOINT] = setpoint_byte(temp);
    if (this->have_plain_) {
      set_fan(body, fan_command(this->plain_.wind));
      next.wind = this->plain_.wind;
    }
    this->enqueue_("turbo off with a new setpoint", body.data(), body.size());
    next.turbo = false;
    next.set_temp = temp;
  } else {
    // Eco and sleep stay on; the setpoint frame only changes the setpoint.
    this->enqueue_(CMD_TEMP_C[temp - MIN_TEMP_C]);
    next.set_temp = temp;
  }
}

void HisenseClimate::control_fan_(const climate::ClimateCall &call, AcStatus &next) {
  const Command *cmd = nullptr;
  uint8_t wind = next.wind;
  if (call.has_custom_fan_mode()) {
    const StringRef name = call.get_custom_fan_mode();
    for (size_t i = 0; i < NUM_FAN_SPEEDS; i++) {
      if (name == FAN_SPEEDS[i]) {
        cmd = FAN_SPEED_COMMANDS[i];
        wind = FAN_SPEED_WIND[i];
      }
    }
  } else if (*call.get_fan_mode() == climate::CLIMATE_FAN_QUIET) {
    cmd = &CMD_SPEED_MUTE;
    wind = WIND_QUIET;
  } else if (*call.get_fan_mode() == climate::CLIMATE_FAN_AUTO) {
    cmd = &CMD_SPEED_AUTO;
    wind = WIND_AUTO;
  }

  if (cmd == nullptr) {
    return;
  } else if (!next.on) {
    ESP_LOGW(TAG, "The AC is off; not sending the fan speed");
  } else if (next.mode != MODE_COOL && next.mode != MODE_FAN) {
    ESP_LOGW(TAG, "The AC only takes fan speed changes in cool and fan-only mode");
  } else if (next.mode == MODE_FAN && is_auto_wind(wind)) {
    ESP_LOGW(TAG, "Auto fan speed isn't available in fan-only mode");
  } else if (wind != next.wind && !(is_auto_wind(wind) && is_auto_wind(next.wind))) {
    Body body = body_of(*cmd);
    // Fan frames also end turbo and sleep. After boost, put the setpoint back too.
    if (next.turbo && this->have_plain_) {
      body[BYTE_SETPOINT] = setpoint_byte(this->plain_.set_temp);
      next.set_temp = this->plain_.set_temp;
    }
    this->enqueue_(cmd->name, body.data(), body.size());
    next.wind = wind;
    next.turbo = false;
    next.sleep = 0;
  }
}

void HisenseClimate::control_swing_(climate::ClimateSwingMode swing, AcStatus &next) {
  if (!next.on) {
    ESP_LOGW(TAG, "The AC is off; not sending the swing mode");
    return;
  }
  const bool vertical = swing == climate::CLIMATE_SWING_VERTICAL || swing == climate::CLIMATE_SWING_BOTH;
  const bool horizontal = swing == climate::CLIMATE_SWING_HORIZONTAL || swing == climate::CLIMATE_SWING_BOTH;
  // Each axis has its own start and stop command.
  if (vertical != next.swing_vertical) {
    this->enqueue_(vertical ? CMD_VERT_SWING : CMD_VERT_DIR);
    next.swing_vertical = vertical;
  }
  if (horizontal != next.swing_horizontal) {
    this->enqueue_(horizontal ? CMD_HOR_SWING : CMD_HOR_DIR);
    next.swing_horizontal = horizontal;
  }
}

void HisenseClimate::control_preset_(const climate::ClimateCall &call, AcStatus &next) {
  enum class Want : uint8_t { NONE, ECO, BOOST, SLEEP, HEAT_8 };
  Want want = Want::NONE;
  uint8_t sleep_level = 0;
  if (call.has_custom_preset()) {
    const StringRef name = call.get_custom_preset();
    if (name == HEAT_8_PRESET) {
      want = Want::HEAT_8;
    } else {
      for (int i = 0; i < 3; i++) {
        if (name == SLEEP_PRESETS[i])
          sleep_level = i + 2;
      }
      if (sleep_level == 0) {
        ESP_LOGW(TAG, "Unknown preset %s", name.c_str());
        return;
      }
      want = Want::SLEEP;
    }
  } else {
    switch (*call.get_preset()) {
      case climate::CLIMATE_PRESET_NONE:
        want = Want::NONE;
        break;
      case climate::CLIMATE_PRESET_ECO:
        want = Want::ECO;
        break;
      case climate::CLIMATE_PRESET_BOOST:
        want = Want::BOOST;
        break;
      case climate::CLIMATE_PRESET_SLEEP:
        want = Want::SLEEP;
        sleep_level = 1;
        break;
      default:
        ESP_LOGW(TAG, "Preset isn't supported");
        return;
    }
  }

  if (want == Want::NONE && !next.on)
    return;
  if (want == Want::BOOST && next.mode == MODE_SMART) {
    ESP_LOGW(TAG, "The AC doesn't allow boost (SUPER on the remote) in SMART mode");
    return;
  }
  if (want == Want::HEAT_8 && next.mode != MODE_HEAT) {
    ESP_LOGW(TAG, "8° Heat only works in heat mode, as on the remote");
    return;
  }
  // As in the HA integration, choosing a preset turns the AC on.
  if (!next.on) {
    this->enqueue_(CMD_ON);
    next.on = true;
  }

  // One frame sets the new preset and ends the old one. Turbo, eco and sleep
  // each have their own setting, so the do-nothing frame is the base and only
  // what changes gets its apply bit.
  const bool heat_8 = is_heat_8(next);
  const bool ending = (next.turbo && want != Want::BOOST) || (next.eco && want != Want::ECO) ||
                      (next.sleep != 0 && want != Want::SLEEP) || (heat_8 && want != Want::HEAT_8);
  Body body = body_of(CMD_NOOP);
  if (want == Want::BOOST && !next.turbo) {
    body[BYTE_PRESETS] |= PRESET_TURBO_ON;
  } else if (want != Want::BOOST && next.turbo) {
    body[BYTE_PRESETS] |= PRESET_TURBO_OFF;
  }
  if (want == Want::ECO && !next.eco) {
    body[BYTE_PRESETS] |= PRESET_ECO_ON;
  } else if (want != Want::ECO && next.eco) {
    body[BYTE_PRESETS] |= PRESET_ECO_OFF;
  }
  if (want == Want::SLEEP && next.sleep != sleep_level) {
    body[BYTE_SLEEP] = sleep_level * 2 + 1;
  } else if (want != Want::SLEEP && next.sleep != 0) {
    body[BYTE_SLEEP] = 0x01;
  }

  // Setpoint: 8° Heat sets 8 °C; leaving it, or leaving turbo, which sets its
  // own setpoint, puts the one from before back.
  uint8_t temp = 0;
  if (want == Want::HEAT_8 && !heat_8) {
    temp = HEAT_8_TEMP;
  } else if (want != Want::HEAT_8 && heat_8) {
    temp = this->setpoint_for_(MODE_HEAT);
  } else if (want != Want::BOOST && next.turbo && this->have_plain_) {
    temp = this->plain_.set_temp;
  }
  if (temp != 0) {
    body[BYTE_SETPOINT] = setpoint_byte(temp);
    next.set_temp = temp;
  }

  // Fan: eco sets the lowest speed, as the remote's ECONOMY button does; turbo
  // and sleep set the fan themselves. Ending a preset puts the fan from before back.
  if (want == Want::ECO && !next.eco) {
    set_fan(body, *FAN_SPEED_COMMANDS[0]);
    next.wind = FAN_SPEED_WIND[0];
  } else if ((want == Want::NONE || want == Want::HEAT_8) && ending && this->have_plain_) {
    set_fan(body, fan_command(this->plain_.wind));
    next.wind = this->plain_.wind;
  }

  if (std::equal(body.begin(), body.end(), CMD_NOOP.data))
    return;
  static const char *const NAMES[] = {"presets off", "eco on", "turbo on", "sleep on", "8° heat on"};
  this->enqueue_(NAMES[static_cast<uint8_t>(want)], body.data(), body.size());
  next.turbo = want == Want::BOOST;
  next.eco = want == Want::ECO;
  next.sleep = want == Want::SLEEP ? sleep_level : 0;
}

}  // namespace hisense_w4a1
}  // namespace esphome
