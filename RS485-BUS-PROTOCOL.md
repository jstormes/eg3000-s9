# EG4 3000EHV-48 / LifePower4 RS485 Bus Protocol

Reverse-engineered from passive bus sniffing, 2026-02-16 through 2026-02-20.

## Physical Layer

| Parameter | Value |
|---|---|
| Interface | RS485 half-duplex |
| Connector | RJ45 (on battery and inverter) |
| Baud rate | 9600 |
| Data bits | 8 |
| Parity | None |
| Stop bits | 1 |
| Protocol | Modbus RTU |

### RJ45 Pinout

| Pin | Wire (T568B) | Signal |
|---|---|---|
| 7 | Brown/White | RS485 B- |
| 8 | Brown | RS485 A+ |

### Battery DIP Switch Configuration (per manufacturer)

Each LifePower4 battery has 4 DIP switches that set a binary address.
Per the EG4 manual, in a 2-battery system:

| Battery | DIP Switch 1 | DIP Switches 2-4 | Address |
|---------|-------------|-------------------|---------|
| Battery #1 | OFF | OFF | 0 |
| Battery #2 | ON | OFF | 1 |

In a 3+ battery system, each additional battery increments the address
(battery 3 = switch 2 on = address 2, etc.).

### Bus Topology

The **inverter is the bus master**. It sends Modbus RTU read requests
on the bus. The batteries respond. Our sniffer is a passive listener.

```
[EG4 3000EHV-48 Inverter] --RS485--> [LifePower4 Battery #1] --RS485--> [LifePower4 Battery #2]
        (master)                        (addr 0, DIP all off)              (addr 1, DIP 1 on)
                   |
                   +--- [Pi Zero W Sniffer] (passive listener)
```

This was verified experimentally:
- **Inverter disconnected**: bus is completely silent (60s capture, zero bytes)
- **Inverter + one battery**: clean request/response pairs every ~0.5s
- **Inverter + two batteries (different addresses)**: bus collisions —
  both batteries respond to address 0x01 polls simultaneously, corrupting
  most frames. CRC verification filters out corrupted data.

### Bus Collision Behavior

With the manufacturer's recommended DIP switch settings (battery 1 at
address 0, battery 2 at address 1), the inverter polls address 0x01
and **both batteries respond**, causing RS485 bus collisions. The
address-0 battery appears to respond to all polls regardless of the
target address.

Despite the collisions, the sniffer's CRC16 verification successfully
filters valid frames. In practice, only one battery's data passes CRC
checks consistently. The inverter itself appears to handle this
collision gracefully — it shows correct SOC on its display and reports
no errors.

For our sniffer's purposes, one battery's SOC reading is sufficient
since both batteries in a parallel bank share the same state of charge.

## Modbus Frame Structure

### Request (Inverter -> Battery)

The inverter sends an 8-byte Modbus RTU request:

```
[SLAVE] [FUNC] [REG_HI] [REG_LO] [COUNT_HI] [COUNT_LO] [CRC_LO] [CRC_HI]
  01      03      00       13        00          11         74       03
```

| Field | Value | Meaning |
|---|---|---|
| Slave address | 0x01 | Target battery address |
| Function code | 0x03 | Read Holding Registers |
| Start register | 0x0013 (19) | First register to read |
| Register count | 0x0011 (17) | Number of registers (19-35) |
| CRC | 0x0374 | Modbus CRC16 (little-endian) |

### Response (Battery -> Inverter)

The response is a 39-byte Modbus RTU frame:

```
[SLAVE] [FUNC] [BYTE_COUNT] [34 bytes of register data] [CRC_LO] [CRC_HI]
  01      03       22         ...                           xx       xx
```

| Field | Value | Meaning |
|---|---|---|
| Slave address | 0x01 | Responding battery |
| Function code | 0x03 | Read Holding Registers |
| Byte count | 0x22 (34) | 17 registers x 2 bytes = 34 |
| Data | 34 bytes | 17 x 16-bit big-endian register values |
| CRC | 2 bytes | Modbus CRC16 (little-endian) |

