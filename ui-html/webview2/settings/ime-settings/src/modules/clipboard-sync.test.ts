import { afterEach, beforeEach, expect, it, vi } from 'vitest';

const hooks = vi.hoisted(() => ({ host: null as null | ((payload: any) => void), scan: null as null | ((text: string) => boolean), stop: vi.fn(), render: vi.fn() }));
vi.mock('../utils/host-messages', () => ({ onHostMessage: (_type: string, handler: any) => { hooks.host = handler; } }));
vi.mock('./shared', () => ({ setupToggleButton: vi.fn() }));
vi.mock('./config-sync', () => ({ updateConfig: vi.fn() }));
vi.mock('./clipboard-qr-scanner', () => ({ setupClipboardQrScanner: (accept: (text: string) => boolean) => { hooks.scan = accept; return { stop: hooks.stop }; } }));
vi.mock('qrcode', () => ({ toCanvas: hooks.render }));
import { setupClipboardSync } from './clipboard-sync';

class Element extends EventTarget {
  value = ''; textContent = ''; hidden = false; disabled = false; dataset = {}; width = 200; height = 200;
  children: Element[] = [];
  attrs = new Map([['aria-checked', 'true']]);
  getAttribute(name: string) { return this.attrs.get(name) ?? null; }
  getContext() { return { clearRect: vi.fn(), drawImage: vi.fn() }; }
  append(...items: Element[]) { this.children.push(...items); }
  replaceChildren(...items: Element[]) { this.children = items; }
  click() { if (!this.disabled) this.dispatchEvent(new Event('click')); }
}
let elements: Map<string, Element>;
let post: ReturnType<typeof vi.fn>;
let poll: () => void;
const code = `msime1:remote:${'a'.repeat(32)}:${'b'.repeat(64)}`;
function element(id: string) {
  if (!elements.has(id)) elements.set(id, new Element());
  return elements.get(id)!;
}
function respond(action: string, requestId: string, extra = {}) {
  hooks.host!({ action, requestId, ok: true, localDevice: 'local', message: '已更新', devices: [], ...extra });
}
function lastRequest() { return JSON.parse(post.mock.calls.at(-1)![0]).data; }

beforeEach(() => {
  vi.clearAllMocks(); elements = new Map(); post = vi.fn();
  hooks.render.mockResolvedValue(undefined);
  vi.stubGlobal('document', { getElementById: element, createElement: () => new Element() });
  vi.stubGlobal('MutationObserver', class { observe() {} });
  vi.stubGlobal('window', { chrome: { webview: { postMessage: post } }, addEventListener: vi.fn(), setTimeout, clearTimeout,
    setInterval: (callback: () => void) => { poll = callback; return 1; }, clearInterval: vi.fn() });
  setupClipboardSync();
  respond('status', lastRequest().requestId);
});
afterEach(() => { vi.unstubAllGlobals(); vi.useRealTimers(); });

it('shows a generated invitation even if a device-list refresh was requested meanwhile', async () => {
  element('clipboardSyncGenerate').click();
  const invite = lastRequest();
  element('clipboardSyncRefresh').click();
  respond('invite', invite.requestId, { code });
  await vi.waitFor(() => expect(element('clipboardSyncQr').hidden).toBe(false));
  expect(element('clipboardSyncInvite').value).toBe(code);
  element('clipboardSyncCancel').click();
  expect(element('clipboardSyncQr').hidden).toBe(true);
});

it('does not redisplay a cancelled invitation from a delayed native response', () => {
  element('clipboardSyncGenerate').click();
  const invite = lastRequest();
  element('clipboardSyncCancel').click();
  respond('invite', invite.requestId, { code });
  expect(element('clipboardSyncInvite').value).toBe('');
  expect(element('clipboardSyncQr').hidden).toBe(true);
});

it('requires explicit confirmation after scanning and passes an empty optional IP to the host', () => {
  const before = post.mock.calls.length;
  expect(hooks.scan!(code)).toBe(true);
  expect(post.mock.calls.length).toBe(before);
  expect(element('clipboardSyncScannedDevice').textContent).toContain('remote');
  element('clipboardSyncPair').click();
  expect(lastRequest()).toMatchObject({ action: 'pair', code, address: '' });
  expect(hooks.scan!('https://example.com')).toBe(false);
});

it('disables pairing and stops capture when the native configuration says sharing is off', () => {
  element('clipboardSyncToggleBtn').attrs.set('aria-checked', 'false');
  element('clipboardSyncRefresh').click();
  respond('status', lastRequest().requestId);
  expect(element('clipboardSyncPair').disabled).toBe(true);
  expect(element('clipboardSyncScan').disabled).toBe(true);
  expect(hooks.stop).toHaveBeenCalled();
});

it('refreshes a reciprocal pairing in the background without overwriting scanned-device feedback', () => {
  (element('tools-settings') as Element & { style: object }).style = { display: '' };
  hooks.scan!(code);
  const feedback = element('clipboardSyncStatus').textContent;
  poll();
  respond('status', lastRequest().requestId, { devices: [{ device: 'remote', address: '192.168.1.20' }] });
  expect(element('clipboardSyncDevices').children).toHaveLength(1);
  expect(element('clipboardSyncDevices').children[0].children[0].children[0].textContent).toBe('remote');
  expect(element('clipboardSyncStatus').textContent).toBe(feedback);
});
