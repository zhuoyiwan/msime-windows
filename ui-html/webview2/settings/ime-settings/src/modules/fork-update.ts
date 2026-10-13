export const FORK_RELEASES = 'https://github.com/zhuoyiwan/msime-windows/releases';
export const FORK_API = 'https://api.github.com/repos/zhuoyiwan/msime-windows/releases?per_page=30';
export const FORK_FEED = 'https://raw.githubusercontent.com/zhuoyiwan/msime-windows/zhuoyiwan-updates/releases.json';

// 发布完成后生成的小清单不占匿名 API 额度；故障时仍可从官方 API 查询。
export async function loadForkReleases(signal: AbortSignal): Promise<unknown[]> {
  let failure: unknown;
  for (const url of [FORK_FEED, FORK_API]) {
    try {
      const response = await fetch(url, { cache: 'no-store', signal });
      if (!response.ok) throw new Error(`更新源返回 HTTP ${response.status}`);
      const payload: unknown = await response.json();
      if (!Array.isArray(payload)) throw new Error('更新清单格式不对');
      return payload;
    } catch (error) {
      failure = error;
      if (signal.aborted) break;
    }
  }
  throw failure;
}
const NAME = /^MetasequoiaIME_Setup_v(\d+\.\d+\.\d+)-zhuoyiwan-local-(\d{8})-r(\d+)(-unsigned)?\.exe$/;
const VERSION = /^v?(\d+\.\d+\.\d+)(?:-zhuoyiwan-local-(\d{8})-r(\d+)(?:-unsigned)?)?$/;

export type ForkUpdate = { tag: string; name: string; sha256: string; version: string; url: string; releaseUrl: string; signed: boolean; order: number[] };
function object(value: unknown): Record<string, unknown> | null {
  return value !== null && typeof value === 'object' && !Array.isArray(value) ? value as Record<string, unknown> : null;
}
function compare(left: number[], right: number[]): number {
  for (let index = 0; index < Math.max(left.length, right.length); index++) {
    const difference = (left[index] ?? 0) - (right[index] ?? 0);
    if (difference) return difference;
  }
  return 0;
}
export function pickForkUpdate(payload: unknown, current: string): ForkUpdate | null {
  if (!Array.isArray(payload)) throw new Error('GitHub 发布列表格式不对');
  const installed = VERSION.exec(current.trim());
  if (!installed) throw new Error('无法识别当前改版版本');
  const baseline = [...installed[1].split('.').map(Number), Number(installed[2] ?? 0), Number(installed[3] ?? 0)];
  if (!baseline.every(Number.isSafeInteger)) throw new Error('版本号超出范围');
  let best: ForkUpdate | null = null;
  for (const raw of payload) {
    const release = object(raw);
    if (!release || release.draft !== false || typeof release.tag_name !== 'string' ||
        !/^zhuoyiwan-[A-Za-z0-9._-]{1,100}$/.test(release.tag_name) || !Array.isArray(release.assets)) continue;
    for (const rawAsset of release.assets) {
      const asset = object(rawAsset);
      if (!asset || typeof asset.name !== 'string' || typeof asset.digest !== 'string') continue;
      const match = NAME.exec(asset.name);
      if (!match || !/^sha256:[0-9a-f]{64}$/.test(asset.digest) || typeof asset.size !== 'number' ||
          !Number.isSafeInteger(asset.size) || asset.size <= 0 || asset.size > 256 * 1024 * 1024) continue;
      const tag = release.tag_name;
      const url = `${FORK_RELEASES}/download/${tag}/${asset.name}`;
      if (asset.browser_download_url !== url) continue;
      const order = [...match[1].split('.').map(Number), Number(match[2]), Number(match[3])];
      if (!order.every(Number.isSafeInteger) || compare(order, baseline) <= 0 || best && compare(order, best.order) <= 0) continue;
      best = { tag, name: asset.name, sha256: asset.digest.slice(7), version: `${match[1]}-zhuoyiwan-local-${match[2]}-r${match[3]}`,
        url, releaseUrl: `${FORK_RELEASES}/tag/${tag}`, signed: !match[4], order };
    }
  }
  return best;
}
