Check the current battery SOC by reading the latest solar-miner service logs from the Pi.

Run:
```bash
ssh jstormes@eg3000 'journalctl -u solar-miner.service --no-pager -n 3'
```

Report the SOC percentage, whether the battery is charging (positive current) or discharging (negative current), the voltage, current in amps, and temperature. Keep the response brief.
