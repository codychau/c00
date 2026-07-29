#include "aipage.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QScrollArea>
#include <QFrame>
#include <QRegularExpression>
#include <QDir>
#include <QFile>
#include <QTextStream>
#include <QMessageBox>
#include <QComboBox>
#include <QEvent>
#include <QFileDialog>

// ── 阻止鼠标滚轮改变控件值 ──
class NoWheelFilter : public QObject
{
public:
    using QObject::QObject;
protected:
    bool eventFilter(QObject *obj, QEvent *event) override
    {
        if (event->type() == QEvent::Wheel)
            return true;
        return QObject::eventFilter(obj, event);
    }
};

AIPage::AIPage(QWidget *parent)
    : QWidget(parent)
{
    m_nowheel = new NoWheelFilter(this);
    m_configDir = QDir::homePath() + "/.config/systemd/user";

    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);

    auto *scroll = new QScrollArea();
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);

    auto *inner = new QWidget();
    auto *layout = new QVBoxLayout(inner);
    layout->setContentsMargins(16, 8, 16, 8);
    layout->setSpacing(10);

    // title
    auto *title = new QLabel("🤖 AI 选项");
    QFont tf = title->font(); tf.setPointSize(14); tf.setBold(true);
    title->setFont(tf);
    layout->addWidget(title);

    m_info = new QLabel("检测中…");
    m_info->setStyleSheet("color: #666; font-size: 12px;");
    layout->addWidget(m_info);

    // Automatic degradation rule section
    m_degradeGroup = new QGroupBox("自动降级规则");
    auto *degradeLayout = new QVBoxLayout(m_degradeGroup);
    degradeLayout->setContentsMargins(8, 8, 8, 8);
    degradeLayout->setSpacing(6);

    // Proxy port and model base path setting (same row)
    auto *proxyPathRow = new QHBoxLayout();
    proxyPathRow->addWidget(new QLabel("代理端口:"));
    m_proxyPortInput = new QLineEdit("8081");
    m_proxyPortInput->setMinimumHeight(26);
    m_proxyPortInput->installEventFilter(m_nowheel);
    proxyPathRow->addWidget(m_proxyPortInput);

    proxyPathRow->addWidget(new QLabel(" 模型基础路径:"));
    m_modelBasePathInput = new QLineEdit("");
    m_modelBasePathInput->setMinimumHeight(26);
    m_modelBasePathInput->installEventFilter(m_nowheel);
    proxyPathRow->addWidget(m_modelBasePathInput, 1);

    auto *browseBtn = new QPushButton("…");
    browseBtn->setFixedWidth(28);
    browseBtn->setFixedHeight(26);
    connect(browseBtn, &QPushButton::clicked, this, [this]() {
        QString path = QFileDialog::getExistingDirectory(
            this, "选择模型基础路径", QDir::homePath());
        if (!path.isEmpty()) {
            m_modelBasePathInput->setText(path);
        }
    });
    proxyPathRow->addWidget(browseBtn);

    degradeLayout->addLayout(proxyPathRow);

    // Startup program and action row with checkbox
    auto *startupRow = new QHBoxLayout();
    m_startupProgramCheckbox = new QCheckBox();
    startupRow->addWidget(m_startupProgramCheckbox);
    startupRow->addWidget(new QLabel("当某个程序启动时:"));
    m_startupProgramInput = new QLineEdit("");
    m_startupProgramInput->setMinimumHeight(26);
    m_startupProgramInput->installEventFilter(m_nowheel);
    startupRow->addWidget(m_startupProgramInput, 1);

    startupRow->addWidget(new QLabel(" 就会 "));

    m_actionDropdown = new QComboBox();
    m_actionDropdown->addItems({"切换到某个模型"});
    m_actionDropdown->setMinimumHeight(26);
    startupRow->addWidget(m_actionDropdown);

    startupRow->addWidget(new QLabel(" 模型文件名:"));
    m_modelNameInput = new QLineEdit("");
    m_modelNameInput->setMinimumHeight(26);
    m_modelNameInput->installEventFilter(m_nowheel);
    startupRow->addWidget(m_modelNameInput, 1);

    degradeLayout->addLayout(startupRow);

    // Model change row with checkbox and dropdown
    auto *modelChangeRow = new QHBoxLayout();
    m_modelChangeCheckbox = new QCheckBox();
    modelChangeRow->addWidget(m_modelChangeCheckbox);
    modelChangeRow->addWidget(new QLabel("模型变化时，则"));
    m_modelChangeDropdown = new QComboBox();
    m_modelChangeDropdown->addItems({"自动根据模型名称重载服务", "使用备选模型代替"});
    m_modelChangeDropdown->setMinimumHeight(26);
    modelChangeRow->addWidget(m_modelChangeDropdown, 1);
    degradeLayout->addLayout(modelChangeRow);

    // Openclaw format row with checkbox
    auto *openclawRow = new QHBoxLayout();
    m_openclawFormatCheckbox = new QCheckBox();
    openclawRow->addWidget(m_openclawFormatCheckbox);
    openclawRow->addWidget(new QLabel("适配Openclaw发送格式"));
    degradeLayout->addLayout(openclawRow);

    layout->addWidget(m_degradeGroup);

    // define 4 services
    m_cards.resize(4);
    m_cards[0].name       = "Ollama";
    m_cards[0].systemdSvc = "ollama.service";
    m_cards[0].procName   = "ollama";
    m_cards[0].port       = 11434;
    m_cards[1].name       = "llama.cpp";
    m_cards[1].systemdSvc = "llama-server.service";
    m_cards[1].procName   = "llama-server";
    m_cards[1].port       = 8080;
    m_cards[2].name       = "vLLM";
    m_cards[2].systemdSvc = "vllm.service";
    m_cards[2].procName   = "vllm";
    m_cards[2].port       = 8000;
    m_cards[3].name       = "LocalAI";
    m_cards[3].systemdSvc = "local-ai.service";
    m_cards[3].procName   = "local-ai";
    m_cards[3].port       = 8080;

    for (int i = 0; i < 4; ++i)
        buildCard(i);

    auto *cardsRow = new QHBoxLayout();
    cardsRow->setSpacing(10);
    for (auto &c : m_cards)
        cardsRow->addWidget(
            static_cast<QGroupBox *>(c.controlsWidget->parentWidget()), 1);
    layout->addLayout(cardsRow);

    layout->addStretch();

    // refresh button
    auto *row = new QHBoxLayout();
    m_refreshBtn = new QPushButton("刷新状态");
    connect(m_refreshBtn, &QPushButton::clicked, this, &AIPage::refresh);
    row->addStretch();
    row->addWidget(m_refreshBtn);
    layout->addLayout(row);

    scroll->setWidget(inner);
    outer->addWidget(scroll);

    // timer
    m_timer = new QTimer(this);
    connect(m_timer, &QTimer::timeout, this, &AIPage::refresh);
    m_timer->start(15000);

    refresh();
}

