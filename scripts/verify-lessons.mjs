#!/usr/bin/env node
// scripts/verify-lessons.mjs — 逐课跑 docs/tour-lessons.js 里的代码，
// 和每课的 expected 逐行比对。
//
//   node scripts/verify-lessons.mjs            # 跑全部课程
//   node scripts/verify-lessons.mjs 名字 ...   # 只跑指定 id
//
// 判据（和 Playground 一致）：`tinyactor run` 的 stdout 就是用户在输出区
// 看到的教学输出。**崩溃报告（`** CRASH …` / 紧跟的 `   at …`）不算**：
// 它带 pid 与帧号，是运行时细节而非教学内容，课程正文里用一句话交代即可。
// 除崩溃报告外 stderr 必须为空——那说明课程代码在报错而不是在演示语言。
//
// 课程数据的取法：docs/tour-lessons.js 里 `const LESSONS = [……];` 之间是
// 严格 JSON（不给 JSON 加注释，注释写在这个文件里），这里按标记切出来解析。
import { readFileSync, writeFileSync, mkdirSync } from 'node:fs';
import { spawnSync } from 'node:child_process';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = dirname(dirname(fileURLToPath(import.meta.url)));
const LESSONS_JS = join(ROOT, 'docs/tour-lessons.js');
const WORK_DIR = join(ROOT, 'build', 'tour-check');

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

// 一条命令跑完整课程：compile + run 一趟（tinyactor run 就是这条路径）。
function runLesson(lesson) {
  mkdirSync(WORK_DIR, { recursive: true });
  const file = join(WORK_DIR, `${lesson.id}.ta`);
  writeFileSync(file, lesson.code);
  const res = spawnSync(join(ROOT, 'tinyactor'), ['run', file], {
    cwd: ROOT,
    encoding: 'utf8',
    timeout: 120000,
  });
  if (res.error) throw res.error;
  return { rc: res.status, stdout: res.stdout ?? '', stderr: res.stderr ?? '' };
}

const lines = (s) => s.replace(/\n+$/, '').split('\n').filter((l) => l !== '');
const notCrash = (l) => !CRASH_LINE.test(l) && !CRASH_AT.test(l);

function verify(lesson) {
  const { rc, stdout, stderr } = runLesson(lesson);
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

const only = process.argv.slice(2);
const lessons = loadLessons().filter((l) => only.length === 0 || only.includes(l.id));
if (lessons.length === 0) {
  console.error(`没有匹配的课程：${only.join(', ')}`);
  process.exit(1);
}

let failed = 0;
for (const lesson of lessons) {
  const problem = verify(lesson);
  if (problem) {
    failed++;
    console.log(`FAIL ${lesson.id}\n    ${problem}`);
  } else {
    console.log(`ok   ${lesson.id}`);
  }
}

console.log(`\n${lessons.length - failed}/${lessons.length} 课程通过`);
process.exit(failed === 0 ? 0 : 1);