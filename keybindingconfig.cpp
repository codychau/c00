#include "keybindingconfig.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QDateTime>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>

// ------------------------------------------------------------------ 通用工具
static int leadingSpaces(const QString &s)
{
    int n = 0;
    while (n < s.size() && s[n] == ' ') ++n;
    return n;
}

// ------------------------------------------------------------------ KDL 轻量工具
// 去掉 // 注释（引号内的 // 不算注释），保留原始引号内容
static QString stripKdlComment(const QString &s)
{
    bool inQuote = false, escaped = false;
    for (int i = 0; i < s.size(); ++i) {
        QChar ch = s[i];
        if (inQuote) {
            if (escaped) escaped = false;
            else if (ch == '\\') escaped = true;
            else if (ch == '"') inQuote = false;
            continue;
        }
        if (ch == '"') { inQuote = true; continue; }
        if (ch == '/' && i + 1 < s.size() && s[i + 1] == '/')
            return s.left(i);
    }
    return s;
}

// 把双引号内的字符替换为占位，便于统计大括号
static QString maskQuoted(const QString &s)
{
    QString out;
    bool inQuote = false, escaped = false;
    for (QChar ch : s) {
        if (inQuote) {
            if (escaped) escaped = false;
            else if (ch == '\\') escaped = true;
            else if (ch == '"') inQuote = false;
            continue;
        }
        if (ch == '"') { inQuote = true; continue; }
        out += ch;
    }
    return out;
}

// 按空白切分 KDL token；unquote=true 时去掉双引号（并处理转义）
static QStringList splitKdlTokens(const QString &s, bool unquote)
{
    QStringList out;
    QString cur;
    bool inQuote = false, escaped = false, has = false;
    for (QChar ch : s) {
        if (inQuote) {
            if (escaped) { cur += ch; escaped = false; }
            else if (ch == '\\') { if (!unquote) cur += ch; escaped = true; }
            else if (ch == '"') { inQuote = false; if (!unquote) cur += ch; }
            else cur += ch;
            has = true;
            continue;
        }
        if (ch == '"') { inQuote = true; if (!unquote) cur += ch; has = true; continue; }
        if (ch.isSpace()) {
            if (has) { out << cur; cur.clear(); has = false; }
            continue;
        }
        cur += ch;
        has = true;
    }
    if (has)
        out << cur;
    return out;
}

static QString kdlQuote(const QString &s)
{
    QString out = "\"";
    for (QChar ch : s) {
        if (ch == '"' || ch == '\\')
            out += '\\';
        out += ch;
    }
    return out + "\"";
}

static QString normalizeMod(const QString &m)
{
    const QString l = m.toLower();
    if (l == "mod" || l == "super" || l == "meta" || l == "win")
        return "Super";
    if (l == "ctrl" || l == "control")
        return "Control";
    if (l == "alt" || l == "shift")
        return l.at(0).toUpper() + l.mid(1);
    return m;
}

// 定位顶层 binds { ... } 的行范围：[bs] 为 "binds {" 行，[be] 为闭合 "}" 行
static bool findKdlBinds(const QStringList &lines, int &bs, int &be)
{
    bs = -1;
    be = -1;
    QRegularExpression re(R"(^binds\s*\{)");
    for (int i = 0; i < lines.size(); ++i) {
        if (leadingSpaces(lines[i]) != 0)
            continue;
        if (re.match(stripKdlComment(lines[i]).trimmed()).hasMatch()) {
            bs = i;
            break;
        }
    }
    if (bs < 0)
        return false;
    int depth = 0;
    for (int i = bs; i < lines.size(); ++i) {
        const QString masked = maskQuoted(stripKdlComment(lines[i]));
        for (QChar ch : masked) {
            if (ch == '{') {
                ++depth;
            } else if (ch == '}') {
                --depth;
                if (depth == 0) {
                    be = i;
                    return true;
                }
            }
        }
    }
    return false;
}

static bool looksScreenshot(const QString &raw)
{
    QString low = raw.toLower();
    return low.contains("grim") || low.contains("swayshot") || low.contains("spectacle")
        || low.contains("screencapture") || low.contains("shotted") || low.contains("screencast");
}