// ──────────────────────────────────────────────────────────
//  buildCard
// ──────────────────────────────────────────────────────────

void AIPage::buildCard(int idx)
{
    auto &card = m_cards[idx];

    // parameter definitions per service
    QList<ParamDef> defs;
    if (idx == 0) {
        ParamDef o1, o2, o3, o4, o5, o6;
        o1  = {"OLLAMA_HOST",          "",    "监听地址",      WT_String, "127.0.0.1:11434"};
        o2  = {"OLLAMA_MODELS",        "",    "模型目录",      WT_String, "~/.ollama/models"};
        o2.browse = true; o2.browseDir = true;
        o3  = {"OLLAMA_NUM_PARALLEL",  "",    "并行请求数",    WT_Int,    "1",   1, 16};
        o4  = {"OLLAMA_KEEP_ALIVE",    "",    "连接保持时间",  WT_String, "5m"};
        o5  = {"OLLAMA_MAX_LOADED_MODELS", "", "最大加载模型数", WT_Int, "3", 1, 100};
        o6  = {"OLLAMA_MAX_QUEUE",     "",    "最大队列请求数", WT_Int,  "512", 1, 10000};
        defs = {o1, o2, o3, o4, o5, o6};
    } else if (idx == 1) {
        auto a = [](const QString &k, const QString &sk, const QString &l,
                     WidgetType wt, const QString &dv, int imin = 0, int imax = 0,
                     double dmin = 0, double dmax = 0, int dec = 2, double st = 0.05,
                     const QStringList &ch = {}, bool tog = false,
                     bool br = false, bool brDir = false) {
            ParamDef p;
            p.key = k; p.shortKey = sk; p.label = l; p.wtype = wt;
            p.defaultValue = dv; p.intMin = imin; p.intMax = imax;
            p.dblMin = dmin; p.dblMax = dmax; p.decimals = dec; p.step = st;
            p.choices = ch; p.isToggle = tog;
            p.browse = br; p.browseDir = brDir;
            return p;
        };

        defs = {
            // ── 基本 ──
            a("host",         "host",  "监听地址",     WT_String, "127.0.0.1"),
            a("port",         "port",  "端口",         WT_Int,    "8080",    1, 65535),
            a("model",        "m",     "模型路径",     WT_String, "", 0, 0, 0, 0, 2, 0.05, {}, false, true, false),
            a("n-gpu-layers", "ngl",   "GPU 层数",    WT_Int,    "-1",     -1, 999),
            a("ctx-size",     "c",     "上下文大小",   WT_Int,    "4096",  512, 131072),
            a("threads",      "t",     "线程数",       WT_Int,    "4",       1, 128),

            // ── 批处理 ──
            a("batch-size",   "b",     "批处理大小",   WT_Int,    "2048",   1, 2048),
            a("ubatch-size",  "ub",    "微批处理大小", WT_Int,    "512",    1, 2048),

            // ── 采样 ──
            a("temp",               "temp",              "采样温度",       WT_Double, "0.80", 0, 0, 0, 2.0, 2, 0.1),
            a("repeat-penalty",     "repeat-penalty",    "重复惩罚",       WT_Double, "1.00", 0, 0, 0, 2.0, 2, 0.05),
            a("top-k",              "top-k",             "Top-K",          WT_Int,    "40",   0, 100),
            a("top-p",              "top-p",             "Top-P",          WT_Double, "0.95", 0, 0, 0, 1.0, 2, 0.05),
            a("min-p",              "min-p",             "Min-P",          WT_Double, "0.05", 0, 0, 0, 1.0, 2, 0.05),
            a("typical-p",          "typical",           "Typical-P",      WT_Double, "1.00", 0, 0, 0, 1.0, 2, 0.05),
            a("tfs-z",              "tfs-z",             "TFS-Z",          WT_Double, "1.00", 0, 0, 0, 10.0, 2, 0.1),
            a("seed",               "s",                 "随机种子",       WT_Int,    "-1", -1, 2147483647),
            a("mirostat",           "mirostat",          "Mirostat 模式",  WT_Int,    "0",    0, 2),
            a("mirostat-lr",        "mirostat-lr",       "Mirostat 学习率", WT_Double, "0.10", 0, 0, 0, 1.0, 2, 0.05),
            a("mirostat-ent",       "mirostat-ent",      "Mirostat 目标熵", WT_Double, "5.00", 0, 0, 0, 10.0, 2, 0.1),
            a("repeat-last-n",      "repeat-last-n",     "重复惩罚范围",   WT_Int,    "64",   -1, 65536),
            a("presence-penalty",   "presence-penalty",  "存在惩罚",       WT_Double, "0.00", 0, 0, 0, 2.0, 2, 0.05),
            a("frequency-penalty",  "frequency-penalty", "频率惩罚",       WT_Double, "0.00", 0, 0, 0, 2.0, 2, 0.05),

            // ── KV 缓存 ──
            a("flash-attn",          "fa",    "Flash Attention",    WT_String, "auto", 0, 0, 0, 0, 0, 0,
               {"auto", "off", "on"}),
            a("cache-type-k",        "ctk",   "K 缓存类型",         WT_String, "f16",  0, 0, 0, 0, 0, 0,
               {"f16", "f32", "bf16", "q8_0", "q4_0", "q4_1", "iq4_nl", "q5_0", "q5_1"}),
            a("cache-type-v",        "ctv",   "V 缓存类型",         WT_String, "f16",  0, 0, 0, 0, 0, 0,
               {"f16", "f32", "bf16", "q8_0", "q4_0", "q4_1", "iq4_nl", "q5_0", "q5_1"}),
            a("no-kv-offload",       "nkvo",  "禁止 KV 卸载",       WT_String, "off",  0, 0, 0, 0, 0, 0,
               {"off", "on"}, true),
            a("mlock",               "mlock", "内存锁定",            WT_String, "off",  0, 0, 0, 0, 0, 0,
               {"off", "on"}, true),
            a("no-mmap",             "nmmap", "禁用内存映射",        WT_String, "off",  0, 0, 0, 0, 0, 0,
               {"off", "on"}, true),
            a("numa",                "numa",  "NUMA 优化",           WT_String, "off",  0, 0, 0, 0, 0, 0,
               {"off", "distribute", "isolate", "numactl"}),

            // ── 推测解码 ──
            a("spec-type",                "spec-type",           "推测解码类型",     WT_String, "none", 0, 0, 0, 0, 0, 0,
               {"none", "draft-simple", "draft-eagle3", "draft-mtp", "draft-dflash",
                "ngram-simple", "ngram-map-k", "ngram-cache"}),
            a("model-draft",              "md",                  "草稿模型路径",     WT_String, "", 0, 0, 0, 0, 2, 0.05, {}, false, true, false),
            a("n-gpu-layers-draft",       "ngld",                "草稿 GPU 层数",    WT_Int,    "-1", -1, 999),
            a("spec-draft-n-max",         "spec-draft-n-max",    "草稿生成长度",     WT_Int,    "3",   1, 20),
            a("spec-draft-p-split",       "draft-p-split",       "草稿拆分概率",     WT_Double, "0.10", 0, 0, 0, 1.0, 2, 0.05),

            // ── 服务器 ──
            a("threads-http",        "threads-http",  "HTTP 线程数",   WT_Int,    "-1",   -1, 128),
            a("parallel",            "np",            "并行槽位数",     WT_Int,    "-1",   -1, 256),
            a("timeout",             "to",            "超时秒数",       WT_Int,    "3600",  1, 86400),
            a("cont-batching",       "cb",            "连续批处理",     WT_String, "off",   0, 0, 0, 0, 0, 0,
               {"off", "on"}, true),
            a("jinja",               "jinja",          "Jinja 模板",     WT_String, "off",   0, 0, 0, 0, 0, 0,
               {"off", "on"}, true),
            a("embedding",           "embedding",      "嵌入模式",       WT_String, "off",   0, 0, 0, 0, 0, 0,
               {"off", "on"}, true),
        };
    } else if (idx == 2) {
        ParamDef v1, v2, v3, v4, v5, v6, v7, v8;
        v1 = {"host",                 "host",    "监听地址",          WT_String, "127.0.0.1"};
        v2 = {"port",                 "port",    "端口",              WT_Int,    "8000", 1, 65535};
        v3 = {"model",                "model",   "模型名称",          WT_String, ""};
        v4 = {"tensor-parallel-size", "tensor-parallel-size", "张量并行数", WT_Int, "1", 1, 8};
        v5.key = "gpu-memory-utilization"; v5.shortKey = "gpu-memory-utilization";
        v5.label = "GPU 内存利用率";
        v5.wtype = WT_Double;
        v5.defaultValue = "0.9";
        v5.dblMin = 0.0; v5.dblMax = 1.0;
        v5.decimals = 2; v5.step = 0.05;
        v6 = {"max-model-len",        "max-model-len", "最大模型长度",  WT_Int, "8192", 64, 524288};
        v7 = {"max-num-seqs",         "max-num-seqs",  "最大并行序列数", WT_Int, "256", 1, 10000};
        v8.key = "dtype"; v8.shortKey = "dtype"; v8.label = "数据类型"; v8.wtype = WT_String;
        v8.defaultValue = "auto";
        defs = {v1, v2, v3, v4, v5, v6, v7, v8};
    } else if (idx == 3) {
        auto a = [](const QString &k, const QString &sk, const QString &l,
                     WidgetType wt, const QString &dv, int imin = 0, int imax = 0,
                     double dmin = 0, double dmax = 0, int dec = 2, double st = 0.05,
                     const QStringList &ch = {}, bool tog = false, bool env = false,
                     bool br = false, bool brDir = false) {
            ParamDef p;
            p.key = k; p.shortKey = sk; p.label = l; p.wtype = wt;
            p.defaultValue = dv; p.intMin = imin; p.intMax = imax;
            p.dblMin = dmin; p.dblMax = dmax; p.decimals = dec; p.step = st;
            p.choices = ch; p.isToggle = tog; p.envVar = env;
            p.browse = br; p.browseDir = brDir;
            return p;
        };
        defs = {
            a("address",          "address",    "监听地址",       WT_String, ":8080"),
            a("models-path",      "models-path","模型路径",       WT_String, "", 0, 0, 0, 0, 2, 0.05, {}, false, false, true, true),
            a("config-dir",       "config-dir", "配置目录",       WT_String, "", 0, 0, 0, 0, 2, 0.05, {}, false, false, true, true),
            a("context-size",     "context-size","上下文大小",     WT_Int,    "512", 512, 131072),
            a("f16",              "f16",         "GPU 加速",       WT_String, "off",  0, 0, 0, 0, 0, 0,
               {"off", "on"}, true),
            a("upload-limit",     "upload-limit","上传限制(MB)",   WT_Int,    "15",   1, 1000),
            a("preload-models",   "preload-models","预加载模型",   WT_String, ""),
            a("cors",             "cors",         "CORS",           WT_String, "off",  0, 0, 0, 0, 0, 0,
               {"off", "on"}, true),
            a("galleries",        "galleries",   "模型库地址",     WT_String, ""),
            // Environment variable (not a CLI flag)
            a("CUDA_VISIBLE_DEVICES", "", "CUDA 设备", WT_String, "0", 0, 0, 0, 0, 0, 0,
              {}, false, true),
        };
    }

    auto *box = new QGroupBox();
    auto *vl  = new QVBoxLayout(box);
    vl->setSpacing(6);

    // header row: name + status + method badge
    auto *hdr = new QHBoxLayout();
    auto *nameLbl = new QLabel(card.name);
    QFont nf = nameLbl->font(); nf.setBold(true); nf.setPointSize(12);
    nameLbl->setFont(nf);
    hdr->addWidget(nameLbl);

    card.statusLbl = new QLabel("检测中…");
    card.statusLbl->setStyleSheet("font-size: 13px;");
    hdr->addWidget(card.statusLbl);

    card.methodLbl = new QLabel("");
    card.methodLbl->setStyleSheet(
        "background: #888; color: white; border-radius: 4px;"
        "padding: 1px 8px; font-size: 11px; font-weight: bold;");
    hdr->addWidget(card.methodLbl);

    hdr->addStretch();
    vl->addLayout(hdr);

    // detail label
    card.detailLbl = new QLabel("");
    card.detailLbl->setWordWrap(true);
    card.detailLbl->setStyleSheet("color: #888; font-size: 11px;");
    vl->addWidget(card.detailLbl);

    // separator
    auto *sep = new QFrame();
    sep->setFrameShape(QFrame::HLine);
    sep->setFrameShadow(QFrame::Sunken);
    vl->addWidget(sep);

    // parameter controls
    card.controlsWidget = new QWidget();
    auto *cl = new QVBoxLayout(card.controlsWidget);
    cl->setContentsMargins(0, 0, 0, 0);
    cl->setSpacing(4);

    card.paramDefs = defs;
    for (auto &p : card.paramDefs) {
        auto *row2 = new QHBoxLayout();
        auto *ll = new QLabel(p.label + ":");
        ll->setFixedWidth(110);
        row2->addWidget(ll);

        QWidget *w = createParamWidget(p, p.defaultValue);
        p.widget = w;
        row2->addWidget(w, 1);

        if (p.browse) {
            auto *browseBtn = new QPushButton("…");
            browseBtn->setFixedWidth(28);
            browseBtn->setFixedHeight(26);
            connect(browseBtn, &QPushButton::clicked, this, [this, &p]() {
                auto *le = qobject_cast<QLineEdit *>(p.widget);
                if (!le) return;
                QString start = le->text().isEmpty()
                    ? QDir::homePath() : le->text();
                QString path;
                if (p.browseDir)
                    path = QFileDialog::getExistingDirectory(
                        this, "选择目录", start);
                else
                    path = QFileDialog::getOpenFileName(
                        this, "选择文件", start,
                        "模型文件 (*.gguf);;所有文件 (*)");
                if (!path.isEmpty()) le->setText(path);
            });
            row2->addWidget(browseBtn);
        }

        cl->addLayout(row2);
    }

    vl->addWidget(card.controlsWidget);
    vl->addStretch();

    // button row
    auto *btnRow = new QHBoxLayout();
    btnRow->addStretch();

    card.applyBtn = new QPushButton("应用");
    card.applyBtn->setFixedWidth(80);
    int idxCaptured = idx;
    connect(card.applyBtn, &QPushButton::clicked, this, [this, idxCaptured]() {
        onApply(idxCaptured);
    });
    btnRow->addWidget(card.applyBtn);

    card.resetBtn = new QPushButton("复原");
    card.resetBtn->setFixedWidth(80);
    connect(card.resetBtn, &QPushButton::clicked, this, [this, idxCaptured]() {
        onReset(idxCaptured);
    });
    btnRow->addWidget(card.resetBtn);

    vl->addLayout(btnRow);
}

