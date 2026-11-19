#include "InstallProgressPage.h"
#include "../InstallerWizard.h"
#include "../backend/InstallEngine.h"
#include <QVBoxLayout>

InstallProgressPage::InstallProgressPage(QWidget *parent)
    : QWizardPage(parent)
{
    setTitle("Installing OS/2 Warp 4.52");
    setSubTitle("Please wait while files are being installed...");

    m_progressBar = new QProgressBar(this);
    m_progressBar->setRange(0, 100);

    m_statusLabel = new QLabel("Preparing...", this);

    m_logView = new QTextEdit(this);
    m_logView->setReadOnly(true);
    m_logView->setFont(QFont("Courier New", 10));

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(m_statusLabel);
    layout->addWidget(m_progressBar);
    layout->addWidget(m_logView);
}

void InstallProgressPage::initializePage()
{
    auto *wizard = qobject_cast<InstallerWizard*>(this->wizard());
    auto *engine = wizard->engine();

    connect(engine, &InstallEngine::progressChanged, this, &InstallProgressPage::onProgress);
    connect(engine, &InstallEngine::installFinished, this, &InstallProgressPage::onInstallFinished);

    QString targetDisk = wizard->field("targetDisk").toString();
    QStringList components = wizard->field("components").toStringList();

    QtConcurrent::run([engine, targetDisk, components]() {
        engine->startInstall(targetDisk, components);
    });
}

void InstallProgressPage::onProgress(int percent, const QString &status)
{
    m_progressBar->setValue(percent);
    m_statusLabel->setText(status);
    m_logView->append(status);
}

void InstallProgressPage::onInstallFinished(bool success)
{
    m_done = true;
    emit completeChanged();
    if (!success) {
        m_statusLabel->setText("Installation failed. Check log above.");
    } else {
        m_statusLabel->setText("Installation complete.");
    }
}