// 将命令串归类到 ActionType（sway 非 exec / niri 通用）
static void classifyAction(const QString &raw, Keybinding &b)
{
    b.customBody = raw;
    QStringList parts = raw.split(' ', Qt::SkipEmptyParts);
    if (!parts.isEmpty() && parts.first() == "workspace"
        && parts.size() >= 2
        && QRegularExpression(R"(\d+)").match(parts[1]).hasMatch()) {
        b.type = ActionType::FocusWorkspace;
        b.workspace = parts[1].toInt();
        return;
    }
    if (looksScreenshot(raw)) {
        if (raw.contains("window", Qt::CaseInsensitive) || raw.contains("-w"))
            b.type = ActionType::ScreenshotWindow;
        else if (raw.contains("screen", Qt::CaseInsensitive)
                 || raw.contains("-f") || raw.contains("--fullscreen") || raw.contains("--output"))
            b.type = ActionType::ScreenshotScreen;
        else
            b.type = ActionType::Screenshot;
        return;
    }
    if (!parts.isEmpty() && parts.first() == "quit") {
        b.type = ActionType::Quit;
        return;
    }
    b.type = ActionType::Launch;
    b.spawnArgs = parts;
}

static QString swayQuote(const QString &c)
{
    if (c.isEmpty())
        return "\"\"";
    if (c.contains(' ') || c.contains('\t'))
        return '"' + c + '"';
    return c;
}

static QString stripQuotes(const QString &s)
{
    QString t = s.trimmed();
    if (t.size() >= 2 && t.front() == '"' && t.back() == '"')
        return t.mid(1, t.size() - 2);
    return t;
}

// ------------------------------------------------------------------ 主接口
static bool processAlive(const QString &name)
{
    QProcess p;
    p.start("pgrep", {"-x", name});
    if (!p.waitForFinished(800)) {
        p.kill();
        p.waitForFinished(200);
        return false;
    }
    return p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0;
}

static QString configHome()
{
    QString h = QProcessEnvironment::systemEnvironment().value("XDG_CONFIG_HOME");
    if (h.isEmpty())
        h = QDir::homePath() + "/.config";
    return h;
}

Compositor KeybindingConfig::detect()
{
    const auto env = QProcessEnvironment::systemEnvironment();
    const QString desk = env.value("XDG_CURRENT_DESKTOP") + " "
                       + env.value("XDG_SESSION_DESKTOP") + " "
                       + env.value("DESKTOP_SESSION");
    if (desk.contains("Hyprland", Qt::CaseInsensitive))
        return Compositor::Hyprland;
    if (desk.contains("niri", Qt::CaseInsensitive))
        return Compositor::Niri;
    if (desk.contains("sway", Qt::CaseInsensitive))
        return Compositor::Sway;

    // 环境变量缺失时（从非会话进程启动等）回退到进程 / 配置文件检测
    if (processAlive("niri"))
        return Compositor::Niri;
    if (processAlive("Hyprland"))
        return Compositor::Hyprland;
    if (processAlive("sway"))
        return Compositor::Sway;

    if (QFile::exists(configHome() + "/niri/config.kdl")
        || QFile::exists(configHome() + "/niri/config.yml"))
        return Compositor::Niri;
    if (QFile::exists(configHome() + "/sway/config"))
        return Compositor::Sway;
    return Compositor::Unknown;
}

QString KeybindingConfig::configPath(Compositor c)
{
    switch (c) {
    case Compositor::Sway:
        return configHome() + "/sway/config";
    case Compositor::Niri: {
        const QString base = configHome() + "/niri/";
        if (QFile::exists(base + "config.kdl"))
            return base + "config.kdl";
        if (QFile::exists(base + "config.yml"))
            return base + "config.yml";
        // 默认使用新版 KDL 格式
        return base + "config.kdl";
    }
    default:
        return QString();
    }
}

NiriFormat KeybindingConfig::niriFormat(const QString &text)
{
    if (QRegularExpression(R"(^\s*binds\s*\{)", QRegularExpression::MultilineOption)
            .match(text).hasMatch())
        return NiriFormat::Kdl;
    if (QRegularExpression(R"(^\s*bindings\s*:)", QRegularExpression::MultilineOption)
            .match(text).hasMatch())
        return NiriFormat::Yaml;
    return NiriFormat::Unknown;
}

