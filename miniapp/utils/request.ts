/**
 * 网络层：包裹 wx.request / wx.uploadFile，统一处理 ApiResponse 外壳、Bearer token、
 * timeout、GET 重试、requestId 透传。
 *
 * 设计选择：
 * - 默认 timeout 来自 ApiConfig.timeoutMs；AI / Scan 类长接口在调用方传入更长的值；
 * - 仅幂等 GET 自动重试一次（最长 1.5s 后再发），其它方法不重试，避免重复提交；
 * - 错误均抛 RequestError，业务层用 instanceof 区分网络层错误 / HTTP 错误 / 业务 ok=false；
 * - 上传走 wx.uploadFile（小程序 multipart 必须用这个，不能用 wx.request body）。
 */

import { API_PREFIX, DEFAULT_API_CONFIG } from "../config/env";
import type { ApiConfig, ApiResponse, AuthSession } from "../types/models";
import { getStorage } from "./storage";

export type HttpMethod = "GET" | "POST" | "PUT" | "DELETE" | "PATCH";

/** 请求选项：path 不需要带 /api/v1 前缀，request 内部自动拼。 */
export interface RequestOptions<TBody = unknown> {
  method?: HttpMethod;
  /** 例如 "/home/overview"；不需要带 "/api/v1"。 */
  path: string;
  data?: TBody;
  /** 默认带 Bearer token；登录/无身份接口显式传 false。 */
  auth?: boolean;
  /** 单次请求超时（毫秒），未传则用 ApiConfig.timeoutMs。 */
  timeoutMs?: number;
  /** 仅幂等 GET 默认重试一次；显式 false 关闭。 */
  retryOnFail?: boolean;
  /** 额外 header，会与默认 header 浅合并。 */
  headers?: Record<string, string>;
}

/** 网络层抛出的错误：业务层根据 kind 区分处理。 */
export class RequestError extends Error {
  readonly kind: "network" | "http" | "business";
  readonly statusCode?: number;
  readonly requestId?: string;
  readonly raw?: unknown;
  constructor(
    kind: "network" | "http" | "business",
    message: string,
    extra?: { statusCode?: number; requestId?: string; raw?: unknown },
  ) {
    super(message);
    this.kind = kind;
    this.statusCode = extra?.statusCode;
    this.requestId = extra?.requestId;
    this.raw = extra?.raw;
  }
}

function getApiConfig(): ApiConfig {
  return { ...DEFAULT_API_CONFIG, ...(getStorage<ApiConfig>("apiConfig") || {}) };
}

function getSession(): AuthSession | null {
  return getStorage<AuthSession>("authSession");
}

function joinUrl(baseUrl: string, prefix: string, path: string): string {
  const cleanBase = baseUrl.replace(/\/+$/, "");
  const cleanPrefix = prefix.startsWith("/") ? prefix : `/${prefix}`;
  const cleanPath = path.startsWith("/") ? path : `/${path}`;
  return `${cleanBase}${cleanPrefix}${cleanPath}`;
}

/** 一次 wx.request 的 Promise 封装，便于上层做 retry。 */
function doRequest<T>(opts: {
  url: string;
  method: HttpMethod;
  data?: unknown;
  header: Record<string, string>;
  timeout: number;
}): Promise<{ statusCode: number; body: ApiResponse<T> | T | undefined }> {
  return new Promise((resolve, reject) => {
    wx.request({
      url: opts.url,
      method: opts.method,
      data: opts.data,
      header: opts.header,
      timeout: opts.timeout,
      success: (res) => {
        resolve({
          statusCode: res.statusCode,
          // wx.request 在 content-type=json 时会自动 parse；非 json 则返回字符串。
          body: res.data as ApiResponse<T> | T | undefined,
        });
      },
      fail: (err) => {
        reject(
          new RequestError("network", err.errMsg || "网络请求失败", { raw: err }),
        );
      },
    });
  });
}

/** 解包 ApiResponse 外壳：返回 data 字段；ok=false 抛业务错。 */
function unwrap<T>(body: ApiResponse<T> | T | undefined, statusCode: number): T {
  // backend 已统一 ApiResponse[T]，但保留对裸响应的兜底，便于早期接口逐步迁移。
  if (body && typeof body === "object" && "ok" in (body as Record<string, unknown>)) {
    const env = body as ApiResponse<T>;
    if (!env.ok) {
      throw new RequestError("business", env.message || "请求失败", {
        statusCode,
        requestId: env.requestId,
        raw: env,
      });
    }
    return env.data as T;
  }
  return body as T;
}

