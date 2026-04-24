"use strict";
const jwt = require('jsonwebtoken');
const secp256k1 = require('secp256k1');

// D.5-delta: secretStore-first user lookup, Mongo fallback.
// Same lazy-load pattern as authController (D.5-gamma).
let _secretStoreLoaded = false;
async function getSecretStore() {
  const secretStore = require('../services/secretStore');
  if (!_secretStoreLoaded) {
    try {
      await secretStore.load();
      _secretStoreLoaded = true;
    } catch (err) {
      console.warn('[auth-mw] secretStore load failed: ' + err.message);
    }
  }
  return secretStore;
}

/**
 * Derive compressed secp256k1 public key from a 64-char hex private key.
 * Returns null on any error.
 */
function pubKeyFromPostingKey(hexKey) {
  try {
    const privBytes = Buffer.from(hexKey, 'hex');
    if (privBytes.length !== 32) return null;
    return Buffer.from(secp256k1.publicKeyCreate(privBytes, true)).toString('hex');
  } catch (_) {
    return null;
  }
}

/**
 * Posting-key authentication — accepts Authorization: Bearer account:hex64key.
 * The server derives the public key from the provided private key and compares
 * it to the stored posting_public_key. No JWT needed on the client.
 */
async function authenticatePostingKey(req, res, next) {
  const authHeader = req.headers['authorization'];
  const token = authHeader && authHeader.split(' ')[1];
  if (!token || !token.includes(':')) return res.status(401).json({ error: 'Access denied.' });

  const colon = token.indexOf(':');
  const account = token.substring(0, colon).trim().toLowerCase();
  const hexKey  = token.substring(colon + 1).trim();

  if (!account || hexKey.length !== 64) return res.status(401).json({ error: 'Invalid posting key format.' });

  const derivedPub = pubKeyFromPostingKey(hexKey);
  if (!derivedPub) return res.status(401).json({ error: 'Invalid posting key.' });

  try {
    const ss = await getSecretStore();
    const user = ss && typeof ss.getUser === 'function' ? ss.getUser(account) : null;
    if (user && user.posting_public_key && user.posting_public_key === derivedPub) {
      req.user = { id: user.user_id, username: account, email: user.email, is_active: true };
      return next();
    }
  } catch (_) {}

  return res.status(401).json({ error: 'Posting key does not match.' });
}

/**
 * Authentication Middleware — D.5-delta.
 *
 * Accepts either a JWT Bearer token OR a posting-key token (account:hex64key).
 * Posting key format is tried first when the token contains a colon and is 67+
 * characters long (account + ":" + 64-char key). Falls back to JWT otherwise.
 */
async function authenticateToken(req, res, next) {
  const authHeader = req.headers['authorization'];
  const token = authHeader && authHeader.split(' ')[1]; // Bearer TOKEN

  if (!token) return res.status(401).json({ error: 'Access denied. No token provided.' });

  // If token looks like account:hexkey, try posting key auth first
  const colon = token.indexOf(':');
  if (colon > 0 && token.length >= 67) {
    const hexKey = token.substring(colon + 1).trim();
    if (hexKey.length === 64 && /^[0-9a-f]+$/i.test(hexKey)) {
      return authenticatePostingKey(req, res, next);
    }
  }

  let decoded;
  try {
    decoded = jwt.verify(token, process.env.JWT_SECRET || process.env.BTCPC_JWT_SECRET);
  } catch (err) {
    return res.status(403).json({ error: 'Invalid token.' });
  }

  // ── Try secretStore first ──
  try {
    const ss = await getSecretStore();
    if (ss && typeof ss.getUser === 'function') {
      let ssRec = null;
      if (decoded.username) {
        ssRec = ss.getUser(decoded.username);
      }
      if (!ssRec && decoded.id) {
        ssRec = (typeof ss.getUserById === 'function') ? ss.getUserById(decoded.id) : null;
      }
      if (ssRec) {
        // Normalise to the shape downstream code expects.
        // secretStore uses user_id; Mongo uses _id — both become id.
        // Legacy records may lack a username field — patch in-place so the
        // record is correct for future lookups without requiring a restart.
        if (!ssRec.username && decoded.username) {
          ssRec.username = decoded.username;
        }
        req.user = {
          id: ssRec.user_id || decoded.id,
          username: ssRec.username || decoded.username,
          email: ssRec.email,
          is_active: ssRec.is_active !== false,
          two_factor_enabled: !!ssRec.two_factor_enabled,
          totp_enabled: !!ssRec.totp_enabled,
          // Forward any extra JWT claims (src, iat, exp, etc.)
          ...decoded,
          // Override with secretStore data to stay canonical
          id: ssRec.user_id || decoded.id,
          username: ssRec.username || decoded.username,
        };
        return next();
      }
    }
  } catch (_) {
    // secretStore failed — fall through to Mongo
  }

  // ── Mongo fallback ──
  try {
    const User = require('../models/User');
    const mongoUser = await User.findById(decoded.id);
    if (mongoUser) {
      req.user = {
        ...decoded,
        // Normalise Mongo _id → id
        id: mongoUser._id.toString(),
        username: mongoUser.username,
        email: mongoUser.email,
      };
      return next();
    }
  } catch (_) {
    // Mongo unavailable — cannot verify user exists
  }

  // If neither secretStore nor Mongo could verify the user exists,
  // reject the request. Never proceed with unverified JWT claims.
  return res.status(401).json({ error: 'authentication service unavailable' });
}

/**
 * Rate Limiting Middleware
 */
const rateLimit = require('express-rate-limit');
const limiter = rateLimit({ windowMs: 60 * 1000, max: 100 });

module.exports = { authenticateToken, authenticatePostingKey, limiter };
