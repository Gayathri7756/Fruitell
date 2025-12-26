# // to check the ports available in the system
import serial.tools.list_ports

ports = serial.tools.list_ports.comports()
if not ports:
    print("No serial ports found.")
for p in ports:
    print(f"{p.device}  |  {p.description}  |  hwid={p.hwid}")
