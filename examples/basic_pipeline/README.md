# Basic Pipeline Example

这个 example 是最小接入示例，重点是演示 API 使用顺序，而不是板级音频链路。

这个 case 是一个完整的独立工程，工程根目录在 `examples/basic_pipeline/`，应用入口在 `examples/basic_pipeline/main/app_main.c`。

它展示的核心流程是：

```c
elastic_pcm_buffer_create()
elastic_pcm_buffer_set_pipeline()
elastic_pcm_buffer_start()
elastic_pcm_buffer_session_begin()
elastic_pcm_buffer_push()
elastic_pcm_buffer_session_end()
```

## 适合什么时候看

- 你要快速接入这个组件
- 你想理解 session begin / end 的语义
- 你准备自己接 websocket / socket / TTS 回调
- 你只需要 callback 输出模型，不需要 Korvo-2 示例
