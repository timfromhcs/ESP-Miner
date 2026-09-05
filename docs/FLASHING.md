# Flashing Guide (Physical USB Only)

> **SAFETY NOTICE:** In accordance with GEMINI.md, network flashing or remote administration is strictly prohibited. Only direct physical USB serial flashing is supported.

## Flashing via USB Serial (esptool)
`ash
# Identify your COM port (e.g. COM3 or /dev/ttyUSB0)
python -m esptool --port COM3 --baud 460800 write_flash 0x10000 build/esp-miner.bin
`

## First Boot
Open a serial terminal at 115200 baud to observe boot logs:
`ash
python -m serial.tools.miniterm COM3 115200
`
