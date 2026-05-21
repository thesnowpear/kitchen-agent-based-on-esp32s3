# AI Adapter Mock

这是“冰箱小精灵”云端 AI Adapter 的最小 Mock 参考实现，用于后续把端侧 `ai_context` 输出接到云端任务接口。

当前目标：

- 固定 `/api/v1/ai/jobs` 请求/响应形态。
- 保留 Mock Provider，比赛现场网络或额度异常时可兜底。
- 不在 ESP32-S3 固件中保存第三方模型 API Key。

运行方式：

```bash
cd cloud/ai_adapter_mock
python -m venv .venv
.venv\Scripts\activate
pip install -r requirements.txt
uvicorn main:app --host 0.0.0.0 --port 8088
```

测试：

```bash
curl -X POST http://127.0.0.1:8088/api/v1/ai/jobs ^
  -H "Content-Type: application/json" ^
  -d "{\"task_type\":\"recipe_generate\",\"request_id\":\"demo\",\"device_id\":\"dev_main_01\",\"local_snapshot_version\":1,\"input\":{\"text\":\"今晚做什么\"},\"context_refs\":{\"inventory\":\"local_snapshot\"}}"
```
