# scratch.py — kernfuzz 共享临时目录
#
# 生成的 .ta 源码要交给 `tinyactor run` 编译；modules S3（#422）起，
# 带 import 的源文件向上必须能找到 vendor/ 根。系统 TMPDIR 上没有
# vendor/，所有生成源码一律编译失败（nightly 2026-11-04 的 152 个
# novel findings 几乎全由此而来）。因此 kernfuzz 的临时文件统一放
# 仓库内的 build/kernfuzz/tmp/ —— 仓库根的 vendor/ 在其上方。

import os
import tempfile

_SCRATCH_ROOT = os.path.join(
    os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))),
    "build", "kernfuzz", "tmp")


def mkdtemp(prefix):
    """tempfile.mkdtemp，但落在 build/kernfuzz/tmp/ 下。"""
    os.makedirs(_SCRATCH_ROOT, exist_ok=True)
    return tempfile.mkdtemp(prefix=prefix, dir=_SCRATCH_ROOT)


def mkstemp(suffix="", prefix=""):
    """tempfile.mkstemp，但落在 build/kernfuzz/tmp/ 下。"""
    os.makedirs(_SCRATCH_ROOT, exist_ok=True)
    return tempfile.mkstemp(suffix=suffix, prefix=prefix, dir=_SCRATCH_ROOT)


def mkstemp_w(suffix="", prefix="", mode="w"):
    """打开一个 scratch 临时文件，返回 (文件对象, 路径)。"""
    fd, path = mkstemp(suffix=suffix, prefix=prefix)
    return os.fdopen(fd, mode), path