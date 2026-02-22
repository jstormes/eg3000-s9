# Modbus RS485 Battery Sniffer

Passive sniffer for the RS485 bus between an EG4 3000EHV-48 inverter and LifePower4 batteries. Decodes Modbus RTU responses and writes a JSON file with the latest battery readings.

Written in C with no external dependencies (POSIX libc only). Designed to run on a Raspberry Pi Zero W with a USB-to-RS485 adapter.

## Building

```
make
```

Cross-compile for armv6l from another machine:

```
make CC=arm-linux-gnueabihf-gcc
```

## Usage

```
./modbus_sniffer -s /dev/ttyUSB0 -o /tmp/battery_data.json
```

### Options

| Flag | Description | Default |
|------|-------------|---------|
| `-s PORT` | Serial port device (required) | — |
| `-o FILE` | JSON output file (required) | — |
| `-b BAUD` | Baud rate | 9600 |
| `-d` | Daemonize (fork to background, log to syslog) | off |
| `-p FILE` | PID file path | `/var/run/modbus_sniffer.pid` |

When running in the foreground, decoded frames are logged to stderr. When daemonized with `-d`, output goes to syslog and can be viewed with `journalctl`.

## Systemd Service

Install and enable the service to start on boot:

```
sudo cp modbus-sniffer.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now modbus-sniffer.service
```

Check status and logs:

```
systemctl status modbus-sniffer.service
journalctl -u modbus-sniffer.service -f
```

## JSON Output

The default output file is `/tmp/battery_data.json`.

The sniffer writes JSON atomically (write to `.tmp`, then `rename()`) so consumers never see partial files. Output goes to `/tmp` by default (tmpfs on Raspbian) to avoid SD card wear.

```json
{
  "updated": "2026-02-17T05:20:02Z",
  "batteries": {
    "1": {
      "timestamp": "2026-02-17T05:20:01Z",
      "slave_id": 1,
      "soc_pct": 95,
      "voltage_v": 53.16,
      "current_a": 0.00,
      "temperature_c": 23,
      "cycle_count": 1125,
      "max_charge_current_a": 19.0,
      "max_discharge_current_a": 20.0,
      "soh_pct": 93,
      "max_charge_voltage_v": 58.00,
      "raw_registers": [1125, 0, 95, 5316, 0, 23, 3332, 19000, 20000, 257, 0, 388, 0, 93, 5800, 0, 0]
    }
  }
}
```

### Fields

**Confirmed** (verified against inverter LCD):

| Field | Unit | Description |
|-------|------|-------------|
| `soc_pct` | % | State of charge (0–100) |
| `voltage_v` | V | Pack voltage |
| `current_a` | A | Pack current (negative = discharge) |
| `temperature_c` | °C | Battery temperature |

**High-confidence** (consistent with LifePower4 specs):

| Field | Unit | Description |
|-------|------|-------------|
| `cycle_count` | — | Per-battery cycle count |
| `max_charge_current_a` | A | BMS max charge current |
| `max_discharge_current_a` | A | BMS max discharge current |
| `soh_pct` | % | State of health |
| `max_charge_voltage_v` | V | BMS charge voltage cutoff |

The `raw_registers` array contains all 17 register values (registers 19–35) for fields not yet decoded. See `../RS485-BUS-PROTOCOL.md` for the full register map.

## Multi-Battery Note

Per the EG4 manual, battery DIP switches should be set to different
addresses (battery 1 = all off = addr 0, battery 2 = switch 1 on =
addr 1). The inverter polls address 0x01 and both batteries respond
(addr 0 responds to all polls), causing bus collisions. The sniffer's
CRC16 verification filters out corrupted frames, so valid data from
one battery is reliably decoded.

The JSON will have a single `"1"` entry. One battery's SOC reading
is sufficient since both batteries in a parallel bank share the same
charge state.

## Hardware Setup

- **Inverter**: EG4 3000EHV-48
- **Batteries**: 2× EG4 LifePower4 (V2 firmware)
- **Adapter**: [EG4 USB Read/Write Cable](https://signaturesolar.com/eg4-usb-read-write-cable/) (USB-to-RS485), tapped passively onto the battery bus
- **Pi**: Raspberry Pi Zero W running Raspbian

RS485 bus uses RJ45 connectors — pin 7 (brown/white) is B−, pin 8 (brown) is A+. The sniffer is read-only and does not transmit on the bus.
