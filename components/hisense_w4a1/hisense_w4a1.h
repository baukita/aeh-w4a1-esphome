#pragma once

#include <deque>
#include <vector>

#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/climate/climate.h"
#include "esphome/components/switch/switch.h"
#include "esphome/components/uart/uart.h"
#include "esphome/core/component.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/preferences.h"

#include "commands.h"

namespace esphome {
namespace hisense_w4a1 {

class HisenseClimate;

/// The front panel display. It follows status bit 169 ("back_led" in
/// pyaehw4a1's field table); bit 170 ("display_led") stays 0 on this AC.
class HisenseDisplaySwitch : public switch_::Switch, public Parented<HisenseClimate> {
 protected:
  void write_state(bool state) override;
};

/// The fields of a 102/0 status frame this component uses. Offsets are from
/// pyaehw4a1's DataPacket.data_102_0 table.
struct AcStatus {
  uint8_t wind{0};      // wind_status
  uint8_t sleep{0};     // sleep_status: 0 off, 1-4 sleep level
  uint8_t mode{0};      // mode_status: 0 fan, 1 heat, 2 cool, 3 dry, 5 SMART
  bool on{false};       // run_status
  uint8_t set_temp{0};  // indoor_temperature_setting
  uint8_t room_temp{0};  // indoor_temperature_status
  bool fahrenheit{false};
  bool swing_vertical{false};    // up_down
  bool swing_horizontal{false};  // left_right
  bool eco{false};               // low_electricity
  bool turbo{false};             // efficient
  bool display{false};           // back_led
};

/// The last setpoint used in each mode, indexed by mode_status (fan, heat,
/// cool, dry); 0 if not known yet. Kept in flash, so it survives restarts.
struct ModeSetpoints {
  uint8_t temp[4];
};

class HisenseClimate : public climate::Climate, public PollingComponent, public uart::UARTDevice {
 public:
  HisenseClimate();
  void setup() override;
  void loop() override;
  void update() override;
  void dump_config() override;

  void set_flow_control_pin(GPIOPin *pin) { this->flow_control_pin_ = pin; }
  void set_display_switch(HisenseDisplaySwitch *sw) { this->display_switch_ = sw; }
  void set_link_sensor(binary_sensor::BinarySensor *sensor) { this->link_sensor_ = sensor; }

  /// Sends display_on or display_off. Called by the display switch.
  void set_display(bool on);

 protected:
  enum class Phase : uint8_t { IDLE, SENDING, WAITING };
  // A frame waiting to go out, already as it's sent on the wire.
  struct Outgoing {
    const char *name;
    std::vector<uint8_t> wire;
  };

  climate::ClimateTraits traits() override;
  void control(const climate::ClimateCall &call) override;
  void control_mode_(climate::ClimateMode mode, uint8_t want_temp, AcStatus &next);
  void control_setpoint_(uint8_t temp, AcStatus &next);
  void control_fan_(const climate::ClimateCall &call, AcStatus &next);
  void control_swing_(climate::ClimateSwingMode swing, AcStatus &next);
  void control_preset_(const climate::ClimateCall &call, AcStatus &next);

  void enqueue_(const Command &cmd);
  void enqueue_(const char *name, const uint8_t *body, size_t len);
  uint8_t setpoint_for_(uint8_t mode) const;
  void remember_(const AcStatus &s);
  void start_next_(uint32_t now);
  void send_(const uint8_t *data, size_t len, uint32_t now);
  void finish_transaction_(uint32_t now);
  void feed_(uint8_t byte);
  void check_frame_();
  void parse_status_(const uint8_t *payload, size_t len);
  void publish_from_(const AcStatus &s, bool force);
  void update_link_(uint32_t now);

  GPIOPin *flow_control_pin_{nullptr};
  HisenseDisplaySwitch *display_switch_{nullptr};
  binary_sensor::BinarySensor *link_sensor_{nullptr};

  // Commands waiting to go out. Each is sent exactly once and never re-sent:
  // the AC may already have run it (and beeped) even if no reply comes back.
  std::deque<Outgoing> pending_;
  bool poll_wanted_{false};
  uint32_t poll_not_before_{0};

  Phase phase_{Phase::IDLE};
  bool in_flight_is_status_{false};
  const char *in_flight_name_{nullptr};
  uint32_t tx_done_at_{0};
  uint32_t reply_deadline_{0};
  uint32_t idle_since_{0};

  std::vector<uint8_t> rx_;
  bool in_frame_{false};
  bool escape_{false};
  uint8_t prev_byte_{0};

  AcStatus status_{};
  bool have_status_{false};
  // The last status with no preset active. Ending a preset puts its setpoint
  // and fan back, since the AC doesn't do that itself.
  AcStatus plain_{};
  bool have_plain_{false};
  ModeSetpoints setpoints_{};
  ESPPreferenceObject setpoints_pref_;
  uint32_t last_status_at_{0};
  uint8_t missed_polls_{0};
  bool display_published_{false};
  bool link_published_{false};
};

}  // namespace hisense_w4a1
}  // namespace esphome
