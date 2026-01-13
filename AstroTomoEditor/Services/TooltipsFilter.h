#pragma once
#include <QObject>

class TooltipsFilter final : public QObject
{
    Q_OBJECT
public:
    static TooltipsFilter& instance();

    void setEnabled(bool on) { mEnabled = on; }
    bool enabled() const { return mEnabled; }

protected:
    bool eventFilter(QObject* obj, QEvent* ev) override;

private:
    TooltipsFilter() = default;
    bool mEnabled = true;
};
