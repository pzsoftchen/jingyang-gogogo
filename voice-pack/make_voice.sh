#!/usr/bin/env bash
# 重新生成角色语音包（macOS 自带 say + ffmpeg）
# 用法：  ./make_voice.sh            # 默认 Tingting 音色、语速 175
#        VOICE=Meijia RATE=160 ./make_voice.sh
# 产物：  ./mp3/0001.mp3 ... 拷到 SD 卡 mp3/ 目录即可
set -euo pipefail
cd "$(dirname "$0")"
VOICE="${VOICE:-Tingting}"
RATE="${RATE:-150}"   # 5 岁半：语速放慢
mkdir -p mp3
cd mp3

say_mp3() {  # $1=编号  $2=台词
  say -v "$VOICE" -r "$RATE" -o tmp.aiff "$2"
  ffmpeg -y -loglevel error -i tmp.aiff -ar 22050 -ac 1 -codec:a libmp3lame -b:a 32k "$1.mp3"
  printf "%s  %s\n" "$1" "$2"
}

# ---------- 0001~0020 角色「小豆」的台词（5 岁半：短句、慢速）----------
say_mp3 0001 "嗨！我是小豆豆，今天我们一起玩！"
say_mp3 0002 "该刷牙啦。上刷刷，下刷刷。"
say_mp3 0003 "刷完啦！牙齿白白的。"
say_mp3 0004 "玩具要回家啦，我们一起送它们回家吧。"
say_mp3 0005 "哇，玩具都回家啦！"
say_mp3 0006 "现在是故事时间，我们看一本书吧。"
say_mp3 0007 "故事讲完啦，你也要睡觉啦。"
say_mp3 0008 "今天两件事都做完啦，击个掌！"
say_mp3 0009 "还差一件，做完我们就睡觉。"
say_mp3 0010 "我困啦，晚安。"
say_mp3 0011 "你教教我嘛，这个我还不会。"
say_mp3 0012 "谢谢你教我！"
say_mp3 0013 "你好厉害，已经好几天啦！"
say_mp3 0014 "今天休息，去外面跑一跑吧！"
say_mp3 0015 "你生气啦？我陪着你。"
say_mp3 0016 "慢慢来，我等你。"
say_mp3 0017 "一、二、三，开始！"
say_mp3 0018 "我给你鼓掌！"
say_mp3 0019 "爸爸妈妈给你留了一句话。"
say_mp3 0020 "我在呢。"

# ---------- 0021~0025 里程碑 ----------
say_mp3 0021 "这几天你都自己做了，不用我提醒。"
say_mp3 0022 "这一整周，你每天都记得。"
say_mp3 0023 "你都不用我提醒了，我注意到了。"
say_mp3 0024 "你现在做得很自然，像是本来就会一样。"
say_mp3 0025 "我注意到，你今天没等我提醒，自己就去了。"

# ---------- 0031~0040 爸妈认可位（过程性表扬；建议换成你自己的录音）----------
say_mp3 0031 "你自己去刷牙了，没有等妈妈叫。"
say_mp3 0032 "你把玩具都放回箱子里了，我看到了。"
say_mp3 0033 "今天有点困，你还是做完了。"
say_mp3 0034 "你昨天说要做，今天真的做了。"
say_mp3 0035 "你把书放回架子上了，很整齐。"
say_mp3 0036 "这一周你每天都记得。"
say_mp3 0037 "你今天帮妈妈拿了东西，谢谢。"
say_mp3 0038 "我不催你，你自己决定什么时候开始。"
say_mp3 0039 "你做完没做完，妈妈都爱你。"
say_mp3 0040 "明天我们继续，慢慢来。"

# ---------- 0401~0420 彩蛋（完成时 7% 概率触发）----------
say_mp3 0401 "我今天在你睡觉的时候，偷偷数了星星。"
say_mp3 0402 "你知道吗，我最喜欢你叫我的名字。"
say_mp3 0403 "我刚才打了个喷嚏，把自己吓了一跳。"
say_mp3 0404 "我发现一个秘密：牙膏是甜的。"
say_mp3 0405 "我今天学会了一个新动作，你看。"
say_mp3 0406 "如果我能变成一只动物，我想变成一只熊。"
say_mp3 0407 "你猜我今天看到什么？我看到一片叶子在转圈。"
say_mp3 0408 "我数到三，你就长大一岁。一、二、三——我骗你的。"
say_mp3 0409 "我今天有点困，但是我想等你。"
say_mp3 0410 "你知道我怎么来的吗？我是被你的问题叫醒的。"
say_mp3 0411 "我偷偷告诉你：我最怕打雷。"
say_mp3 0412 "我今天给自己取了个外号，叫小豆豆侠。"
say_mp3 0413 "你笑起来的时候，我的屏幕会变亮。"
say_mp3 0414 "我昨天做了一个梦，梦见我们一起去海边。"
say_mp3 0415 "我有一个小秘密，但是现在还不能说。"
say_mp3 0416 "你今天有没有觉得，我有点不一样？"
say_mp3 0417 "我给你唱首歌吧，啦啦啦——好了，唱完了。"
say_mp3 0418 "我喜欢你按下我的时候，那个声音。"
say_mp3 0419 "我哪儿也不去，就在这儿等你。"
say_mp3 0420 "今天也是很好的一天，谢谢你陪我。"

# ---------- 0501~0503 完成音效（1.85 走 I2S；DevKitC 用蜂鸣器）----------
chime() {  # $1=文件名  $2=ffmpeg 参数  $3=滤镜
  ffmpeg -y -loglevel error $2 -filter_complex "$3" -ar 22050 -ac 1 -b:a 32k "$1.mp3"
}
chime 0501 '-f lavfi -i sine=frequency=1320:duration=0.10 -f lavfi -i sine=frequency=1760:duration=0.12' \
      '[0:a][1:a]concat=n=2:v=0:a=1,afade=t=out:st=0.16:d=0.06'
chime 0502 '-f lavfi -i sine=frequency=1760:duration=0.08 -f lavfi -i sine=frequency=1320:duration=0.08 -f lavfi -i sine=frequency=1980:duration=0.12' \
      '[0:a][1:a][2:a]concat=n=3:v=0:a=1,afade=t=out:st=0.22:d=0.06'
chime 0503 '-f lavfi -i sine=frequency=990:duration=0.09 -f lavfi -i sine=frequency=1480:duration=0.14' \
      '[0:a][1:a]concat=n=2:v=0:a=1,afade=t=out:st=0.17:d=0.06'

rm -f tmp.aiff
echo "完成：$(ls -1 *.mp3 | wc -l | tr -d ' ') 个文件，共 $(du -sh . | cut -f1)"
