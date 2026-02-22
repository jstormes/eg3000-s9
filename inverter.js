const fs = require('fs');
const config = require('./config');

let connected = false;

const STALE_MS = 60000;

async function connect() {
  connected = true;
  console.log(
    `[inverter] Reading battery data from ${config.batteryJson}`
  );
}

async function disconnect() {
  connected = false;
}

async function readSOC() {
  if (!connected) await connect();

  let data;
  try {
    const raw = fs.readFileSync(config.batteryJson, 'utf8');
    data = JSON.parse(raw);
  } catch (err) {
    throw new Error(`[inverter] Cannot read ${config.batteryJson}: ${err.message}`);
  }

  if (!data.batteries || Object.keys(data.batteries).length === 0) {
    throw new Error('[inverter] No battery data in JSON file');
  }

  /* Pick the first battery entry (slave ID 1 in typical setups) */
  const key = Object.keys(data.batteries)[0];
  const bat = data.batteries[key];

  /* Check staleness using the battery's own timestamp */
  const batTime = new Date(bat.timestamp).getTime();
  const age = Date.now() - batTime;
  if (age > STALE_MS) {
    throw new Error(
      `[inverter] Data is stale (last update ${Math.round(age / 1000)}s ago)`
    );
  }

  return {
    soc: bat.soc_pct,
    batteryVoltage: bat.voltage_v,
    batteryCurrent: bat.current_a,
    temperature: bat.temperature_c,
    pvPower: null,
  };
}

module.exports = { readSOC, connect, disconnect };
