#include "qt_accessible_spike.h"
#include "host_qt.h"

#include <QtGui/QAccessible>
#include <QtWidgets/QAccessibleWidget>

namespace tk::qt6
{

namespace
{

// Non-QObject-backed leaf accessible object (mirrors Qt's own
// QAccessibleHyperlink pattern for "virtual" children that have no
// corresponding QObject) — this is exactly the situation tk::Widget
// children are in: real objects with real semantics, but not QObjects.
class ButtonAccessible : public QAccessibleInterface, public QAccessibleActionInterface
{
public:
    explicit ButtonAccessible(QWidget* surface) : surface_(surface) {}

    // ---- QAccessibleInterface ----
    bool isValid() const override
    {
        return surface_ != nullptr;
    }
    QObject* object() const override
    {
        return nullptr; // no QObject backs this node, like QAccessibleHyperlink
    }
    QWindow* window() const override
    {
        return surface_ ? surface_->windowHandle() : nullptr;
    }
    QAccessibleInterface* childAt(int, int) const override
    {
        return nullptr; // leaf
    }
    QAccessibleInterface* parent() const override
    {
        return QAccessible::queryAccessibleInterface(surface_);
    }
    QAccessibleInterface* child(int) const override
    {
        return nullptr; // leaf
    }
    int childCount() const override
    {
        return 0;
    }
    int indexOfChild(const QAccessibleInterface*) const override
    {
        return -1;
    }
    QString text(QAccessible::Text t) const override
    {
        if (t == QAccessible::Name)
            return QStringLiteral("Tesseract accessibility spike button");
        return {};
    }
    void setText(QAccessible::Text, const QString&) override {}
    QRect rect() const override
    {
        if (!surface_)
            return {};
        return QRect(surface_->mapToGlobal(QPoint(0, 0)), surface_->size());
    }
    QAccessible::Role role() const override
    {
        return QAccessible::Button;
    }
    QAccessible::State state() const override
    {
        QAccessible::State s{};
        s.focusable = true;
        return s;
    }
    void* interface_cast(QAccessible::InterfaceType t) override
    {
        if (t == QAccessible::ActionInterface)
            return static_cast<QAccessibleActionInterface*>(this);
        return nullptr;
    }

    // ---- QAccessibleActionInterface ----
    QStringList actionNames() const override
    {
        return {QAccessibleActionInterface::pressAction()};
    }
    void doAction(const QString& actionName) override
    {
        if (actionName == QAccessibleActionInterface::pressAction())
            qWarning("[tesseract-access-qt spike] press action received");
    }
    QStringList keyBindingsForAction(const QString&) const override
    {
        return {};
    }

private:
    QWidget* surface_;
};

// Root interface for the tk::qt6::Surface QWidget itself. Overrides just
// enough of QAccessibleWidget's default (real-QObject-child-based)
// navigation to inject the one hardcoded virtual child instead.
class SurfaceAccessible : public QAccessibleWidget
{
public:
    explicit SurfaceAccessible(QWidget* surface)
        : QAccessibleWidget(surface, QAccessible::Pane)
    {
    }

    int childCount() const override
    {
        return 1;
    }
    QAccessibleInterface* child(int index) const override
    {
        if (index != 0)
            return nullptr;
        if (child_id_ == 0)
        {
            auto* iface = new ButtonAccessible(widget());
            child_id_   = QAccessible::registerAccessibleInterface(iface);
        }
        return QAccessible::accessibleInterface(child_id_);
    }
    int indexOfChild(const QAccessibleInterface* iface) const override
    {
        if (child_id_ != 0 && iface == QAccessible::accessibleInterface(child_id_))
            return 0;
        return -1;
    }

private:
    // QAccessibleCache owns the registered interface once
    // registerAccessibleInterface() returns — see its ownership contract —
    // so this class must not delete it; just remember the id to reuse the
    // same instance across repeated child(0) queries. Mutable: child() is
    // const per QAccessibleInterface's contract, but lazily creating the
    // one virtual child on first query is an implementation detail, not an
    // observable state change.
    mutable QAccessible::Id child_id_ = 0;
};

QAccessibleInterface* factory(const QString&, QObject* object)
{
    // Surface has no Q_OBJECT (it never needed its own signals/slots), so
    // it has no metaObject()/className() of its own — Qt's factory
    // dispatch would call us with classname == "QWidget" (its nearest
    // Q_OBJECT ancestor) for every plain QWidget in the app, not just
    // Surface instances. dynamic_cast works regardless of Q_OBJECT (RTTI
    // only needs a polymorphic type, which QWidget already is), so use it
    // directly instead of trying to match on classname.
    if (auto* surface = dynamic_cast<Surface*>(object))
        return new SurfaceAccessible(surface);
    return nullptr;
}

} // namespace

void install_accessible_spike_factory()
{
    QAccessible::installFactory(factory);
}

} // namespace tk::qt6
