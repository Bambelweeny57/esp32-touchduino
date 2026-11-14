import serial, sys, time
s = serial.Serial(sys.argv[1], 115200, timeout=0.2)
if len(sys.argv) > 3 and sys.argv[3] == 'reset':
    s.dtr = False; s.rts = True; time.sleep(0.1); s.rts = False
end = time.time() + float(sys.argv[2])
while time.time() < end:
    d = s.read(4096)
    if d: sys.stdout.write(d.decode('utf-8', 'replace')); sys.stdout.flush()