export async function request<TResponse, TBody = unknown>(
  options: RequestOptions<TBody>,
): Promise<TResponse> {
  const config = getApiConfig();
  const session = getSession();
  const method: HttpMethod = options.method || "GET";
  const timeout = options.timeoutMs ?? config.timeoutMs;

  const header: Record<string, string> = {
    "content-type": "application/json",
    ...(options.headers || {}),
  };
  if (options.auth !== false && session?.token) {
    header.Authorization = `Bearer ${session.token}`;
  }

  const url = joinUrl(config.baseUrl, API_PREFIX, options.path);
  // GET 默认开启重试；其它方法不重试以免重复提交（POST /inventory 等）。
  const shouldRetry =
    options.retryOnFail !== false && method === "GET";

  let attempt = 0;
  const maxAttempts = shouldRetry ? 2 : 1;
  let lastError: unknown;

  while (attempt < maxAttempts) {
    attempt += 1;
    try {
      const { statusCode, body } = await doRequest<TResponse>({
        url,
        method,
        data: options.data,
        header,
        timeout,
      });

      if (statusCode < 200 || statusCode >= 300) {
        // 4xx 不重试（参数错误重试无意义）；5xx 留给下一次重试。
        const isServerErr = statusCode >= 500;
        const err = new RequestError("http", `HTTP ${statusCode}`, {
          statusCode,
          raw: body,
        });
        if (!isServerErr || attempt >= maxAttempts) {
          throw err;
        }
        lastError = err;
        // 5xx：短退避再试。
        await sleep(800);
        continue;
      }

      return unwrap<TResponse>(body, statusCode);
    } catch (err) {
      if (err instanceof RequestError && err.kind === "business") {
        // 业务错不重试，直接抛。
        throw err;
      }
      lastError = err;
      if (attempt >= maxAttempts) {
        break;
      }
      // 网络错或服务器错：短退避再试一次。
      await sleep(800);
    }
  }

  if (lastError instanceof Error) {
    throw lastError;
  }
  throw new RequestError("network", "请求失败");
}

/** multipart 文件上传：用于 /inventory/scan。
 *
 * - filePath 是 wx.chooseMedia 拿到的本地临时路径；
 * - extraFormData 会和文件一起作为 multipart fields 上传；
 * - response 仍然是 ApiResponse 包壳，需要自己 JSON.parse。
 */
export function uploadFile<TResponse>(opts: {
  path: string;
  filePath: string;
  fileFieldName?: string;
  formData?: Record<string, string | number | boolean>;
  timeoutMs?: number;
}): Promise<TResponse> {
  const config = getApiConfig();
  const session = getSession();
  const url = joinUrl(config.baseUrl, API_PREFIX, opts.path);
  const header: Record<string, string> = {};
  if (session?.token) {
    header.Authorization = `Bearer ${session.token}`;
  }

  return new Promise((resolve, reject) => {
    const task = wx.uploadFile({
      url,
      filePath: opts.filePath,
      name: opts.fileFieldName || "file",
      formData: opts.formData,
      header,
      timeout: opts.timeoutMs ?? 60_000,
      success: (res) => {
        if (res.statusCode < 200 || res.statusCode >= 300) {
          reject(
            new RequestError("http", `HTTP ${res.statusCode}`, {
              statusCode: res.statusCode,
              raw: res.data,
            }),
          );
          return;
        }
        try {
          // wx.uploadFile 总是返回字符串 body；这里手动 parse。
          const parsed = JSON.parse(res.data || "{}");
          resolve(unwrap<TResponse>(parsed, res.statusCode));
        } catch (err) {
          reject(
            new RequestError("http", "服务端返回非 JSON", {
              statusCode: res.statusCode,
              raw: res.data,
            }),
          );
        }
      },
      fail: (err) => {
        reject(new RequestError("network", err.errMsg || "上传失败", { raw: err }));
      },
    });
    // task 上有 onProgressUpdate；本期不强求暴露给业务层。
    void task;
  });
}

function sleep(ms: number): Promise<void> {
  return new Promise((resolve) => setTimeout(resolve, ms));
}
