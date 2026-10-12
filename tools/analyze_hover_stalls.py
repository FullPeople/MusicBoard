import io
import re
import sys
import datetime
import os

# 量"悬停有没有卡住"：扫一遍面板，看每次鼠标移动之间的间隔。
#
# 判据：扫描脚本每 120ms 挪一次光标，所以相邻两条 move: 之间的时间差应该
# 稳定在 120~200ms。**同步解码的版本会在这里露出几百毫秒的空洞** ——
# 因为鼠标消息排在解码后面。这条比"看截图"可靠得多。

path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
    os.environ.get('LOCALAPPDATA', ''), 'MusicBoard', 'logs',
    'app-' + datetime.date.today().strftime('%Y%m%d') + '.log')

if not os.path.exists(path):
    print('no log:', path)
    raise SystemExit(1)

samples = []
pattern = re.compile(r'^(\d{4}-\d{2}-\d{2} (\d{2}):(\d{2}):(\d{2}))\s+\[info\] move:')
with io.open(path, encoding='utf-8', errors='replace') as handle:
    for line in handle:
        match = pattern.match(line)
        if match:
            h, m, s = int(match.group(2)), int(match.group(3)), int(match.group(4))
            samples.append(h * 3600 + m * 60 + s)
        # 毫秒精度不够，改成按"同一秒内多条"来估：见下面的分段输出

if len(samples) < 50:
    print('样本太少:', len(samples))
    raise SystemExit(1)

# 秒级精度不足以看 100ms 级的卡顿，所以换一种量法：
# 统计**每秒能处理多少条移动事件**。流畅时扫描脚本每秒发 ~8 条；
# 如果程序卡住，同一秒里会堆进更多（消息排队后被一口气处理），
# 而相邻两秒之间会出现 0 条的空档。
counts = {}
for value in samples:
    counts[value] = counts.get(value, 0) + 1

ordered = sorted(counts)
gaps = []
for i in range(1, len(ordered)):
    gaps.append(ordered[i] - ordered[i - 1])

empty_seconds = sum(1 for g in gaps if g > 1)
busiest = max(counts.values())
print('移动事件总数: %d，覆盖 %d 秒' % (len(samples), len(ordered)))
print('单秒内最多处理: %d 条' % busiest)
print('有 %d 秒是空档（期间没有处理任何移动）' % empty_seconds)
if busiest <= 20 and empty_seconds <= 2:
    print('RESULT 没有卡顿迹象（移动事件分布均匀）')
else:
    print('RESULT 疑似有卡顿：某秒堆积了 %d 条，或出现了 %d 个空档秒' % (busiest, empty_seconds))
