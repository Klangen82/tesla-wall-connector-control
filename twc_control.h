#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

// Experimental FW 26.x controller. Community references and hardware test
// procedure are in README.md. This is a control request, not a Tesla safety API.
namespace twc_control {
enum class Mode { AUTO, MANUAL, MANUAL_FIXED, UNREGULATED };
enum class Zone { BOOT, PAUSE, RESTART_WAIT, START, INCREASE, HOLD, REDUCE, HARD, UNREGULATED };
enum class Fault { NONE, BOOT, CONFIG, HA, HOUSE, METER, VITALS };

inline const char *zone_name(Zone z) {
  switch (z) {
    case Zone::BOOT: return "Boot / pause requested";
    case Zone::PAUSE: return "Pause requested";
    case Zone::RESTART_WAIT: return "Waiting for stable charging headroom";
    case Zone::START: return "Startup";
    case Zone::INCREASE: return "Increase";
    case Zone::HOLD: return "Hold";
    case Zone::REDUCE: return "Reduce";
    case Zone::HARD: return "Overload / hard reduction";
    case Zone::UNREGULATED: return "Unregulated / Tesla One limit";
  }
  return "Unknown";
}
inline const char *fault_name(Fault f) {
  switch (f) {
    case Fault::NONE: return "-";
    case Fault::BOOT: return "Booting";
    case Fault::CONFIG: return "Invalid electrical settings";
    case Fault::HA: return "HA disconnected / awaiting new readings";
    case Fault::HOUSE: return "House current unavailable or stale";
    case Fault::METER: return "House meter reporting heartbeat missing or stale";
    case Fault::VITALS: return "TWC feedback unavailable or stale";
  }
  return "Unknown";
}

struct Phase {
  float trusted = NAN;
  float pending = NAN;
  bool valid = false;
  uint32_t updated_ms = 0;

  void update(float x, uint32_t now, bool confirm_drops = false) {
    updated_ms = now;
    valid = std::isfinite(x) && x >= 0 && x <= 200;
    if (!valid) { pending = NAN; return; }
    // Confirmation is opt-in for a periodically reporting source. Changed-only
    // HA updates might never repeat a stable drop, so holding it by default
    // would prevent recovery indefinitely. Never count timer ticks as samples.
    if (!confirm_drops || !std::isfinite(trusted) || x >= trusted - 3.0f) {
      trusted = x;
      pending = NAN;
    } else if (std::isfinite(pending) && std::fabs(x - pending) <= 1.0f) {
      trusted = std::min(x, pending);
      pending = NAN;
    } else {
      pending = x;
    }
  }
  bool available(uint32_t now, uint32_t timeout) const {
    return valid && (timeout == 0 || uint32_t(now - updated_ms) < timeout);
  }
};

struct Settings {
  float main_fuse = 16;
  float conductor = 16;
  float max_charge = 32;
  float manual_target = 6;
  float margin = 1;
  float fallback = 6;
  float minimum = 6;
  float tolerance = 1;
  uint32_t fault_delay_ms = 8000;
  uint32_t recovery_ms = 30000;
  uint32_t resume_delay_ms = 30000;
  float resume_margin = 1;
  // Zero: availability comes from HA unknown/unavailable + API connection.
  // Enable only with a source that really sends recurring unchanged samples.
  uint32_t house_timeout_ms = 0;
  bool watchdog = true;
  bool meter_guard = false;
};

struct Input {
  Mode mode = Mode::AUTO;
  int phases = 3;
  bool ready = false;
  bool ha_connected = false;
  bool feedback_fresh = false;
  bool meter_reporting = false;
  bool contactor = false;
  float ev[3] = {0, 0, 0};
  float vehicle = 0;
  uint32_t now = 0;
};

struct Output {
  float reported[3] = {0, 0, 0};
  float target = 0;
  float headroom = NAN;
  Zone zone = Zone::PAUSE;
  Fault fault = Fault::NONE;
  bool watchdog_active = false;
};

class Controller {
 public:
  Phase house[3];

  void invalidate_house() {
    for (auto &p : house) p = Phase{};
  }
  bool house_available(int phases, uint32_t now, uint32_t timeout) const {
    if (phases < 1 || phases > 3) return false;
    for (int i = 0; i < phases; ++i)
      if (!house[i].available(now, timeout)) return false;
    return true;
  }