QStringList KeybindingConfig::reloadCommand(Compositor c)
{
    switch (c) {
    case Compositor::Sway:
        return {"swaymsg", "reload"};
    case Compositor::Niri:
        // niri 通过 SIGHUP 重载配置
        return {"sh", "-c", "kill -HUP $(pgrep -x niri) 2>/dev/null || true"};
    default:
        return {};
    }
}

QString KeybindingConfig::backupPath(const QString &configFile)
{
    QFileInfo fi(configFile);
    QString stamp = QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss");
    QString base = fi.absolutePath() + "/" + fi.baseName() + ".bak-" + stamp;
    return base;
}

QPair<int, int> KeybindingConfig::editableRegion(Compositor c, const QString &text)
{
    QStringList lines = text.split('\n');
    if (c == Compositor::Sway) {
        QRegularExpression re(R"(^\s*bind(l|r|m|code)?\b)");
        int first = -1, last = -1;
        for (int i = 0; i < lines.size(); ++i) {
            if (re.match(lines[i]).hasMatch()) {
                if (first < 0) first = i;
                last = i;
            }
        }
        if (first < 0)
            return {-1, -1};
        return {first, last + 1};
    }
    if (c == Compositor::Niri) {
        if (niriFormat(text) == NiriFormat::Kdl) {
            int bs = -1, be = -1;
            if (!findKdlBinds(lines, bs, be))
                return {-1, -1};
            // 仅替换 binds { ... } 内部内容，保留首尾大括号行
            return {bs + 1, be};
        }
        int bi = -1;
        for (int i = 0; i < lines.size(); ++i) {
            if (lines[i].trimmed() == "bindings:") {
                bi = i;
                break;
            }
        }
        if (bi < 0)
            return {-1, -1};
        int end = lines.size();
        for (int i = bi + 1; i < lines.size(); ++i) {
            if (lines[i].trimmed().isEmpty())
                continue;
            if (leadingSpaces(lines[i]) == 0) {
                end = i;
                break;
            }
        }
        // 保留 bindings: 头行本身
        return {bi + 1, end};
    }
    return {-1, -1};
}

// ------------------------------------------------------------------ 解析
static Keybinding parseSwayLine(const QString &raw)
{
    Keybinding b;
    QString t = raw.trimmed();
    QRegularExpression re(R"(^bind(?:code)?\s+(.*)$)");
    auto m = re.match(t);
    if (!m.hasMatch())
        return b;

    QString rest = m.captured(1).trimmed();
    int sp = rest.indexOf(' ');
    QString modPart = (sp < 0) ? rest : rest.left(sp);
    QString cmd = (sp < 0) ? QString() : rest.mid(sp + 1).trimmed();

    QStringList groups = modPart.split(',', Qt::SkipEmptyParts);
    QString group = groups.isEmpty() ? QString() : groups.first();
    QStringList words = group.split('+', Qt::SkipEmptyParts);
    if (!words.isEmpty()) {
        b.key = words.last();
        for (int i = 0; i < words.size() - 1; ++i)
            b.modifiers.append(words[i]);
    }

    if (cmd.isEmpty()) {
        b.type = ActionType::Other;
        b.customBody = QString("bind ") + modPart;
        return b;
    }
    QString execCmd = cmd;
    if (execCmd.startsWith("exec", Qt::CaseInsensitive)) {
        b.type = ActionType::LaunchShell;
        b.shellCommand = stripQuotes(execCmd.mid(4));
        b.customBody = cmd;
        return b;
    }
    classifyAction(stripQuotes(cmd), b);
    return b;
}

// niri 单个 node 解析
static void niriStore(const QString &name, const QString &val, const QStringList &payload,
                      bool block, Keybinding &b)
{
    QString n = name.toLower();
    if (n == "key") {
        b.key = val.trimmed();
    } else if (n == "keycode") {
        b.key = val.trimmed();
        b._hasKeycode = true;
    } else if (n == "modifiers") {
        QStringList items = block ? payload : val.trimmed().remove('[').remove(']').split(',');
        for (const auto &it : items) {
            QString tk = it.trimmed();
            if (!tk.isEmpty())
                b.modifiers.append(normalizeMod(tk));
        }
    } else if (n == "command" || n == "script-action") {
        b.isScriptAction = (n == "script-action");
        classifyAction(val.trimmed(), b);
    } else {
        QString txt;
        if (block) {
            txt = n + ":\n";
            for (const auto &it : payload)
                txt += "- " + it + "\n";
            txt = txt.trimmed();
        } else {
            txt = n + ": " + val.trimmed();
        }
        b._extra.append(txt);
    }
}

