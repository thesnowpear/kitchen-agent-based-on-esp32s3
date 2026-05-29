# 冰箱小精灵微信小程序骨架

这是“冰箱小精灵”的微信小程序原生前端骨架，当前只包含页面、状态和接口封装，不绑定生产后端。

## 目录结构

```text
miniapp/
  app.json              小程序页面、窗口和 tabBar 配置
  app.ts                全局状态、登录态和后端配置加载
  app.wxss              全局样式
  project.config.json   微信开发者工具项目配置
  sitemap.json          小程序索引配置
  config/               默认后端 API 配置
  services/             业务接口封装
  types/                页面和接口数据类型
  utils/                请求、存储和格式化工具
  pages/                登录、首页、设备绑定、库存、提醒、设置/隐私
```

## 用微信开发者工具打开

1. 打开微信开发者工具。
2. 选择“导入项目”。
3. 项目目录选择仓库下的 `miniapp/`。
4. AppID 可先选择测试号，或使用“无 AppID”模式预览页面骨架。
5. 进入后默认首页为 `pages/home/index`，启动时会静默 `wx.login`，失败时首页会显示重试入口。

## 本地联调方式

1. 在“详情 > 本地设置”中勾选“不校验合法域名、web-view、TLS 版本以及 HTTPS 证书”，仅用于本地调试。
2. 打开小程序“设置/隐私”页，填写后端 API Base URL，例如：

```text
http://165.154.23.36:6005
```

3. 页面会把配置保存到本地缓存，后续请求通过 `utils/request.ts` 自动读取。
4. 当前预留接口路径：

```text
POST /api/v1/auth/wechat-login
GET  /api/v1/home/overview
GET  /api/v1/devices/primary
POST /api/v1/devices/bind
GET  /api/v1/inventory
POST /api/v1/inventory/refresh
GET  /api/v1/reminders
POST /api/v1/reminders/{id}/confirm
GET  /api/v1/settings
PUT  /api/v1/settings
```

## 当前页面

- 首页总览：展示设备在线状态、库存摘要、临期提醒和刷新入口。
- 设备绑定：支持输入绑定码和扫码读取绑定码。
- 库存：展示库存列表、临期状态和刷新按钮。
- 拍照入库：选择图片并上传到后端视觉识别，候选项可直接入库或编辑后入库。
- AI 对话：向后端发送聊天请求，设备在线时可走冰箱贴本地 AI，离线时云端兜底。
- 提醒：展示提醒队列并预留确认提醒接口。
- 设置/隐私：配置 API Base URL、同步策略和隐私开关。

## 说明

- 本骨架不包含第三方 UI 库和 npm 依赖，保持微信小程序原生轻量结构。
- 生产环境需要在微信公众平台配置合法请求域名，并将 `config/env.ts` 中的默认地址替换为正式服务。
- API Key、设备密钥等敏感信息不应写入小程序源码，后续应由后端或设备侧安全保存。
