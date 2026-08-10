#!/usr/bin/env node
// scripts/verify-lessons.mjs — 验证 docs/tour-lessons.js 的每一课都能在 WASM 里
// 编译 + 运行，且实际 stdout 与 expected 逐行一致。
//
// 用法：
//   node scripts/verify-lessons.mjs
//
// 退出码：
//   0 — 全部课程通过（N/24 lessons compile+run OK）
//   1 — 有课程编译失败、运行失败或输出不匹配
//   2 — 数据文件 / 脚本环境问题（LESSONS 解析失败等）
//
// 实现要点：
//   * 用 docs/wasm/tinyactor-vm.js（CommonJS 导出 createTavm）加载 WASM VM
//   * 每课独立创建一个 VM 实例，避免课程之间互相污染
//   * 编译：FS.writeFile('user.ta') → callMain(['lib/bootstrap.tabc','user.ta','user.tabc'])
//   * 运行：callMain(['user.tabc'])，stdout 只取 print() 输出（printErr 单独收集，
//     不参与 expected 比对；进程崩溃的堆栈走 stderr，不影响 stdout 匹配）
//   * 比对：实际输出与 expected 都做「trim 末尾空白 + 按行拆分」后逐行相等

import { readFileSync } from 'node:fs';
import { createRequire } from 'node:module';
import { fileURLToPath } from 'node:url';

const REPO_ROOT = fileURLToPath(new URL('..', import.meta.url)); // 以 / 结尾
const LESSONS_FILE = fileURLToPath(new URL('../docs/tour-lessons.js', import.meta.url));
const VM_JS = fileURLToPath(new URL('../docs/wasm/tinyactor-vm.js', import.meta.url));

const ALLOWED_SECTIONS = ['Basics', 'Functions', 'Flow control', 'Data types', 'Actors', 'Modules'];