static void parseNiriNode(const QStringList &lines, int start, int itemIndent, int keyCol,
                          Keybinding &b, int &next)
{
    int j = start;
    bool first = true;
    while (j < lines.size()) {
        QString line = lines[j];
        if (line.trimmed().isEmpty()) {
            ++j;
            continue;
        }
        int ind = leadingSpaces(line);
        if (!first) {
            if (ind == itemIndent && line.trimmed().startsWith('-'))
                break;
            if (ind < keyCol)
                break;
        }
        QString work = line;
        if (first) {
            int p = work.indexOf('-');
            if (p >= 0) {
                work = work.mid(p + 1);
                if (!work.isEmpty() && work[0] == ' ')
                    work.remove(0, 1);
            }
            first = false;
        }
        QString trimmed = work.trimmed();
        int colon = trimmed.indexOf(':');
        if (colon < 0) {
            ++j;
            continue;
        }
        QString name = trimmed.left(colon).trimmed();
        QString val = trimmed.mid(colon + 1).trimmed();
        if (val.isEmpty()) {
            QStringList payload;
            int k = j + 1;
            while (k < lines.size()) {
                QString l2 = lines[k];
                if (l2.trimmed().isEmpty()) {
                    payload << "";
                    ++k;
                    continue;
                }
                if (leadingSpaces(l2) <= keyCol)
                    break;
                QString it = l2.trimmed();
                if (it.startsWith('-'))
                    it = it.mid(1).trimmed();
                payload << it;
                ++k;
            }
            niriStore(name, val, payload, true, b);
            j = k;
        } else {
            niriStore(name, val, {val}, false, b);
            ++j;
        }
    }
    next = j;
}

// 将 niri KDL 的单个 action 语句归类
static void classifyKdlAction(const QString &stmt, Keybinding &b)
{
    b.customBody = stmt.trimmed();
    b.isScriptAction = false;
    QStringList parts = splitKdlTokens(b.customBody, true);
    if (parts.isEmpty()) {
        b.type = ActionType::Other;
        return;
    }
    const QString name = parts.first();
    if (name == "spawn") {
        QStringList args = parts.mid(1);
        if (args.size() >= 3 && (args[0] == "sh" || args[0] == "/bin/sh" || args[0] == "bash")
            && args[1] == "-c") {
            b.type = ActionType::LaunchShell;
            b.shellCommand = args.mid(2).join(" ");
        } else {
            b.type = ActionType::Launch;
            b.spawnArgs = args;
        }
        return;
    }
    if (name == "screenshot") {
        b.type = ActionType::Screenshot;
        return;
    }
    if (name == "screenshot-screen") {
        b.type = ActionType::ScreenshotScreen;
        return;
    }
    if (name == "screenshot-window") {
        b.type = ActionType::ScreenshotWindow;
        return;
    }
    if (name == "focus-workspace") {
        b.type = ActionType::FocusWorkspace;
        if (parts.size() >= 2)
            b.workspace = parts[1].toInt();
        return;
    }
    if (name == "quit") {
        b.type = ActionType::Quit;
        return;
    }
    b.type = ActionType::Other;
}

