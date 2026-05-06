#include "LoginDialog.h"

#include <QDesktopServices>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QUrl>
#include <QVBoxLayout>

namespace qt6 {

LoginDialog::LoginDialog(tesseract::Client& client, QWidget* parent)
    : QDialog(parent)
    , client_(client)
{
    setWindowTitle(tr("Sign in to Tesseract"));
    setModal(true);
    setMinimumWidth(420);

    // ---- Form page ----
    auto* formPage   = new QWidget(this);
    auto* formLayout = new QFormLayout(formPage);

    hsEdit_ = new QLineEdit("matrix.org", formPage);
    hsEdit_->setPlaceholderText(tr("e.g. matrix.org"));
    formLayout->addRow(tr("Homeserver:"), hsEdit_);

    formError_ = new QLabel(formPage);
    formError_->setWordWrap(true);
    formError_->setStyleSheet("color: palette(dark);");
    formError_->setVisible(false);
    formLayout->addRow(formError_);

    auto* formButtons = new QHBoxLayout;
    formButtons->addStretch(1);
    auto* closeBtn = new QPushButton(tr("Close"), formPage);
    signInBtn_     = new QPushButton(tr("Sign in"), formPage);
    signInBtn_->setDefault(true);
    formButtons->addWidget(closeBtn);
    formButtons->addWidget(signInBtn_);
    formLayout->addRow(formButtons);

    // ---- Waiting page ----
    auto* waitPage   = new QWidget(this);
    auto* waitLayout = new QVBoxLayout(waitPage);
    waitLayout->setContentsMargins(16, 16, 16, 16);

    waitingLbl_ = new QLabel(tr("Waiting for sign-in in your browser…"), waitPage);
    waitingLbl_->setWordWrap(true);
    waitLayout->addWidget(waitingLbl_);

    auto* waitButtons = new QHBoxLayout;
    waitButtons->addStretch(1);
    cancelBtn_ = new QPushButton(tr("Cancel"), waitPage);
    waitButtons->addWidget(cancelBtn_);
    waitLayout->addLayout(waitButtons);

    // ---- Stack ----
    stack_ = new QStackedWidget(this);
    stack_->addWidget(formPage);   // index 0
    stack_->addWidget(waitPage);   // index 1

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(12, 12, 12, 12);
    outer->addWidget(stack_);

    // ---- Connections ----
    connect(signInBtn_, &QPushButton::clicked, this, &LoginDialog::onSignIn);
    connect(closeBtn,   &QPushButton::clicked, this, &QDialog::reject);
    connect(cancelBtn_, &QPushButton::clicked, this, &LoginDialog::onCancel);

    // Cross-thread signals (worker → GUI thread).
    connect(this, &LoginDialog::beginCompleted,
            this, &LoginDialog::onBeginCompleted, Qt::QueuedConnection);
    connect(this, &LoginDialog::awaitCompleted,
            this, &LoginDialog::onAwaitCompleted, Qt::QueuedConnection);

    showForm();
}

LoginDialog::~LoginDialog() {
    cancelled_.store(true);
    client_.cancel_oauth();
    joinWorker();
}

// ---------------------------------------------------------------------------

void LoginDialog::showForm() {
    stack_->setCurrentIndex(0);
    signInBtn_->setEnabled(true);
    hsEdit_->setEnabled(true);
    hsEdit_->setFocus();
}

void LoginDialog::showWaiting(const QString& /*redirectUri*/) {
    stack_->setCurrentIndex(1);
}

// ---------------------------------------------------------------------------

void LoginDialog::onSignIn() {
    QString hs = hsEdit_->text().trimmed();
    if (hs.isEmpty()) {
        formError_->setText(tr("Please enter a homeserver."));
        formError_->setVisible(true);
        return;
    }
    formError_->setVisible(false);
    signInBtn_->setEnabled(false);
    hsEdit_->setEnabled(false);

    // Phase 1 on a worker thread: discovery + listener bind + URL build.
    joinWorker();
    cancelled_.store(false);
    worker_ = std::thread([this, hs = hs.toStdString()]() {
        auto flow = client_.begin_oauth(hs);
        if (cancelled_.load()) return;
        if (!flow) {
            emit beginCompleted(false, QString::fromStdString(flow.message));
            return;
        }
        // Hand the URL to the GUI thread; it'll open the browser and then
        // start phase 2.
        emit beginCompleted(true, QString::fromStdString(flow.auth_url));
    });
}

void LoginDialog::onBeginCompleted(bool ok, QString errorOrAuthUrl) {
    joinWorker();

    if (!ok) {
        formError_->setText(tr("Sign-in failed: %1").arg(errorOrAuthUrl));
        formError_->setVisible(true);
        showForm();
        return;
    }

    // Open the URL via Qt's helper (falls back through xdg-open / open / start
    // depending on platform — same effect as Client::open_in_browser, but
    // uses the Qt event loop's hooks).
    if (!QDesktopServices::openUrl(QUrl(errorOrAuthUrl))) {
        tesseract::Client::open_in_browser(errorOrAuthUrl.toStdString());
    }

    showWaiting(errorOrAuthUrl);

    // Phase 2 on a worker thread: block on the loopback listener.
    cancelled_.store(false);
    worker_ = std::thread([this]() {
        auto res = client_.await_oauth();
        if (cancelled_.load()) return;
        emit awaitCompleted(res.ok, QString::fromStdString(res.message));
    });
}

void LoginDialog::onAwaitCompleted(bool ok, QString error) {
    joinWorker();

    if (ok) {
        accept();
    } else {
        formError_->setText(tr("Sign-in failed: %1").arg(error));
        formError_->setVisible(true);
        showForm();
    }
}

void LoginDialog::onCancel() {
    cancelled_.store(true);
    client_.cancel_oauth();
    waitingLbl_->setText(tr("Cancelling…"));
    cancelBtn_->setEnabled(false);
    // The worker thread will return shortly; reject() once it's joined to
    // avoid racing on Client state.
    joinWorker();
    reject();
}

void LoginDialog::joinWorker() {
    if (worker_.joinable()) worker_.join();
}

} // namespace qt6