// ---------- 1. 读取 LESSONS（数组部分是严格 JSON） ----------
function loadLessons() {
  const raw = readFileSync(LESSONS_FILE, 'utf8');

  // 定位数组起始：找独立一行开头（行首）的 `const LESSONS = [`。
  // 不能直接 raw.indexOf('const LESSONS') / raw.indexOf('[') —— 文件头部
  // 注释里也有 "`const LESSONS = [ ... ];`"，会取到错误位置（注释里的 `[`）。
  const decl = /^const LESSONS\s*=\s*\[/m.exec(raw);
  if (!decl) {
    throw new Error(`无法在 ${LESSONS_FILE} 中找到独立一行的 "const LESSONS = [" 声明`);
  }
  const start = decl.index + decl[0].indexOf('[');

  // 定位数组结束：从 start 做括号配对扫描，跳过字符串字面量。
  // 数组是严格 JSON，`]` 只会出现在数组闭合处或字符串值内部，
  // 因此配对扫描不依赖「数组必须是文件里最后一个 ]」，也不会被
  // 数组之后的 `;`、注释（哪怕注释里含 `]`）误导。
  let depth = 0;
  let inString = false;
  let end = -1;
  for (let i = start; i < raw.length; i++) {
    const ch = raw[i];
    if (inString) {
      if (ch === '\\') { i++; continue; } // 跳过转义序列（如 \"）
      if (ch === '"') inString = false;
      continue;
    }
    if (ch === '"') { inString = true; continue; }
    if (ch === '[') { depth++; continue; }
    if (ch === ']') {
      depth--;
      if (depth === 0) { end = i; break; }
    }
  }
  if (end === -1) {
    throw new Error(`无法在 ${LESSONS_FILE} 中找到与起始 [ 配对的 ]`);
  }

  // 防御性校验：数组闭合之后只允许空白 / 分号 / 注释，防止截到错误位置。
  const tail = raw.slice(end + 1);
  const leftover = tail.replace(/^\s*;?\s*/, '').replace(/^\/\/[^\n]*/, '').trim();
  if (leftover !== '') {
    throw new Error(`LESSONS 数组之后有意外内容: ${JSON.stringify(tail.slice(0, 50))}`);
  }

  let lessons;
  try {
    lessons = JSON.parse(raw.slice(start, end + 1));
  } catch (e) {
    throw new Error(`LESSONS 数组不是合法 JSON: ${e.message}`);
  }
  if (!Array.isArray(lessons) || lessons.length === 0) {
    throw new Error('LESSONS 不是非空数组');
  }

  // 数据完整性检查
  const ids = new Set();
  for (const [i, l] of lessons.entries()) {
    const n = i + 1;
    for (const key of ['id', 'section', 'title', 'text', 'code', 'expected']) {
      if (typeof l[key] !== 'string' || l[key].length === 0) {
        throw new Error(`第 ${n} 课缺少或为空字段: ${key}`);
      }
    }
    if (!ALLOWED_SECTIONS.includes(l.section)) {
      throw new Error(`第 ${n} 课 section "${l.section}" 不在允许列表 ${ALLOWED_SECTIONS.join(' / ')} 中`);
    }
    if (!/^[a-z0-9]+(-[a-z0-9]+)*$/.test(l.id)) {
      throw new Error(`第 ${n} 课 id "${l.id}" 不是 kebab-case`);
    }
    if (ids.has(l.id)) {
      throw new Error(`id 重复: ${l.id}`);
    }
    ids.add(l.id);
  }
  return lessons;
}

// ---------- 2. 输出比对工具 ----------
// TA 的 print 自带换行；这里统一 trim 末尾空白后按行拆分，忽略文件末尾多余的换行。
function toLines(s) {
  const t = String(s).replace(/\r\n/g, '\n').replace(/\s+$/, '');
  if (t === '') return [];
  return t.split('\n');
}

function linesEqual(a, b) {
  const la = toLines(a);
  const lb = toLines(b);
  if (la.length !== lb.length) return false;
  for (let i = 0; i < la.length; i++) {
    if (la[i] !== lb[i]) return false;
  }
  return true;
}

// ---------- 3. 编译 + 运行一课 ----------
function fileExists(mod, path) {
  try {
    mod.FS.readFile(path);
    return true;
  } catch (e) {
    return false;
  }
}

async function runLesson(createTavm, lesson) {
  let stdout = [];
  let stderr = [];

  let mod;
  try {
    mod = await createTavm({
      noInitialRun: true,
      print: (s) => stdout.push(String(s)),
      printErr: (s) => stderr.push(String(s)),
    });
  } catch (e) {
    return { ok: false, stage: 'load', detail: `WASM VM 加载失败: ${e}` };
  }

  // 幂等：清理上一次的产物
  try { mod.FS.unlink('user.ta'); } catch (e) {}
  try { mod.FS.unlink('user.tabc'); } catch (e) {}
  mod.FS.writeFile('user.ta', lesson.code);

  // ① 编译：lib/bootstrap.tabc user.ta user.tabc
  stdout.length = 0;
  stderr.length = 0;
  let rc;
  try {
    rc = mod.callMain(['lib/bootstrap.tabc', 'user.ta', 'user.tabc']);
  } catch (e) {
    return { ok: false, stage: 'compile', detail: `编译器异常: ${e}` };
  }
  const compileText = [stdout.join('\n'), stderr.join('\n')].join('\n').trim();
  if (rc !== 0 || !fileExists(mod, 'user.tabc') || /parse error|type error|compile aborted/i.test(compileText)) {
    return { ok: false, stage: 'compile', detail: compileText || `编译失败（rc=${rc}，未生成 user.tabc）` };
  }

  // ② 运行：user.tabc，stdout 参与比对
  stdout.length = 0;
  stderr.length = 0;
  try {
    rc = mod.callMain(['user.tabc']);
  } catch (e) {
    return {
      ok: false,
      stage: 'run',
      detail: `运行异常: ${e}`,
      stderr: stderr.join('\n'),
    };
  }
  return { ok: true, actual: stdout.join('\n') };
}

// ---------- 4. 主流程 ----------
async function main() {
  let lessons;
  try {
    lessons = loadLessons();
  } catch (e) {
    console.error(`verify-lessons: ${e.message}`);
    process.exit(2);
  }
  const createTavm = createRequire(VM_JS)(VM_JS);

  console.log(`验证 ${LESSONS_FILE}`);
  console.log(`共 ${lessons.length} 课，逐课编译 + 运行…\n`);

  let pass = 0;
  const failures = [];
  for (const lesson of lessons) {
    const res = await runLesson(createTavm, lesson);
    if (res.ok && linesEqual(res.actual, lesson.expected)) {
      pass++;
      console.log(`  ✓ ${lesson.id.padEnd(24)} ${lesson.title}`);
    } else {
      failures.push({ lesson, res });
      console.log(`  ✗ ${lesson.id} — ${lesson.title}`);
      if (!res.ok) {
        console.log(`      stage:    ${res.stage}`);
        if (res.detail) console.log(`      detail:   ${res.detail}`);
        if (res.stderr) console.log(`      stderr:   ${JSON.stringify(res.stderr)}`);
      } else {
        console.log(`      expected: ${JSON.stringify(toLines(lesson.expected))}`);
        console.log(`      actual:   ${JSON.stringify(toLines(res.actual))}`);
      }
    }
  }

  console.log(`\n${pass}/${lessons.length} lessons compile+run OK`);
  if (failures.length > 0) {
    console.error(`\n${failures.length} 课失败，详见上方输出`);
    process.exit(1);
  }
  process.exit(0);
}

main();