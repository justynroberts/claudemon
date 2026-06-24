#!/usr/bin/env python3
"""Reset the board (DTR/RTS auto-reset) and print serial for N seconds.

`pio device monitor` needs a TTY and fails when scripted/backgrounded, so use
this to capture boot logs from automation or a plain pipe.

    python3 scripts/serial_capture.py [/dev/cu.usbserial-XX] [seconds]

Needs pyserial (the PlatformIO-bundled python has it:
/opt/homebrew/Cellar/platformio/*/libexec/bin/python).
"""
import sys, time
import serial  # type: ignore

port = sys.argv[1] if len(sys.argv) > 1 else "/dev/cu.usbserial-10"
secs = float(sys.argv[2]) if len(sys.argv) > 2 else 16.0

p = serial.Serial()
p.port = port
p.baudrate = 115200
p.timeout = 0.2
p.dtr = False
p.rts = False
p.open()
# Pulse EN (classic ESP32 auto-reset) to reboot into the app.
p.setDTR(False); p.setRTS(True); time.sleep(0.1)
p.setRTS(False); time.sleep(0.1)

end = time.time() + secs
while time.time() < end:
    data = p.read(4096)
    if data:
        sys.stdout.write(data.decode("utf-8", "replace"))
        sys.stdout.flush()
p.close()
