# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

This repository contains three sub-projects for RTSP streaming on Windows:

- **RTSP-Player** — SDL2 + FFmpeg RTSP streaming player. Pulls a stream from an RTSP server, decodes, and renders video/audio.
- **RTSP-Pusher** — Desktop screen + audio capture and encoding, pushing to an RTSP server (e.g. MediaMTX).
- **RTSP-Server** — Placeholder directory (contains only a separate `.git` and `README.md`). The actual RTSP server is external (MediaMTX or similar).

The Player and Pusher are independent applications that communicate indirectly through an external RTSP server. There is no shared code between sub-projects.

## Common Build Pattern

All sub-projects share the same build conventions:

- **Build system**: CMake 3.16+, **compiler**: MSVC 2017, **language**: C++17 (`/utf-8`)
- **Dependencies**: Pre-built FFmpeg and SDL2 libraries in each sub-project's `3rd/` directory
- **x86 (32-bit)**: FFmpeg 4.2.x dev libs (older ABI, requires `avcodec_register_all()`)
- **x64 (64-bit)**: FFmpeg 7.x dev libs (newer ABI, `AVChannelLayout` struct, auto codec registration)
- **Output**: `bin/MSVC2017_${ARCH}_<Config>/<executable>.exe`; DLLs copied there via CMake `POST_BUILD`

Each sub-project has its own build trees and `3rd/` directory — no shared build infrastructure.

## Shared Architecture Patterns

Despite no shared code, both sub-projects follow the same architectural patterns:

- **Single-process, multi-threaded** SDL2 applications with a main-thread event loop (`SDL_PollEvent` + `SDL_Delay(1)`)
- **Atomic state machines** with explicit transitions (compare-exchange pattern), rejecting invalid transitions
- **Serial/generation mechanism** — a counter incremented on reconnect, propagated to every queue and worker; stale data with an older serial is discarded silently, enabling clean pipeline restart without manual flushing
- **SDL timer-driven events** — `SDL_AddTimer` callbacks push `SDL_USEREVENT` for periodic stats, reconnect scheduling, and auto-exit
- **3rd/ layout**: `3rd/SDL2/` and `3rd/FFmpeg/{x86|x64}/` per sub-project

## Known Issues

From integration testing (2026-07-09, ~2800s session):

1. **Pusher: Reconnect serial double-increment** — `PusherLifecycleManager` increments `m_serial` on both stop and start, causing serial to advance by 2 per reconnect instead of 1
2. **Pusher: Config vs help text mismatch** — `PusherConfig.h` defaults bitrate to 8000 kbps; `--help` output says 20000 kbps
3. **Pusher: Reconnect state not entered** — `PusherStateMachine` never enters `Recovering`/`Reconnecting` states during reconnect flow
4. **Player: PacketQueue overflow causes corruption** — Video queue overflow drops a single non-key H.264 packet while continuing to feed the decoder, breaking the reference chain and causing artifacts until the next IDR
5. **No test infrastructure** in either sub-project

## Pending Work

**Keyframe-aware PacketQueue overflow fix** (Player, high priority):

Modify `RTSP-Player/src/PacketQueue.*`, `StreamLifecycleManager.cpp`, and `VideoDecodeThread.cpp` to:
- Add keyframe-aware mode to video PacketQueue
- On overflow, clear the entire stale GOP instead of dropping a single non-key packet
- Enter `waitingForKeyframe` state, discarding non-key packets until the next IDR
- Set a `discontinuity` flag; `VideoDecodeThread` calls `avcodec_flush_buffers()` before feeding the recovery keyframe
- Converts corruption into at most one GOP of freeze (~1s at 30fps / GOP=30)

See `docs/session-memory-2026-07-09.md` for full diagnosis, verification procedure, and A/B test plan.

## Reference

- `RTSP-Player/CLAUDE.md` — detailed Player architecture, component tree, CLI reference, render loop, low-latency modes
- `RTSP-Pusher/CLAUDE.md` — detailed Pusher thread pipeline, CLI reference, GPU QSV path, FFmpeg version differences
- `docs/project-overview-logic.md` — cross-project logic overview (Chinese)
- `docs/session-memory-2026-07-09.md` — integration test session diagnosis and modification plan (Chinese)

## 禁令

1. 不画蛇添足：严格遵守三条禁令——不添加未被要求的内容，小修改不额外重构周边；不为不可能发生的情况做预防，仅在真实需求边界校验；三行重复内容，好过过早的抽象（避免为消灭冗余搞复杂系统）；禁止过度设计，如写简单功能时，不得额外搭建复杂框架。
2. 如实汇报：事情没做好，需明确说明并附上具体情况，未完成的步骤不得暗示已完成；事情做好后，不添加无必要的免责声明，核心目标是准确报告，而非防御性报告，杜绝“吹牛”和“过度谦虚”。
3. 独立思考：禁止外包理解，不得写“根据你的调查结果处理”等甩锅判断权的表述；需先消化所有信息、做出独立判断后，再给出明确方向；子任务描述需明确“做什么、为什么、已排除什么”，确保基于完整上下文独立决策。
4. 不猜测、不编造：子任务启动后，若对进展一无所知，禁止编造、预测结果；被询问进展时，仅能回复“还在处理中”，绝不进行任何猜测，避免产生幻觉。
5. 先看再改：硬规则——必须先完整读取文件内容，才能进行编辑操作，未读取就修改将被视为错误；修改文章、代码前，需先复述原文关键内容，确认理解无误后再执行修改，杜绝凭记忆、幻觉修改。
6. 规范沟通格式：禁止使用emoji、冗余话术，能用一句话说完的内容不使用三句；明确标点规则（如特定场景禁用冒号，避免内容截断）；输出需先说结论、后说理由，改变“先铺垫后说重点”的默认习惯，保证简洁、重点突出。
7. 当发现自己在：“解释而不是执行”、“猜测结果”、“未读取内容就修改”，必须立即停止并说明问题。