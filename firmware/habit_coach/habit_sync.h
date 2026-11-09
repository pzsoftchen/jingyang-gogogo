// 服务器同步来的"今天是否完成"怎么落到设备本地状态。
//
// 为什么不能直接 done = (server == 1)：
//   断网时孩子完成的习惯会进本地上报队列（queue）。WiFi 恢复后先 sync 再
//   flushQueue，这时服务器还不知道这件事，会回 done=0——直接清掉的话，
//   孩子刚点亮的星星会突然消失十几秒。
#pragma once

// local_done: 设备本地当前状态；server_done: 服务器 H= 行里的 0/1；
// queued: 这个习惯还有未上报的本地打卡。
static inline bool habit_done_from_server(bool local_done, int server_done,
                                          bool queued) {
  if (server_done == 1) return true;
  if (queued) return local_done;
  return false;
}
