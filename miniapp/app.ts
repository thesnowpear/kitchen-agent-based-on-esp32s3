/**
 * miniapp 全局入口：维护跨页共享的会话 / 家庭 / 设备 / 首屏聚合数据。
 *
 * 启动顺序（与重构前的"先过 login 页"不同）：
 *   onLaunch
 *     ├─ 恢复 storage（apiConfig / session / activeHome / activeDevice）
 *     ├─ ensureSession()：没 session 时静默 wx.login → loginWithWechatCode
 *     │   - 不弹任何同意页；本期 backend 用 demo openid（sha256(code)）兜底，不需要 userInfo；
 *     │   - 失败也不阻塞首屏：home 页根据 hasDevice / errorText 给出对应空态。
 *     └─ bootstrap()：拿到 session 后异步拉一次 home overview，让首屏几乎零等待。
 *
 * 页面消费：
 *   const app = getApp<MiniAppGlobalData>();
 *   const cached = app.globalData.lastOverview;
 *   if (cached) this.setData({ overview: cached });
 *   const fresh = await getHomeOverview(); app.setOverview(fresh);
 */

import { DEFAULT_API_CONFIG, DEFAULT_HOME_NAME } from "./config/env";
import { getHomeOverview, loginWithWechatCode } from "./services/api";
import type {
  ApiConfig,
  AuthSession,
  DeviceSummary,
  HomeInfo,
  HomeOverview,
} from "./types/models";
import { getStorage, setStorage } from "./utils/storage";
import { updateOfflineSnapshot } from "./utils/localFeatures";
import { syncNow } from "./services/sync";

/** 全局状态。所有 setX 方法都会同步写 storage（缓存 key 与 storage 函数对齐）。 */
export interface MiniAppGlobalData {
  apiConfig: ApiConfig;
  session: AuthSession | null;
  activeHome: HomeInfo | null;
  activeDevice: DeviceSummary | null;
  /** 最近一次拉到的 home overview，用于首屏秒开；带 ts 便于过期判定。 */
  lastOverview: HomeOverview | null;
  lastOverviewAt: number;
  /** bootstrap 是否在跑：避免重复并发拉取。 */
  bootstrapInFlight: boolean;
  /** ensureSession 是否在跑：避免短时间内多次 wx.login（每秒最多一次）。 */
  loginInFlight: boolean;
  /** 上一次静默登录失败的原因；首页可读出来给提示。 */
  lastLoginError?: string;
  /** 从库存分区进入拍照页时暂存推荐 zone；scan 是 tabBar 页，不能用 navigateTo query 传参。 */
  pendingScanZone?: string;
}

/** 小程序 App 实例公开方法签名，方便页面 getApp 后类型推断。 */
export interface MiniAppInstance {
  globalData: MiniAppGlobalData;
  setApiConfig(config: ApiConfig): void;
  setSession(session: AuthSession | null): void;
  setActiveHome(home: HomeInfo | null): void;
  setActiveDevice(device: DeviceSummary | null): void;
  setOverview(overview: HomeOverview | null): void;
  /** 首页 / 设置页可主动调；内部带串行锁，重复调用会复用已在跑的 promise。 */
  ensureSession(force?: boolean): Promise<AuthSession | null>;
  bootstrap(force?: boolean): Promise<HomeOverview | null>;
}

