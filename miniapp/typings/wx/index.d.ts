declare const wx: WechatMiniprogram.Wx;
declare const App: WechatMiniprogram.App.Constructor;
declare const Page: WechatMiniprogram.Page.Constructor;
declare const Component: WechatMiniprogram.Component.Constructor;
declare const getApp: WechatMiniprogram.GetApp;

declare namespace WechatMiniprogram {
  interface IAnyObject {
    [key: string]: any;
  }

  interface GeneralCallbackResult {
    errMsg: string;
  }

  interface RequestSuccessCallbackResult<T = any> {
    data: T;
    statusCode: number;
    header: IAnyObject;
  }

  interface RequestTask {
    abort(): void;
  }

  interface UploadTask {
    abort(): void;
    onProgressUpdate(callback: (res: { progress: number; totalBytesSent: number; totalBytesExpectedToSend: number }) => void): void;
  }

  interface LoginSuccessCallbackResult extends GeneralCallbackResult {
    code: string;
  }

  interface Wx {
    request<T = any>(option: {
      url: string;
      data?: any;
      method?: "GET" | "POST" | "PUT" | "DELETE" | "PATCH";
      header?: IAnyObject;
      timeout?: number;
      success?: (res: RequestSuccessCallbackResult<T>) => void;
      fail?: (res: GeneralCallbackResult) => void;
      complete?: (res: GeneralCallbackResult) => void;
    }): RequestTask;
    login(option: {
      success?: (res: LoginSuccessCallbackResult) => void;
      fail?: (res: GeneralCallbackResult) => void;
    }): void;
    scanCode(option: {
      onlyFromCamera?: boolean;
      scanType?: string[];
      success?: (res: { result: string; errMsg: string }) => void;
      fail?: (res: GeneralCallbackResult) => void;
    }): void;
    showToast(option: { title: string; icon?: "success" | "error" | "loading" | "none"; duration?: number }): void;
    showLoading(option: { title: string; mask?: boolean }): void;
    hideLoading(): void;
    navigateTo(option: { url: string; success?: (res: GeneralCallbackResult) => void; fail?: (res: GeneralCallbackResult) => void }): void;
    switchTab(option: { url: string; success?: (res: GeneralCallbackResult) => void; fail?: (res: GeneralCallbackResult) => void }): void;
    redirectTo(option: { url: string; success?: (res: GeneralCallbackResult) => void; fail?: (res: GeneralCallbackResult) => void }): void;
    reLaunch(option: { url: string; success?: (res: GeneralCallbackResult) => void; fail?: (res: GeneralCallbackResult) => void }): void;
    navigateBack(option?: { delta?: number }): void;
    stopPullDownRefresh(): void;
    chooseMedia(option: {
      count?: number;
      mediaType?: Array<"image" | "video" | "mix">;
      sourceType?: Array<"album" | "camera">;
      camera?: "back" | "front";
      sizeType?: Array<"original" | "compressed">;
      success?: (res: {
        tempFiles: Array<{ tempFilePath: string; size: number; duration?: number; height?: number; width?: number; thumbTempFilePath?: string }>;
        type: "image" | "video" | "mix";
        errMsg: string;
      }) => void;
      fail?: (res: GeneralCallbackResult) => void;
    }): void;
    showModal(option: {
      title?: string;
      content?: string;
      confirmText?: string;
      confirmColor?: string;
      cancelText?: string;
      success?: (res: { confirm: boolean; cancel: boolean; errMsg: string }) => void;
      fail?: (res: GeneralCallbackResult) => void;
    }): void;
    setClipboardData(option: {
      data: string;
      success?: (res: GeneralCallbackResult) => void;
      fail?: (res: GeneralCallbackResult) => void;
    }): void;
    uploadFile(option: {
      url: string;
      filePath: string;
      name: string;
      formData?: IAnyObject;
      header?: IAnyObject;
      timeout?: number;
      success?: (res: { data: string; statusCode: number; header: IAnyObject; errMsg: string }) => void;
      fail?: (res: GeneralCallbackResult) => void;
      complete?: (res: GeneralCallbackResult) => void;
    }): UploadTask;
    getStorageSync<T = any>(key: string): T;
    setStorageSync<T = any>(key: string, data: T): void;
    removeStorageSync(key: string): void;
  }

  interface BaseEvent {
    currentTarget: { dataset: IAnyObject };
    target: { dataset: IAnyObject };
  }

  interface Input extends BaseEvent {
    detail: { value: string };
  }

  interface SwitchChange extends BaseEvent {
    detail: { value: boolean };
  }

  interface PickerChange extends BaseEvent {
    detail: { value: string | number };
  }

  namespace App {
    type Constructor = <TGlobalData extends object, TOptions extends object = IAnyObject>(
      options: TOptions & { globalData: TGlobalData } & ThisType<TOptions & { globalData: TGlobalData }>
    ) => void;
  }

  namespace Page {
    interface Instance<TData extends object = IAnyObject> {
      data: TData;
      setData(data: Partial<TData> | IAnyObject, callback?: () => void): void;
      getTabBar?(): Component.Instance;
    }

    type Constructor = <T extends object>(options: T & ThisType<T & Instance>) => void;
  }

  namespace Component {
    interface Instance<TData extends object = IAnyObject> {
      data: TData;
      setData(data: Partial<TData> | IAnyObject, callback?: () => void): void;
    }

    type Constructor = <T extends object>(options: T & ThisType<T & Instance>) => void;
  }

  type GetApp = <T extends object = any>() => T & {
    globalData: any;
  };
}
