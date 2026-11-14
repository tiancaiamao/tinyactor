#!/usr/bin/env node
// scripts/verify-lessons.mjs — 逐课跑 docs/tour-lessons.js 里的代码，
// 和每课的 expected 逐行比对。跑两条路：
//
//   native — `tinyactor run`（每课一颗新进程）
//   wasm   — docs/wasm/*.js 里的真产物，也就是 Playground 在浏览器里跑的那份
//            （同一个实例反复 callMain，和页面一样）
//
// 两条都跑的理由：课程是给浏览器里的 WASM 编译器用的，只测原生会让
// docs/wasm 悄悄过期——历史上它就这么烂了一个月（26 课里有课直接编译不过）。
//
//   node scripts/verify-lessons.mjs            # 跑全部课程
//   node scripts/verify-lessons.mjs 名字 ...   # 只跑指定 id
//
// 判据（和 Playground 一致）：教学输出走 stdout。**崩溃报告（`** CRASH …` /
// 紧跟的 `   at …`）不算**：它带 pid 与帧号，是运行时细节而非教学内容，课程
// 正文里用一句话交代即可。除崩溃报告外 stderr 必须为空——那说明课程代码在
// 报错而不是在演示语言。
//
// 课程数据的取法：docs/tour-lessons.js 里 `const LESSONS = [……];` 之间是
// 严格 JSON（不给 JSON 加注释，注释写在这个文件里），这里按标记切出来解析。
import { readFileSync, writeFileSync, mkdirSync } from 'node:fs';
import { spawnSync } from 'node:child_process';
import { createRequire } from 'node:module';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = dirname(dirname(fileURLToPath(import.meta.url)));
const LESSONS_JS = join(ROOT, 'docs/tour-lessons.js');
const WORK_DIR = join(ROOT, 'build', 'tour-check');
const WASM_JS = join(ROOT, 'docs/wasm/tinyactor-vm.js');
const DRIVER = 'vendor/tinyactor/lib/bootstrap.tabc'; // wasm 里的编译器路径

// 崩溃报告的两行形态（src/scheduler.c 的 proc_die 输出）。
const CRASH_LINE = /^\*\* CRASH /;
const CRASH_AT = /^   at /;

function loadLessons() {
  const src = readFileSync(LESSONS_JS, 'utf8');
  const start = src.indexOf('[', src.indexOf('\nconst LESSONS ='));
  const end = src.lastIndexOf('];');
  if (start < 0 || end < 0) throw new Error(`${LESSONS_JS}: 找不到 const LESSONS = [ ... ];`);
  const lessons = JSON.parse(src.slice(start, end + 1));

  const seen = new Set();
  for (const l of lessons) {
    for (const key of ['id', 'section', 'title', 'text', 'code', 'expected']) {
      if (typeof l[key] !== 'string' || l[key] === '') {
        throw new Error(`课程 ${JSON.stringify(l.id)} 缺少字段 ${key}`);
      }
    }
    if (seen.has(l.id)) throw new Error(`课程 id 重复：${l.id}`);
    seen.add(l.id);
  }
  return lessons;
}

// ---- native: compile + run 一趟（tinyactor run 就是这条路径） --------------
function nativeRunner() {
  mkdirSync(WORK_DIR, { recursive: true });
  return (lesson) => {
    const file = join(WORK_DIR, `${lesson.id}.ta`);
    writeFileSync(file, lesson.code);
    const res = spawnSync(join(ROOT, 'tinyactor'), ['run', file], {
      cwd: ROOT,
      encoding: 'utf8',
      timeout: 120000,
    });
    if (res.error) throw res.error;
    return { rc: res.status, stdout: res.stdout ?? '', stderr: res.stderr ?? '' };
  };
}

// ---- wasm: 浏览器里那份产物，一个实例反复 callMain（同 playground.html）----
async function wasmRunner() {
  const createTavm = createRequire(import.meta.url)(WASM_JS);
  const out = [];
  const err = [];
  const mod = await createTavm({
    noInitialRun: true,
    print: (s) => out.push(String(s)),
    printErr: (s) => err.push(String(s)),
  });
  // 预热：单线程 wasm 起来时 stderr 有一句 io poller 缺席的提示，那是模块级
  // 噪声、不属于任何一课 —— 先跑一遍内嵌的 hello.tabc 把它吃掉。
  mod.callMain(['-q', 'hello.tabc']);
  out.length = 0;
  err.length = 0;

  const clean = (f) => {
    try {
      mod.FS.unlink(f);
    } catch {
      /* 首次不存在，正常 */
    }
  };

  return (lesson) => {
    out.length = 0;
    err.length = 0;
    clean('user.ta');
    clean('user.tabc'); // 上一课的产物必须清掉，否则编译失败也会被跑起来
    mod.FS.writeFile('user.ta', lesson.code);
    if (mod.callMain(['-q', DRIVER, 'user.ta', 'user.tabc', '']) !== 0) {
      return { rc: 1, stdout: out.join('\n'), stderr: err.join('\n') };
    }
    out.length = 0; // 编译期的进度/告警不属于课程输出
    err.length = 0;
    return { rc: mod.callMain(['-q', 'user.tabc']), stdout: out.join('\n'), stderr: err.join('\n') };
  };
}

const lines = (s) => s.replace(/\n+$/, '').split('\n').filter((l) => l !== '');
const notCrash = (l) => !CRASH_LINE.test(l) && !CRASH_AT.test(l);

function compare(lesson, { rc, stdout, stderr }) {
  if (rc !== 0) {
    const out = [...lines(stdout), ...lines(stderr)];
    return `退出码 ${rc}：\n    ${out.join('\n    ')}`;
  }
  const noise = lines(stderr).filter(notCrash);
  if (noise.length) return `stderr 不该有内容：\n    ${noise.join('\n    ')}`;
  const got = lines(stdout).filter(notCrash).join('\n');
  const want = lesson.expected.replace(/\n+$/, '');
  if (got !== want) {
    return `输出不符\n      期望: ${JSON.stringify(want)}\n      实际: ${JSON.stringify(got)}`;
  }
  return null;
}

async function runAll(label, runner, lessons) {
  let failed = 0;
  console.log(`--- ${label}`);
  for (const lesson of lessons) {
    const problem = compare(lesson, runner(lesson));
    if (problem) {
      failed++;
      console.log(`FAIL ${label} ${lesson.id}\n    ${problem}`);
    } else {
      console.log(`ok   ${lesson.id}`);
    }
  }
  return failed;
}

const only = process.argv.slice(2);
const lessons = loadLessons().filter((l) => only.length === 0 || only.includes(l.id));
if (lessons.length === 0) {
  console.error(`没有匹配的课程：${only.join(', ')}`);
  process.exit(1);
}

const failedNative = await runAll('native', nativeRunner(), lessons);
const failedWasm = await runAll('wasm', await wasmRunner(), lessons);

const failed = failedNative + failedWasm;
console.log(`\n${lessons.length * 2 - failed}/${lessons.length * 2} 课次通过（native + wasm）`);
process.exit(failed === 0 ? 0 : 1);