// ──────────────────────────────────────────────────────────
//  parameter widget helpers
// ──────────────────────────────────────────────────────────

QWidget *AIPage::createParamWidget(ParamDef &def, const QString &value)
{
    QWidget *w = nullptr;
    if (!def.choices.isEmpty()) {
        auto *cb = new QComboBox();
        cb->addItems(def.choices);
        int idx = cb->findText(value);
        if (idx >= 0) cb->setCurrentIndex(idx);
        cb->setMinimumHeight(26);
        w = cb;
    } else if (def.wtype == WT_Int) {
        auto *sp = new QSpinBox();
        sp->setRange(def.intMin, def.intMax);
        sp->setValue(value.toInt());
        sp->setMinimumHeight(26);
        w = sp;
    } else if (def.wtype == WT_Double) {
        auto *sp = new QDoubleSpinBox();
        sp->setRange(def.dblMin, def.dblMax);
        sp->setDecimals(def.decimals);
        sp->setSingleStep(def.step);
        sp->setValue(value.toDouble());
        sp->setMinimumHeight(26);
        w = sp;
    } else {
        auto *le = new QLineEdit(value);
        le->setMinimumHeight(26);
        w = le;
    }
    w->installEventFilter(m_nowheel);
    return w;
}

QString AIPage::getParamValue(const ParamDef &def)
{
    if (!def.choices.isEmpty())
        return static_cast<QComboBox *>(def.widget)->currentText();
    if (def.wtype == WT_Int)
        return QString::number(static_cast<QSpinBox *>(def.widget)->value());
    if (def.wtype == WT_Double)
        return QString::number(
            static_cast<QDoubleSpinBox *>(def.widget)->value(), 'f', def.decimals);
    return static_cast<QLineEdit *>(def.widget)->text();
}

