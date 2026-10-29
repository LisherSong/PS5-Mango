# cJSON 1.7.19 (vendored, unmodified)

- 上游: https://github.com/DaveGamble/cJSON （MIT，13k stars，活跃维护）
- 版本: v1.7.19（2025-09-09 发布），tag tarball
- 文件: cJSON.c / cJSON.h / LICENSE —— **原样未改**（与 libsmb2/sqlite3
  等同级 vendored 模式一致：上游代码不吃我们的 -Werror，编译时 -w 放松）
-  sha256(cJSON.c): 见 VENDORED-SHA.txt
-  sha256(cJSON.h): 见 VENDORED-SHA.txt
- 用途: 云盘驱动（P3）解析四家网盘 API 的 JSON 响应。自带的
  src/json_util.c 只有 strbuf + json_escape（序列化侧），无解析器。
- 选取记录（2026-10-25，用户规矩「遇问题先查 GitHub」）：查 cJSON
  releases/license 后选 MIT + 单文件 + 零依赖，两平台（prospero-clang /
  cc）直接编。