static QList<Keybinding> parseNiriKdl(const QString &text)
{
    QList<Keybinding> out;
    QStringList lines = text.split('\n');
    int bs = -1, be = -1;
    if (!findKdlBinds(lines, bs, be))
        return out;

    // 取出 binds { ... } 内部文本（不含外层大括号）
    QStringList interior;
    {
        QString l = stripKdlComment(lines[bs]);
        int p = l.indexOf('{');
        if (p >= 0 && p + 1 < l.size())
            interior << l.mid(p + 1);
    }
    for (int i = bs + 1; i < be; ++i)
        interior << stripKdlComment(lines[i]);
    {
        QString l = stripKdlComment(lines[be]);
        int p = l.lastIndexOf('}');
        if (p > 0)
            interior << l.left(p);
    }

    // 按顶层大括号切分出每个 bind node
    const QString all = interior.join('\n');
    QStringList nodes;
    QString cur;
    int depth = 0;
    bool inQuote = false, escaped = false;
    for (QChar ch : all) {
        cur += ch;
        if (inQuote) {
            if (escaped) escaped = false;
            else if (ch == '\\') escaped = true;
            else if (ch == '"') inQuote = false;
            continue;
        }
        if (ch == '"') { inQuote = true; continue; }
        if (ch == '{') {
            ++depth;
        } else if (ch == '}') {
            --depth;
            if (depth == 0) {
                nodes << cur;
                cur.clear();
            }
        }
    }

    int idx = 0;
    for (const QString &node : nodes) {
        int bpos = node.indexOf('{');
        if (bpos < 0)
            continue;
        QString head = node.left(bpos).trimmed();
        QString body = node.mid(bpos + 1);
        if (body.endsWith('}'))
            body.chop(1);

        QStringList headToks = splitKdlTokens(head, false);
        if (headToks.isEmpty())
            continue;

        Keybinding b;
        QStringList keyParts = headToks.first().split('+', Qt::SkipEmptyParts);
        if (keyParts.size() == 1) {
            b.key = keyParts.first();
        } else {
            b.key = keyParts.last();
            for (int p = 0; p < keyParts.size() - 1; ++p)
                b.modifiers.append(normalizeMod(keyParts[p]));
        }
        for (int t = 1; t < headToks.size(); ++t) {
            QString prop = headToks[t].trimmed();
            if (!prop.isEmpty())
                b._extra << prop;
        }

        QStringList stmts;
        for (const QString &st : body.split(';')) {
            QString t = st.trimmed();
            if (!t.isEmpty())
                stmts << t;
        }
        if (stmts.size() == 1) {
            classifyKdlAction(stmts.first(), b);
        } else if (stmts.size() > 1) {
            b.type = ActionType::Other;
            b.customBody = stmts.join("; ");
        } else {
            b.type = ActionType::Other;
            b.customBody = QString();
        }

        b.id = "niri-" + QString::number(idx++);
        out.append(b);
    }
    return out;
}

static QList<Keybinding> parseNiriYaml(const QString &text)
{
    QList<Keybinding> out;
    QStringList lines = text.split('\n');
    int bi = -1;
    for (int i = 0; i < lines.size(); ++i) {
        if (lines[i].trimmed() == "bindings:") {
            bi = i;
            break;
        }
    }
    if (bi < 0)
        return out;

    int itemIndent = -1;
    for (int i = bi + 1; i < lines.size(); ++i) {
        if (lines[i].trimmed().isEmpty())
            continue;
        if (leadingSpaces(lines[i]) <= 0)
            break;
        if (lines[i].trimmed().startsWith('-')) {
            itemIndent = leadingSpaces(lines[i]);
            break;
        }
    }
    if (itemIndent < 0)
        return out;

    int keyCol = itemIndent + 2;
    int i = bi + 1, idx = 0;
    while (i < lines.size()) {
        QString line = lines[i];
        if (line.trimmed().isEmpty()) {
            ++i;
            continue;
        }
        int ind = leadingSpaces(line);
        if (ind == itemIndent && line.trimmed().startsWith('-')) {
            Keybinding b;
            int ni = 0;
            parseNiriNode(lines, i, itemIndent, keyCol, b, ni);
            b.id = "niri-" + QString::number(idx++);
            out.append(b);
            i = ni;
        } else if (ind < itemIndent) {
            break;
        } else {
            ++i;
        }
    }
    return out;
}

QList<Keybinding> KeybindingConfig::parse(Compositor c, const QString &text)
{
    if (c == Compositor::Sway) {
        QList<Keybinding> out;
        int idx = 0;
        for (const QString &line : text.split('\n')) {
            QRegularExpression re(R"(^\s*bind(?:code)?\b)");
            if (re.match(line).hasMatch()) {
                Keybinding b = parseSwayLine(line);
                b.id = "sway-" + QString::number(idx++);
                if (b.isValid())
                    out.append(b);
            }
        }
        return out;
    }
    if (c == Compositor::Niri) {
        if (niriFormat(text) == NiriFormat::Yaml)
            return parseNiriYaml(text);
        return parseNiriKdl(text);
    }
    return {};
}

