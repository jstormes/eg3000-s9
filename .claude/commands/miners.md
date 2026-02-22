Query each miner's status via the Braiins OS GraphQL API from the Pi.

Run (sources password from the systemd service environment):
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
      console.log(m.host + \": \" + info?.modelName + \" | \" + Math.round(s?.power?.approxConsumptionW) + \"W/\" + s?.power?.limitW + \"W | \" + (s?.realHashrate?.mhs15M / 1e6).toFixed(2) + \" TH/s | \" + s?.temperature?.degreesC + \"C | pool: \" + s?.poolStatus + \" | tuner: \" + s?.tunerStatus);
    } catch (e) {
      console.error(m.host + \": UNREACHABLE - \" + e.message);
    }
  }
})();
"'
```

Report each miner's power consumption, power limit, hashrate, temperature, pool status, and tuner status. Flag any miners that are unreachable.