void AIPage::setParamValue(ParamDef &def, const QString &value)
{
    if (!def.choices.isEmpty()) {
        auto *cb = static_cast<QComboBox *>(def.widget);
        int idx = cb->findText(value);
        if (idx >= 0) cb->setCurrentIndex(idx);
        return;
    }
    if (def.wtype == WT_Int)
        static_cast<QSpinBox *>(def.widget)->setValue(value.toInt());
    else if (def.wtype == WT_Double)
        static_cast<QDoubleSpinBox *>(def.widget)->setValue(value.toDouble());
    else
        static_cast<QLineEdit *>(def.widget)->setText(value);
}

// ──────────────────────────────────────────────────────────
//  runCmd helper
// ──────────────────────────────────────────────────────────

void AIPage::runCmd(const QString &cmd, const QStringList &args,
                    std::function<void(const QString &, int)> cb)
{
    auto *p = new QProcess(this);
    auto handler = [p, cb](int code, QProcess::ExitStatus) {
        cb(QString::fromUtf8(p->readAllStandardOutput()).trimmed(), code);
        p->deleteLater();
    };
    connect(p, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, handler);
    p->setProcessChannelMode(QProcess::MergedChannels);
    p->start(cmd, args);
}

// ──────────────────────────────────────────────────────────
//  status detection
// ──────────────────────────────────────────────────────────