### Passive Sniffing Notes

When sniffing the bus, the request and response arrive back-to-back in
the receive buffer with no gap (the Pi Zero's UART is not fast enough
to detect the inter-frame silence). A typical capture looks like:

```
[8-byte request][39-byte response] = 47 bytes total
```

To parse, scan the byte stream for the signature `[XX] 03 22` (any
non-zero slave, function 3, byte count 34), extract 39 bytes from that
offset, and verify CRC before decoding.

## Register Map: Registers 19-35

All register values are 16-bit unsigned integers transmitted
big-endian. Signed values (e.g., current) use two's complement.

### Confirmed Fields

These fields have been verified against the inverter's LCD display.

| Reg | Offset | Field | Raw Example | Scale | Unit | Notes |
|---|---|---|---|---|---|---|
| 21 | 2 | **SOC** | 96 | x1 | % | State of charge (0-100) |
| 22 | 3 | **Pack Voltage** | 5317 | /100 | V | 5317 = 53.17V |
| 23 | 4 | **Pack Current** | 0xFF9A (-102) | /100 signed | A | Negative = discharge, positive = charge |
| 24 | 5 | **Temperature** | 23 | x1 | C | Average battery temperature |

Display showed: SOC 96%, voltage 53.20V, current -0.70A, temp ~23C.
The small discrepancies in voltage (53.17 vs 53.20) and current
(-1.02 vs -0.70) are expected — individual battery values differ
slightly from the aggregate the inverter displays.

### High-Confidence Inferred Fields

These values are physically plausible and consistent with the
LifePower4 specifications (100Ah LFP, 48V nominal, 16S).

| Reg | Offset | Field | Raw Example | Scale | Unit | Reasoning |
|---|---|---|---|---|---|---|
| 26 | 7 | **Max Charge Current** | 19000 | /1000 | A | 19.0A; ~0.2C for 100Ah battery |
| 27 | 8 | **Max Discharge Current** | 20000 | /1000 | A | 20.0A; ~0.2C for 100Ah battery |
| 32 | 13 | **SOH** | 93 | x1 | % | State of health (0-100) |
| 33 | 14 | **Max Charge Voltage** | 5800 | /100 | V | 58.00V; BMS protection cutoff |

### Uncertain Fields

| Reg | Offset | Field (best guess) | Raw Example | Notes |
|---|---|---|---|---|
| 19 | 0 | **Cycle Count** (per-battery) | 1125 / 3175 | Differs between batteries; plausible cycle counts |
| 25 | 6 | **Cell Voltage Limit** | 3332 | 3332mV/cell x 16 cells = 53.3V; may be per-cell charge limit |
| 28 | 9 | **Flags / Version** | 257 (0x0101) | Both bytes = 0x01; possibly battery count or protocol version |
| 30 | 11 | **String Cycle Count** | 388 | Shared across both batteries; string-level metric |

### Unknown Fields (always zero)

| Reg | Offset |
|---|---|
| 20 | 1 |
| 29 | 10 |
| 31 | 12 |
| 34 | 15 |
| 35 | 16 |

These may be reserved, or may become non-zero under conditions not
observed during testing (e.g., alarm states, different charge phases).

## Multi-Battery Behavior

With 2 batteries on the bus, two distinct response variants were
observed during a clean capture (Feb 16, before DIP switch experiments):

