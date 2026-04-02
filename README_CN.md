# esp_elastic_pcm_buffer

`esp_elastic_pcm_buffer` 是一个面向实时 PCM 音频流的弹性缓存组件。

它的目标不是单纯“多存几包数据”，而是在网络到达节奏不稳定时，尽量把播放侧维持在一个更平滑、更可控的状态：

- 数据来得忽快忽慢时，先做缓存保护
- 缓存偏低时，适当放慢消费速度
- 缓存偏高时，适当加快消费速度
- 会话结束时，支持直接丢弃尾包，或者直接排空尾包

这个组件很适合下面这类场景：

- websocket 语聊
- 云端 TTS 流式下发 PCM
- 局域网或互联网 PCM 实时播放
- 有轻微抖动、但又不想上完整 RTP/JB 栈的场景

## 核心功能

`esp_elastic_pcm_buffer` 当前提供下面这些能力：

- 固定容量的 PCM 帧缓存
- `start / low / target / high` 四级水位控制
- 自动 `prefill`
- underflow / overflow 统计
- 根据当前水位计算推荐播放速度
- 接入 `esp_audio_effects` 里的 sonic 做轻微变速
- 支持内部 consumer task + callback 输出模式
- 支持 session begin / end 这种会话型控制

## 它解决的是什么问题

假设播放侧理想情况下每 `60 ms` 消费一帧 PCM，但网络并不会刚好每 `60 ms` 到一帧：

- 有时候 `30 ms` 就来一帧
- 有时候要 `90 ms`
- 偶尔还会连续慢几次

如果收到就直接播放，很容易出现：

- 一会儿快，一会儿断
- 缓存瞬间见底
- 或者缓存越积越多，整体时延越来越大

`esp_elastic_pcm_buffer` 的思路是：

1. 先把数据存起来
2. 通过水位判断当前是“偏低、正常、偏高”
3. 再决定是否正常播、慢一点播、快一点播

所以它更像是一个“带水位策略和轻微变速能力的 PCM 弹性缓存层”。

## 工作模式

这个组件从使用方式上，可以理解为两层模式。

### 1. 低层手动模式

这层是最基础的缓存能力：

- 上层调用 `elastic_pcm_buffer_push()` 塞数据
- 上层自己决定什么时候 `elastic_pcm_buffer_pop()`

适合：

- 你自己已经有播放时钟
- 你想自己控制何时消费
- 你只想复用缓存和水位判断，不想用内部 task

### 2. 高层 pipeline 模式

这层是当前更推荐的模式：

- 上层持续 `elastic_pcm_buffer_push()`
- 组件内部自己起 consumer task
- 组件内部完成水位判断、sonic 处理、回调输出

对应 API 路径一般是：

```c
elastic_pcm_buffer_create()
elastic_pcm_buffer_set_pipeline()
elastic_pcm_buffer_start()
elastic_pcm_buffer_session_begin()
elastic_pcm_buffer_push()
```

适合：

- websocket 语聊
- 流式 TTS
- 边收边播 PCM
- 上层只想“喂数据”，不想自己写一套消费时钟

## 水位模式

组件内部最核心的是水位控制。

配置里有这四个值：

```c
size_t start_watermark;
size_t low_watermark;
size_t target_watermark;
size_t high_watermark;
```

它们分别表示：

- `start_watermark`
  至少积累到多少帧，才允许正式开始消费

- `low_watermark`
  低水位警戒线。低于它说明缓存已经偏低

- `target_watermark`
  理想巡航点。系统希望长期稳定在这个附近

- `high_watermark`
  高水位警戒线。高于它说明积压偏多，端到端时延在上涨

### 水位图

```text
0 ---- low ---- target ---- high ---- capacity
      ^          ^           ^
      |          |           |
   偏低区      理想区      偏高区

start_watermark:
只决定“什么时候允许开始播”
```

## 内部状态模式

当前公开状态枚举是：

- `ELASTIC_PCM_BUFFER_STATE_PREFILLING`
- `ELASTIC_PCM_BUFFER_STATE_NORMAL`
- `ELASTIC_PCM_BUFFER_STATE_LOW_WATER`
- `ELASTIC_PCM_BUFFER_STATE_HIGH_WATER`
- `ELASTIC_PCM_BUFFER_STATE_UNDERFLOW`

可以这样理解：

### `PREFILLING`

含义：

- 还在攒包
- 虽然已经开始收数据，但还不应该正式播

触发：

- 刚启动
- flush 之后
- buffer 被读空之后重新进入预填充

### `NORMAL`

含义：

- 当前水位比较健康
- 可以按正常节奏播放

### `LOW_WATER`

含义：

- 当前缓存偏低
- 有 underflow 风险

动作倾向：

- sonic 速度往 `< 1.0x` 调一点
- 让消费稍微慢下来，等缓存补回来

### `HIGH_WATER`

含义：

- 当前缓存偏高
- 累积时延在变大

动作倾向：

- sonic 速度往 `> 1.0x` 调一点
- 让播放稍微追赶，把积压慢慢吃掉

### `UNDERFLOW`

含义：

- 当前已经没有可消费数据
- buffer 见底

结果：

- 回到 prefill 逻辑
- 等新数据重新积累

## 速度补偿模式

`esp_elastic_pcm_buffer` 不是做大幅 time-stretch，它只做“轻微调速”。

当前思路是：

- 水位偏低时，推荐速度低于 `1.0x`
- 水位接近目标值时，推荐速度接近 `1.0x`
- 水位偏高时，推荐速度高于 `1.0x`

也就是说：

- 偏低：慢一点播，保护缓存
- 偏高：快一点播，减少时延

这部分实际由 sonic 完成。

## 会话控制模式

这个组件不是“只创建一次然后永远不停”的死板模型，它更适合会话式使用。

例如语聊场景里，一轮说话结束后，过几分钟又开始下一轮：

- 这一轮结束：调用 `elastic_pcm_buffer_session_end()`
- 下一轮开始：调用 `elastic_pcm_buffer_session_begin()`

这样就不会把上一轮残留的尾数据带到下一轮。

## 结束模式

`elastic_pcm_buffer_session_end()` 支持两种停止方式。

### `ELASTIC_PCM_BUFFER_STOP_MODE_DISCARD`

含义：

- 立即丢掉 buffer 中剩余数据
- sonic 也不再继续处理尾包

适合：

- 当前对话已经结束
- 旧尾音不想再播
- 下一轮内容更重要

### `ELASTIC_PCM_BUFFER_STOP_MODE_DRAIN_DIRECT`

含义：

- 把 buffer 里剩余数据直接吐出去
- 这时不再走水位控制，也不再做 sonic 调整

适合：

- 虽然结束了，但还想把尾音直接播完

## Example 在哪里

示例代码在 [examples](examples)。

当前主要有两个 example：

- [examples/compare_playback](examples/compare_playback)
  这是“对比测试 example”，用于比较 `direct` 和 `elastic` 两种播放路径的效果。它基于 Korvo-2，从 SD 卡读取 `/sdcard/jb.pcm`，并且内置了抖动模拟和 console 控制。

- [examples/basic_pipeline](examples/basic_pipeline)
  这是“最小 pipeline example”，用于演示最基础的接入顺序和 callback 输出方式，不强调板级音频链路。

如果你是第一次接这个组件：

- 想听效果、调参数、看 direct / elastic 差异，先看 [examples/compare_playback](examples/compare_playback)
- 想快速理解 API 调用顺序，先看 [examples/basic_pipeline](examples/basic_pipeline)
