import { afterEach, beforeEach, expect, it, vi } from 'vitest';
import { applyThemeConfig, applyWallpaperPalette, reapplyThemeConfig, resolveTheme } from './theme';

class FakeImage {
  src = '/assets/softkbd.png';
}

function fakeElement() {
  return {
    style: {} as Record<string, string>,
    setAttribute: vi.fn(),
    classList: { toggle: vi.fn() }
  };
}

let previewLoaded = false;
let image: FakeImage;

beforeEach(() => {
  previewLoaded = false;
  image = new FakeImage();
  vi.stubGlobal('HTMLImageElement', FakeImage);
  vi.stubGlobal('window', { matchMedia: () => ({ matches: true, onchange: null }) });
  vi.stubGlobal('document', {
    documentElement: fakeElement(),
    body: fakeElement(),
    querySelector: (selector: string) =>
      selector === '.screenkb-preview' && previewLoaded ? fakeElement() : null,
    querySelectorAll: () => [],
    getElementById: (id: string) => (id === 'screenKeyboardPreviewImage' && previewLoaded ? image : null)
  });
});

afterEach(() => {
  vi.unstubAllGlobals();
});

it('follows the device independently of the global and other surface modes', () => {
  expect(resolveTheme('dark', 'system')).toBe('light');
  expect(resolveTheme('dark', 'follow')).toBe('dark');
  expect(resolveTheme('system', 'dark')).toBe('dark');
  applyThemeConfig({ theme_mode: 'dark', theme_settings: 'system', theme_cand: 'dark' });
  expect(document.documentElement.setAttribute).toHaveBeenCalledWith('data-theme', 'light');
});

it('reapplies an independent system surface when the device mode changes', () => {
  const media = { matches: true, onchange: null as null | (() => void) };
  vi.stubGlobal('window', { matchMedia: () => media });
  vi.resetModules();
  // 单独重新加载模块，让监听器绑定这次测试的设备主题来源。
  return import('./theme').then(module => {
    module.applyThemeConfig({ theme_mode: 'dark', theme_settings: 'system', theme_cand: 'light' });
    media.matches = false;
    media.onchange?.();
    expect(document.documentElement.setAttribute).toHaveBeenLastCalledWith('data-theme', 'dark');
  });
});

it('themes a preview page that loads after the snapshot was applied', async () => {
  applyThemeConfig({ theme_mode: 'system', theme_screen_keyboard: 'follow' });
  await vi.dynamicImportSettled();
  expect(image.src).toBe('/assets/softkbd.png');

  previewLoaded = true;
  reapplyThemeConfig();
  await vi.dynamicImportSettled();
  expect(image.src).toBe('/assets/softkbd_light.png');
});

it('uses the supplied wallpaper palette and removes it when disabled', () => {
  const setProperty = vi.fn();
  const removeProperty = vi.fn();
  document.documentElement.style.setProperty = setProperty;
  document.documentElement.style.removeProperty = removeProperty;
  const palette = { accent: 0x123456, surface: 0xfafafa, card: 0xeeeeee, border: 0xcccccc,
    text: 0x111111, secondary: 0x444444, selected: 0xdddddd };
  applyWallpaperPalette('light', { wallpaper_colors: true, wallpaper_palette: { light: palette, dark: palette } });
  expect(setProperty).toHaveBeenCalledWith('--accent-color', '#123456');
  expect(setProperty).toHaveBeenCalledWith('--text-color', '#111111');
  applyWallpaperPalette('light', { wallpaper_colors: false });
  expect(removeProperty).toHaveBeenCalledWith('--accent-color');
  expect(removeProperty).toHaveBeenCalledWith('--text-color');
});
