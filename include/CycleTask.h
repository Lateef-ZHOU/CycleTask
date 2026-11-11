#pragma once
#include <QtQml/qqmlregistration.h>
#include <QScopeGuard>
#include "TaskBase.h"
#include "WindProfile.h"

// 环形四轴：页面持世界角输入，协程生成 Collector 并直接表达编排。
class CycleTask final : public TaskBase
{
    Q_OBJECT
    QML_ELEMENT
    QML_ATTACHED(CycleTask)
    QML_UNCREATABLE("由对应 QML 官邸创建和拥有")
public:
    static CycleTask *qmlAttachedProperties(QObject *owner) { return new CycleTask(owner); }
    explicit CycleTask(QObject *parent) : TaskBase(parent) {}
    ~CycleTask() override { cancelRun(); }
    [[nodiscard]] QString displayName() const override { return QStringLiteral("环形四轴"); }
    [[nodiscard]] QString subtitle() const override { return QStringLiteral("顺序循环"); }
    [[nodiscard]] int beamCount() const noexcept override { return WindProfile::kBeams; }
private:
    DeviceCoro run(LockRequest, GMBFamily &gmb, Cards cards, QList<double> worldAzimuths,
                   double worldElevation, int accumSec, std::chrono::milliseconds hold,
                   std::optional<GMBMoveParameters::Speeds> speeds, std::optional<QDir> saveRoot) override
    {
        const auto completion = qScopeGuard([this] {
            QMetaObject::invokeMethod(this, &CycleTask::refreshRunState, Qt::QueuedConnection);
        });
        const auto lock = co_await HeldLock{};
        if (!lock)
        {
            Q_EMIT logMessage("设备被占用或未连接，任务未启动");
            co_return;
        }
        // 全部世界目标先经官邸 paramsFor 校验，再发任何任务指令；协程只保存运动参数，到位结果仍是 Collector。
        QList<GMBMoveParameters> targets;
        for (const double azimuth : worldAzimuths)
        {
            auto parameters = gmb.paramsFor(gmb.gimbalOf(WorldModel(azimuth, worldElevation)), hold, speeds);
            if (!parameters)
            {
                Q_EMIT logMessage("转台目标或运动参数无效，任务未启动");
                co_return;
            }
            targets.push_back(std::move(*parameters));
        }
        QDateTime sessionTime = beginSession();
        constexpr int last = WindProfile::kBeams - 1;
        for (;;)
        {
            for (int beam = 0; beam <= last; ++beam)
            {
                const auto moved = co_await moveTo(lock, gmb, targets[beam], beam);
                if (moved.empty())
                {
                    Q_EMIT logMessage("转台 失败 / 设备断开 / 任务停止");
                    co_return;
                }
                const auto framed = co_await acquireAt(lock, gmb, cards, moved, beam, accumSec, saveRoot, sessionTime, beam == last);
                if (framed.empty())
                {
                    Q_EMIT logMessage("采集卡 失败 / 设备断开 / 任务停止");
                    co_return;
                }
                completeBeam();
            }
            sessionTime = QDateTime::currentDateTime();
        }
    }
};
