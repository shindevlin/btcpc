"use strict";

const http = require("http");
const express = require("express");

const mockRecordNodeActivity = jest.fn();
const mockBroadcast = jest.fn();
const mockCreateMessage = jest.fn((type, data, nodeId) => ({ type, data, nodeId }));
const mockGetConnectedCount = jest.fn(() => 1);
const mockGetTruthStatus = jest.fn(() => ({ observer: false, external_peer_count: 1, truth_bearing: true }));
const mockGetAllAccounts = jest.fn(() => [{ username: "btcpctest-a" }]);
const mockGetAccount = jest.fn((username) => ({
  username,
  node_types: ["btcpctest"],
  heartbeat_epoch: 123,
  last_announced_epoch: 123,
  last_registered_epoch: 123,
  p2p_address: "ws://10.0.0.1:6942",
}));
const mockGetChainHeight = jest.fn(() => 123);
const mockGetNetworkPolicy = jest.fn(() => ({
  btcpctestDeveloperEnabled: true,
  btcpctestDeveloperAllowlist: ["alice", "bob"],
}));

jest.mock("express-rate-limit", () => () => (req, res, next) => next());

jest.spyOn(global, "setInterval").mockImplementation(() => 1);
jest.spyOn(global, "clearInterval").mockImplementation(() => {});

jest.mock("../src/chain/stateStore", () => ({
  getAccount: (...args) => mockGetAccount(...args),
  getAllAccounts: () => mockGetAllAccounts(),
  getChainHeight: () => mockGetChainHeight(),
  getNetworkPolicy: () => mockGetNetworkPolicy(),
}));

jest.mock("../src/p2p/protocol", () => ({
  recordNodeActivity: (...args) => mockRecordNodeActivity(...args),
  createMessage: (...args) => mockCreateMessage(...args),
}));

jest.mock("../src/p2p/network", () => ({
  NODE_ID: "browser-relay",
  broadcast: (...args) => mockBroadcast(...args),
  getConnectedCount: () => mockGetConnectedCount(),
}));

jest.mock("../src/chain/clockConsensus", () => ({
  getTruthStatus: (...args) => mockGetTruthStatus(...args),
}));

jest.mock("jsonwebtoken", () => ({
  verify: jest.fn(),
}));

const jwt = require("jsonwebtoken");
const publicRoutes = require("../src/routes/publicRoutes");

function makeTestServer() {
  const app = express();
  app.use(express.json());
  app.set("trust proxy", true);
  app.use("/public", publicRoutes);
  const server = http.createServer(app);
  return new Promise((resolve) => {
    server.listen(0, "127.0.0.1", () => {
      resolve({ server, port: server.address().port });
    });
  });
}

function closeServer(server) {
  return new Promise((resolve) => server.close(resolve));
}

function request(port, method, path, body, headers) {
  return new Promise((resolve, reject) => {
    const reqHeaders = Object.assign({ "Content-Type": "application/json" }, headers || {});
    const data = body ? JSON.stringify(body) : null;
    if (data) reqHeaders["Content-Length"] = Buffer.byteLength(data);
    const req = http.request(
      { host: "127.0.0.1", port, method, path, headers: reqHeaders },
      (res) => {
        let raw = "";
        res.on("data", (chunk) => (raw += chunk));
        res.on("end", () => {
          let parsed = null;
          try { parsed = raw ? JSON.parse(raw) : null; } catch (_) { parsed = raw; }
          resolve({ status: res.statusCode, body: parsed });
        });
      }
    );
    req.on("error", reject);
    if (data) req.write(data);
    req.end();
  });
}

describe("machine-status — reduced information disclosure", () => {
  let testServer;
  let port;

  beforeAll(async () => {
    const s = await makeTestServer();
    testServer = s.server;
    port = s.port;
  });

  afterAll(async () => {
    if (testServer) await closeServer(testServer);
  });

  test("returns 200 with expected safe fields", async () => {
    const res = await request(port, "GET", "/public/machine-status");
    expect(res.status).toBe(200);
    expect(typeof res.body.uptime_sec).toBe("number");
    expect(typeof res.body.load_avg).toBe("number");
    expect(typeof res.body.mem_free_mb).toBe("number");
    expect(typeof res.body.mem_total_mb).toBe("number");
    expect(typeof res.body.chain_height).toBe("number");
    expect(typeof res.body.timestamp).toBe("number");
    expect(typeof res.body.ollama).toBe("object");
    expect(typeof res.body.ollama.running).toBe("boolean");
    expect(typeof res.body.truth_bearing).toBe("boolean");
    expect(typeof res.body.connected_peers).toBe("number");
    expect(typeof res.body.external_peers).toBe("number");
  });

  test("does not expose hostname", async () => {
    const res = await request(port, "GET", "/public/machine-status");
    expect(res.body).not.toHaveProperty("hostname");
  });

  test("does not expose peer addresses or node IDs", async () => {
    const res = await request(port, "GET", "/public/machine-status");
    expect(res.body).not.toHaveProperty("peers");
  });

  test("does not expose Ollama model inventory", async () => {
    const res = await request(port, "GET", "/public/machine-status");
    expect(res.body.ollama).not.toHaveProperty("models");
  });

  test("process entries do not contain PIDs or RSS", async () => {
    const res = await request(port, "GET", "/public/machine-status");
    const processes = res.body.processes || [];
    for (const proc of processes) {
      expect(proc).not.toHaveProperty("pid");
      expect(proc).not.toHaveProperty("rss_mb");
      expect(typeof proc.role).toBe("string");
    }
  });
});

