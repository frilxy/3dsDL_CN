# 简体中文

这份汉化不改上游源码：英文原文留在树里，中文放在 `zh/zh.tsv`，构建前由
`scripts/zh/apply.py` 写到工作区。所以跟随上游只要合并、再跑一次脚本。

## 日常更新

```sh
git fetch upstream
git merge upstream/main          # 冲突通常为零：改动都在 zh/ 和 scripts/zh/
python3 scripts/zh/apply.py      # 重新套用词表
```

`apply.py` 会打印「N/M blocks applied」。**那些没套上的条目就是上游改过的文案**，
按报出的文件和首行对上 `zh/zh.tsv` 改掉即可，改的是数据文件，不是代码。

```sh
python3 scripts/zh/apply.py --check    # 只检查不写盘，未匹配时退出码 1
python3 scripts/zh/apply.py --reverse  # 还原成上游英文，便于对比
```

## 词表格式

一行一条，三列，用制表符分隔：

```
<源文件路径>\t<原文>\t<译文>
```

换行写成 `\n`，制表符 `\t`，反斜杠 `\\`。一条可以是**一句、一行，也可以是连续多行**，
所以动态拼接的提示（`"无法读取 " + path + "。"`）和整个函数体都能写进来。

应用时同一文件内**长块优先**，宽块先占住自己的文本，短块才不会把它改坏。

## 改了文案之后

```sh
python3 scripts/zh/extract.py --gap=4 --dirs <上游目录> <已汉化目录> > zh/zh.tsv
```

用它可以从「英文树 / 中文树」重新导出一份词表；`--gap=4` 会把中间相隔不超过 4 行的
两处改动并成一块，这样新增的行也能被后续重放匹配到。

## 测试

`tests/` 不动，仍然是上游英文断言。CI 里宿主测试跑纯上游代码，中文构建单独跑编译，
因此上游改测试不会产生冲突。

## 版本号

`zh/VERSION_SUFFIX` 里是 `cn`，`apply.py` 把 `VERSION` 写成 `0.9.0-cn`：
**上游版本 + 语种后缀，不带序号**。上游升到 0.9.1 就是 `0.9.1-cn`，不需要手改。

`scripts/version.sh` 认这个不带数字的 `cn` 标签，按预发布的最高序号 14 编码成 CIA 版本。
一个上游版本只对应一个正式版；同一上游版本要再发预览，用 `-cn-rc.1` 这类带序号的标签，
并把它标成 prerelease，免得到正式版的检查更新里冒出来。

## 术语

`ticket`、`seed` 保留英文，不译成「票据」「种子」——前者在中文里指金融票据，
后者会被当成 BT 下载种子。CIA、DLC、3DSX 同理。中文与英文之间留一个空格。

## 网页与编辑器

`source/core/remote_page.cpp` 里的管理页和 `resources/editor/editor.js` 也在词表里。
编辑器改完要重新打包：

```sh
./scripts/make-editor.sh    # 需要 Node，生成 romfs/web/editor.js.gz
```

CI 的 `zh.yml` 会自动做这一步。