  Output tick(const Settings &s, const Input &in) {
    Output out;
    const int phases = std::max(1, std::min(3, in.phases));
    const float dither = ((in.now / 1000u) % 2u == 0) ? 0.05f : -0.05f;
    const float limit = std::isfinite(s.conductor) && s.conductor >= 6 && s.conductor <= 200
        ? s.conductor : 200.0f;
    auto publish = [&](float value) {
      for (int i = 0; i < 3; ++i)
        out.reported[i] = i < phases ? std::max(0.0f, value + dither) : 0.0f;
    };
    if (in.mode != mode_ || phases != phases_) {
      mode_ = in.mode;
      phases_ = phases;
      reset_regulation_();
      fault_pending_ = recovering_ = watchdog_active_ = false;
      low_budget_paused_ = resume_pending_ = false;
    }
    if (limit != previous_limit_) {
      reset_regulation_();
      previous_limit_ = limit;
    }
    if (!in.ready) {
      out.zone = Zone::BOOT;
      out.fault = Fault::BOOT;
      publish(limit + 1.2f);
      return out;
    }
    // Intentional opt-out: preserve a responsive RS485 meter but do not impose
    // software limits, depend on HA/vitals, or let the watchdog override it.
    if (in.mode == Mode::UNREGULATED) {
      reset_regulation_();
      fault_pending_ = recovering_ = watchdog_active_ = false;
      low_budget_paused_ = resume_pending_ = false;
      out.zone = Zone::UNREGULATED;
      out.target = NAN;  // The ESP does not know/set Tesla One's actual ceiling.
      publish(0.05f);
      return out;
    }
    const bool load_balanced = in.mode != Mode::MANUAL_FIXED;
    if (!valid_settings_(s, load_balanced)) out.fault = Fault::CONFIG;
    else if (load_balanced && !in.ha_connected) out.fault = Fault::HA;
    else if (load_balanced && !house_available(phases, in.now, s.house_timeout_ms)) out.fault = Fault::HOUSE;
    else if (load_balanced && s.meter_guard && !in.meter_reporting) out.fault = Fault::METER;
    else if (!valid_feedback_(in, phases)) out.fault = Fault::VITALS;

    if (out.fault != Fault::NONE) {
      reset_regulation_();
      recovering_ = false;
      resume_pending_ = false;
      if (!fault_pending_) { fault_pending_ = true; fault_since_ = in.now; }
      if (s.watchdog && uint32_t(in.now - fault_since_) >= s.fault_delay_ms)
        watchdog_active_ = true;
      if (!s.watchdog) watchdog_active_ = false;
      out.watchdog_active = watchdog_active_;
      publish(limit + 1.2f);
      return out;  // Missing required control data always requests pause.
    }
    fault_pending_ = false;
    if (!s.watchdog) watchdog_active_ = false;
    if (watchdog_active_) {
      if (!recovering_) { recovering_ = true; recovered_since_ = in.now; }
      if (uint32_t(in.now - recovered_since_) >= s.recovery_ms) {
        watchdog_active_ = false;
        recovering_ = false;
      }
    }
    out.watchdog_active = watchdog_active_;

    float actual = in.contactor ? in.vehicle : 0.0f;
    float headroom = load_balanced ? s.main_fuse - s.margin : s.max_charge;
    float worst_total = 0;
    bool all_phases = true;
    for (int i = 0; i < phases; ++i) {
      const float ev = in.contactor ? in.ev[i] : 0.0f;
      actual = std::max(actual, ev);
      all_phases = all_phases && ev >= 2.0f;
      // Meter includes the car. Do not subtract the entire incomer reading
      // from available CHARGER current or add the car twice to the meter.
      if (load_balanced) {
        const float total = std::max(house[i].trusted, ev);
        const float other = std::max(0.0f, total - ev);
        headroom = std::min(headroom, s.main_fuse - s.margin - other);
        worst_total = std::max(worst_total, total);
      }
    }
    out.headroom = load_balanced ? std::max(0.0f, headroom) : NAN;
    const float allowed = std::max(0.0f, std::min(s.max_charge, headroom));
    float desired = in.mode == Mode::AUTO ? allowed : std::min(s.manual_target, allowed);
    if (watchdog_active_) desired = std::min(desired, s.fallback);
    out.target = std::max(0.0f, std::floor(desired));
    if (out.target < s.minimum) {
      out.target = 0;
      out.zone = Zone::PAUSE;
      reset_regulation_();
      low_budget_paused_ = load_balanced;
      resume_pending_ = false;
      publish(limit + 1.2f);
      return out;
    }
    if (low_budget_paused_) {
      // Restart margin applies to the house budget, not the manual request or
      // watchdog fallback (both may intentionally be exactly 6 A).
      if (headroom < s.minimum + s.resume_margin) {
        resume_pending_ = false;
      } else if (!resume_pending_) {
        resume_pending_ = true;
        resume_since_ = in.now;
      }
      if (!resume_pending_ || uint32_t(in.now - resume_since_) < s.resume_delay_ms) {
        out.target = 0;
        out.zone = Zone::RESTART_WAIT;
        reset_regulation_();
        publish(limit + 1.2f);
        return out;
      }
      low_budget_paused_ = resume_pending_ = false;
    }
    if (!in.contactor || actual < 2.0f) {
      reset_regulation_();
      out.zone = Zone::START;
      publish(std::max(0.0f, limit - out.target));
      return out;
    }
    if (!charging_) { charging_ = true; charging_since_ = in.now; }
    const float error = actual - out.target;
    // Tolerance is ONLY for tracking the requested slider. Electrical limits
    // and the reserved main-fuse margin take precedence, without an 8s delay.
    const bool protective = (load_balanced && worst_total > s.main_fuse - s.margin + 0.1f) ||
        actual > s.max_charge + 0.1f;
    if (load_balanced && worst_total > s.main_fuse + 0.1f) {
      out.zone = Zone::HARD;
      over_pending_ = false;
      publish(limit + 1.2f);
    } else if (protective || error > s.tolerance) {
      if (!over_pending_) { over_pending_ = true; over_since_ = in.now; }
      const bool act = protective || error >= 3.0f || uint32_t(in.now - over_since_) >= 8000u;
      out.zone = act ? Zone::REDUCE : Zone::HOLD;
      publish(act ? limit + 0.9f : limit - 0.25f);
    } else if (!all_phases && uint32_t(in.now - charging_since_) < 15000u) {
      // A multi-phase session can start on L1 alone. Leave headroom on idle
      // phases until they engage, but never bypass a protective reduction.
      out.zone = Zone::START;
      over_pending_ = false;
      for (int i = 0; i < phases; ++i)
        out.reported[i] = std::max(0.0f, std::min(limit, limit + in.ev[i] - out.target) + dither);
    } else if (error < -s.tolerance) {
      out.zone = Zone::INCREASE;
      over_pending_ = false;
      // Correlate with the car's ramp, rather than a static fake house load.
      publish(std::max(0.0f, limit + actual - out.target));
    } else {
      out.zone = Zone::HOLD;
      over_pending_ = false;
      publish(limit - 0.25f);
    }
    // Releasing a constraint can invite a fresh ramp. Allow the published
    // value to fall at most 2 A/s in INCREASE, while rises/protective braking
    // always pass immediately. This does not damp the car's rising echo.
    if (out.zone == Zone::INCREASE && published_) {
      const float dt = std::min(2.0f, uint32_t(in.now - published_ms_) / 1000.0f);
      for (int i = 0; i < phases; ++i)
        out.reported[i] = std::max(out.reported[i], previous_[i] - 2.0f * dt);
    }
    for (int i = 0; i < 3; ++i) previous_[i] = out.reported[i];
    published_ms_ = in.now;
    published_ = true;
    return out;
  }

