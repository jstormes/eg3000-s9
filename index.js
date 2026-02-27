const config = require('./config');
const { readSOC, connect, disconnect } = require('./inverter');
const { createMiners } = require('./miner');

const STATES = { IDLE: 'IDLE', MINING: 'MINING' };
let state = STATES.IDLE;
let running = true;
let consecutiveErrors = 0;
const MAX_CONSECUTIVE_ERRORS = 3;
const currentHistory = [];

function recordCurrent(amps) {
  currentHistory.push(amps);
  while (currentHistory.length > config.thresholds.currentWindowSize) {
    currentHistory.shift();
  }
}

function isChargingSustained(soc) {
  // Batteries full — skip charge check, solar has nowhere to go
  if (soc >= config.thresholds.batteriesFullSOC) return true;
  if (currentHistory.length < config.thresholds.currentWindowSize) return false;
  const avg = currentHistory.reduce((sum, v) => sum + v, 0) / currentHistory.length;
  return avg >= config.thresholds.minChargeCurrentA;
}

if (!config.miners.password) {
  console.error('[control] MINER_PASSWORD environment variable is required');
  process.exit(1);
}

const miners = createMiners(config.miners.hosts, config.miners.password);

async function areMiningActively() {
  const results = await Promise.allSettled(
    miners.map(async (m) => {
      const data = await m.graphql(
        '{bosminer{info{summary{power{approxConsumptionW}}}}}'
      );
      return data?.data?.bosminer?.info?.summary?.power?.approxConsumptionW || 0;
    })
  );
  const totalWatts = results
    .filter((r) => r.status === 'fulfilled')
    .reduce((sum, r) => sum + r.value, 0);
  return totalWatts > 50;
}

async function startAllMiners() {
  const target = Math.min(
    config.miners.powerTargetW,
    config.miners.maxPowerTargetW
  );
  const results = await Promise.allSettled(
    miners.map(async (m) => {
      await m.startMining();
      await m.setPowerTarget(target);
    })
  );
  const failed = results.filter((r) => r.status === 'rejected');
  if (failed.length > 0) {
    for (const f of failed) {
      console.error(`[control] Failed to start miner: ${f.reason.message}`);
    }
  }
}

async function stopAllMiners() {
  for (let attempt = 1; attempt <= 3; attempt++) {
    const results = await Promise.allSettled(
      miners.map((m) => m.stopMining())
    );
    const failed = results.filter((r) => r.status === 'rejected');
    if (failed.length === 0) return;
    for (const f of failed) {
      console.error(
        `[control] Failed to stop miner (attempt ${attempt}/3): ${f.reason.message}`
      );
    }
    if (attempt < 3) {
      await new Promise((resolve) => setTimeout(resolve, 5000));
    }
  }
  console.error('[control] WARNING: some miners may still be running after 3 stop attempts');
}

async function controlLoop() {
  console.log(`[control] Solar Miner Controller starting`);
  console.log(
    `[control] Thresholds: start >= ${config.thresholds.startMiningSOC}%, stop < ${config.thresholds.stopMiningSOC}%`
  );
  console.log(
    `[control] Miners: ${config.miners.hosts.join(', ')}`
  );
  console.log(
    `[control] Power target: ${config.miners.powerTargetW}W per miner (max ${config.miners.maxPowerTargetW}W)`
  );
  console.log(
    `[control] Poll interval: ${config.pollIntervalMs / 1000}s`
  );
  console.log(
    `[control] Charge check: avg current >= ${config.thresholds.minChargeCurrentA}A over ${config.thresholds.currentWindowSize} readings (skip at SOC >= ${config.thresholds.batteriesFullSOC}%)`
  );

  while (running) {
    try {
      const { soc, batteryVoltage, batteryCurrent, temperature, pvPower } = await readSOC();
      consecutiveErrors = 0;

      if (batteryCurrent !== null) {
        recordCurrent(batteryCurrent);
      }

      // Check actual miner state — don't trust internal state alone
      let minersRunning;
      try {
        minersRunning = await areMiningActively();
      } catch {
        minersRunning = state === STATES.MINING;
      }

      // Sync internal state with reality
      if (minersRunning && state === STATES.IDLE) {
        console.log('[control] Detected miners running while state was IDLE — correcting state');
        state = STATES.MINING;
      } else if (!minersRunning && state === STATES.MINING) {
        console.log('[control] Detected miners stopped while state was MINING — correcting state');
        state = STATES.IDLE;
      }

      const extras = [];
      if (batteryVoltage !== null) extras.push(`${batteryVoltage}V`);
      if (batteryCurrent !== null) extras.push(`${batteryCurrent}A`);
      if (temperature !== null) extras.push(`${temperature}°C`);
      if (pvPower !== null) extras.push(`PV: ${pvPower}W`);
      const extraStr = extras.length > 0 ? ` | ${extras.join(' | ')}` : '';

      console.log(
        `[${new Date().toISOString()}] SOC: ${soc}% | State: ${state} | Miners: ${miners.length}${extraStr}`
      );

      if (state === STATES.IDLE && soc >= config.thresholds.startMiningSOC) {
        if (isChargingSustained(soc)) {
          const reason = soc >= config.thresholds.batteriesFullSOC
            ? `batteries full (SOC ${soc}% >= ${config.thresholds.batteriesFullSOC}%)`
            : `avg current ${(currentHistory.reduce((s, v) => s + v, 0) / currentHistory.length).toFixed(1)}A >= ${config.thresholds.minChargeCurrentA}A`;
          console.log(
            `[control] SOC ${soc}% >= ${config.thresholds.startMiningSOC}% & ${reason} — starting miners`
          );
          await startAllMiners();
          state = STATES.MINING;
        } else {
          const avg = currentHistory.length > 0
            ? (currentHistory.reduce((s, v) => s + v, 0) / currentHistory.length).toFixed(1)
            : 'n/a';
          console.log(
            `[control] SOC ${soc}% >= ${config.thresholds.startMiningSOC}% but waiting for sustained charge (avg ${avg}A, need >= ${config.thresholds.minChargeCurrentA}A over ${config.thresholds.currentWindowSize} readings, have ${currentHistory.length})`
          );
        }
      } else if (
        state === STATES.MINING &&
        soc < config.thresholds.stopMiningSOC
      ) {
        console.log(
          `[control] SOC ${soc}% < ${config.thresholds.stopMiningSOC}% — stopping miners`
        );
        await stopAllMiners();
        state = STATES.IDLE;
      }
    } catch (err) {
      consecutiveErrors++;
      console.error(
        `[control] Error (${consecutiveErrors}/${MAX_CONSECUTIVE_ERRORS}): ${err.message}`
      );

      // Fail safe: if we can't read battery data, stop miners to protect batteries
      if (consecutiveErrors >= MAX_CONSECUTIVE_ERRORS) {
        console.error(
          `[control] FAIL-SAFE: ${consecutiveErrors} consecutive errors — stopping miners to protect batteries`
        );
        await stopAllMiners();
        state = STATES.IDLE;
      }
    }

    // Sleep until next poll
    await new Promise((resolve) => setTimeout(resolve, config.pollIntervalMs));
  }
}

async function shutdown() {
  console.log('\n[control] Shutting down...');
  running = false;
  if (state === STATES.MINING) {
    console.log('[control] Stopping all miners before exit');
    await stopAllMiners();
  }
  await disconnect();
  console.log('[control] Shutdown complete');
  process.exit(0);
}

process.on('SIGINT', shutdown);
process.on('SIGTERM', shutdown);

controlLoop().catch((err) => {
  console.error(`[control] Fatal error: ${err.message}`);
  process.exit(1);
});