App<MiniAppGlobalData>({
  globalData: {
    apiConfig: DEFAULT_API_CONFIG,
    session: null,
    activeHome: null,
    activeDevice: null,
    lastOverview: null,
    lastOverviewAt: 0,
    bootstrapInFlight: false,
    loginInFlight: false,
  },

  onLaunch() {
    // 1) 恢复 ApiConfig（用户在设置页改过 baseUrl 时会 setStorage）
    const savedApiConfig = getStorage<ApiConfig>("apiConfig");
    if (savedApiConfig) {
      this.globalData.apiConfig = { ...DEFAULT_API_CONFIG, ...savedApiConfig };
    }
    // 2) 恢复 session
    const savedSession = getStorage<AuthSession>("authSession");
    if (savedSession) {
      this.globalData.session = savedSession;
    }
    // 3) 恢复上次的 home / device 缓存（拿来做骨架，不影响实际 fetch）
    const savedHome = getStorage<HomeInfo>("activeHome");
    if (savedHome) {
      this.globalData.activeHome = savedHome;
    }
    const savedDevice = getStorage<DeviceSummary>("activeDevice");
    if (savedDevice) {
      this.globalData.activeDevice = savedDevice;
    }

    // 4) 异步：先静默登录，再拉 overview。整条链路失败也不阻塞首屏。
    void this.ensureSession()
      .then((session) => {
        if (session?.token) {
          return this.bootstrap();
        }
        return null;
      })
      .catch(() => {
        /* ensureSession / bootstrap 内部都已经吞错；这里再兜一层 */
      });
  },

  onShow() {
    if (this.globalData.session?.token) {
      void syncNow().catch((err) => {
        console.warn("foreground sync failed", err);
      });
    }
  },

  /** 静默微信登录：仅 wx.login 拿 code，不调 wx.getUserProfile（避免任何二次确认弹窗）。
   *  失败时把错误塞到 lastLoginError，让 home 页给出"重试"按钮。 */
  async ensureSession(force = false): Promise<AuthSession | null> {
    const data = this.globalData;
    if (!force && data.session?.token) {
      return data.session;
    }
    if (data.loginInFlight) {
      // 等待已在跑的登录完成；不暴露这个 promise 给外部，简化为轮询 100ms x 30。
      for (let i = 0; i < 30; i++) {
        await sleep(100);
        if (!data.loginInFlight) break;
      }
      return data.session;
    }
    data.loginInFlight = true;
    data.lastLoginError = undefined;
    try {
      const loginRes = await new Promise<WechatMiniprogram.LoginSuccessCallbackResult>(
        (resolve, reject) => {
          wx.login({
            success: resolve,
            fail: (err) => reject(new Error(err.errMsg || "wx.login 失败")),
          });
        },
      );
      if (!loginRes.code) {
        throw new Error("微信登录 code 获取失败");
      }
      const loginData = await loginWithWechatCode(loginRes.code);
      const session: AuthSession = {
        token: loginData.accessToken,
        userId: loginData.userId,
        openid: loginData.openid,
        isPlaceholderSession: loginData.isPlaceholderSession,
      };
      this.setSession(session);
      return session;
    } catch (err) {
      const message =
        err instanceof Error ? err.message : "静默登录失败";
      data.lastLoginError = message;
      console.warn("[ensureSession] failed:", message);
      return null;
    } finally {
      data.loginInFlight = false;
    }
  },

  /** 拉取并缓存 home overview。force=false 时 30s 内不重复拉取。 */
  async bootstrap(force = false) {
    const data = this.globalData;
    if (!data.session?.token) {
      return null;
    }
    if (data.bootstrapInFlight) {
      return data.lastOverview;
    }
    if (!force && data.lastOverview && Date.now() - data.lastOverviewAt < 30_000) {
      return data.lastOverview;
    }
    data.bootstrapInFlight = true;
    try {
      const overview = await getHomeOverview();
      this.setOverview(overview);
      void syncNow().catch((err) => {
        console.warn("bootstrap sync failed", err);
      });
      if (overview.device) {
        this.setActiveDevice(overview.device);
      }
      // overview 自身不带 home_id（backend 依赖 token 推断），仅当 storage 已有 home 才同步。
      if (!data.activeHome) {
        // 占位 home：让后续页面有个名字可显示，等真实多家庭功能再补。
        this.setActiveHome({ homeId: "default", name: DEFAULT_HOME_NAME });
      }
      return overview;
    } catch (err) {
      // 业务/网络错都吞掉，让各页面自己决定怎么展示空态。
      console.warn("bootstrap overview failed", err);
      return null;
    } finally {
      data.bootstrapInFlight = false;
    }
  },

  setApiConfig(config: ApiConfig) {
    this.globalData.apiConfig = config;
    setStorage("apiConfig", config);
  },

  setSession(session: AuthSession | null) {
    this.globalData.session = session;
    setStorage("authSession", session);
  },

  setActiveHome(home: HomeInfo | null) {
    this.globalData.activeHome = home;
    setStorage("activeHome", home);
  },

  setActiveDevice(device: DeviceSummary | null) {
    this.globalData.activeDevice = device;
    setStorage("activeDevice", device);
  },

  setOverview(overview: HomeOverview | null) {
    this.globalData.lastOverview = overview;
    this.globalData.lastOverviewAt = overview ? Date.now() : 0;
    if (overview) {
      updateOfflineSnapshot({ overview });
    }
  },
} as MiniAppInstance);

function sleep(ms: number): Promise<void> {
  return new Promise((resolve) => setTimeout(resolve, ms));
}
