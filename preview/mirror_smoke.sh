#!/usr/bin/env bash
# nasrs mirror 的端到端冒烟测试：真的建目录、真的拷文件、真的比对。
# 单测只覆盖判据，这一份证明"执行层真能把主网拷成副本"，包括 mtime 保留
# （不保留的话每一轮都会全量重拷）和符号链接不被跟进去。
set -u
BIN=${BIN:-/home/cody/c00-app/src/nasrs/target/debug/nasrs}
W=$(mktemp -d /tmp/mirror-smoke.XXXXXX)
P=$W/primary; S=$W/secondary
pass=0; fail=0
ok(){ echo "  ✓ $1"; pass=$((pass+1)); }
no(){ echo "  ✗ $1"; fail=$((fail+1)); }

mkdir -p "$P/movies/sub" "$S"
printf 'AAA' > "$P/a.txt"
printf 'BBBBBBBBBB' > "$P/movies/b.mkv"
printf 'CCC' > "$P/movies/sub/c.srt"
# 1970 之前的时间戳之外，再造一个 mtime 明确的历史文件：
# 若执行层不保留 mtime，副本这一份下一轮一定被当成"变了"再拷一遍
touch -d '2001-02-03 04:05:06' "$P/a.txt"
# 符号链接：指向系统文件。跟进去就等于把 /etc/hosts 拷进副本
ln -s /etc/hosts "$P/hosts-link"
# 上次崩在半路留下的临时文件：以 . 开头 + .nastmp 结尾，枚举必须整类跳过
printf 'half' > "$P/.a.txt.nastmp"

echo "== 1) 只报告（不得写入） =="
out=$("$BIN" mirror --primary "$P" --secondary "$S")
echo "$out" | sed 's/^/    /'
echo "$out" | grep -q "落后      : 3 个文件" && ok "报告说落后 3 个文件" || no "落后数不对"
[ -z "$(ls -A "$S")" ] && ok "--report 没有写任何东西" || no "--report 往副本里写了东西"

echo "== 2) 真同步一轮 =="
"$BIN" mirror --primary "$P" --secondary "$S" --once >/dev/null || no "--once 返回非 0"
[ -f "$S/a.txt" ] && [ -f "$S/movies/b.mkv" ] && [ -f "$S/movies/sub/c.srt" ] \
    && ok "三个文件都到了副本（含子目录）" || no "副本缺文件"
# 比对时排掉主网有、副本**故意**没有的两样：符号链接（不跟进去）和临时文件
# （不参与比对）。它们各有专门断言，混进 diff 只会把真差异埋掉。
diff -r --exclude='.*.nastmp' --exclude='hosts-link' "$P" "$S" >/dev/null \
    && ok "内容与主网逐字节一致" || no "内容有差异"
[ ! -e "$S/hosts-link" ] && ok "符号链接没有被跟进去" || no "副本里出现了符号链接的目标"
[ ! -e "$S/.a.txt.nastmp" ] && ok "临时文件没被当成正经文件拷过去" || no "临时文件被拷进副本"

echo "== 3) mtime 必须保留 =="
pm=$(stat -c %Y "$P/a.txt"); sm=$(stat -c %Y "$S/a.txt")
[ "$pm" = "$sm" ] && ok "mtime 一致（$pm）" || no "mtime 没保留：主 $pm 副 $sm"
"$BIN" mirror --primary "$P" --secondary "$S" | grep -q "副本与主网一致" \
    && ok "第二轮报告说已一致（没有全量重拷）" || no "已一致却被判为有差异"

echo "== 4) 等长但内容变了也要重拷 =="
printf 'XYZ' > "$P/a.txt"          # 长度仍是 3，只改内容与时间
"$BIN" mirror --primary "$P" --secondary "$S" | grep -q "落后      : 1 个文件" \
    && ok "等长改内容被识别为落后" || no "等长改内容被判为已同步"
"$BIN" mirror --primary "$P" --secondary "$S" --once >/dev/null
[ "$(cat "$S/a.txt")" = "XYZ" ] && ok "副本内容已更新" || no "副本内容没更新"

echo "== 5) 副本上的多余文件默认不删 =="
printf 'old' > "$S/gone.mkv"
"$BIN" mirror --primary "$P" --secondary "$S" | grep -q "副本多余  : 1 个文件" && ok "报告点出多余 1 个" || no "没报出多余文件"
"$BIN" mirror --primary "$P" --secondary "$S" --once >/dev/null
[ -f "$S/gone.mkv" ] && ok "默认不删（误删同步过去就两份都没了）" || no "默认竟然删了"
"$BIN" mirror --primary "$P" --secondary "$S" --once --delete-extras | grep -q "删除 1 个" && ok "--delete-extras 删掉了它" || no "删除没执行"
[ ! -e "$S/gone.mkv" ] || no "文件还在"

echo "== 6) 危险的路径组合必须被挡下 =="
"$BIN" mirror --primary "$P" --secondary "$P" >/dev/null 2>&1
[ $? -eq 1 ] && ok "两个副本同目录：拒绝" || no "同目录没挡住"
mkdir -p "$P/inner"
"$BIN" mirror --primary "$P" --secondary "$P/inner" 2>&1 | grep -q "把副本再镜像进副本" \
    && ok "副本嵌套在主网里：拒绝并说明后果" || no "嵌套没挡住"
"$BIN" mirror --primary "$S" --secondary "$P/.." >/dev/null 2>&1; rc=$?
[ $rc -eq 0 ] || [ $rc -eq 1 ] || no "父目录当副本时返回了意外退出码 $rc"

echo "== 7) --json 给程序读 =="
"$BIN" mirror --primary "$P" --secondary "$S" --json > "$W/j.json"
python3 - "$W/j.json" <<'PY' && ok "JSON 可解析且字段自洽" || no "JSON 有问题"
import json,sys
d=json.load(open(sys.argv[1]))
assert d["ok"] is True, d
assert d["src_files"]==3, d
assert d["lag_files"]==0 and d["lag_bytes"]==0, d
assert isinstance(d["errors"],list), d
PY
"$BIN" mirror --primary "$P" --secondary "$S" --once --json | python3 -c 'import json,sys; d=json.load(sys.stdin); assert d["copied_files"]==0' && ok "--once --json 静默且可解析" || no "--json 输出有问题"

echo "== 8) 大文件分块拷贝 =="
head -c 67108864 /dev/urandom > "$P/big.mkv"
"$BIN" mirror --primary "$P" --secondary "$S" --once >/dev/null
cmp "$P/big.mkv" "$S/big.mkv" && ok "64 MiB 逐字节相同（1 MiB 分块循环没问题）" || no "大文件内容不一致"
# 拷贝过程中不该留下临时文件（rename 成功就要消失）
[ -z "$(find "$S" -name '.*.nastmp' -print -quit)" ] && ok "副本里没有残留的 .nastmp" || no "副本里留着临时文件"

echo "== 9) 缺参数、未知参数 =="
"$BIN" mirror --primary "$P" >/dev/null 2>&1; [ $? -eq 2 ] && ok "少一个目录：退出码 2" || no "缺参数没到 2"
"$BIN" mirror --primary >/dev/null 2>&1; [ $? -eq 2 ] && ok "--primary 漏值：退出码 2" || no "漏值没到 2"
"$BIN" mirror --wat >/dev/null 2>&1; [ $? -eq 2 ] && ok "未知参数：退出码 2" || no "未知参数没到 2"

echo
echo "通过 $pass 项，失败 $fail 项（工作目录 $W）"
[ $fail -eq 0 ] || exit 1
