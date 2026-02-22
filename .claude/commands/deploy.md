Deploy the solar miner controller to the Raspberry Pi and restart the service.

Steps:

1. Copy the application files to the Pi:
```bash
scp index.js inverter.js miner.js config.js package.json jstormes@eg3000:~/solar1/
```

2. If solar-miner.service was modified, also copy and install it:
```bash
scp solar-miner.service jstormes@eg3000:~/solar1/
ssh jstormes@eg3000 'sudo cp ~/solar1/solar-miner.service /etc/systemd/system/ && sudo systemctl daemon-reload'
```

3. Restart the service:
```bash
ssh jstormes@eg3000 'sudo systemctl restart solar-miner.service'
```

4. Wait a few seconds and verify it started:
```bash
ssh jstormes@eg3000 'sleep 3 && journalctl -u solar-miner.service --no-pager -n 10'
```

Report success or failure. Show the startup logs.