void AIPage::checkService(int idx)
{
    auto &card = m_cards[idx];
    card.statusLbl->setText("检测中…");
    card.detailLbl->setText("");
    card.methodLbl->setText("");
    card.svcFilePath.clear();
    updateControlStates(idx);

    // 1) systemd --user
    runCmd("systemctl", {"--user", "is-active", card.systemdSvc},
           [this, idx](const QString &out, int code) {
        if (code == 0 && out == "active") {
            finishSystemdCheck(idx, true);
            return;
        }
        // 2) systemd system-level
        runCmd("systemctl", {"is-active", m_cards[idx].systemdSvc},
               [this, idx](const QString &out2, int code2) {
            if (code2 == 0 && out2 == "active") {
                finishSystemdCheck(idx, false);
                return;
            }
            // 3) fallback: pgrep + try to find service file anyway
            finishProcessCheck(idx);
        });
    });
}

void AIPage::finishSystemdCheck(int idx, bool userScope)
{
    auto &c = m_cards[idx];
    c.running = true;
    c.startMethod = "service";
    c.statusLbl->setStyleSheet("color: #2e7d32; font-size: 13px;");
    c.statusLbl->setText("🟢 运行中");
    c.methodLbl->setStyleSheet(
        "background: #2e7d32; color: white; border-radius: 4px;"
        "padding: 1px 8px; font-size: 11px; font-weight: bold;");
    c.methodLbl->setText("systemd");

    QStringList showArgs = {"show", "-P", "MainPID", "-P", "FragmentPath",
                            c.systemdSvc};
    if (userScope)
        showArgs.prepend("--user");

    runCmd("systemctl", showArgs, [this, idx](const QString &out, int) {
        auto &c2 = m_cards[idx];
        QStringList lines = out.split('\n');
        // systemctl outputs -P values alphabetically (FragmentPath before MainPID)
        c2.svcFilePath = lines.value(0);
        c2.pidStr = lines.value(1);
        QString detail = QString("PID: %1").arg(c2.pidStr);
        if (!c2.svcFilePath.isEmpty())
            detail += QString("\n服务文件: %1").arg(c2.svcFilePath);
        else
            tryFindServiceFile(idx);
        c2.detailLbl->setText(detail);
        if (!c2.svcFilePath.isEmpty())
            readServiceFile(idx);
        updateControlStates(idx);
    });
}

