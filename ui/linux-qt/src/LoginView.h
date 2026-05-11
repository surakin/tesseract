#pragma once
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QStackedWidget>
#include <QString>
#include <QWidget>

#include <tesseract/client.h>

#include <atomic>
#include <thread>

namespace qt6 {

/// Inline sign-in view shown inside the main window when the user is not
/// logged in. Drives the same two-phase OAuth / MAS flow as the previous
/// modal LoginDialog (form → worker → browser → worker → done), but is a
/// plain QWidget the main window can swap into a QStackedWidget instead of
/// running modally.
class LoginView final : public QWidget {
    Q_OBJECT
public:
    explicit LoginView(tesseract::Client& client, QWidget* parent = nullptr);
    ~LoginView() override;

    /// Return the view to its initial "form" state. Call before showing the
    /// view again after a successful sign-in or logout.
    void reset();

signals:
    void loginSucceeded();

    /// Worker → UI thread completion notifications.
    void beginCompleted(bool ok, QString errorOrAuthUrl);
    void awaitCompleted(bool ok, QString error);

private slots:
    void onSignIn();
    void onCancel();
    void onBeginCompleted(bool ok, QString errorOrAuthUrl);
    void onAwaitCompleted(bool ok, QString error);

private:
    void showForm();
    void showWaiting();
    void joinWorker();

    tesseract::Client& client_;

    QStackedWidget* stack_       = nullptr;
    QLineEdit*      hsEdit_      = nullptr;
    QPushButton*    signInBtn_   = nullptr;
    QLabel*         formError_   = nullptr;
    QLabel*         waitingLbl_  = nullptr;
    QPushButton*    cancelBtn_   = nullptr;

    std::thread       worker_;
    std::atomic<bool> cancelled_ { false };
};

} // namespace qt6
