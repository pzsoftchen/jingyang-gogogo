// 主机端测试：服务器同步的"完成"状态要双向生效。
// 家长页撤回后，设备上的勾选必须跟着消失；但离线补记还没上报时不能清。
#include <cstdio>

#include "../habit_coach/habit_sync.h"

static int failures = 0;

#define CHECK_EQ(a, b)                                                     \
  do {                                                                     \
    auto _a = (a);                                                         \
    auto _b = (b);                                                         \
    if (!(_a == _b)) {                                                     \
      std::printf("FAIL %s:%d: %s == %s (%d != %d)\n", __FILE__, __LINE__, \
                  #a, #b, (int)_a, (int)_b);                               \
      failures++;                                                          \
    }                                                                      \
  } while (0)

int main() {
  // 服务器说完成 → 设备勾上
  CHECK_EQ(habit_done_from_server(false, 1, false), true);
  CHECK_EQ(habit_done_from_server(false, 1, true), true);

  // 服务器说没完成（家长页撤回了）→ 设备要清掉勾选
  CHECK_EQ(habit_done_from_server(true, 0, false), false);

  // 离线补记还在队列里、服务器还不知道 → 保留本地勾选，别让星星闪没
  CHECK_EQ(habit_done_from_server(true, 0, true), true);

  // 本地本来就没完成 → 保持没完成
  CHECK_EQ(habit_done_from_server(false, 0, false), false);

  if (failures) {
    std::printf("%d habit_sync test(s) failed\n", failures);
    return 1;
  }
  std::printf("all habit_sync tests passed\n");
  return 0;
}
