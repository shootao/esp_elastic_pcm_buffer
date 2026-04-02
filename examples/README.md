# Examples

`esp_elastic_pcm_buffer/examples` 下面现在有两个独立 case：

## 1. `examples/compare_playback`

这是推荐先跑的“对比测试 example”。

它的目标不是演示最少 API，而是让你直接听到、看到两种模式的差异：

- `direct` 模式：收到一包播一包，不做弹性缓存修正
- `elastic` 模式：进入 `esp_elastic_pcm_buffer`，按水位和推荐速度做平滑输出

这个示例的特点：

- 基于 `ESP32-S3-Korvo-2`
- 从 SD 卡读取 `/sdcard/jb.pcm`
- 输入 PCM 格式固定为 `16 kHz / 16-bit / mono`
- 每帧 `60 ms`
- 用宏模拟上下抖动范围
- 提供 console 命令切换 `direct` / `elastic`
- 支持 `stop discard` 和 `stop drain`

适合你要做这些事情时使用：

- 验证弹性缓存有没有效果
- 听 direct 和 elastic 的差别
- 调整水位和速度参数
- 验证 stop mode 行为

这个 case 本身是一个独立工程，入口代码在 `examples/compare_playback/main/app_main.c`。

## 2. `examples/basic_pipeline`

这是最小 pipeline example。

它只关注 API 调用顺序，不依赖 Korvo-2、SD 卡和实际扬声器输出，重点是演示下面这条使用链路：

```c
elastic_pcm_buffer_create()
elastic_pcm_buffer_set_pipeline()
elastic_pcm_buffer_start()
elastic_pcm_buffer_session_begin()
elastic_pcm_buffer_push()
elastic_pcm_buffer_session_end()
```

适合你要做这些事情时使用：

- 看最小接入方式
- 快速理解 session begin / end 语义
- 参考 callback 输出模式
- 自己接入 websocket / socket / TTS 流时当模板

这个 case 也是一个独立工程，入口代码在 `examples/basic_pipeline/main/app_main.c`。

## 怎么选

如果你现在想“做功能验证 / 听效果 / 调参数”，优先看 `examples/compare_playback`。

如果你现在想“看 API 怎么接 / 最小代码怎么写”，优先看 `examples/basic_pipeline`。
