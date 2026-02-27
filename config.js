const config = {
  batteryJson: process.env.BATTERY_JSON || '/tmp/battery_data.json',
  miners: {
    hosts: (process.env.MINER_HOSTS || 'miner1')
      .split(',')
      .map((h) => h.trim())
      .filter(Boolean),
    password: process.env.MINER_PASSWORD || '',
    powerTargetW: parseInt(process.env.MINER_POWER_TARGET_W, 10) || 700,
    maxPowerTargetW: parseInt(process.env.MINER_MAX_POWER_TARGET_W, 10) || 1200,
  },
  thresholds: {
    startMiningSOC: parseInt(process.env.START_MINING_SOC, 10) || 95,
    stopMiningSOC: parseInt(process.env.STOP_MINING_SOC, 10) || 92,
    minChargeCurrentA: parseFloat(process.env.MIN_CHARGE_CURRENT_A) || 2.0,
    currentWindowSize: parseInt(process.env.CURRENT_WINDOW_SIZE, 10) || 6,
  },
  pollIntervalMs: parseInt(process.env.POLL_INTERVAL_MS, 10) || 30000,
};

module.exports = config;
