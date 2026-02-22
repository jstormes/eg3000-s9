class BraiinsMiner {
  constructor(host, password) {
    this.host = host;
    this.password = password;
    this.sessionId = null;
    this.sessionExpiry = 0;
    this.baseUrl = `http://${host}`;
  }

  async login() {
    const query = `mutation{auth{login(username:"root",password:${JSON.stringify(this.password)}){...on VoidResult{void}...on AuthError{message}}}}`;
    const res = await fetch(`${this.baseUrl}/graphql`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ query }),
    });
    if (!res.ok) {
      throw new Error(`[miner ${this.host}] Login HTTP error: ${res.status}`);
    }
    const data = await res.json();
    const result = data?.data?.auth?.login;
    if (result?.message) {
      throw new Error(`[miner ${this.host}] Login failed: ${result.message}`);
    }

    const cookie = res.headers.get('set-cookie') || '';
    const match = cookie.match(/session_id=([^;]+)/);
    if (!match) {
      throw new Error(`[miner ${this.host}] No session cookie in login response`);
    }
    this.sessionId = match[1];
    // Session lasts 1 hour; refresh after 50 minutes
    this.sessionExpiry = Date.now() + 50 * 60 * 1000;
  }

  async ensureAuth() {
    if (!this.sessionId || Date.now() >= this.sessionExpiry) {
      await this.login();
    }
  }

  async graphql(query) {
    await this.ensureAuth();
    const res = await fetch(`${this.baseUrl}/graphql`, {
      method: 'POST',
      headers: {
        'Content-Type': 'application/json',
        Cookie: `session_id=${this.sessionId}`,
      },
      body: JSON.stringify({ query }),
    });

    if (res.status === 401) {
      await this.login();
      const retry = await fetch(`${this.baseUrl}/graphql`, {
        method: 'POST',
        headers: {
          'Content-Type': 'application/json',
          Cookie: `session_id=${this.sessionId}`,
        },
        body: JSON.stringify({ query }),
      });
      if (!retry.ok) {
        throw new Error(`[miner ${this.host}] GraphQL failed: ${retry.status}`);
      }
      return retry.json();
    }

    if (!res.ok) {
      throw new Error(`[miner ${this.host}] GraphQL failed: ${res.status}`);
    }
    return res.json();
  }

  async startMining() {
    const data = await this.graphql(
      'mutation{bosminer{start{...on VoidResult{void}...on BosminerError{message}}}}'
    );
    const err = data?.data?.bosminer?.start?.message;
    if (err) throw new Error(`[miner ${this.host}] Start failed: ${err}`);
    console.log(`[miner ${this.host}] Mining started`);
  }

  async stopMining() {
    const data = await this.graphql(
      'mutation{bosminer{stop{...on VoidResult{void}...on BosminerError{message}}}}'
    );
    const err = data?.data?.bosminer?.stop?.message;
    if (err) throw new Error(`[miner ${this.host}] Stop failed: ${err}`);
    console.log(`[miner ${this.host}] Mining stopped`);
  }

  async setPowerTarget(watts) {
    const data = await this.graphql(
      `mutation{bosminer{config{updateAutotuning(input:{mode:POWER_TARGET,powerTarget:${Math.round(watts)}},apply:true){...on AutotuningOut{autotuning{mode powerTarget}}...on AutotuningError{message powerTarget}...on AttributeError{message}}}}}`
    );
    const result = data?.data?.bosminer?.config?.updateAutotuning;
    if (result?.message) {
      throw new Error(`[miner ${this.host}] setPowerTarget failed: ${result.message}`);
    }
    console.log(`[miner ${this.host}] Power target set to ${watts}W`);
  }

  async getSummary() {
    const data = await this.graphql(
      '{bosminer{info{modelName summary{realHashrate{mhs15M} poolStatus power{approxConsumptionW limitW} tunerStatus temperature{degreesC}}}}}'
    );
    return data?.data?.bosminer?.info;
  }
}

function createMiners(hosts, password) {
  return hosts.map((host) => new BraiinsMiner(host, password));
}

module.exports = { BraiinsMiner, createMiners };
