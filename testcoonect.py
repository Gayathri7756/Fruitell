# raw_view.py
import time, serial
PORT="COM7"; BAUD=115200
ser = serial.Serial(PORT, BAUD, timeout=1)
time.sleep(1.2)          # let the board reboot
ser.reset_input_buffer(); ser.reset_output_buffer()
ser.write(b"TRAIN:ON\n") # ask device to stream CSV continuously (while RUNNING)
print("Sent TRAIN:ON; reading raw lines...\n")
while True:
    line = ser.readline().decode(errors="ignore").strip()
    if line:
        print(line)
