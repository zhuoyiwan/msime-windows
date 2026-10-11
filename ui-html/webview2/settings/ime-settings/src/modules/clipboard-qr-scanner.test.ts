import { afterEach, beforeEach, expect, it, vi } from 'vitest';
import { setupClipboardQrScanner } from './clipboard-qr-scanner';

const reader = vi.hoisted(() => ({ decodeFromStream: vi.fn(), decodeFromImageUrl: vi.fn() }));
vi.mock('@zxing/browser', () => ({ BrowserQRCodeReader: class {
  decodeFromStream = reader.decodeFromStream;
  decodeFromImageUrl = reader.decodeFromImageUrl;
} }));

class Element extends EventTarget {
  open = false; srcObject: unknown = null; value = ''; textContent = ''; files: unknown[] = [];
  style = { display: '' };
  showModal() { this.open = true; }
  close() { this.open = false; this.dispatchEvent(new Event('close')); }
  click() { this.dispatchEvent(new Event('click')); }
}
const ids = ['clipboardSyncScanDialog', 'clipboardSyncScanVideo', 'clipboardSyncScan', 'clipboardSyncScanStatus',
  'clipboardSyncStopScan', 'clipboardSyncQrFile', 'clipboardSyncScanImage', 'tools-settings'];
let elements: Map<string, Element>;
let doc: EventTarget & { hidden: boolean };
let win: EventTarget;
let cameraStop: ReturnType<typeof vi.fn>;
let scannerStop: ReturnType<typeof vi.fn>;
let getUserMedia: ReturnType<typeof vi.fn>;
let accept: ReturnType<typeof vi.fn<(text: string) => boolean>>;
let status: ReturnType<typeof vi.fn<(text: string, error?: boolean) => void>>;
let pageChanged: () => void;

beforeEach(() => {
  vi.clearAllMocks();
  elements = new Map(ids.map(id => [id, new Element()]));
  doc = Object.assign(new EventTarget(), { hidden: false, getElementById: (id: string) => elements.get(id) });
  win = new EventTarget();
  cameraStop = vi.fn(); scannerStop = vi.fn();
  getUserMedia = vi.fn().mockResolvedValue({ getTracks: () => [{ stop: cameraStop }] });
  reader.decodeFromStream.mockResolvedValue({ stop: scannerStop });
  accept = vi.fn().mockReturnValue(true); status = vi.fn();
  vi.stubGlobal('document', doc); vi.stubGlobal('window', win);
  vi.stubGlobal('navigator', { mediaDevices: { getUserMedia } });
  vi.stubGlobal('MutationObserver', class {
    constructor(callback: () => void) { pageChanged = callback; }
    observe() {}
  });
});
afterEach(() => { vi.unstubAllGlobals(); vi.restoreAllMocks(); });

it('stops all camera tracks when permission completes after the dialog was closed', async () => {
  let resolve!: (stream: unknown) => void;
  getUserMedia.mockReturnValue(new Promise(r => { resolve = r; }));
  setupClipboardQrScanner(accept, status);
  elements.get('clipboardSyncScan')!.click();
  await vi.waitFor(() => expect(getUserMedia).toHaveBeenCalled());
  elements.get('clipboardSyncStopScan')!.click();
  resolve({ getTracks: () => [{ stop: cameraStop }] });
  await vi.waitFor(() => expect(cameraStop).toHaveBeenCalledOnce());
  expect(reader.decodeFromStream).not.toHaveBeenCalled();
  expect(elements.get('clipboardSyncScanDialog')!.open).toBe(false);
});

it('stops the scanner and video tracks when a credential is accepted', async () => {
  setupClipboardQrScanner(accept, status);
  elements.get('clipboardSyncScan')!.click();
  await vi.waitFor(() => expect(reader.decodeFromStream).toHaveBeenCalled());
  const callback = reader.decodeFromStream.mock.calls[0][2];
  callback({ getText: () => 'credential' });
  expect(accept).toHaveBeenCalledWith('credential');
  expect(cameraStop).toHaveBeenCalledOnce();
  expect(scannerStop).toHaveBeenCalledOnce();
  expect(elements.get('clipboardSyncScanVideo')!.srcObject).toBeNull();
});

it('closes the camera on pagehide and does not accept stale scan callbacks', async () => {
  setupClipboardQrScanner(accept, status);
  elements.get('clipboardSyncScan')!.click();
  await vi.waitFor(() => expect(reader.decodeFromStream).toHaveBeenCalled());
  win.dispatchEvent(new Event('pagehide'));
  reader.decodeFromStream.mock.calls[0][2]({ getText: () => 'stale' });
  expect(cameraStop).toHaveBeenCalledOnce();
  expect(accept).not.toHaveBeenCalled();
});

it('ignores image decoding that finishes after cancellation and revokes its local URL', async () => {
  let resolve!: (result: unknown) => void;
  reader.decodeFromImageUrl.mockReturnValue(new Promise(r => { resolve = r; }));
  vi.spyOn(URL, 'createObjectURL').mockReturnValue('blob:local');
  const revoke = vi.spyOn(URL, 'revokeObjectURL').mockImplementation(() => {});
  const scanner = setupClipboardQrScanner(accept, status);
  const file = elements.get('clipboardSyncQrFile')!;
  file.files = [{ size: 100, type: 'image/png' }];
  file.dispatchEvent(new Event('change'));
  await vi.waitFor(() => expect(reader.decodeFromImageUrl).toHaveBeenCalled());
  scanner.stop(); resolve({ getText: () => 'stale' });
  await vi.waitFor(() => expect(revoke).toHaveBeenCalledWith('blob:local'));
  expect(accept).not.toHaveBeenCalled();
});

it.each(['cancel', 'hidden', 'navigation'])('stops camera capture on %s', async action => {
  setupClipboardQrScanner(accept, status);
  elements.get('clipboardSyncScan')!.click();
  await vi.waitFor(() => expect(reader.decodeFromStream).toHaveBeenCalled());
  if (action === 'cancel') elements.get('clipboardSyncScanDialog')!.dispatchEvent(new Event('cancel'));
  if (action === 'hidden') { doc.hidden = true; doc.dispatchEvent(new Event('visibilitychange')); }
  if (action === 'navigation') { elements.get('tools-settings')!.style.display = 'none'; pageChanged(); }
  expect(cameraStop).toHaveBeenCalledOnce();
  expect(scannerStop).toHaveBeenCalledOnce();
  expect(elements.get('clipboardSyncScanDialog')!.open).toBe(false);
});