void AIPage::finishProcessCheck(int idx)
{
    auto &c = m_cards[idx];

    // try to find service file for param reading even in process mode
    tryFindServiceFile(idx);

    runCmd("pgrep", {"-f", c.procName},
           [this, idx](const QString &out, int) {
        auto &c2 = m_cards[idx];
        QString pidStr = out.trimmed();
        if (!pidStr.isEmpty()) {
            c2.running = true;
            c2.startMethod = "process";
            c2.pidStr = pidStr.split('\n').first();
            c2.statusLbl->setStyleSheet("color: #2e7d32; font-size: 13px;");
            c2.statusLbl->setText("🟢 运行中");
            c2.methodLbl->setStyleSheet(
                "background: #e65100; color: white; border-radius: 4px;"
                "padding: 1px 8px; font-size: 11px; font-weight: bold;");
            c2.methodLbl->setText("进程");
            QString detail = QString("PID: %1\n(非 systemd 启动)").arg(c2.pidStr);
            if (!c2.svcFilePath.isEmpty())
                detail += QString("\n服务文件: %1").arg(c2.svcFilePath);
            c2.detailLbl->setText(detail);
        } else {
            c2.running = false;
            c2.startMethod = "unknown";
            c2.pidStr.clear();
            c2.statusLbl->setStyleSheet("color: #888; font-size: 13px;");
            c2.statusLbl->setText("⚪ 未运行");
            c2.methodLbl->setStyleSheet(
                "background: #888; color: white; border-radius: 4px;"
                "padding: 1px 8px; font-size: 11px; font-weight: bold;");
            c2.methodLbl->setText("未启动");
            c2.detailLbl->setText("");
        }
        updateControlStates(idx);
    });
}

void AIPage::tryFindServiceFile(int idx)
{
    auto &c = m_cards[idx];
    if (!c.svcFilePath.isEmpty()) return;

    // try systemctl --user show FragmentPath
    runCmd("systemctl", {"--user", "show", "-P", "FragmentPath", c.systemdSvc},
           [this, idx](const QString &out, int) {
        auto &c2 = m_cards[idx];
        QString path = out.trimmed();
        if (!path.isEmpty() && QFile::exists(path)) {
            c2.svcFilePath = path;
            readServiceFile(idx);
            return;
        }
        // fallback to common paths
        QStringList candidates = {
            m_configDir + "/" + c2.systemdSvc,
            "/etc/systemd/system/" + c2.systemdSvc,
            "/usr/lib/systemd/system/" + c2.systemdSvc
        };
        for (const auto &p : candidates) {
            if (QFile::exists(p)) {
                c2.svcFilePath = p;
                readServiceFile(idx);
                return;
            }
        }
    });
}

// ──────────────────────────────────────────────────────────
//  read service file parameters
// ──────────────────────────────────────────────────────────

void AIPage::readServiceFile(int idx)
{
    auto &card = m_cards[idx];
    if (card.svcFilePath.isEmpty()) return;

    QFile f(card.svcFilePath);
    if (!f.open(QIODevice::ReadOnly)) return;

    QString content = QString::fromUtf8(f.readAll());
    f.close();

    // Join continuation lines so multi-line ExecStart can be parsed
    content.replace(QRegularExpression(R"(\\\s*\n\s*)"), " ");

    bool isOllama = (idx == 0);

    for (auto &p : card.paramDefs) {
        QString found;
        if (isOllama) {
            QString pattern = QStringLiteral(
                R"re(Environment="%1=(.*)")re")
                    .arg(QRegularExpression::escape(p.key));
            QRegularExpression re(pattern);
            auto m = re.match(content);
            if (m.hasMatch())
                found = m.captured(1).trimmed();
        } else {
            QString escKey = QRegularExpression::escape(p.key);

            // Environment variable: Environment="KEY=VALUE"
            if (p.envVar) {
                QString pat = QStringLiteral(
                    R"re(Environment="%1=(.*)")re").arg(escKey);
                auto m2 = QRegularExpression(pat).match(content);
                if (m2.hasMatch())
                    found = m2.captured(1).trimmed();
            }

            // Try long form: --key value
            if (found.isEmpty()) {
                QString pattern = QStringLiteral(
                    R"re(--%1\s+(\S+))re").arg(escKey);
                QRegularExpression re(pattern);
                auto m = re.match(content);
                if (m.hasMatch()) {
                    found = m.captured(1).trimmed();
                }
            }
            // Try short form: -shortKey value (with whitespace before dash)
            if (found.isEmpty() && !p.shortKey.isEmpty()
                                  && p.shortKey != p.key) {
                QString escShort = QRegularExpression::escape(p.shortKey);
                QString shortPattern = QStringLiteral(
                    R"re(\s-%1\s+(\S+))re").arg(escShort);
                QRegularExpression shortRe(shortPattern);
                auto sm = shortRe.match(content);
                if (sm.hasMatch())
                    found = sm.captured(1).trimmed();
            }
            // Toggle flag: --key or -shortKey present without value → "on"
            if (found.isEmpty() && p.isToggle) {
                QStringList toTry;
                toTry << QStringLiteral(R"re(\s--%1(?:\s|\\|\n|$))re").arg(escKey);
                if (!p.shortKey.isEmpty() && p.shortKey != p.key) {
                    QString escS = QRegularExpression::escape(p.shortKey);
                    toTry << QStringLiteral(R"re(\s-%1(?:\s|\\|\n|$))re").arg(escS);
                }
                for (const auto &pat : toTry) {
                    if (QRegularExpression(pat).match(content).hasMatch()) {
                        found = "on";
                        break;
                    }
                }
            }
        }
        if (!found.isEmpty())
            setParamValue(p, found);
    }
}

// ──────────────────────────────────────────────────────────
//  apply via service
// ──────────────────────────────────────────────────────────

