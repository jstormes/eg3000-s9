View recent logs from a service on the Pi. Default is solar-miner.

If $ARGUMENTS is empty or "solar" or "miner", run:
```bash
ssh jstormes@eg3000 'journalctl -u solar-miner.service --no-pager -n 20'
```

If $ARGUMENTS is "sniffer" or "modbus", run:
```bash
ssh jstormes@eg3000 'journalctl -u modbus-sniffer.service --no-pager -n 20'
```

If $ARGUMENTS is "all", run both commands.

Show the output and summarize any errors or notable events.
