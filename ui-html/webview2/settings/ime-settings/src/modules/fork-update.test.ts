import { describe, expect, it } from 'vitest';
import { FORK_RELEASES, pickForkUpdate } from './fork-update';
const tag = 'zhuoyiwan-20261013-r3';
function release(revision = 9, override: Record<string, unknown> = {}) {
  const name = `MetasequoiaIME_Setup_v0.7.1-zhuoyiwan-local-20261013-r${revision}-unsigned.exe`;
  return { tag_name: tag, draft: false, prerelease: true, assets: [{ name, size: 100,
    digest: `sha256:${'a'.repeat(64)}`, browser_download_url: `${FORK_RELEASES}/download/${tag}/${name}`, ...override }] };
}
describe('改版更新选择', () => {
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
