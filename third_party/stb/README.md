# stb_vorbis

Ogg Vorbis 解码器，单文件、公共领域。

## 来源与锁定

| 项 | 值 |
| --- | --- |
| 上游 | https://github.com/nothings/stb |
| 文件 | `stb_vorbis.c` |
| 版本 | v1.22（文件头自述） |
| 作者 | Sean Barrett（原始版本 2007） |
| 许可 | 公共领域 / MIT 双许可，见文件内声明 |
| SHA256 | `4c7cb2ff1f7011e9d67950446b7eb9ca044f2e464d76bfbb0b84dd2e23e65636` |
| 拉取日期 | 2026-09-16 |

按项目纪律，第三方依赖必须锁到具体版本并记录哈希。更换版本时同时更新本文件与
`docs/validation/` 下对应那篇记录。

## 为什么需要它

Media Foundation 在这台机器上解不了 Ogg Vorbis：

```
[低][宁静] 平和 温暖.ogg
  FAIL  fromByteStream hr=0xC00D36C4     # MF_E_BYTESOURCE_UNKNOWN_TYPE
```

而真实曲库里 ogg 占 137/491（28%）。只靠 Media Foundation 会丢掉超过四分之一的曲目。

## 刻意关掉的两组 API

编译时定义了两个宏（见 `CMakeLists.txt`）：

### `STB_VORBIS_NO_STDIO`

排除 `stb_vorbis_open_filename` 这类文件 API。它们内部走 `fopen` + `char*` 路径，
**在中文文件名上必然失败** —— 和 `char** argv`、`getenv` 是同一个坑。

本项目的做法是：自己用宽字符 Win32 API 把文件读进内存，再走
`stb_vorbis_open_memory`。这让那个坑在结构上不可能发生，而不是靠"记得别用那个函数"。

内存输入是可 seek 的，所以 `stb_vorbis_stream_length_in_samples` 能直接跳到末尾，
取时长不必整段解码。

### `STB_VORBIS_NO_PUSHDATA_API`

`stb_vorbis_open_pushdata` 那套流式接口本项目用不到。少一组 API 少一处出错面。

## 使用位置

只在 `src/audio/decoder_vorbis.cpp` 里引入，且以 `STB_VORBIS_HEADER_ONLY` 方式编译，
保证实现在独立的编译单元里只出现一次。

## 不参与警告即错误

该目标单独加了 `/w`。第三方代码不参与本项目的警告标准 ——
要求别人的代码满足我们的 `/W4 /WX` 没有意义，只会带来无法修复的噪音。
