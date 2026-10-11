import { expect, it } from 'vitest';
import QRCode from 'qrcode';
import { BrowserQRCodeReader } from '@zxing/browser';
import { BinaryBitmap, HybridBinarizer, RGBLuminanceSource } from '@zxing/library';
import { parseClipboardPairingCode } from './clipboard-pairing-code';

const code = `msime1:device.abc:${'a'.repeat(32)}:${'b'.repeat(64)}`;

it('accepts the complete credential and rejects links, malformed secrets and identities', () => {
  expect(parseClipboardPairingCode(`  ${code}\n`)).toEqual({ code, device: 'device.abc' });
  for (const invalid of [code + ':extra', code.slice(0, -1), code.replace('device.abc', 'bad/id'),
    code.replace('a'.repeat(32), 'A'.repeat(32)), `https://example.com/?code=${code}`, 'javascript:alert(1)',
    code.replace('device.abc', 'x'.repeat(129)), code + '\0']) {
    expect(parseClipboardPairingCode(invalid)).toBeNull();
  }
});

it('roundtrips generated QR modules through the production QR decoder including the longest device ID', () => {
  for (const credential of [code, code.replace('device.abc', 'x'.repeat(128))]) {
    const qr = QRCode.create(credential, { errorCorrectionLevel: 'M' });
    const scale = 4;
    const width = (qr.modules.size + 8) * scale;
    const pixels = new Uint8ClampedArray(width * width).fill(255);
    for (let y = 0; y < width; ++y) for (let x = 0; x < width; ++x) {
      const mx = Math.floor(x / scale) - 4, my = Math.floor(y / scale) - 4;
      if (mx >= 0 && my >= 0 && mx < qr.modules.size && my < qr.modules.size && qr.modules.get(my, mx))
        pixels[y * width + x] = 0;
    }
    const bitmap = new BinaryBitmap(new HybridBinarizer(new RGBLuminanceSource(pixels, width, width)));
    const decoded = new BrowserQRCodeReader().decodeBitmap(bitmap).getText();
    expect(decoded).toBe(credential);
    expect(parseClipboardPairingCode(decoded)).not.toBeNull();
  }
});
