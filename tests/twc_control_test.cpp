#include "../components/twc_vitals/twc_control.h"
#include <cassert>
#include <iostream>
#include <limits>

using namespace twc_control;
static bool near(float a, float b) { return std::fabs(a - b) < 0.001f; }
struct Rig {
  Controller c;
  Settings s;
  Input in;
  Rig() {
    // Most existing scenarios exercise tracking, without restart hysteresis.
    s.resume_delay_ms = 0;
    s.resume_margin = 0;
    s.main_fuse = 20;
    s.conductor = 32;
    s.max_charge = 16;
    in.ready = in.ha_connected = in.feedback_fresh = true;
    in.contactor = true;
    in.phases = 3;
    in.vehicle = 10;
    for (int i = 0; i < 3; ++i) { in.ev[i] = 10; c.house[i].update(18, 0); }
  }
  Output tick(uint32_t now = 0) { in.now = now; return c.tick(s, in); }
};

int main() {
  {
    Rig r;
    r.in.mode = Mode::MANUAL_FIXED;
    r.in.ha_connected = false;
    r.c.invalidate_house();
    r.s.meter_guard = true;
    r.s.main_fuse = NAN; // Unused house settings do not block fixed control.
    r.s.margin = NAN;
    r.s.resume_margin = NAN;
    for (int phases = 1; phases <= 3; ++phases) {
      r.in.phases = phases;
      for (float target : {6.0f, 8.0f, 16.0f}) {
        r.s.manual_target = target;
        const auto o = r.tick();
        assert(o.fault == Fault::NONE && o.target == target);
        assert(std::isnan(o.headroom));
      }
    }
    r.s.manual_target = 48;
    assert(r.tick().target == 16); // Software charge ceiling still applies.
    r.s.manual_target = 0;
    assert(r.tick().target == 0);
    r.s.manual_target = 5;
    assert(r.tick().target == 0);
    r.s.manual_target = 8;
    r.s.resume_delay_ms = 30000;
    assert(r.tick().target == 8); // Manual pause does not require house headroom.
    r.in.feedback_fresh = false;
    assert(r.tick(1000).fault == Fault::VITALS);
    assert(r.tick(9000).watchdog_active);
    r.in.feedback_fresh = true;
    assert(r.tick(10000).target == 6); // Recovery fallback remains active.
    assert(r.tick(40000).target == 8);
    r.s.conductor = NAN;
    assert(r.tick(41000).fault == Fault::CONFIG);
  }
  {
    Rig r;
    r.in.mode = Mode::MANUAL_FIXED;
    r.s.manual_target = 8;
    for (auto &p : r.c.house) p.update(100, 0);
    assert(r.tick().target == 8); // Ignores even available overloaded house readings.
    r.in.vehicle = 17;
    for (auto &ev : r.in.ev) ev = 17;
    assert(r.tick().zone == Zone::REDUCE); // Charge ceiling protection retained.
    r.in.mode = Mode::MANUAL;
    assert(r.tick().target == 0); // Existing Manual still uses the house budget.
    r.c.invalidate_house();
    assert(r.tick().fault == Fault::HOUSE);
    r.in.mode = Mode::AUTO;
    assert(r.tick().fault == Fault::HOUSE);
    r.in.mode = Mode::MANUAL_FIXED;
    assert(r.tick().fault == Fault::NONE);
    r.in.ha_connected = false;
    r.in.mode = Mode::MANUAL;
    assert(r.tick().fault == Fault::HA);
    r.in.mode = Mode::AUTO;
    assert(r.tick().fault == Fault::HA);
  }
  {
    Rig r;
    r.s.resume_delay_ms = 30000;
    r.s.resume_margin = 1;
    r.in.contactor = false;
    for (auto &p : r.c.house) p.update(14, 0); // 5 A headroom: pause now.
    assert(r.tick().zone == Zone::PAUSE);
    for (auto &p : r.c.house) p.update(13, 1000); // 6 A cannot release latch.
    assert(r.tick(1000).zone == Zone::RESTART_WAIT);
    for (auto &p : r.c.house) p.update(12, 2000); // 7 A starts the timer.
    assert(r.tick(2000).target == 0);
    assert(r.tick(31999).zone == Zone::RESTART_WAIT);
    // A dip resets the timer even if it would otherwise permit 6 A.
    for (auto &p : r.c.house) p.update(12.5, 32000);
    assert(r.tick(32000).zone == Zone::RESTART_WAIT);
    for (auto &p : r.c.house) p.update(12, 33000);
    r.tick(33000);
    assert(r.tick(62999).target == 0);
    assert(r.tick(63000).zone == Zone::START);
    assert(r.tick(63000).target == 7);
    // Restart margin must not restrict normal running at 6 A.
    for (auto &p : r.c.house) p.update(13, 64000);
    assert(r.tick(64000).target == 6);
  }
  {
    Rig r;
    r.s.resume_delay_ms = 30000;
    r.s.resume_margin = 1;
    r.in.mode = Mode::MANUAL;
    r.s.manual_target = 6;
    for (auto &p : r.c.house) p.update(25, 0); // EV 10, other 15 => only 4 A.
    assert(r.tick().target == 0);
    for (auto &p : r.c.house) p.update(18, 1000);
    assert(r.tick(1000).zone == Zone::RESTART_WAIT);
    r.in.feedback_fresh = false;
    assert(r.tick(30000).fault == Fault::VITALS);
    r.in.feedback_fresh = true;
    assert(r.tick(31000).zone == Zone::RESTART_WAIT);
    assert(r.tick(60999).target == 0);
    assert(r.tick(61000).target == 6); // Manual 6 A can restart with spare headroom.
  }
  {
    Rig r;
    r.s.resume_delay_ms = 30000;
    r.s.resume_margin = 1;
    r.in.contactor = false;
    for (auto &p : r.c.house) p.update(15, 0xffff0000u);
    assert(r.tick(0xffff0000u).zone == Zone::PAUSE);
    for (auto &p : r.c.house) p.update(12, 0xfffffff0u);
    assert(r.tick(0xfffffff0u).zone == Zone::RESTART_WAIT);
    assert(r.tick(uint32_t(0xfffffff0u + 30000u)).zone == Zone::START);
  }
  {
    Rig r;
    r.s.meter_guard = true;
    assert(r.tick().fault == Fault::METER);
    r.in.meter_reporting = true;
    assert(r.tick(1000).fault == Fault::NONE);
    r.in.meter_reporting = false;
    assert(r.tick(2000).target == 0);
    r.in.mode = Mode::UNREGULATED;
    assert(r.tick(3000).zone == Zone::UNREGULATED);
  }
  {
    Rig r;
    // House includes 10 A car + 8 A other load: 20 - 1 - 8 = 11 A.
    auto o = r.tick();
    assert(near(o.target, 11));
    assert(near(o.headroom, 11));
    assert(o.zone == Zone::HOLD);
    assert(near(o.reported[0], 31.8f));
    assert(near(o.reported[0], o.reported[2]));
    // A loaded L3 constrains the entire session, independent of mean load.
    r.c.house[2].update(19, 1000);
    assert(near(r.tick(1000).target, 10));
  }
  {
    Rig r;
    r.in.mode = Mode::MANUAL;
    r.s.manual_target = 6;
    auto o = r.tick();
    assert(near(o.target, 6));
    assert(o.zone == Zone::REDUCE);  // >3 A above a changed target acts immediately.
    assert(near(o.reported[0], 32.95f));
    // At the manual target, hold below L rather than commanding more decline.
    r.in.vehicle = 6;
    for (int i = 0; i < 3; ++i) r.in.ev[i] = 6;
    o = r.tick(1000);
    assert(o.zone == Zone::HOLD);
    assert(near(o.reported[0], 31.7f));
    // Request can never override headroom or configured maximum.
    r.s.manual_target = 48;
    assert(r.tick(2000).target <= 16);
  }
  {
    Rig r;
    r.in.mode = Mode::MANUAL;
    r.s.manual_target = 8;
    assert(r.tick().zone == Zone::HOLD);  // Marginal 2 A excess waits 8 s.
    assert(r.tick(7999).zone == Zone::HOLD);
    assert(r.tick(8000).zone == Zone::REDUCE);
    // Recovery into the tolerance clears the persistence timer.
    r.in.vehicle = 8;
    for (auto &ev : r.in.ev) ev = 8;
    assert(r.tick(9000).zone == Zone::HOLD);
    r.in.vehicle = 10;
    for (auto &ev : r.in.ev) ev = 10;
    assert(r.tick(10000).zone == Zone::HOLD);
  }
  {
    Rig r;
    r.in.mode = Mode::MANUAL;
    r.s.manual_target = 11;
    r.in.vehicle = 11.8f;
    for (int i = 0; i < 3; ++i) { r.in.ev[i] = 11.8f; r.c.house[i].update(19.8f, 0); }
    // Slider tolerance never permits using the reserved main-fuse margin.
    assert(r.tick().zone == Zone::REDUCE);
    r.c.house[1].update(21, 1);
    assert(r.tick(1).zone == Zone::HARD);
  }
  {
    Rig r;
    r.in.mode = Mode::MANUAL;
    r.s.manual_target = 0;
    auto o = r.tick();
    assert(o.zone == Zone::PAUSE && o.target == 0);
    assert(o.reported[0] > 33.1f);
    // Targets below vehicle minimum request pause instead of trying 1..5 A.
    r.s.manual_target = 5;
    assert(r.tick().target == 0);
  }
  {
    Rig r;
    r.in.mode = Mode::UNREGULATED;
    r.in.ha_connected = r.in.feedback_fresh = false;
    r.c.invalidate_house();
    r.s.max_charge = 6;
    auto o = r.tick();
    assert(o.zone == Zone::UNREGULATED && std::isnan(o.target));
    assert(near(o.reported[0], 0.1f));
    assert(!o.watchdog_active);
    assert(near(r.tick(1000).reported[0], 0));
    // Deliberate opt-out remains independent of missing HA/vitals over time.
    assert(r.tick(300000).zone == Zone::UNREGULATED);
    // Returning to regulated mode requires valid new inputs, not cached zeros.
    r.in.mode = Mode::AUTO;
    assert(r.tick(301000).zone == Zone::PAUSE);
    // Even restored bypass cannot bypass the initial safe boot sequence.
    r.in.mode = Mode::UNREGULATED;
    r.in.ready = false;
    assert(r.tick().zone == Zone::BOOT);
  }
  {
    Rig r;
    r.in.feedback_fresh = false;
    auto o = r.tick();
    assert(o.fault == Fault::VITALS && o.zone == Zone::PAUSE);
    assert(!o.watchdog_active);
    assert(r.tick(8000).watchdog_active);
    // Fault recovery needs continuous healthy data and uses fallback safely.
    r.in.feedback_fresh = true;
    o = r.tick(9000);
    assert(o.watchdog_active && o.target == 6);
    assert(r.tick(38999).watchdog_active);
    assert(!r.tick(39000).watchdog_active);
    // A new fault interrupts the recovery interval.
    r.in.ha_connected = false;
    r.tick(40000);
    r.tick(48000);
    r.in.ha_connected = true;
    r.tick(49000);
    r.in.ha_connected = false;
    r.tick(50000);
    r.in.ha_connected = true;
    assert(r.tick(79000).watchdog_active);
    assert(!r.tick(109000).watchdog_active);
  }
  {
    Rig r;
    r.s.watchdog = false;
    r.in.feedback_fresh = false;
    assert(r.tick(100000).zone == Zone::PAUSE);  // Data guards are mandatory.
    assert(!r.tick(120000).watchdog_active);
    r.in.feedback_fresh = true;
    assert(r.tick(121000).zone == Zone::HOLD);
  }
  {
    Rig r;
    r.in.contactor = false;
    for (auto &p : r.c.house) { p = Phase{}; p.update(8, 0); }
    assert(r.tick().zone == Zone::START);
    r.in.contactor = true;
    r.in.ev[1] = r.in.ev[2] = 0;
    r.c.house[1] = r.c.house[2] = Phase{};
    r.c.house[1].update(0, 0);
    r.c.house[2].update(0, 0);
    r.s.main_fuse = 40;
    auto o = r.tick(1000);
    assert(o.zone == Zone::START);
    assert(o.reported[1] < o.reported[0]);
    r.in.ev[1] = r.in.ev[2] = 10;
    assert(r.tick(2000).zone == Zone::INCREASE);
    // A single-phase car on a 3-phase site must not stay in startup forever.
    r.in.ev[1] = r.in.ev[2] = 0;
    assert(r.tick(17000).zone == Zone::INCREASE);
  }
  {
    Rig r;
    r.in.phases = 1;
    r.c.house[1].update(NAN, 0);
    assert(r.tick().fault == Fault::NONE);
    assert(r.tick().reported[1] == 0);
    r.in.phases = 2;
    assert(r.tick().fault == Fault::HOUSE);
  }
  {
    Rig r;
    r.s.main_fuse = 40;
    r.in.mode = Mode::MANUAL;
    r.s.manual_target = 10;
    auto hold = r.tick();
    r.s.manual_target = 16;
    auto grow = r.tick(1000);
    assert(grow.zone == Zone::INCREASE);
    assert(grow.reported[0] >= hold.reported[0] - 2.0f);
    // An overload is never slowed by the release-rate limiter.
    r.c.house[0].update(42, 1001);
    assert(r.tick(1001).zone == Zone::HARD);
  }
  {
    Phase p;
    p.update(20, UINT32_MAX - 1000, true);
    p.update(2, UINT32_MAX - 500, true);
    assert(near(p.trusted, 20));
    p.update(2.5f, 0, true);
    assert(near(p.trusted, 2));
    assert(p.available(1000, 2000));
    assert(!p.available(2000, 2000));
    assert(p.available(200000, 0));  // Unchanged HA state is not proof of staleness.
    p.update(std::numeric_limits<float>::infinity(), 1);
    assert(!p.available(1, 0));
    p.update(-1, 2);
    assert(!p.available(2, 0));
    p.update(NAN, 3);
    assert(!p.available(3, 0));
    p.update(20, 4);
    p.update(2, 5);
    assert(near(p.trusted, 2));  // Changed-only HA data must not wait for a repeat.
  }
  {
    Rig r;
    r.s.conductor = NAN;
    auto o = r.tick();
    assert(o.fault == Fault::CONFIG);
    for (float x : o.reported) assert(std::isfinite(x) && x > 200);
    r.s.conductor = 32;
    r.in.ev[2] = NAN;
    assert(r.tick().fault == Fault::VITALS);
  }
  std::cout << "All TWC control scenarios passed\n";
}
