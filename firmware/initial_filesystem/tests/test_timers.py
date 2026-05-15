import time
from machine import Timer, WDT, Pin

print("=== Timer & WDT Test ===")
oled_clear(True)
oled_set_cursor(0, 0)
oled_println("Timer & WDT Test")

# ── Timer.PERIODIC ───────────────────────────────────────────────────────────

tick_count = 0

def on_tick(t):
    global tick_count
    tick_count += 1

tim = Timer(0)
tim.init(period=50, mode=Timer.PERIODIC, callback=on_tick)
time.sleep_ms(300)
tim.deinit()

ok = tick_count >= 4
print("  periodic: {} ticks in 300ms -> {}".format(tick_count, "OK" if ok else "FAIL"))
oled_println("periodic: " + ("OK" if ok else "FAIL"))

# ── Timer.ONE_SHOT ───────────────────────────────────────────────────────────

one_shot_fired = False

def on_oneshot(t):
    global one_shot_fired
    one_shot_fired = True

tim2 = Timer(1)
tim2.init(period=100, mode=Timer.ONE_SHOT, callback=on_oneshot)
time.sleep_ms(200)
tim2.deinit()

ok2 = one_shot_fired
print("  one-shot: {} -> {}".format(one_shot_fired, "OK" if ok2 else "FAIL"))
oled_println("one-shot: " + ("OK" if ok2 else "FAIL"))

# ── WDT ──────────────────────────────────────────────────────────────────────

wdt = WDT(timeout=8000)
wdt.feed()
print("  WDT: created and fed -> OK")
oled_println("WDT: OK")

# ── select.poll (basic smoke) ────────────────────────────────────────────────

try:
    import select
    p = select.poll()
    print("  select.poll: OK")
    oled_println("select: OK")
except Exception as e:
    print("  select.poll: FAIL -", e)
    oled_println("select: FAIL")

# ── machine.time_pulse_us (just verify it's importable) ─────────────────────

try:
    from machine import time_pulse_us
    print("  time_pulse_us: OK (importable)")
    oled_println("pulse: OK")
except Exception as e:
    print("  time_pulse_us: FAIL -", e)
    oled_println("pulse: FAIL")

# ── Summary ──────────────────────────────────────────────────────────────────

oled_println("")
oled_println("Done!")
oled_show()
print("=== Timer & WDT Test Done ===")

time.sleep_ms(3000)
exit()
