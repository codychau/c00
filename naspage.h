#ifndef NASPAGE_H
#define NASPAGE_H

#include <QWidget>
#include <QVector>
#include <QLabel>
#include <QPushButton>
#include <QComboBox>
#include <QLineEdit>
#include <QSpinBox>
#include <QVector>
#include "nasmodel.h"
#include "nasboardwidget.h"
#include "nascontroller.h"

class QCheckBox;
class QLabel;

// ─────────────────────────────────────────────────────────────
//  NAS 页面：挑分区 → 拖线组 RAID → 导出 iSCSI
//
//  分工：
//   NasBoardWidget  只管画与报"用户想连谁"，无副作用
//   NasController   只管生成/执行命令，不碰界面
//   NasPage         夹在中间：拦下危险操作、把结论翻译给人看
//
//  安全线（改动前请先读）：
//   - 拖线本身**不碰磁盘**，随便试
//   - 「创建阵列」和「导出 iSCSI」是两条独立确认：阵列建成了还不一定对外
//   - 破坏性命令必须逐条展示给用户，不接受一次性"我确认了全部"
// ─────────────────────────────────────────────────────────────
class NasPage : public QWidget
{
    Q_OBJECT
public:
    explicit NasPage(QWidget *parent = nullptr);

private slots:
    void refresh();                       // 重新枚举分区与环境
    void onSlotClicked(const QString &device, int slotIndex);
    void onSlotRejected(const QString &device, int slotIndex, const QString &why);
    void onEngineChanged(int index);
    void createArray();
    void exportIscsi();
    void teardownIscsi();
    void showStepLog(int index, bool ok, const QString &output);
    void onAllFinished(bool success);

private:
    // 把画板上的连线收集成可执行的 Plan
    Nas::Plan currentPlan() const;
    void updateSummary();
    // 逐条展示命令并要用户确认；返回要执行的步骤（可能为空 = 用户取消）
    QVector<NasController::Step> confirmSteps(const QVector<NasController::Step> &steps,
                                              const QString &title);
    void setStatus(const QString &text, bool bad = false);

    NasBoardWidget *m_board = nullptr;
    QLabel *m_envLabel = nullptr;      // 环境是否齐备
    QLabel *m_status = nullptr;
    QComboBox *m_engine = nullptr;
    QLineEdit *m_volName = nullptr;    // 阵列/卷名（破坏性确认时要手输核对）
    QSpinBox *m_chunk = nullptr;       // 条带粒度
    QLineEdit *m_iqn = nullptr;
    QLineEdit *m_portal = nullptr;     // portal 监听地址（**不是**网段白名单）
    QLabel *m_portalNote = nullptr;
    QCheckBox *m_anonAcl = nullptr;    // 允许匿名 initiator（默认关）
    QPushButton *m_createBtn = nullptr;
    QPushButton *m_exportBtn = nullptr;
    QPushButton *m_teardownBtn = nullptr;
    QPushButton *m_refreshBtn = nullptr;

    NasController m_ctl;
    NasController::Env m_env;
    QVector<NasBoardWidget::Disk> m_disks;
    QString m_pendingConfirmName;      // 需要手输核对的卷名
    // 最近一次成功导出的对象，撤销时要用同一组值，不能让用户改完再撤
    QString m_currentIqn;
    QString m_currentBlockDev;
};

#endif // NASPAGE_H
