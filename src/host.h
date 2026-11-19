/* host.h — 宿主初始化（src/main.c）
 *
 * 抽出来的理由：裸名谓词 cfunc 的清单是 compile.ta `bare_cfunc` 白名单的
 * 宿主侧契约。
 */
#ifndef TA_HOST_H
#define TA_HOST_H

#include "ta.h"

/* vm_new + 裸名谓词 + 静态 C 模块注册 + vm_seal_host_modules。
 * 不做 proc_new（两个宿主的 entry 建法不同）。 */
VM *ta_host_init(void);

/* NWORKERS 环境变量的应用（宿主各调一次，在 ta_host_init 之后）。 */
void ta_host_nworkers(VM *vm);

#endif /* TA_HOST_H */