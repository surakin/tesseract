#include "RecoveryDialog.h"

#include <QFormLayout>
#include <QHBoxLayout>
#include <QVBoxLayout>

namespace qt6 {

RecoveryDialog::RecoveryDialog(tesseract::Client& client, QWidget* parent)
    : QDialog(parent)
    , client_(client)
{
    setWindowTitle(tr("Verify this device"));
    setModal(true);
    setMinimumWidth(420);

    // ---- Form page ----
    auto* formPage   = new QWidget(this);
    auto* formLayout = new QFormLayout(formPage);

    auto* intro = new QLabel(
        tr("Enter your recovery key or passphrase to verify this device "
           "and decrypt historical messages."),
        formPage);
    intro->setWordWrap(true);
    formLayout->addRow(intro);

    keyEdit_ = new QLineEdit(formPage);
    keyEdit_->setPlaceholderText(tr("Recovery key or passphrase"));
    keyEdit_->setEchoMode(QLineEdit::Password);
    formLayout->addRow(tr("Recovery key:"), keyEdit_);

    formError_ = new QLabel(formPage);
    formError_->setWordWrap(true);
    formError_->setStyleSheet("color: #b00020;");
    formError_->setVisible(false);
    formLayout->addRow(formError_);

    auto* formButtons = new QHBoxLayout;
    formButtons->addStretch(1);
    auto* skipBtn = new QPushButton(tr("Skip"), formPage);
    verifyBtn_    = new QPushButton(tr("Verify"), formPage);
    verifyBtn_->setDefault(true);
    formButtons->addWidget(skipBtn);
    formButtons->addWidget(verifyBtn_);
    formLayout->addRow(formButtons);

    // ---- Waiting page ----
    auto* waitPage   = new QWidget(this);
    auto* waitLayout = new QVBoxLayout(waitPage);
    waitLayout->setContentsMargins(16, 16, 16, 16);

    progressLbl_ = new QLabel(tr("Unlocking secret storage…"), waitPage);
    progressLbl_->setWordWrap(true);
    waitLayout->addWidget(progressLbl_);

    auto* waitButtons = new QHBoxLayout;
    waitButtons->addStretch(1);
    closeBtn_ = new QPushButton(tr("Close"), waitPage);
    closeBtn_->setEnabled(false);
    waitButtons->addWidget(closeBtn_);
    waitLayout->addLayout(waitButtons);

    // ---- Stack ----
    stack_ = new QStackedWidget(this);
    stack_->addWidget(formPage);   // index 0
    stack_->addWidget(waitPage);   // index 1

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(12, 12, 12, 12);
    outer->addWidget(stack_);

    // ---- Connections ----
    connect(verifyBtn_, &QPushButton::clicked, this, &RecoveryDialog::onVerify);
    connect(skipBtn,    &QPushButton::clicked, this, &QDialog::reject);
    connect(closeBtn_,  &QPushButton::clicked, this, &RecoveryDialog::onClose);

    connect(this, &RecoveryDialog::recoverCompleted,
            this, &RecoveryDialog::onRecoverCompleted, Qt::QueuedConnection);

    showForm();
}

RecoveryDialog::~RecoveryDialog() {
    cancelled_.store(true);
    joinWorker();
}

void RecoveryDialog::showForm() {
    stack_->setCurrentIndex(0);
    verifyBtn_->setEnabled(true);
    keyEdit_->setEnabled(true);
    keyEdit_->setFocus();
}

void RecoveryDialog::showWaiting() {
    stack_->setCurrentIndex(1);
}

void RecoveryDialog::onVerify() {
    QString key = keyEdit_->text().trimmed();
    if (key.isEmpty()) {
        formError_->setText(tr("Please enter a recovery key or passphrase."));
        formError_->setVisible(true);
        return;
    }
    formError_->setVisible(false);
    verifyBtn_->setEnabled(false);
    keyEdit_->setEnabled(false);

    showWaiting();

    joinWorker();
    cancelled_.store(false);
    worker_ = std::thread([this, key = key.toStdString()]() {
        auto res = client_.recover(key);
        if (cancelled_.load()) return;
        emit recoverCompleted(res.ok, QString::fromStdString(res.message));
    });
}

void RecoveryDialog::onRecoverCompleted(bool ok, QString error) {
    joinWorker();

    if (!ok) {
        formError_->setText(tr("Recovery failed: %1").arg(error));
        formError_->setVisible(true);
        showForm();
        return;
    }

    recoverDone_ = true;
    progressLbl_->setText(tr("Downloading historical keys…"));
    // closeBtn_ stays disabled until backup state reaches Enabled.
}

void RecoveryDialog::onClose() {
    accept();
}

void RecoveryDialog::onBackupProgress(const tesseract::BackupProgress& progress) {
    if (!recoverDone_) return;

    switch (progress.state) {
        case tesseract::BackupState::Enabled:
            progressLbl_->setText(
                tr("Done. Imported %1 keys.")
                    .arg(static_cast<qulonglong>(progress.imported_keys)));
            closeBtn_->setEnabled(true);
            closeBtn_->setDefault(true);
            break;
        case tesseract::BackupState::Downloading:
            progressLbl_->setText(
                tr("Importing keys… %1 imported.")
                    .arg(static_cast<qulonglong>(progress.imported_keys)));
            break;
        case tesseract::BackupState::Disabled:
            progressLbl_->setText(tr("Backup is not enabled on the server."));
            closeBtn_->setEnabled(true);
            break;
        default:
            // Keep current text.
            break;
    }
}

void RecoveryDialog::joinWorker() {
    if (worker_.joinable()) worker_.join();
}

} // namespace qt6
