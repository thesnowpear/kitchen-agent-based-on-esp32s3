from __future__ import annotations

from typing import Any, Dict, Literal
from uuid import uuid4

from fastapi import FastAPI
from pydantic import BaseModel, Field


app = FastAPI(title="Fridge Spirit AI Adapter Mock", version="0.1.0")


TaskType = Literal[
    "chat_assist",
    "recognize_ingredients",
    "inventory_parse",
    "recipe_generate",
    "shopping_list_generate",
    "reminder_explain",
    "voice_intent_parse",
]


class AIJobRequest(BaseModel):
    task_type: TaskType
    request_id: str = Field(min_length=1)
    device_id: str = Field(min_length=1)
    local_snapshot_version: int = 0
    input: Dict[str, Any] = Field(default_factory=dict)
    context_refs: Dict[str, Any] = Field(default_factory=dict)


class AIJobResponse(BaseModel):
    job_id: str
    request_id: str
    task_type: TaskType
    status: str
    result_json: Dict[str, Any]
    confidence: float
    needs_confirmation: bool
    prompt_version: str
    model_provider: str
    model_name: str


def build_mock_result(task_type: TaskType) -> tuple[Dict[str, Any], float, bool]:
    if task_type == "recipe_generate":
        return (
            {
                "schema_version": 1,
                "type": "recipe_generate",
                "recipe": {
                    "name": "番茄鸡蛋汤",
                    "use_inventory": ["番茄", "鸡蛋"],
                    "missing": ["葱花，可选"],
                    "time_minutes": 15,
                    "steps": ["番茄切块", "少油炒番茄", "加水煮开", "淋入蛋液并调味"],
                },
            },
            0.86,
            False,
        )
    if task_type == "recognize_ingredients":
        return (
            {
                "schema_version": 1,
                "type": "recognize_ingredients",
                "candidates": [
                    {
                        "name": "番茄",
                        "quantity": "约2-3个",
                        "confidence": 0.82,
                        "doubt": "Mock Provider 未接入真实图片",
                    }
                ],
                "confirm_fields": ["名称", "数量", "保质期", "位置"],
            },
            0.82,
            True,
        )
    if task_type == "shopping_list_generate":
        return (
            {
                "schema_version": 1,
                "type": "shopping_list_generate",
                "suggested": ["绿叶菜", "面条"],
                "optional": ["葱花", "低脂酸奶"],
            },
            0.8,
            True,
        )
    return (
        {
            "schema_version": 1,
            "type": task_type,
            "reply": "这是 Mock AI Adapter 返回的结构化结果；正式结果仍需规则校验和用户确认。",
        },
        0.78,
        task_type != "chat_assist",
    )


@app.post("/api/v1/ai/jobs", response_model=AIJobResponse)
def create_ai_job(job: AIJobRequest) -> AIJobResponse:
    result, confidence, needs_confirmation = build_mock_result(job.task_type)
    return AIJobResponse(
        job_id=f"mock_{uuid4().hex[:12]}",
        request_id=job.request_id,
        task_type=job.task_type,
        status="completed",
        result_json=result,
        confidence=confidence,
        needs_confirmation=needs_confirmation,
        prompt_version="fridge-spirit-dev-v1",
        model_provider="mock",
        model_name="mock-provider",
    )


@app.get("/health")
def health() -> Dict[str, str]:
    return {"status": "ok", "service": "ai_adapter_mock"}