 private:
  static bool valid_settings_(const Settings &s, bool load_balanced) {
    return (!load_balanced || (std::isfinite(s.main_fuse) && s.main_fuse >= 6 && s.main_fuse <= 200 &&
        std::isfinite(s.margin) && s.margin >= 0 && s.margin < s.main_fuse &&
        std::isfinite(s.resume_margin) && s.resume_margin >= 0 && s.resume_margin <= 5)) &&
        std::isfinite(s.conductor) && s.conductor >= 6 && s.conductor <= 200 &&
        std::isfinite(s.max_charge) && s.max_charge >= 6 && s.max_charge <= 80 &&
        std::isfinite(s.manual_target) && s.manual_target >= 0 &&
        std::isfinite(s.fallback) && s.fallback >= 0 &&
        std::isfinite(s.minimum) && s.minimum >= 6 &&
        std::isfinite(s.tolerance) && s.tolerance >= 0;
  }
  static bool valid_feedback_(const Input &in, int phases) {
    if (!in.feedback_fresh || !std::isfinite(in.vehicle) || in.vehicle < 0 || in.vehicle > 200)
      return false;
    for (int i = 0; i < phases; ++i)
      if (!std::isfinite(in.ev[i]) || in.ev[i] < 0 || in.ev[i] > 200) return false;
    return true;
  }
  void reset_regulation_() { charging_ = over_pending_ = published_ = false; }
  Mode mode_ = Mode::AUTO;
  int phases_ = 3;
  bool charging_ = false;
  uint32_t charging_since_ = 0;
  bool over_pending_ = false;
  uint32_t over_since_ = 0;
  bool fault_pending_ = false;
  uint32_t fault_since_ = 0;
  bool watchdog_active_ = false;
  bool recovering_ = false;
  uint32_t recovered_since_ = 0;
  bool published_ = false;
  uint32_t published_ms_ = 0;
  float previous_[3] = {0, 0, 0};
  float previous_limit_ = NAN;
  bool low_budget_paused_ = false;
  bool resume_pending_ = false;
  uint32_t resume_since_ = 0;
};

inline Controller &controller() { static Controller instance; return instance; }
}  // namespace twc_control
