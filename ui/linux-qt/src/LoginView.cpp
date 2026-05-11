#include "LoginView.h"

#include <QDesktopServices>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QUrl>
#include <QVBoxLayout>

namespace qt6 {

LoginView::LoginView(tesseract::Client& client, QWidget* parent)
    : QWidget(parent)
    , client_(client)
{
    setObjectName("loginView");
    setStyleSheet("#loginView { background-color: #F0F2F5; }");

    // ---- Centered card hosting the form / waiting pages ----
    auto* card = new QFrame(this);
    card->setObjectName("loginCard");
    card->setStyleSheet(
        "#loginCard { background-color:#FFFFFF; border:1px solid #D0D3D8; "
        "border-radius:8px; }");
    card->setMinimumWidth(420);
    card->setMaximumWidth(480);

    auto* cardLayout = new QVBoxLayout(card);
    cardLayout->setContentsMargins(24, 24, 24, 24);
    cardLayout->setSpacing(12);

    auto* title = new QLabel(tr("Sign in to Tesseract"), card);
    title->setStyleSheet("font-size:18px; font-weight:bold; color:#111111;");
    cardLayout->addWidget(title);

    // ---- Form page ----
    auto* formPage   = new QWidget(card);
    auto* formLayout = new QFormLayout(formPage);
    formLayout->setContentsMargins(0, 0, 0, 0);

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
    signInBtn_ = new QPushButton(tr("Sign in"), formPage);
    signInBtn_->setDefault(true);
    signInBtn_->setStyleSheet(
        "QPushButton { background-color:#0084FF; color:white; border:none; "
        "border-radius:4px; padding:6px 16px; font-weight:bold; }"
        "QPushButton:hover { background-color:#0077E5; }"
        "QPushButton:disabled { background-color:#A0C4E8; }");
    formButtons->addWidget(signInBtn_);
    formLayout->addRow(formButtons);

    // ---- Waiting page ----
    auto* waitPage   = new QWidget(card);
    auto* waitLayout = new QVBoxLayout(waitPage);
    waitLayout->setContentsMargins(0, 0, 0, 0);

    waitingLbl_ = new QLabel(tr("Waiting for sign-in in your browser…"), waitPage);
    waitingLbl_->setWordWrap(true);
    waitLayout->addWidget(waitingLbl_);

    auto* waitButtons = new QHBoxLayout;
    waitButtons->addStretch(1);
    cancelBtn_ = new QPushButton(tr("Cancel"), waitPage);
    waitButtons->addWidget(cancelBtn_);
    waitLayout->addLayout(waitButtons);

    // ---- Stack ----
    stack_ = new QStackedWidget(card);
    stack_->addWidget(formPage);   // index 0
    stack_->addWidget(waitPage);   // index 1
    cardLayout->addWidget(stack_);

    // ---- Outer layout: center the card horizontally and vertically ----
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->addStretch(1);
    auto* hCenter = new QHBoxLayout;
    hCenter->addStretch(1);
    hCenter->addWidget(card);
    hCenter->addStretch(1);
    outer->addLayout(hCenter);
    outer->addStretch(2);

    // ---- Connections ----
    connect(signInBtn_, &QPushButton::clicked, this, &LoginView::onSignIn);
    connect(cancelBtn_, &QPushButton::clicked, this, &LoginView::onCancel);

    connect(this, &LoginView::beginCompleted,
            this, &LoginView::onBeginCompleted, Qt::QueuedConnection);
    connect(this, &LoginView::awaitCompleted,
            this, &LoginView::onAwaitCompleted, Qt::QueuedConnection);

    showForm();
}

LoginView::~LoginView() {
    cancelled_.store(true);
    client_.cancel_oauth();
    joinWorker();
}

// ---------------------------------------------------------------------------

void LoginView::reset() {
    cancelled_.store(true);
    client_.cancel_oauth();
    joinWorker();
    cancelled_.store(false);
    formError_->setVisible(false);
    showForm();
}

void LoginView::showForm() {
    stack_->setCurrentIndex(0);
    signInBtn_->setEnabled(true);
    hsEdit_->setEnabled(true);
    cancelBtn_->setEnabled(true);
    waitingLbl_->setText(tr("Waiting for sign-in in your browser…"));
    hsEdit_->setFocus();
}

void LoginView::showWaiting() {
    stack_->setCurrentIndex(1);
}

// ---------------------------------------------------------------------------

void LoginView::onSignIn() {
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
        emit beginCompleted(true, QString::fromStdString(flow.auth_url));
    });
}

void LoginView::onBeginCompleted(bool ok, QString errorOrAuthUrl) {
    joinWorker();

    if (!ok) {
        formError_->setText(tr("Sign-in failed: %1").arg(errorOrAuthUrl));
        formError_->setVisible(true);
        showForm();
        return;
    }

    if (!QDesktopServices::openUrl(QUrl(errorOrAuthUrl))) {
        tesseract::Client::open_in_browser(errorOrAuthUrl.toStdString());
    }

    showWaiting();

    // Phase 2 on a worker thread: block on the loopback listener.
    cancelled_.store(false);
    worker_ = std::thread([this]() {
        auto res = client_.await_oauth();
        if (cancelled_.load()) return;
        emit awaitCompleted(res.ok, QString::fromStdString(res.message));
    });
}

void LoginView::onAwaitCompleted(bool ok, QString error) {
    joinWorker();

    if (ok) {
        emit loginSucceeded();
    } else {
        formError_->setText(tr("Sign-in failed: %1").arg(error));
        formError_->setVisible(true);
        showForm();
    }
}

void LoginView::onCancel() {
    cancelled_.store(true);
    client_.cancel_oauth();
    waitingLbl_->setText(tr("Cancelling…"));
    cancelBtn_->setEnabled(false);
    joinWorker();
    showForm();
}

void LoginView::joinWorker() {
    if (worker_.joinable()) worker_.join();
}

} // namespace qt6