void AIPage::applyService(int idx)
{
    auto &card = m_cards[idx];
    if (card.svcFilePath.isEmpty()) {
        findServiceFile(idx);
        if (card.svcFilePath.isEmpty()) {
            QMessageBox::warning(this, "错误",
                QString("找不到 %1 的服务文件").arg(card.name));
            return;
        }
    }

    QFile f(card.svcFilePath);
    if (!f.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, "错误",
            QString("无法读取服务文件: %1").arg(card.svcFilePath));
        return;
    }
    QString content = QString::fromUtf8(f.readAll());
    f.close();

    // Join continuation lines for consistent matching
    QString flat = content;
    flat.replace(QRegularExpression(R"(\\\s*\n\s*)"), " ");

    bool isOllama = (idx == 0);

    for (const auto &p : card.paramDefs) {
        QString newVal = getParamValue(p);

        if (isOllama) {
            QString escKey = QRegularExpression::escape(p.key);
            QString pattern = QStringLiteral(
                R"re(Environment="%1=.*)re").arg(escKey);
            QRegularExpression re(pattern);
            QString replacement = QStringLiteral(
                "Environment=\"%1=%2\"").arg(p.key, newVal);
            if (content.contains(re)) {
                content.replace(re, replacement);
            } else {
                int svcPos = content.indexOf("[Service]");
                if (svcPos >= 0) {
                    int lineEnd = content.indexOf('\n', svcPos);
                    if (lineEnd < 0) lineEnd = svcPos;
                    content.insert(lineEnd + 1, replacement + "\n");
                }
            }
        } else {
            QString escKey = QRegularExpression::escape(p.key);

            // ── Environment variable: Environment="KEY=VALUE" ──
            if (p.envVar) {
                QString pat = QStringLiteral(
                    R"re(Environment="%1=.*)re").arg(escKey);
                QString repl = QStringLiteral(
                    "Environment=\"%1=%2\"").arg(p.key, newVal);
                if (content.contains(QRegularExpression(pat)))
                    content.replace(QRegularExpression(pat), repl);
                else {
                    int svcPos = content.indexOf("[Service]");
                    if (svcPos >= 0) {
                        int le = content.indexOf('\n', svcPos);
                        if (le < 0) le = svcPos;
                        content.insert(le + 1, repl + "\n");
                    }
                }
                continue;
            }

            // ── Toggle flag: --key present/absent, no value ──
            if (p.isToggle) {
                // Check long and short forms
                bool hasFlag = false;
                QStringList flagPats;
                flagPats << QStringLiteral(R"re(\s--%1(?:\s|\\|\n|$))re").arg(escKey);
                if (!p.shortKey.isEmpty() && p.shortKey != p.key) {
                    QString escS = QRegularExpression::escape(p.shortKey);
                    flagPats << QStringLiteral(R"re(\s-%1(?:\s|\\|\n|$))re").arg(escS);
                }
                for (const auto &pat : flagPats) {
                    if (QRegularExpression(pat).match(flat).hasMatch()) {
                        hasFlag = true;
                        break;
                    }
                }

                if (newVal == "on" && !hasFlag) {
                    QRegularExpression esRe(QStringLiteral("ExecStart=.+"));
                    auto m = esRe.match(flat);
                    if (m.hasMatch()) {
                        int lastCont = content.lastIndexOf(" \\\n");
                        QString add = QString(" \\\n    --%1").arg(p.key);
                        if (lastCont >= 0)
                            content.insert(lastCont + 3, add.mid(3));
                        else
                            content.replace(m.captured(0),
                                m.captured(0) + " " + add.trimmed());
                    }
                } else if (newVal == "off" && hasFlag) {
                    for (const auto &pat : flagPats)
                        content.remove(QRegularExpression(pat));
                }
                continue;
            }

            // ── Value-based param: --key value ──
            QString replacement = QStringLiteral(
                "--%1 %2").arg(p.key, newVal);
            bool found = false;

            // Try long form: --key value
            QString longPat = QStringLiteral(
                R"re(--%1\s+\S+)re").arg(escKey);
            QRegularExpression longRe(longPat);
            if (flat.contains(longRe)) {
                found = true;
                QString origMatch;
                auto mi = longRe.match(flat);
                if (mi.hasMatch()) origMatch = mi.captured(0);
                flat.replace(longRe, replacement);
                if (!origMatch.isEmpty())
                    content.replace(origMatch, replacement);
                else
                    content.replace(longRe, replacement);
            }

            // Try short form: -shortKey value
            if (!found && !p.shortKey.isEmpty()
                       && p.shortKey != p.key) {
                QString escShort = QRegularExpression::escape(p.shortKey);
                QString shortPat = QStringLiteral(
                    R"re(\s-%1\s+\S+)re").arg(escShort);
                QRegularExpression shortRe(shortPat);
                if (flat.contains(shortRe)) {
                    found = true;
                    QString origMatch;
                    auto mi = shortRe.match(flat);
                    if (mi.hasMatch()) origMatch = mi.captured(0);
                    flat.replace(shortRe,
                        QString(" %1").arg(replacement));
                    QString escOrig = QRegularExpression::escape(origMatch);
                    content.replace(
                        QRegularExpression(escOrig),
                        QString(" %1").arg(replacement));
                }
            }

            if (!found) {
                QRegularExpression esRe(QStringLiteral("ExecStart=.+"));
                auto m = esRe.match(flat);
                if (m.hasMatch()) {
                    QString appendStr = QString(" \\\n    %1").arg(replacement);
                    int lastCont = content.lastIndexOf(" \\\n");
                    if (lastCont >= 0)
                        content.insert(lastCont + 3, appendStr.mid(3));
                    else
                        content.replace(m.captured(0),
                            m.captured(0) + " " + replacement);
                }
            }
        }
    }

    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QMessageBox::warning(this, "错误",
            QString("无法写入服务文件: %1").arg(card.svcFilePath));
        return;
    }
    f.write(content.toUtf8());
    f.close();

    // reload & restart
    runCmd("systemctl", {"--user", "daemon-reload"},
           [this, idx](const QString &, int) {
        auto &c = m_cards[idx];
        runCmd("systemctl", {"--user", "restart", c.systemdSvc},
               [this, idx](const QString &out, int code) {
            auto &c2 = m_cards[idx];
            if (code == 0)
                QMessageBox::information(this, "成功",
                    QString("%1 已重新启动").arg(c2.name));
            else
                QMessageBox::warning(this, "错误",
                    QString("重启 %1 失败:\n%2").arg(c2.name, out));
            QTimer::singleShot(2000, this, &AIPage::refresh);
        });
    });
}

