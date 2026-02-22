Run a full system status check on the Pi. Execute these commands and summarize the results:

1. Check both services are running:
```bash
ssh jstormes@eg3000 'systemctl is-active solar-miner.service modbus-sniffer.service'
```

2. Get latest battery data:
```bash
ssh jstormes@eg3000 'journalctl -u solar-miner.service --no-pager -n 3'
```

3. Check miner status (sources password from the systemd service environment):
```bash
ssh jstormes@eg3000 'cd ~/solar1 && eval $(systemctl show solar-miner.service -p Environment --value | tr " " "\n" | grep MINER_PASSWORD) && export MINER_PASSWORD && node -e "
const config = require(\"./config\");
const { createMiners } = require(\"./miner\");
const miners = createMiners(config.miners.hosts, config.miners.password);
(async () => {
  for (const m of miners) {
    try {
      const data = await m.graphql(\"{bosminer{info{modelName summary{realHashrate{mhs15M} poolStatus power{approxConsumptionW limitW} tunerStatus temperature{degreesC}}}}}\");
      const info = data?.data?.bosminer?.info;
      const s = info?.summary;
      console.log(m.host + \": \" + Math.round(s?.power?.approxConsumptionW) + \"W/\" + s?.power?.limitW + \"W | \" + (s?.realHashrate?.mhs15M / 1e6).toFixed(2) + \" TH/s | \" + s?.temperature?.degreesC + \"C | pool: \" + s?.poolStatus);
    } catch (e) {
      console.error(m.host + \": UNREACHABLE - \" + e.message);
    }
  }
})();
"'
```

Provide a concise summary with:
- Service status (both services)
- Battery: SOC, charging/discharging, voltage, current, temperature
- Each miner: power, hashrate, temperature, pool status
- Any warnings or issues
