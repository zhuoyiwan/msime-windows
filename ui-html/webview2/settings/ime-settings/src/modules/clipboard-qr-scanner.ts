export function setupClipboardQrScanner(accept: (text: string) => boolean, status: (text: string, error?: boolean) => void): { stop(): void } {
  const element = (id: string) => document.getElementById(id)!;
  const dialog = element('clipboardSyncScanDialog') as HTMLDialogElement;
  const video = element('clipboardSyncScanVideo') as HTMLVideoElement;
  let controls: { stop(): void } | undefined;
  let stream: MediaStream | undefined;
  let generation = 0;
  const stop = () => {
    ++generation;
    controls?.stop(); controls = undefined;
    stream?.getTracks().forEach(track => track.stop()); stream = undefined;
    video.srcObject = null;
    if (dialog.open) dialog.close();
  };
  element('clipboardSyncScan').addEventListener('click', async () => {
    stop();
    const current = generation;
    dialog.showModal();
    element('clipboardSyncScanStatus').textContent = '正在打开摄像头…';
    try {
      const { BrowserQRCodeReader } = await import('@zxing/browser');
      if (current !== generation) return;
      const camera = await navigator.mediaDevices.getUserMedia({ audio: false, video: { facingMode: { ideal: 'environment' } } });
      if (current !== generation) { camera.getTracks().forEach(track => track.stop()); return; }
      stream = camera;
      element('clipboardSyncScanStatus').textContent = '请将对方的配对二维码放入画面。';
      const scanner = await new BrowserQRCodeReader().decodeFromStream(camera, video, result => {
        if (!result || current !== generation) return;
        if (accept(result.getText())) stop();
        else element('clipboardSyncScanStatus').textContent = '这不是有效的水杉配对二维码，请扫描对方设置页生成的二维码。';
      });
      if (current !== generation) scanner.stop();
      else controls = scanner;
    } catch {
      if (current === generation) {
        stop(); status('无法使用摄像头。请允许摄像头访问，或选择二维码图片、手动粘贴配对码。', true);
      }
    }
  });
  element('clipboardSyncStopScan').addEventListener('click', stop);
  dialog.addEventListener('cancel', stop);
  dialog.addEventListener('close', () => { if (stream || controls) stop(); });
  const fileInput = element('clipboardSyncQrFile') as HTMLInputElement;
  element('clipboardSyncScanImage').addEventListener('click', () => fileInput.click());
  fileInput.addEventListener('change', async () => {
    const file = fileInput.files?.[0]; fileInput.value = '';
    if (!file) return;
    stop();
    const current = generation;
    if (file.size > 8 * 1024 * 1024 || !['image/png', 'image/jpeg', 'image/webp', 'image/bmp'].includes(file.type)) {
      status('请选择不超过 8 MB 的 PNG、JPEG、WebP 或 BMP 图片。', true); return;
    }
    const url = URL.createObjectURL(file);
    try {
      const { BrowserQRCodeReader } = await import('@zxing/browser');
      if (current !== generation) return;
      const result = await new BrowserQRCodeReader().decodeFromImageUrl(url);
      if (current !== generation) return;
      if (accept(result.getText())) stop();
      else status('图片中不是有效的水杉配对二维码。', true);
    } catch { if (current === generation) status('未能识别配对二维码，请选择清晰的二维码图片。', true); }
    finally { URL.revokeObjectURL(url); }
  });
  document.addEventListener('visibilitychange', () => { if (document.hidden) stop(); });
  window.addEventListener('pagehide', stop);
  const page = document.getElementById('tools-settings');
  if (page) new MutationObserver(() => { if (page.style.display === 'none') stop(); }).observe(page, { attributes: true, attributeFilter: ['style'] });
  return { stop };
}
