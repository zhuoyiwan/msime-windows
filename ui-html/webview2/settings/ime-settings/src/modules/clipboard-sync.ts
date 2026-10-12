import { onHostMessage } from '../utils/host-messages';
import { serializeHostMessage } from '../../../../shared/messages';
import type { SettingsMessage } from '../../../../shared/messages';
import { setupToggleButton } from './shared';
import { updateConfig } from './config-sync';
import { parseClipboardPairingCode } from './clipboard-pairing-code';
import { setupClipboardQrScanner } from './clipboard-qr-scanner';

type Request = Extract<SettingsMessage, { type: 'clipboardSyncRequest' }>['data'];

export function setupClipboardSync(): void {
  if (!document.getElementById('clipboardSyncSection')) return;
  let counter = 0;
  let latestRequest = '';
  let invitationRequest = '';
  let statusRequest = '';
  let expiryTimer: number | undefined;
  const input = (id: string) => document.getElementById(id) as HTMLInputElement;
  const element = (id: string) => document.getElementById(id)!;
  const setStatus = (message: string, error = false) => {
    element('clipboardSyncStatus').textContent = message;
    element('clipboardSyncStatus').dataset.kind = error ? 'error' : 'idle';
  };
  const scanner = setupClipboardQrScanner(text => {
    const parsed = parseClipboardPairingCode(text);
    if (!parsed) return false;
    // A scan fills the form; pairing still requires explicit confirmation.
    input('clipboardSyncCode').value = parsed.code;
    element('clipboardSyncScannedDevice').textContent = `识别到设备：${parsed.device}`;
    element('clipboardSyncScannedDevice').hidden = false;
    setStatus('已识别配对二维码，请确认设备后点击“配对并连接”。');
    return true;
  }, setStatus);
  const clearCode = () => {
    input('clipboardSyncInvite').value = '';
    for (const id of ['clipboardSyncQr', 'clipboardSyncCancel', 'clipboardSyncManualCode']) element(id).hidden = true;
    const canvas = element('clipboardSyncQrCanvas') as HTMLCanvasElement;
    canvas.getContext('2d')?.clearRect(0, 0, canvas.width, canvas.height);
    if (expiryTimer !== undefined) window.clearTimeout(expiryTimer);
    expiryTimer = undefined;
  };
  const post = (action: Request['action'], fields: Partial<Request> = {}, quiet = false) => {
    const requestId = `clipboard-${++counter}`;
    if (!quiet) latestRequest = requestId;
    if (action === 'status') statusRequest = requestId;
    if (action === 'invite') invitationRequest = requestId;
    if (action === 'cancel') invitationRequest = '';
    window.chrome?.webview?.postMessage(serializeHostMessage({
      type: 'clipboardSyncRequest', data: { ...fields, action, requestId }
    }));
  };
  const refreshAvailability = () => {
    const enabled = ['clipboardHistoryToggleBtn', 'clipboardSyncToggleBtn']
      .every(id => element(id).getAttribute('aria-checked') === 'true');
    for (const id of ['clipboardSyncGenerate', 'clipboardSyncScan', 'clipboardSyncScanImage', 'clipboardSyncPair'])
      (element(id) as HTMLButtonElement).disabled = !enabled;
    if (!enabled) {
      clearCode(); scanner.stop();
      setStatus('请先开启剪贴板管理和局域网共享，再配对设备。');
    }
  };
  const availability = new MutationObserver(refreshAvailability);
  for (const id of ['clipboardHistoryToggleBtn', 'clipboardSyncToggleBtn'])
    availability.observe(element(id), { attributes: true, attributeFilter: ['aria-checked'] });
  refreshAvailability();
  setupToggleButton('clipboardSyncToggleBtn', active => {
    updateConfig('utility.clipboard_lan_sync', active);
    if (!active) { clearCode(); scanner.stop(); post('cancel'); }
  });
  element('clipboardSyncGenerate').addEventListener('click', () => { clearCode(); post('invite'); });
  element('clipboardSyncCancel').addEventListener('click', () => { clearCode(); post('cancel'); });
  element('clipboardSyncPair').addEventListener('click', () => {
    const parsed = parseClipboardPairingCode(input('clipboardSyncCode').value);
    if (!parsed) { setStatus('请输入完整配对码，或扫描水杉配对二维码。', true); return; }
    if (parsed.device === element('clipboardSyncDevice').textContent) {
      setStatus('这是本机的配对码，请使用另一台设备生成的配对码。', true); return;
    }
    const address = input('clipboardSyncAddress').value.trim();
    input('clipboardSyncCode').value = '';
    element('clipboardSyncScannedDevice').hidden = true;
    post('pair', { code: parsed.code, address });
  });
  input('clipboardSyncCode').addEventListener('input', () => { element('clipboardSyncScannedDevice').hidden = true; });
  window.addEventListener('pagehide', clearCode);
  element('clipboardSyncRefresh').addEventListener('click', () => post('status'));
  onHostMessage('clipboardSyncResponse', payload => {
    const currentInvite = payload.action === 'invite' && payload.requestId === invitationRequest;
    if (payload.requestId !== latestRequest && !currentInvite && payload.requestId !== statusRequest) return;
    if (payload.requestId === latestRequest || currentInvite) setStatus(payload.message, !payload.ok);
    element('clipboardSyncDevice').textContent = payload.localDevice;
    if (payload.action === 'invite' && payload.ok && payload.code && !(element('clipboardSyncGenerate') as HTMLButtonElement).disabled) {
      const code = payload.code;
      input('clipboardSyncInvite').value = code;
      element('clipboardSyncCancel').hidden = false;
      element('clipboardSyncManualCode').hidden = false;
      const seconds = Math.min(300, Math.max(1, payload.expiresIn ?? 300));
      expiryTimer = window.setTimeout(() => { clearCode(); setStatus('配对码已过期，请重新生成。'); }, seconds * 1000);
      void import('qrcode').then(async QRCode => {
        const detached = document.createElement('canvas');
        await QRCode.toCanvas(detached, code, { errorCorrectionLevel: 'M', margin: 4, scale: 6 });
        if (input('clipboardSyncInvite').value !== code) return;
        const canvas = element('clipboardSyncQrCanvas') as HTMLCanvasElement;
        canvas.width = detached.width; canvas.height = detached.height;
        canvas.getContext('2d')?.drawImage(detached, 0, 0);
        element('clipboardSyncQr').hidden = false;
      }).catch(() => { if (input('clipboardSyncInvite').value === code) setStatus('二维码生成失败，可展开并复制配对码。', true); });
    }
    const list = element('clipboardSyncDevices');
    list.replaceChildren(...payload.devices.map(peer => {
      const row = document.createElement('li');
      const label = document.createElement('span'); label.className = 'clipboard-peer-label';
      const identity = document.createElement('strong'); identity.textContent = peer.device;
      const address = document.createElement('small'); address.textContent = peer.address
        ? `已保存地址 ${peer.address} · 自动更新；此项不代表当前已连接`
        : '尚未获得连接地址 · 请确认两端共享已开启，或填写对方的局域网 IPv4 地址';
      label.append(identity, address);
      const remove = document.createElement('button');
      remove.type = 'button'; remove.className = 'clipboard-button clipboard-quiet'; remove.textContent = '撤销配对';
      remove.addEventListener('click', () => post('remove', { device: peer.device }));
      row.append(label, remove);
      return row;
    }));
    if (!payload.devices.length) {
      const empty = document.createElement('li'); empty.className = 'clipboard-empty'; empty.textContent = '暂无配对设备'; list.append(empty);
    }
    refreshAvailability();
  });
  post('status');
  // The inviter's native process pairs independently; refresh both sides' lists
  // while this page is visible without replacing scan/action feedback.
  const poll = window.setInterval(() => {
    if (!document.hidden && document.getElementById('tools-settings')?.style.display !== 'none' &&
      !(element('clipboardSyncPair') as HTMLButtonElement).disabled) post('status', {}, true);
  }, 3000);
  window.addEventListener('pagehide', () => window.clearInterval(poll));
}
