# Solar Miner Controller

Automatically starts and stops cryptocurrency miners based on battery state of charge. When solar panels fill the batteries above a threshold, mining begins. When battery SOC drops below a lower threshold, mining stops.

Designed for a Raspberry Pi Zero W controlling Antminer S9 miners (Braiins OS) with battery data from an EG4 3000EHV-48 inverter.

## Architecture

```
[EG4 3000EHV-48 Inverter] --RS485--> [LifePower4 Batteries]
                                |
              [modbus_sniffer (C)] ---> /tmp/battery_data.json
                                              |
                                    [index.js (Node.js)] --GraphQL--> [Antminer S9 #1]
                                                                  \--> [Antminer S9 #2]
```

Two services run on the Pi:

1. **modbus-sniffer** — C program that passively sniffs the RS485 bus and writes battery data to a JSON file (see `sniffer/README.md`)
2. **solar-miner** — Node.js controller that reads the JSON file and starts/stops miners via Braiins OS GraphQL API

## Prerequisites

- Raspberry Pi with network access to the miners
- Node.js 20+ installed (`node --version` to check)
- The `modbus-sniffer` service running (see `sniffer/README.md`)
- Antminer S9 miners running Braiins OS with GraphQL API on port 80
- Miner hostnames resolvable from the Pi (e.g., `miner1` in `/etc/hosts`)

## Deployment

### 1. Copy files to the Pi

From the development machine:

```bash
rsync -av --exclude='.git' --exclude='.idea' --exclude='node_modules' \
  ./ jstormes@eg3000:~/solar1/
```

Or copy individual files:

```bash
scp index.js inverter.js miner.js config.js package.json solar-miner.service \
  jstormes@eg3000:~/solar1/
```

### 2. Install the systemd service

SSH into the Pi:

```bash
ssh jstormes@eg3000
```

Install and enable the service:

```bash
sudo cp ~/solar1/solar-miner.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now solar-miner.service
```

### 3. Verify it's running

```bash
systemctl status solar-miner.service
journalctl -u solar-miner.service -f
```

Expected output:

```
[control] Solar Miner Controller starting
[control] Thresholds: start >= 95%, stop < 92%
[control] Miners: miner1
[control] Power target: 800W per miner (max 1200W)
[control] Poll interval: 30s
[2026-02-22T00:29:43.568Z] SOC: 99% | State: IDLE | Miners: 1 | 54.05V | 0A | 25°C
```

## Configuration

All settings are controlled via environment variables with sensible defaults. To override, either:

- Edit the systemd service file (`/etc/systemd/system/solar-miner.service`), uncomment and change the `Environment=` lines, then `sudo systemctl daemon-reload && sudo systemctl restart solar-miner.service`
- Or export variables before running manually: `MINER_HOSTS=miner1,miner2 node index.js`

### Environment Variables

| Variable | Default | Description |
|----------|---------|-------------|
| `BATTERY_JSON` | `/tmp/battery_data.json` | Path to the JSON file written by the modbus sniffer |
| `MINER_HOSTS` | `miner1` | Comma-separated list of miner hostnames or IPs |
| `MINER_PASSWORD` | *(required)* | Braiins OS root password (shared across all miners) |
| `MINER_POWER_TARGET_W` | `800` | Power target in watts set on each miner when mining starts |
| `MINER_MAX_POWER_TARGET_W` | `1200` | Hard cap on power target per miner (APW3++ limit at 120V) |
| `START_MINING_SOC` | `95` | Battery SOC % at or above which mining starts |
| `STOP_MINING_SOC` | `92` | Battery SOC % below which mining stops |
| `POLL_INTERVAL_MS` | `30000` | How often to check battery SOC (milliseconds) |

### Adding a Second Miner

When the second S9 is set up (e.g., at hostname `miner2`):

1. Add `miner2` to `/etc/hosts` on the Pi
2. Edit the service file:
   ```
   Environment=MINER_HOSTS=miner1,miner2
   ```
3. Reload and restart:
   ```bash
   sudo systemctl daemon-reload
   sudo systemctl restart solar-miner.service
   ```

### Power Target Notes

- The APW3++ PSU at 120V AC (EG4 3000EHV output) limits each S9 to ~1200W
- The efficiency sweet spot is ~700-800W per miner
- Autotuning takes ~30 minutes to stabilize after power target changes
- At 800W per miner with 2 miners, total draw is ~1600W — well within the EG4 3000EHV's 3000W capacity

### Fail-Safe Behavior

The controller is designed to protect batteries from over-discharge:

- **Stale or missing battery data**: If the battery JSON file can't be read for 3 consecutive polls (~90 seconds at default interval), miners are automatically stopped. This covers sniffer crashes, file corruption, or any other data loss.
- **Stop retries**: When stopping miners, the controller retries up to 3 times with 5-second delays. Stopping is safety-critical — a single network glitch shouldn't leave miners running.
- **Clean shutdown**: On SIGINT/SIGTERM, miners are stopped before the controller exits.
- **Default state is off**: The controller starts in IDLE state. Miners only start when battery SOC is confirmed above the start threshold.

### SOC Threshold Strategy

The 3% gap between start (95%) and stop (92%) prevents rapid on/off cycling. Adjust based on your setup:

- **Wider gap** (e.g., start 98%, stop 85%) — fewer start/stop cycles, less mining time
- **Narrower gap** (e.g., start 95%, stop 93%) — more mining time, more cycling

## Managing the Service

```bash
# View status
systemctl status solar-miner.service

# View live logs
journalctl -u solar-miner.service -f

# Restart after config changes
sudo systemctl daemon-reload
sudo systemctl restart solar-miner.service

# Stop mining temporarily
sudo systemctl stop solar-miner.service

# Disable from starting on boot
sudo systemctl disable solar-miner.service

# Re-enable on boot
sudo systemctl enable solar-miner.service
```

## Manual Testing

Run the controller interactively (Ctrl+C to stop — it will cleanly shut down miners):

```bash
cd ~/solar1
node index.js
```

Test a single miner connection:

```bash
cd ~/solar1
node -e "
const { BraiinsMiner } = require('./miner');
const m = new BraiinsMiner('miner1', 'your-password');
(async () => {
  const s = await m.getSummary();
  console.log(JSON.stringify(s, null, 2));
})();
"
```

## Claude Code Commands

Custom slash commands are available in `.claude/commands/` for common operations. These require a Claude Code session in this project directory.

| Command | Description |
|---------|-------------|
| `/soc` | Quick battery state of charge check — reports SOC %, charge/discharge status, voltage, current, and temperature |
| `/logs` | View recent service logs. Use `/logs` for solar-miner, `/logs sniffer` for modbus-sniffer, `/logs all` for both |
| `/deploy` | Deploy updated files to the Pi and restart the solar-miner service |
| `/miners` | Check all miners — power consumption, hashrate, temperature, pool and tuner status |
| `/status` | Full system check — both services, battery state, and all miner details |

## Dependencies

Zero npm dependencies. Uses Node.js built-in `fs` and `fetch` (Node 20+).

## Hardware

- **Inverter**: EG4 3000EHV-48
- **Batteries**: 2x EG4 LifePower4 (V2 firmware)
- **Miners**: Antminer S9 with Braiins OS+
- **Controller**: Raspberry Pi Zero W (Raspbian Trixie)
- **RS485 Adapter**: EG4 USB Read/Write Cable (USB-to-RS485)