// ------------------------------------------------------------------ 渲染
static QString niriRenderCustom(const QString &raw)
{
    const QStringList actions = {
        "workspace", "screenshot", "screenshot-clipboard", "focus-output",
        "focus-next-output", "launch-floating-window", "cycle-focus",
        "cycle-layer-window", "cycle-layer-overlay", "cycle-layer-overlay-top",
        "waybar", "lock", "show-keyboard-overlay", "logout", "exit"
    };
    QString t = raw.trimmed();
    if (t.isEmpty())
        return QString();
    QString first = t.split(' ', Qt::SkipEmptyParts).first();
    if (actions.contains(first))
        return "script-action: " + t;
    return "command: " + t;
}

static QString niriRenderNode(const Keybinding &b)
{
    struct PP {
        QString text;
        bool block;
        QStringList sub;
    };
    QList<PP> props;
    if (!b.key.isEmpty())
        props.append({(b._hasKeycode ? "keycode: " : "key: ") + b.key, false, {}});

    {
        QString fmt;
        switch (b.type) {
        case ActionType::FocusWorkspace:
            fmt = "script-action: workspace " + QString::number(b.workspace);
            break;
        case ActionType::Screenshot:
        case ActionType::ScreenshotScreen:
        case ActionType::ScreenshotWindow:
            fmt = "script-action: screenshot";
            break;
        case ActionType::Launch:
            fmt = b.isScriptAction
                      ? "script-action: " + b.spawnArgs.join(" ")
                      : "command: " + b.customBody;
            break;
        case ActionType::Quit:
        case ActionType::Other:
        default:
            fmt = niriRenderCustom(b.customBody);
            break;
        }
        if (!fmt.isEmpty()) {
            bool block = fmt.contains('\n');
            props.append({fmt, block, block ? fmt.split('\n').mid(1) : QStringList()});
        }
    }

    if (!b.modifiers.isEmpty())
        props.append({"modifiers: [" + b.modifiers.join(", ") + "]", false, {}});

    for (const QString &e : b._extra) {
        bool block = e.contains('\n');
        props.append({e, block, block ? e.split('\n').mid(1) : QStringList()});
    }

    const int itemIndent = 2, propIndent = 4, listIndent = 6;
    QString it(itemIndent, ' ');
    QString pp(propIndent, ' ');
    QString li(listIndent, ' ');

    QString out = it + "- " + props.first().text;
    for (int i = 1; i < props.size(); ++i) {
        const PP &p = props[i];
        if (p.block) {
            out += "\n" + pp + p.text;
            for (const QString &ln : p.sub)
                out += "\n" + li + "- " + ln;
        } else {
            out += "\n" + pp + p.text;
        }
    }
    return out;
}

static QString renderNiriYaml(const QList<Keybinding> &binds)
{
    QStringList lines;
    for (const Keybinding &b : binds)
        lines.append(niriRenderNode(b));
    return lines.join('\n');
}

static QString renderNiriKdlNode(const Keybinding &b)
{
    QString keySpec = b.key;
    if (!b.modifiers.isEmpty())
        keySpec = b.modifiers.join("+") + "+" + b.key;
    if (keySpec.isEmpty())
        return QString();

    QString head = "    " + keySpec; // binds 块内统一 4 空格缩进
    for (const QString &p : b._extra) {
        QString t = p.trimmed();
        if (!t.isEmpty())
            head += " " + t;
    }

    QString action;
    switch (b.type) {
    case ActionType::Launch: {
        QStringList args = b.spawnArgs;
        if (args.isEmpty() && !b.customBody.isEmpty())
            args = splitKdlTokens(b.customBody, true).mid(1);
        QStringList quoted;
        for (const QString &a : args)
            quoted << kdlQuote(a);
        action = quoted.isEmpty() ? "spawn" : "spawn " + quoted.join(" ");
        break;
    }
    case ActionType::LaunchShell: {
        QString cmd = b.shellCommand.isEmpty() ? b.customBody : b.shellCommand;
        action = "spawn \"sh\" \"-c\" " + kdlQuote(cmd);
        break;
    }
    case ActionType::FocusWorkspace:
        action = (b.workspace > 0 || b.customBody.isEmpty())
                     ? "focus-workspace " + QString::number(b.workspace)
                     : b.customBody.trimmed();
        break;
    case ActionType::Quit:
        action = "quit";
        break;
    case ActionType::Screenshot:
        action = "screenshot";
        break;
    case ActionType::ScreenshotScreen:
        action = "screenshot-screen";
        break;
    case ActionType::ScreenshotWindow:
        action = "screenshot-window";
        break;
    case ActionType::Other:
    default:
        action = b.customBody.trimmed();
        break;
    }

    if (action.isEmpty())
        return head + " { }";
    if (action.contains('\n')) {
        QString out = head + " {\n";
        for (const QString &ln : action.split('\n')) {
            if (!ln.trimmed().isEmpty())
                out += "        " + ln.trimmed() + "\n";
        }
        out += "    }";
        return out;
    }
    return head + " { " + action + "; }";
}

