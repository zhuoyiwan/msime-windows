import { afterEach, describe, expect, it, vi } from 'vitest';
import { FORK_API, FORK_FEED, FORK_RELEASES, loadForkReleases, pickForkUpdate } from './fork-update';
afterEach(() => vi.unstubAllGlobals());
const tag = 'zhuoyiwan-20261013-r3';
function release(revision = 9, override: Record<string, unknown> = {}) {
  const name = `MetasequoiaIME_Setup_v0.7.1-zhuoyiwan-local-20261013-r${revision}-unsigned.exe`;
  return { tag_name: tag, draft: false, prerelease: true, assets: [{ name, size: 100,
    digest: `sha256:${'a'.repeat(64)}`, browser_download_url: `${FORK_RELEASES}/download/${tag}/${name}`, ...override }] };
}
describe('改版更新选择', () => {
  it('静态清单可用时不访问已限流的 API', async () => {
    const request = vi.fn(async (url: string) => ({ ok: url === FORK_FEED, status: 403,
      json: async () => [release(10)] }));
    vi.stubGlobal('fetch', request);
    const rows = await loadForkReleases(new AbortController().signal);
    expect(pickForkUpdate(rows, '0.7.1-zhuoyiwan-local-20261013-r9')?.name).toContain('-r10-');
    expect(request).toHaveBeenCalledTimes(1);
  });
  it('静态源故障时回退 API，两处失败不能误报已是最新', async () => {
    const request = vi.fn(async (url: string) => ({ ok: url === FORK_API, status: 404,
      json: async () => [release(10)] }));
    vi.stubGlobal('fetch', request);
    await expect(loadForkReleases(new AbortController().signal)).resolves.toHaveLength(1);
    expect(request).toHaveBeenCalledTimes(2);
    vi.stubGlobal('fetch', vi.fn(async () => ({ ok: false, status: 403 })));
    await expect(loadForkReleases(new AbortController().signal)).rejects.toThrow('403');
  });
  it('同一基础版本按日期和数字修订号更新', () => {
    expect(pickForkUpdate([release(10), release(9)], 'v0.7.1-zhuoyiwan-local-20261013-r9')?.name).toContain('-r10-');
    expect(pickForkUpdate([release(9)], 'v0.7.1-zhuoyiwan-local-20261013-r10')).toBeNull();
    expect(pickForkUpdate([release(9)], 'v0.7.1-zhuoyiwan-local-20261012-r99')).not.toBeNull();
  });
  it('不下载草稿、其他平台、上游资产、无摘要和越界 URL', () => {
    expect(pickForkUpdate([{ ...release(), draft: true }], '0.7.1')).toBeNull();
    for (const fields of [{ digest: null }, { digest: 'sha256:abc' }, { size: -1 }, { size: 1024 ** 3 },
      { name: '../../bad.exe' }, { browser_download_url: 'https://evil.example/bad.exe' }]) {
      expect(pickForkUpdate([release(9, fields)], '0.7.1')).toBeNull();
    }
  });
  it('坏的最新条目不能盖住较低但完整的发布', () => {
    expect(pickForkUpdate([release(10, { digest: null }), release(9)], '0.7.1')?.name).toContain('-r9-');
    expect(() => pickForkUpdate({}, '0.7.1')).toThrow();
  });
});
