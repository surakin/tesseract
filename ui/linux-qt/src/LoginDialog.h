#pragma once
#include <QDialog>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QStackedWidget>
#include <QString>

#include <tesseract/client.h>

#include <atomic>
#include <thread>

namespace qt6 {

/// Modal sign-in dialog driving the OAuth / Matrix Authentication Service
/// flow. Two pages (form, waiting) are shown via a QStackedWidget. The
/// OAuth work runs on a std::thread and reports back via Qt signals so the
/// UI thread stays responsive while the user is in their browser.
///
/// Usage:
///   LoginDialog dlg(client, this);
///   if (dlg.exec() == QDialog::Accepted) {
///       client.start_sync(...);
///   }
class LoginDialog final : public QDialog {
    Q_OBJECT
public:
    explicit LoginDialog(tesseract::Client& client, QWidget* parent = nullptr);
    ~LoginDialog() override;

signals:
    /// Worker → UI thread completion notifications. Auto-connections deliver
    /// these as queued events on the GUI thread.
    void beginCompleted(bool ok, QString errorOrAuthUrl);
    void awaitCompleted(bool ok, QString error);

private slots:
    void onSignIn();
    void onCancel();
    void onBeginCompleted(bool ok, QString errorOrAuthUrl);
    void onAwaitCompleted(bool ok, QString error);

private:
    void showForm();
    void showWaiting(const QString& redirectUri);
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
