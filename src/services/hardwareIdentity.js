"use strict";

const crypto = require("crypto");

function _trim(value) {
  return typeof value === "string" ? value.trim() : "";
}

function _normalizeMac(value) {
  return _trim(value).toLowerCase().replace(/[^0-9a-f]/g, "");
}

function _normalizeGeneric(value) {
  return _trim(value);
}

function _sha256Hex(text) {
  return crypto.createHash("sha256").update(String(text || ""), "utf8").digest("hex");
}

function _pickRawIdentity(input) {
  const candidates = [
    ["hardware_id", input.hardware_id],
    ["hardware_id", input.hardware_identifier],
    ["device_mac", input.device_mac],
    ["device_mac", input.mac],
    ["serial_number", input.serial_number],
    ["serial_number", input.device_serial],
    ["serial_number", input.serial],
    ["device_id", input.device_id],
  ];

  for (const [kind, raw] of candidates) {
    const value = _trim(raw);
    if (!value) continue;
    return { kind, value };
  }
  return null;
}

function normalizeHardwareIdentity(input, fallbackSeed, fallbackKind) {
  const src = input && typeof input === "object" ? input : {};
  const providedHash = _trim(src.hardware_hash);

  if (providedHash) {
    if (!/^[a-f0-9]{64}$/i.test(providedHash)) {
      throw new Error("hardware_hash must be 64-char hex");
    }
    return {
      hardware_hash: providedHash.toLowerCase(),
      hardware_id_kind: src.hardware_id_kind || null,
      hardware_id: src.hardware_id || src.hardware_identifier || src.device_mac || src.serial_number || src.device_serial || src.serial || src.device_id || null,
    };
  }

  const rawIdentity = _pickRawIdentity(src);
  if (rawIdentity) {
    const normalizedValue = rawIdentity.kind === "device_mac"
      ? _normalizeMac(rawIdentity.value)
      : _normalizeGeneric(rawIdentity.value);
    const hashedSource = rawIdentity.kind + ":" + normalizedValue.toLowerCase();
    return {
      hardware_hash: _sha256Hex(hashedSource),
      hardware_id_kind: rawIdentity.kind,
      hardware_id: normalizedValue,
    };
  }

  const seed = _trim(fallbackSeed);
  if (!seed) {
    return {
      hardware_hash: null,
      hardware_id_kind: null,
      hardware_id: null,
    };
  }

  const kind = fallbackKind || "device_id";
  return {
    hardware_hash: _sha256Hex(kind + ":" + seed.toLowerCase()),
    hardware_id_kind: kind,
    hardware_id: seed,
  };
}

module.exports = {
  normalizeHardwareIdentity,
};
