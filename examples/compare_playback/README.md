# Compare Playback Example

这个 example 用来对比两条播放路径：

- `direct`
  收到一帧就直接送到播放设备，不做弹性缓存修正

- `elastic`
  先进入 `esp_elastic_pcm_buffer`，再根据当前水位做平滑输出

## 输入条件

- 板级：`ESP32-S3-Korvo-2`
- 文件：`/sdcard/jb.pcm`
- PCM：`16 kHz / 16-bit / mono`
- 帧长：`60 ms`

## 示例特性

- 从 SD 卡读取 PCM
- 使用宏模拟上下抖动范围
- console 切换 `direct` / `elastic`
- 支持 `stop discard`
- 支持 `stop drain`

## 常用命令

```text
mode direct
mode elastic
play /sdcard/jb.pcm
stop
status
```

上电后默认会自动以 `direct` 模式开始播放，方便先听到“无弹性缓存”的基线效果，再切到 `elastic` 重新测试。

这个 case 是一个完整的独立工程，工程根目录就在 `examples/compare_playback/`，实际应用入口在 `examples/compare_playback/main/app_main.c`。
