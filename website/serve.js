#!/usr/bin/env node
"use strict";

/**
 * Static file server for btcpc.net — serves the website/ directory on port 4243.
 * Supports extensionless URLs (/install → install.html, /clock → clock.html).
 */

const express = require("express");
const path = require("path");
const http = require("http");

const app = express();
const PORT = process.env.PORT || 4243;
const ROOT = __dirname;
const DOCS_ROOT = path.resolve(__dirname, "..", "docs");
const WIKI_ROOT = path.resolve(__dirname, "..", ".code-review-graph", "wiki");
const API_PORT = process.env.BTCPC_API_PORT || 3000;

// Permissions-Policy header for PWA sensor access on Android Chrome
app.use((req, res, next) => {
  res.setHeader(
    "Permissions-Policy",
    "accelerometer=*, gyroscope=*, magnetometer=*, ambient-light-sensor=*"
  );
  next();
});

// Proxy /api/* to the BTCPC API for sensor nodes and bot endpoints
app.use("/api", (req, res) => {
  const options = {
    hostname: "127.0.0.1",
    port: API_PORT,
    path: "/api" + req.url,
    method: req.method,
    headers: req.headers,
  };
  const proxyReq = http.request(options, (proxyRes) => {
    res.writeHead(proxyRes.statusCode, proxyRes.headers);
    proxyRes.pipe(res);
  });
  proxyReq.on("error", (err) => {
    res.status(502).json({ error: "API unreachable: " + err.message });
  });
  if (req.method === "POST" || req.method === "PUT") {
    let body = "";
    req.on("data", (chunk) => (body += chunk));
    req.on("end", () => proxyReq.end(body));
  } else {
    proxyReq.end();
  }
});

// Proxy /v1/* to the BTCPC inference API (pull-model, models, etc.)
app.use("/v1", (req, res) => {
  const options = {
    hostname: "127.0.0.1",
    port: API_PORT,
    path: "/v1" + req.url,
    method: req.method,
    headers: req.headers,
  };
  const proxyReq = http.request(options, (proxyRes) => {
    res.writeHead(proxyRes.statusCode, proxyRes.headers);
    proxyRes.pipe(res);
  });
  proxyReq.on("error", (err) => {
    if (!res.headersSent) res.status(502).json({ error: "API unreachable: " + err.message });
  });
  // Stream the request body through (needed for POST /v1/node/pull-model)
  req.pipe(proxyReq);
});

// Proxy /node/* to the BTCPC API (epoch info, node list, etc.)
app.use("/node", (req, res) => {
  const options = {
    hostname: "127.0.0.1",
    port: API_PORT,
    path: "/api/node" + req.url,
    method: req.method,
    headers: req.headers,
  };
  const proxyReq = http.request(options, (proxyRes) => {
    res.writeHead(proxyRes.statusCode, proxyRes.headers);
    proxyRes.pipe(res);
  });
  proxyReq.on("error", (err) => {
    if (!res.headersSent) res.status(502).json({ error: "API unreachable: " + err.message });
  });
  req.pipe(proxyReq);
});

// Proxy /public/* to the BTCPC API for browser clock nodes
app.use("/public", (req, res) => {
  const options = {
    hostname: "127.0.0.1",
    port: API_PORT,
    path: "/public" + req.url,
    method: req.method,
    headers: req.headers,
  };
  const proxyReq = http.request(options, (proxyRes) => {
    res.writeHead(proxyRes.statusCode, proxyRes.headers);
    proxyRes.pipe(res);
  });
  proxyReq.on("error", (err) => {
    res.status(502).json({ error: "API unreachable: " + err.message });
  });
  if (req.method === "POST" || req.method === "PUT") {
    let body = "";
    req.on("data", (chunk) => (body += chunk));
    req.on("end", () => proxyReq.end(body));
  } else {
    proxyReq.end();
  }
});

app.get("/docs", (_req, res) => {
  res.redirect("/docs/");
});

function sendDocsFile(res, root, relPath) {
  const normalized = path.normalize(relPath).replace(/^([/\\])+/, "");
  const abs = path.join(root, normalized);
  if (!abs.startsWith(root)) return false;
  if (!require("fs").existsSync(abs)) return false;
  if (!require("fs").statSync(abs).isFile()) return false;
  res.sendFile(abs);
  return true;
}

app.get(/^\/docs\/?$/, (req, res, next) => {
  if (sendDocsFile(res, DOCS_ROOT, "index.html")) return;
  next();
});

app.get(/^\/docs\/(.*)$/, (req, res, next) => {
  const rel = req.path.replace(/^\/docs\/?/, "");
  if (!rel) {
    if (sendDocsFile(res, DOCS_ROOT, "index.html")) return;
    return next();
  }
  if (rel.startsWith("code-wiki/")) {
    const wikiRel = rel.slice("code-wiki/".length);
    if (wikiRel === "README.md" && sendDocsFile(res, DOCS_ROOT, "code-wiki/README.md")) return;
    if (wikiRel === "index.md" && sendDocsFile(res, DOCS_ROOT, "code-wiki/index.md")) return;
    if (wikiRel && sendDocsFile(res, WIKI_ROOT, wikiRel)) return;
    if (wikiRel === "" && sendDocsFile(res, WIKI_ROOT, "index.md")) return;
  }
  if (sendDocsFile(res, DOCS_ROOT, rel)) return;
  next();
});

app.get("/obsidian", (_req, res) => {
  res.redirect("/tree/");
});

app.get("/gource", (_req, res) => {
  res.redirect("/tree/");
});

app.get(/^\/tree\/?$/, (_req, res) => {
  res.sendFile(path.join(ROOT, "gource.html"));
});

app.use("/tree", express.static(path.join(ROOT, "public"), {
  extensions: ["html", "json"],
  etag: true,
  lastModified: true,
}));

// Extensionless URLs: /install → /install.html, /clock → /clock.html, etc.
app.use((req, res, next) => {
  if (req.path === "/" || req.path.includes(".") || req.path.startsWith("/docs")) return next();
  const htmlPath = path.join(ROOT, req.path + ".html");
  require("fs").stat(htmlPath, (err, stats) => {
    if (!err && stats.isFile()) {
      res.sendFile(htmlPath);
    } else {
      next();
    }
  });
});

// Serve static files from website/
app.use(express.static(ROOT, {
  extensions: ["html"],
  etag: true,
  lastModified: true,
}));

// Fallback: 404
app.use((req, res) => {
  res.status(404).sendFile(path.join(ROOT, "index.html"));
});

app.listen(PORT, "127.0.0.1", () => {
  console.log(`[website] serving ${ROOT} on http://127.0.0.1:${PORT}`);
});
