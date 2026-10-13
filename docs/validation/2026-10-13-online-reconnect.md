# 中断连接与延迟供音

使用 0.3.3 对应的本机原生解码检查程序，读取独立生成的 30 秒 WAV 和 MP3。HTTP 服务第一次发送音频时主动切断连接，后续通过 Range 续传，并在数据块之间加入延迟。两种来源都实际发生断连和带偏移的重新请求；各 12 项原生检查通过，包括定位后 PCM 与连续解码一致、两秒缓存、提前结束判定、取消、实际音频输出及停止。

探针 SHA256：593d06af96c3a482ea07e5feda1635147f410bbbd2860cf74301d5cdbb6f08b3f。本机完整证据为 `artifacts/online-reconnect-1b4b3b964ac04d9e9899f18e9f33b0db/`。没有使用用户曲库，没有改变产品播放代码或重新发行安装包。

先运行 `tools/verify_online_audio.py --output artifacts/online-validation-truncated-preparation` 生成作者夹具，再运行 `tools/verify_online_reconnect.py`。探针和随附媒体工具需先按现有构建流程准备；新证据写入独立目录，原报告不覆盖。

这组结果涵盖短暂连接中断和延迟数据块，不代表长时间断网、链接过期、实体跨网络设备或全部国内外平台验收。
