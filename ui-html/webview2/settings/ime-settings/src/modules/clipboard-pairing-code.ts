// QR payloads use the same complete msime1 pairing credential as manual entry.
// This parser is only UI validation; the native pairing store remains authoritative.
export function parseClipboardPairingCode(value: string): { code: string; device: string } | null {
  const code = value.trim();
  if (code.length > 240) return null;
  const match = /^msime1:([A-Za-z0-9._-]{1,128}):[0-9a-f]{32}:[0-9a-f]{64}$/.exec(code);
  return match ? { code, device: match[1] } : null;
}
