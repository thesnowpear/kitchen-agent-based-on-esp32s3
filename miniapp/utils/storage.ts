export function getStorage<T>(key: string): T | null {
  try {
    const value = wx.getStorageSync<T | null>(key);
    return value || null;
  } catch (error) {
    console.warn("读取本地缓存失败", key, error);
    return null;
  }
}

export function setStorage<T>(key: string, value: T | null): void {
  try {
    if (value === null || value === undefined) {
      wx.removeStorageSync(key);
      return;
    }
    wx.setStorageSync(key, value);
  } catch (error) {
    console.warn("写入本地缓存失败", key, error);
  }
}