static QString renderNiriKdl(const QList<Keybinding> &binds)
{
    QStringList lines;
    for (const Keybinding &b : binds) {
        QString node = renderNiriKdlNode(b);
        if (!node.isEmpty())
            lines.append(node);
    }
    return lines.join('\n');
}

static QString renderSway(const QList<Keybinding> &binds)
{
    QStringList lines;
    for (const Keybinding &b : binds) {
        QString mod = b.modifiers.isEmpty()
                          ? b.key
                          : b.modifiers.join("+") + "+" + b.key;
        QString cmd;
        switch (b.type) {
        case ActionType::Launch:
            cmd = b.spawnArgs.join(" ");
            break;
        case ActionType::LaunchShell:
            cmd = "exec " + b.shellCommand;
            break;
        case ActionType::FocusWorkspace:
            cmd = "workspace " + QString::number(b.workspace);
            break;
        case ActionType::Quit:
        case ActionType::Other:
        case ActionType::Screenshot:
        case ActionType::ScreenshotScreen:
        case ActionType::ScreenshotWindow:
            cmd = b.customBody;
            break;
        }
        if (cmd.isEmpty())
            cmd = b.customBody;
        // sway 内置命令（exec/workspace）不能加引号，仅对启动外部命令做转义
        QString outCmd = (b.type == ActionType::Launch) ? swayQuote(cmd) : cmd;
        lines << "bind " + mod + " " + outCmd;
    }
    return lines.join('\n');
}

QString KeybindingConfig::render(Compositor c, const QList<Keybinding> &binds, NiriFormat niriFmt)
{
    if (c == Compositor::Sway)
        return renderSway(binds);
    if (c == Compositor::Niri) {
        if (niriFmt == NiriFormat::Yaml)
            return renderNiriYaml(binds);
        return renderNiriKdl(binds);
    }
    return QString();
}

// ------------------------------------------------------------------ 显示辅助
QString renderHotkey(const QVector<QString> &mods, const QString &key, Compositor c)
{
    if (mods.isEmpty())
        return key;
    if (c == Compositor::Sway)
        return mods.join("+") + "+" + key;
    // niri
    return mods.join(" + ") + " + " + key;
}

QString keybindingDescribe(const Keybinding &b, Compositor c)
{
    QString hot = renderHotkey(b.modifiers, b.key, c);
    QString action;
    switch (b.type) {
    case ActionType::Launch:
        action = "启动";
        break;
    case ActionType::LaunchShell:
        action = "执行命令";
        break;
    case ActionType::FocusWorkspace:
        action = "切换工作区";
        break;
    case ActionType::Quit:
        action = "退出";
        break;
    case ActionType::Screenshot:
        action = "截屏";
        break;
    case ActionType::ScreenshotScreen:
        action = "截屏(全屏)";
        break;
    case ActionType::ScreenshotWindow:
        action = "截屏(窗口)";
        break;
    case ActionType::Other:
        action = "自定义";
        break;
    }
    QString cmd;
    switch (b.type) {
    case ActionType::Launch:
        cmd = b.spawnArgs.join(" ");
        break;
    case ActionType::LaunchShell:
        cmd = b.shellCommand;
        break;
    case ActionType::FocusWorkspace:
        cmd = (b.workspace > 0)
                  ? "workspace " + QString::number(b.workspace)
                  : b.customBody;
        break;
    case ActionType::Quit:
    case ActionType::Other:
    case ActionType::Screenshot:
    case ActionType::ScreenshotScreen:
    case ActionType::ScreenshotWindow:
        cmd = b.customBody;
        break;
    }
    return hot + "  \u279C  " + action + (cmd.isEmpty() ? QString() : "  " + cmd);
}
