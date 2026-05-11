#pragma once
#include <QDialog>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QStackedWidget>

#include <tesseract/client.h>
#include <tesseract/types.h>

#include <atomic>
#include <thread>

namespace qt6 {

/// Modal dialog driving the Matrix key-recovery flow (Step 6).
///
/// Mirrors LoginDialog: a QStackedWidget swaps between a form page (recovery
/// key / passphrase entry) and a waiting page (key-import progress driven by
/// IEventHandler::on_backup_progress). The blocking recover() call runs on a
/// worker std::thread and signals back via Qt::QueuedConnection.
///
/// Usage:
///   RecoveryDialog dlg(client, this);
///   dlg.onBackupProgress(progress);   // forward from MainWindow as updates arrive
///   dlg.exec();
class RecoveryDialog final : public QDialog {
    Q_OBJECT
public:
    explicit RecoveryDialog(tesseract::Client& client, QWidget* parent = nullptr);
    ~RecoveryDialog() override;

    /// Forward a backup-progress update into the dialog. Safe to call from
    /// the GUI thread (the MainWindow's queued slot).
    void onBackupProgress(const tesseract::BackupProgress& progress);

signals:
    /// Worker → UI thread completion notification. Auto-connection delivers
    /// this as a queued event on the GUI thread.
    void recoverCompleted(bool ok, QString error);

private slots:
    void onVerify();
    void onClose();
    void onRecoverCompleted(bool ok, QString error);

private:
    void showForm();
    void showWaiting();
    void joinWorker();

    tesseract::Client& client_;

    QStackedWidget* stack_       = nullptr;
    QLineEdit*      keyEdit_     = nullptr;
    QPushButton*    verifyBtn_   = nullptr;
    QLabel*         formError_   = nullptr;
    QLabel*         progressLbl_ = nullptr;
    QPushButton*    closeBtn_    = nullptr;

    std::thread       worker_;
    std::atomic<bool> cancelled_ { false };
    bool              recoverDone_ = false;
};

} // namespace qt6
