import { onHostMessage } from '../utils/host-messages';
import { serializeHostMessage } from '../../../../shared/messages';
import type { SettingsMessage } from '../../../../shared/messages';
import { setupToggleButton } from './shared';
import { updateConfig } from './config-sync';

type Request = Extract<SettingsMessage, { type: 'clipboardSyncRequest' }>['data'];

export function setupClipboardSync(): void {
  if (!document.getElementById('clipboardSyncSection')) return;
  let counter = 0;
  let latestRequest = '';
  let expiryTimer: number | undefined;
  const input = (id: string) => document.getElementById(id) as HTMLInputElement;
  const status = document.getElementById('clipboardSyncStatus')!;
  const clearCode = () => {
    input('clipboardSyncInvite').value = '';
    if (expiryTimer !== undefined) window.clearTimeout(expiryTimer);
    expiryTimer = undefined;
  };
  const post = (action: Request['action'], fields: Partial<Request> = {}) => {
    latestRequest = `clipboard-${++counter}`;
    window.chrome?.webview?.postMessage(serializeHostMessage({
      type: 'clipboardSyncRequest',
      data: { ...fields, action, requestId: latestRequest }
    }));
  };
  setupToggleButton('clipboardSyncToggleBtn', active => {
    updateConfig('utility.clipboard_lan_sync', active);
    if (!active) { clearCode(); post('cancel'); }
  });
  document.getElementById('clipboardSyncGenerate')?.addEventListener('click', () => {
    clearCode(); post('invite');
  });
  document.getElementById('clipboardSyncCancel')?.addEventListener('click', () => {
    clearCode(); post('cancel');
  });
  document.getElementById('clipboardSyncPair')?.addEventListener('click', () => {
    const code = input('clipboardSyncCode').value.trim();
    const address = input('clipboardSyncAddress').value.trim();
    input('clipboardSyncCode').value = '';
    post('pair', { code, address });
  });
  document.getElementById('clipboardSyncRefresh')?.addEventListener('click', () => post('status'));
  onHostMessage('clipboardSyncResponse', payload => {
    if (payload.requestId !== latestRequest) return;
    status.textContent = payload.message;
    document.getElementById('clipboardSyncDevice')!.textContent = payload.localDevice;
    if (payload.action === 'invite' && payload.ok && payload.code) {
      input('clipboardSyncInvite').value = payload.code;
      expiryTimer = window.setTimeout(clearCode, (payload.expiresIn ?? 300) * 1000);
    }
    const list = document.getElementById('clipboardSyncDevices')!;
    list.replaceChildren(...payload.devices.map(peer => {
      const row = document.createElement('li');
      const label = document.createElement('span');
      label.textContent = `${peer.device} · ${peer.address || '由对方发起连接'}`;
      const remove = document.createElement('button');
      remove.type = 'button'; remove.className = 'dict-button secondary'; remove.textContent = '撤销配对';
      remove.addEventListener('click', () => post('remove', { device: peer.device }));
      row.append(label, remove);
      return row;
    }));
  });
  post('status');
}