describe("public clock heartbeat security", () => {
  let testServer;
  let port;

  beforeAll(async () => {
    const s = await makeTestServer();
    testServer = s.server;
    port = s.port;
  });

  afterAll(async () => {
    if (testServer) await closeServer(testServer);
    jest.restoreAllMocks();
  });

  beforeEach(() => {
    jest.clearAllMocks();
  });

  test("does not credit or broadcast when JWT is missing", async () => {
    jwt.verify.mockImplementation(() => {
      throw new Error("missing token");
    });

    const res = await request(port, "POST", "/public/clock-heartbeat", {
      account: "alice",
      client_id: "0123456789abcdef0123456789abcdef",
    }, { "X-Forwarded-For": "203.0.113.10" });

    expect(res.status).toBe(200);
    expect(res.body.verified).toBe(false);
    expect(res.body.credited).toBe(false);
    expect(mockRecordNodeActivity).not.toHaveBeenCalled();
    expect(mockBroadcast).not.toHaveBeenCalled();
  });

  test("credits and broadcasts only when JWT matches the account", async () => {
    jwt.verify.mockReturnValue({ username: "alice" });

    const res = await request(
      port,
      "POST",
      "/public/clock-heartbeat",
      {
        account: "alice",
        client_id: "0123456789abcdef0123456789abcdef",
      },
      { Authorization: "Bearer good-token", "X-Forwarded-For": "203.0.113.11" }
    );

    expect(res.status).toBe(200);
    expect(res.body.verified).toBe(true);
    expect(res.body.credited).toBe(true);
    expect(mockRecordNodeActivity).toHaveBeenCalledWith(
      "0123456789abcdef0123456789abcdef",
      "alice",
      expect.any(Number)
    );
    expect(mockCreateMessage).toHaveBeenCalledWith(
      "CLOCK_HEARTBEAT",
      expect.objectContaining({ account: "alice", source: "browser" }),
      "browser-relay"
    );
    expect(mockBroadcast).toHaveBeenCalled();
  });
});

describe("public testnet rewards", () => {
  let testServer;
  let port;

  beforeAll(async () => {
    const s = await makeTestServer();
    testServer = s.server;
    port = s.port;
  });

  afterAll(async () => {
    if (testServer) await closeServer(testServer);
  });

  test("exposes a separate BTCPCTEST reward summary", async () => {
    const res = await request(port, "GET", "/public/testnet/rewards");
    expect(res.status).toBe(200);
    expect(res.body.network).toBe("btcpctest");
    expect(res.body.native_token).toBe("BTCPCTEST");
    expect(res.body.bonus_token).toBe("BTCPC");
    expect(typeof res.body.summary).toBe("object");
    expect(res.body.summary.btcpctest_nodes).toBeGreaterThanOrEqual(0);
    expect(res.body.summary.work_mode).toBe("report_only");
    expect(res.body.summary.developer_access_required).toBe(true);
  });
});

describe("public testnet access policy", () => {
  let testServer;
  let port;

  beforeAll(async () => {
    const s = await makeTestServer();
    testServer = s.server;
    port = s.port;
  });

  afterAll(async () => {
    if (testServer) await closeServer(testServer);
  });

  test("exposes allowlist status without usernames", async () => {
    const res = await request(port, "GET", "/public/testnet/access");
    expect(res.status).toBe(200);
    expect(res.body.network).toBe("btcpctest");
    expect(res.body.developer_access_enabled).toBe(true);
    expect(res.body.developer_access_source).toBe("policy");
    expect(res.body.developer_access_allowlist_count).toBe(2);
    expect(res.body.developer_access_allow_all).toBe(false);
    expect(res.body.developer_access_username_scoped).toBe(true);
    expect(res.body).not.toHaveProperty("allowlist");
    expect(res.body).not.toHaveProperty("usernames");
  });
});