**Variant A (Battery #1, cycles=1125):**
```
01 03 22 04 65 00 00 00 60 14 C5 00 00 00 17 0D 04 4A 38 4E 20 01 01 00 00 01 84 00 00 00 5D 16 A8 00 00 00 00 55 2C
```

**Variant B (Battery #2, cycles=3175):**
```
01 03 22 0C 67 00 00 00 60 14 C5 FF 9A 00 17 0D 04 4A 38 4E 20 01 01 00 00 01 84 00 00 00 5D 16 A8 00 00 00 00 10 81
```

### Differences between batteries

| Register | Battery A | Battery B | Interpretation |
|---|---|---|---|
| 19 (cycle count) | 1125 | 3175 | Different usage histories |
| 23 (current) | 0.00A | -1.02A | Normal imbalance in parallel batteries |

All other registers are identical — they reflect string-level or
BMS-aggregate values shared across the battery bank.

### Single-Battery Test (Feb 18)

With only one battery connected (cycles=3175, DIP all off) and the
inverter present, clean request/response pairs were observed with no
collisions. Max charge current dropped from 19.0A to 5.0A and max
discharge current from 20.0A to 10.0A, reflecting the single-battery
configuration.

## Experimental Findings (Feb 20)

### DIP Switch Experiments

| Configuration | Result |
|---|---|
| Battery 1 addr 0, Battery 2 addr 1 | Bus collisions; only battery 1 data passes CRC |
| Battery 1 addr 1, Battery 2 addr 0 | Bus collisions; still only cycles=1125 passes CRC |
| One battery only + inverter | Clean data, no collisions |
| One battery only, no inverter | Silent bus (confirms inverter is bus master) |
| Inverter disconnected, batteries connected | Silent bus |

### Key Conclusions

1. The **inverter is the bus master** — it generates all Modbus requests.
   The bus is completely silent without the inverter connected.
2. The inverter polls **address 0x01** only.
3. The address-0 battery responds to address-1 polls (likely because
   Modbus address 0 is the broadcast address).
4. With both batteries at different addresses, both respond simultaneously,
   causing bus collisions. CRC filtering recovers valid frames from one battery.
5. The inverter handles the collisions internally and displays correct
   aggregate SOC despite the corrupted bus traffic.
6. For the sniffer, one battery's SOC is sufficient — both batteries
   in a parallel bank share the same charge state.

## Relationship to Official Documentation

The **EG4-LL Battery MODBUS Protocol V01.06** (2017) defines a
different register layout for the same register range. That document
describes the battery's own direct Modbus interface (queried from
register 0), not the protocol observed on this bus. The LifePower4 V2
firmware appears to have rearranged the register positions without a
published update to the protocol document.

| Reg | V01.06 Label | Actual (observed) |
|---|---|---|
| 19 | Temp Avg | Cycle count (per-battery) |
| 21 | Cap Remaining | SOC |
| 22 | Max Charging Current | Pack Voltage |
| 23 | SOH | Pack Current |
| 24 | SOC | Temperature |

Do not rely on the V01.06 document for decoding this bus traffic.

## References

- [EG4-LL Battery MODBUS Protocol V01.06](https://github.com/gonzalop/wombatt/raw/refs/heads/main/docs/ref/EG4-LL-MODBUS-Communication-Protocol_ENG-correct-1.pdf) (outdated register labels)
- [gonzalop/wombatt](https://github.com/gonzalop/wombatt) — EG4 protocol reference PDFs
- [mr-manuel/venus-os_dbus-serialbattery](https://github.com/mr-manuel/venus-os_dbus-serialbattery) — eg4_ll.py driver
- [DIY Solar Forum: Decoding EG4 Lifepower4 BMS data](https://diysolarforum.com/threads/decoding-eg4-lifepower4-bms-data.47735/)
- [DIY Solar Forum: EG4-LL v2 ID1 Modbus Registers](https://diysolarforum.com/threads/eg4-ll-v2-id1-modbus-registers.67247/)
- [Solar Assistant: EG4 battery setup](https://solar-assistant.io/help/battery/eg4)

## Test Environment

- **Inverter**: EG4 3000EHV-48
- **Batteries**: 2x EG4 LifePower4 (V2 firmware)
- **Sniffer**: Raspberry Pi Zero W + [EG4 USB Read/Write Cable](https://signaturesolar.com/eg4-usb-read-write-cable/) (USB-to-RS485)
- **Testing dates**: 2026-02-16 through 2026-02-20
