#include "TooltipsFilter.h"

#include <QEvent>
#include <QToolTip>

TooltipsFilter& TooltipsFilter::instance()
{
    static TooltipsFilter s;
    return s;
}

bool TooltipsFilter::eventFilter(QObject* obj, QEvent* ev)
{
    if (!mEnabled && ev && ev->type() == QEvent::ToolTip)
    {
        QToolTip::hideText();
        ev->ignore();
        return true; // съели событие, тултип не появится
    }
    return QObject::eventFilter(obj, ev);
}