// ──────────────────────────────────────────────────────────
//  apply via process (kill + restart)
// ──────────────────────────────────────────────────────────

void AIPage::applyProcess(int idx)
{
    auto &card = m_cards[idx];

    if (card.pidStr.isEmpty()) {
        QMessageBox::warning(this, "错误",
            QString("%1 未运行，无法重启").arg(card.name));
        return;
    }

    // kill
    runCmd("kill", {card.pidStr}, [this, idx](const QString &, int) {
        QTimer::singleShot(600, this, [this, idx]() {
            auto &c2 = m_cards[idx];
            QProcess proc;

            if (idx == 0) {
                // Ollama: env vars + ollama serve
                auto penv = QProcessEnvironment::systemEnvironment();
                for (const auto &p : c2.paramDefs)
                    penv.insert(p.key, getParamValue(p));
                proc.setProcessEnvironment(penv);
                proc.setProgram("ollama");
                proc.setArguments({"serve"});
            } else if (idx == 1) {
                // llama-server --flag value ...
                QStringList args;
                args << "llama-server";
                for (const auto &p : c2.paramDefs) {
                    args << QString("--%1").arg(p.key);
                    args << getParamValue(p);
                }
                proc.setProgram(args.first());
                args.removeFirst();
                proc.setArguments(args);
            } else {
                // vllm serve --flag value ...
                QStringList args;
                args << "vllm" << "serve";
                for (const auto &p : c2.paramDefs) {
                    args << QString("--%1").arg(p.key);
                    args << getParamValue(p);
                }
                proc.setProgram(args.first());
                args.removeFirst();
                proc.setArguments(args);
            }

            proc.setProcessChannelMode(QProcess::ForwardedChannels);
            if (!proc.startDetached()) {
                QMessageBox::warning(this, "错误",
                    QString("启动 %1 失败").arg(c2.name));
                return;
            }

            QMessageBox::information(this, "成功",
                QString("%1 已重新启动").arg(c2.name));
            QTimer::singleShot(2000, this, &AIPage::refresh);
        });
    });
}

// ──────────────────────────────────────────────────────────
//  find service file
// ──────────────────────────────────────────────────────────

void AIPage::findServiceFile(int idx)
{
    auto &card = m_cards[idx];
    QString svcName = card.systemdSvc;

    // try systemctl show first
    runCmd("systemctl", {"--user", "show", "-P", "FragmentPath", svcName},
           [this, idx](const QString &out, int) {
        auto &c = m_cards[idx];
        QString path = out.trimmed();
        if (!path.isEmpty() && QFile::exists(path)) {
            c.svcFilePath = path;
            readServiceFile(idx);
            return;
        }
        // fallback
        QStringList candidates = {
            m_configDir + "/" + c.systemdSvc,
            "/etc/systemd/system/" + c.systemdSvc,
            "/usr/lib/systemd/system/" + c.systemdSvc
        };
        for (const auto &p : candidates) {
            if (QFile::exists(p)) {
                c.svcFilePath = p;
                readServiceFile(idx);
                return;
            }
        }
    });
}

// ──────────────────────────────────────────────────────────
//  apply button
// ──────────────────────────────────────────────────────────

void AIPage::onApply(int idx)
{
    auto &card = m_cards[idx];

    if (!card.running) {
        if (!card.svcFilePath.isEmpty() || card.startMethod == "service") {
            applyService(idx);
        } else {
            QMessageBox::information(this, "提示",
                QString("%1 未运行。请先启动后再使用应用功能。")
                    .arg(card.name));
        }
        return;
    }

    auto ret = QMessageBox::question(this, "确认",
        QString("%1 正在运行。\n修改参数将重启该服务，是否继续？")
            .arg(card.name),
        QMessageBox::Yes | QMessageBox::No);

    if (ret != QMessageBox::Yes) return;

    if (card.startMethod == "service")
        applyService(idx);
    else
        applyProcess(idx);
}

// ──────────────────────────────────────────────────────────
//  reset button
// ──────────────────────────────────────────────────────────

void AIPage::onReset(int idx)
{
    auto &card = m_cards[idx];

    for (auto &p : card.paramDefs)
        setParamValue(p, p.defaultValue);

    if (!card.svcFilePath.isEmpty())
        readServiceFile(idx);
}

// ──────────────────────────────────────────────────────────
//  update control enabled state
// ──────────────────────────────────────────────────────────

void AIPage::updateControlStates(int idx)
{
    auto &card = m_cards[idx];
    card.controlsWidget->setEnabled(card.running);
    card.applyBtn->setEnabled(card.running);
}

// ──────────────────────────────────────────────────────────
//  refresh all
// ──────────────────────────────────────────────────────────

void AIPage::refresh()
{
    for (auto &c : m_cards) {
        c.running = false;
        c.startMethod = "unknown";
        c.pidStr.clear();
    }

    m_info->setText("正在检查服务状态…");

    for (int i = 0; i < 3; ++i)
        checkService(i);

    QTimer::singleShot(2500, this, [this]() {
        int running = 0;
        for (const auto &c : m_cards)
            if (c.running) ++running;
        m_info->setText(QString("AI 后端: %1 / 3 运行中").arg(running));
    });
}
