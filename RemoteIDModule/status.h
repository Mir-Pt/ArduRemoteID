/*
  状态 JSON 接口声明
  生成包含当前 RID 模块完整状态的 JSON 字符串，供 Web 界面 AJAX 轮询使用。
*/
#pragma once

// 生成当前模块状态的 JSON 字符串（包含 BasicID/Location/System 等所有 RID 字段）
String status_json(void);

