#include "mainwindow.h"
#include "./ui_mainwindow.h"

#include <QColorSpace>
#include <QDebug>
#include <QDragEnterEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QMessageBox>
#include <QMimeData>

#include <QCborMap>
#include <QCborValue>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>

#include <QCollator>
#include <QTreeWidgetItem>

#include <jxl/color_encoding.h>
#include <jxl/encode_cxx.h>
#include <jxl/resizable_parallel_runner_cxx.h>

#include "jxfrstchconfig.h"
#include "jxlutils.h"
#include "utils/jxlencoderobject.h"

#define USE_STREAMING_OUTPUT // need libjxl >= 0.10.0

namespace jxfrstch {
QString blendModeToString(JxlBlendMode blendMode) {
    switch (blendMode) {
    case JXL_BLEND_ADD:
        return QString("ADD");
        break;
    case JXL_BLEND_MULADD:
        return QString("MULADD");
        break;
    case JXL_BLEND_MUL:
        return QString("MUL");
        break;
    case JXL_BLEND_REPLACE:
        return QString("REPLACE");
        break;
    case JXL_BLEND_BLEND:
        return QString("BLEND");
        break;
    default:
        return QString();
        break;
    }
}

JxlBlendMode stringToBlendMode(const QString &st) {
    if (st == "ADD") {
        return JXL_BLEND_ADD;
    } else if (st == "MULADD") {
        return JXL_BLEND_MULADD;
    } else if (st == "MUL") {
        return JXL_BLEND_MUL;
    } else if (st == "REPLACE") {
        return JXL_BLEND_REPLACE;
    } else if (st == "BLEND") {
        return JXL_BLEND_BLEND;
    } else {
        return JXL_BLEND_BLEND;
    }
}
}

class Q_DECL_HIDDEN MainWindow::Private
{
public:
    bool isEncoding{false};
    bool encodeAbort{false};
    bool isUnsavedChanges{false};
    QString windowTitle{"JXL Frame Stitching"};
    QString configSaveFile{};
    QList<QByteArray> supportedFiles{};

    QCollator collator;
    QVector<jxfrstch::InputFileData> inputFileList;
    QScopedPointer<JXLEncoderObject> encObj;

    QScopedPointer<QLabel> statLabel;
};

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
    , d(new Private)
{
    ui->setupUi(this);

    ui->verticalSpacer->changeSize(20, 40, QSizePolicy::Minimum, QSizePolicy::Expanding);

    ui->selectedFrameBox->setEnabled(false);
    ui->selectedFileLabel->setText("---");

    d->windowTitle += " v" + QString(PROJECT_VERSION);

    resetApp();

    d->supportedFiles.append(QImageReader::supportedImageFormats());
    d->supportedFiles.append("jxl");

    d->collator.setNumericMode(true);
    ui->treeWidget->setColumnWidth(0, 120);
    ui->treeWidget->setColumnWidth(1, 40);
    ui->treeWidget->setColumnWidth(2, 40);
    ui->treeWidget->setColumnWidth(3, 40);
    ui->treeWidget->setColumnWidth(4, 48);
    ui->treeWidget->setColumnWidth(5, 48);
    ui->treeWidget->setColumnWidth(6, 60);
    ui->progressBarSub->hide();

    d->statLabel.reset(new QLabel(this));
    ui->statusBar->addPermanentWidget(d->statLabel.get());

    d->statLabel->setAlignment(Qt::AlignRight);
    d->statLabel->clear();

    QImageReader::setAllocationLimit(0);

    connect(ui->clearFilesBtn, &QPushButton::clicked, this, [&]() {
        ui->statusBar->showMessage(
            "Import image frames by drag and dropping into the file list or pressing Add Files...");
        d->inputFileList.clear();
        ui->treeWidget->clear();
    });

    connect(ui->treeWidget, &QTreeWidget::itemSelectionChanged, this, &MainWindow::selectingFrames);

    connect(ui->saveAsRefSpn, &QSpinBox::valueChanged, this, [&](int v) {
        setUnsaved();
        if (v > 0) {
            ui->frameDurationSpn->setValue(0);
            ui->frameRefSpinBox->setEnabled(false);
            ui->frameDurationSpn->setEnabled(false);
            ui->pageEndChk->setEnabled(false);
        } else {
            ui->frameDurationSpn->setValue(1);
            ui->frameRefSpinBox->setEnabled(true);
            ui->frameDurationSpn->setEnabled(true);
            ui->pageEndChk->setEnabled(true);
        }
    });
    connect(ui->alphaEnableChk, &QCheckBox::toggled, this, [&]() {
        setUnsaved();
        if (!ui->alphaEnableChk->isChecked()) {
            ui->alphaLosslessChk->setEnabled(false);
            ui->alphaPremulChk->setEnabled(false);
        } else {
            ui->alphaLosslessChk->setEnabled(true);
            ui->alphaPremulChk->setEnabled(true);
        }
    });
    connect(ui->isAnimatedBox, &QGroupBox::toggled, this, [&]() {
        setUnsaved();
        if (!ui->isAnimatedBox->isChecked()) {
            ui->frameRefSpinBox->setEnabled(false);
            ui->frameDurationSpn->setEnabled(false);
            ui->pageEndChk->setEnabled(false);
            ui->saveAsRefSpn->setEnabled(false);
        } else {
            ui->frameRefSpinBox->setEnabled(ui->saveAsRefSpn->value() == 0);
            ui->frameDurationSpn->setEnabled(ui->saveAsRefSpn->value() == 0);
            ui->pageEndChk->setEnabled(ui->saveAsRefSpn->value() == 0);
            ui->saveAsRefSpn->setEnabled(ui->treeWidget->selectedItems().size() == 1);
        }
    });

    connect(ui->numeratorSpn, &QSpinBox::valueChanged, this, &MainWindow::setUnsaved);
    connect(ui->denominatorSpn, &QSpinBox::valueChanged, this, &MainWindow::setUnsaved);
    connect(ui->loopsSpinBox, &QSpinBox::valueChanged, this, &MainWindow::setUnsaved);
    connect(ui->distanceSpn, &QDoubleSpinBox::valueChanged, this, &MainWindow::setUnsaved);
    connect(ui->effortSpn, &QSpinBox::valueChanged, this, &MainWindow::setUnsaved);
    connect(ui->colorSpaceCmb, &QComboBox::currentIndexChanged, this, &MainWindow::setUnsaved);
    connect(ui->bitDepthCmb, &QComboBox::currentIndexChanged, this, &MainWindow::setUnsaved);
    connect(ui->alphaLosslessChk, &QCheckBox::toggled, this, &MainWindow::setUnsaved);
    connect(ui->alphaPremulChk, &QCheckBox::toggled, this, &MainWindow::setUnsaved);

    connect(ui->applyFrameBtn, &QPushButton::clicked, this, &MainWindow::currentFrameSettingChanged);
    connect(ui->outFileDirBtn, &QPushButton::clicked, this, &MainWindow::selectOutputFile);
    connect(ui->addFilesBtn, &QPushButton::clicked, this, &MainWindow::addFiles);
    connect(ui->removeSelectedBtn, &QPushButton::clicked, this, &MainWindow::removeSelected);
    connect(ui->resetOrderBtn, &QPushButton::clicked, this, &MainWindow::resetOrder);

    connect(ui->encodeBtn, &QPushButton::clicked, this, [&]() {
        if (d->encObj->isRunning() && !d->encodeAbort) {
            ui->statusBar->showMessage("Aborting encode, please wait until current frame is finished...");
            ui->encodeBtn->setText("Aborting...");
            d->encObj->abortEncode(true);
            d->encodeAbort = true;
        } else if (!d->encObj->isRunning()) {
            d->encodeAbort = false;
            if (ui->treeWidget->topLevelItemCount() > 0) {
                ui->encodeBtn->setText("Abort");
                doEncode();
            }
        }
    });

    connect(ui->actionSave_settings, &QAction::triggered, this, [&]() {
        saveConfigAs(true);
    });

    connect(ui->actionSave, &QAction::triggered, this, [&]() {
        saveConfigAs(false);
    });

    connect(ui->actionBasic_usage, &QAction::triggered, this, [&]() {
        QMessageBox::about(this, "Basic Usage", jxfrstch::basicUsage);
    });

    connect(ui->actionAbout, &QAction::triggered, this, [&]() {
        QMessageBox::about(this, "About", jxfrstch::aboutData);
    });

    connect(ui->actionAbout_Qt, &QAction::triggered, this, [&]() {
        QMessageBox::aboutQt(this, "About Qt");
    });

    connect(ui->actionEnable_effort_11, &QAction::triggered, this, [&](bool val) {
        if (val) {
            ui->effortSpn->setMaximum(11);
        } else {
            ui->effortSpn->setMaximum(10);
        }
    });

    connect(ui->actionNew_project, &QAction::triggered, this, &MainWindow::resetApp);
    connect(ui->actionOpen_settings, &QAction::triggered, this, [&]() {
        openConfig();
    });

    d->encObj.reset(new JXLEncoderObject());

    connect(d->encObj.get(), &JXLEncoderObject::sigStatusText, this, [&](const QString &status) {
        ui->statusBar->showMessage(status);
    });
    connect(d->encObj.get(), &JXLEncoderObject::sigThrowError, this, [&](const QString &status) {
        QMessageBox::critical(this, "Error", status);
    });
    connect(d->encObj.get(), &JXLEncoderObject::sigCurrentMainProgressBar, this, [&](const int &progress, const bool &success) {
        QTreeWidgetItem *selItem = ui->treeWidget->topLevelItem(success ? progress - 1 : progress);
        ui->progressBar->show();
        ui->treeWidget->setCurrentItem(selItem);
        selItem->setBackground(0, success ? QColor(128, 255, 255) : QColor(255, 255, 96));
        ui->progressBar->setValue(progress);
    });
    connect(d->encObj.get(), &JXLEncoderObject::sigEnableSubProgressBar, this, [&](const bool &enabled, const int &setMax) {
        ui->progressBarSub->setVisible(enabled);
        ui->progressBarSub->setMaximum(setMax);
    });
    connect(d->encObj.get(), &JXLEncoderObject::sigCurrentSubProgressBar, this, [&](const int &progress) {
        ui->progressBarSub->setValue(progress);
    });
    connect(d->encObj.get(), &JXLEncoderObject::sigSpeedStats, this, [&](const QString &status) {
        d->statLabel->setText(status);
    });
    connect(d->encObj.get(), &JXLEncoderObject::finished, this, [&]() {
        ui->encodeBtn->setText("Encode");
        ui->menuBar->setEnabled(true);
        ui->frameListGrp->setEnabled(true);
        ui->globalSettingGrp->setEnabled(true);
        setAcceptDrops(true);
    });
}

MainWindow::~MainWindow()
{
    delete ui;
    d.reset();
}

void MainWindow::resetApp()
{
    if (d->isUnsavedChanges) {
        const auto res = QMessageBox::warning(this,
                                              "Warning",
                                              "You have unsaved changes! Would you like to save before making changes?",
                                              QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel);
        if (res == QMessageBox::Yes) {
            if (!saveConfig()) {
                return;
            }
        } else if (res == QMessageBox::No) {
            d->isUnsavedChanges = false;
        } else if (res == QMessageBox::Cancel) {
            return;
        }
    }
    d->configSaveFile.clear();
    d->inputFileList.clear();
    ui->treeWidget->clear();
    setWindowTitle(d->windowTitle);
    ui->isAnimatedBox->setChecked(true);
    ui->numeratorSpn->setValue(1);
    ui->denominatorSpn->setValue(1);
    ui->distanceSpn->setValue(0.0);
    ui->effortSpn->setValue(1);
    ui->colorSpaceCmb->setCurrentIndex(0);
    ui->bitDepthCmb->setCurrentIndex(0);
    ui->alphaEnableChk->setChecked(true);
    ui->alphaLosslessChk->setChecked(true);
    ui->alphaPremulChk->setChecked(false);
    ui->outFileLineEdit->clear();
    ui->statusBar->showMessage("Import image frames by drag and dropping into the file list or pressing Add Files...");
    ui->progressBar->hide();
    ui->photonNoiseSpn->setValue(0.0);
    ui->autoCropChk->setChecked(false);
    ui->autoCropTreshSpn->setValue(0.0);
}

void MainWindow::setUnsaved()
{
    if (ui->treeWidget->topLevelItemCount() > 0) {
        d->isUnsavedChanges = true;
    } else {
        d->isUnsavedChanges = false;
    }
}

void MainWindow::dragEnterEvent(QDragEnterEvent *event)
{
    if (event->mimeData()->hasUrls()) {
        event->accept();
    }
}

void MainWindow::dragMoveEvent(QDragMoveEvent *event)
{
    if (event->mimeData()->hasUrls()) {
        event->accept();
    }
}

void MainWindow::dropEvent(QDropEvent *event)
{
    if (event->mimeData()->hasUrls()) {
        QFileInfo finfo(event->mimeData()->urls().at(0).toLocalFile());
        if (finfo.isFile() && finfo.suffix().toLower() == "frstch") {
            if (d->isUnsavedChanges) {
                const auto res = QMessageBox::warning(this,
                                                      "Warning",
                                                      "You have unsaved changes! Would you like to save before making changes?",
                                                      QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel);
                if (res == QMessageBox::Yes) {
                    if (!saveConfig()) {
                        return;
                    }
                } else if (res == QMessageBox::No) {
                    d->isUnsavedChanges = false;
                } else if (res == QMessageBox::Cancel) {
                    return;
                }
            }
            openConfig(finfo.absoluteFilePath());
        } else if (finfo.isFile() || event->mimeData()->urls().count() > 1) {
            QStringList fileList;
            foreach (const QUrl &url, event->mimeData()->urls()) {
                const QFileInfo fileInfo(url.toLocalFile());
                if (fileInfo.isFile()
                    && ui->treeWidget->findItems(fileInfo.absoluteFilePath(), Qt::MatchExactly, 0).isEmpty()
                    && d->supportedFiles.contains(fileInfo.suffix().toLower())) {
                    fileList.append(fileInfo.absoluteFilePath());
                }
            }
            appendFilesFromList(fileList);
        }
    }
}

void MainWindow::resetOrder()
{
    d->inputFileList.clear();
    for (int i = 0; i < ui->treeWidget->topLevelItemCount(); i++) {
        const auto *itm = ui->treeWidget->topLevelItem(i);

        jxfrstch::InputFileData ifd;
        ifd.filename = itm->data(0, 0).toString();
        ifd.isRefFrame = itm->data(1, 0).toInt();
        ifd.frameDuration = itm->data(2, 0).toInt();
        ifd.frameReference = itm->data(3, 0).toInt();
        ifd.frameXPos = itm->data(4, 0).toInt();
        ifd.frameYPos = itm->data(5, 0).toInt();
        ifd.blendMode = jxfrstch::stringToBlendMode(itm->data(6, 0).toString());
        ifd.frameName = itm->data(7, 0).toString();
        d->inputFileList.append(ifd);
    }
    std::sort(d->inputFileList.begin(),
              d->inputFileList.end(),
              [&](const jxfrstch::InputFileData &lhs, const jxfrstch::InputFileData &rhs) {
                  return d->collator.compare(lhs.filename, rhs.filename) < 0;
              });
    ui->treeWidget->clear();
    foreach (const auto &ifd, d->inputFileList) {
        QTreeWidgetItem *item = new QTreeWidgetItem(ui->treeWidget);
        item->setData(0, 0, ifd.filename);
        item->setData(1, 0, ifd.isRefFrame);
        item->setData(2, 0, ifd.frameDuration);
        item->setData(3, 0, ifd.frameReference);
        item->setData(4, 0, ifd.frameXPos);
        item->setData(5, 0, ifd.frameYPos);
        item->setData(6, 0, jxfrstch::blendModeToString(ifd.blendMode));
        item->setData(7, 0, ifd.frameName);
        item->setBackground(0, {});
        item->setFlags(item->flags() & ~Qt::ItemIsDropEnabled);
        if (ifd.isRefFrame) {
            item->setBackground(1, QColor(128, 255, 128));
        }
        ui->treeWidget->addTopLevelItem(item);
        QTreeWidget a;
    }
    d->inputFileList.clear();
}

void MainWindow::addFiles()
{
    QString ifiles("Image Files (");
    foreach (const auto &v, d->supportedFiles) {
        ifiles += "*.";
        ifiles += v;
        ifiles += " ";
    }
    ifiles = ifiles.trimmed();
    ifiles += ")";
    const QList<QUrl> tmpFiles = QFileDialog::getOpenFileUrls(this, "Add files...", {}, ifiles);
    if (!tmpFiles.isEmpty()) {
        QFileInfo finfo(tmpFiles.at(0).toLocalFile());
        if (finfo.isFile() || tmpFiles.count() > 1) {
            QStringList fileList;
            foreach (const QUrl &url, tmpFiles) {
                const QFileInfo fileInfo(url.toLocalFile());

                if (fileInfo.isFile()
                    && ui->treeWidget->findItems(fileInfo.absoluteFilePath(), Qt::MatchExactly, 0).isEmpty()
                    && d->supportedFiles.contains(fileInfo.suffix().toLower())) {
                    fileList.append(fileInfo.absoluteFilePath());
                }
            }
            appendFilesFromList(fileList);
        }
    }
}

void MainWindow::appendFilesFromList(const QStringList &lst) {
    if (!lst.isEmpty()) {
        // ui->treeWidget->clear();
        d->inputFileList.clear();

        foreach (const QString &absurl, lst) {
            jxfrstch::InputFileData ifd;
            ifd.filename = absurl;
            d->inputFileList.append(ifd);
        }
        std::sort(d->inputFileList.begin(),
                  d->inputFileList.end(),
                  [&](const jxfrstch::InputFileData &lhs, const jxfrstch::InputFileData &rhs) {
                      return d->collator.compare(lhs.filename, rhs.filename) < 0;
                  });
        foreach (const auto &ifd, d->inputFileList) {
            QTreeWidgetItem *item = new QTreeWidgetItem(ui->treeWidget);
            item->setData(0, 0, ifd.filename);
            item->setData(1, 0, ifd.isRefFrame);
            item->setData(2, 0, ifd.frameDuration);
            item->setData(3, 0, ifd.frameReference);
            item->setData(4, 0, ifd.frameXPos);
            item->setData(5, 0, ifd.frameYPos);
            item->setData(6, 0, jxfrstch::blendModeToString(ifd.blendMode));
            item->setData(7, 0, ifd.frameName);
            item->setBackground(0, {});
            item->setFlags(item->flags() & ~Qt::ItemIsDropEnabled);
            if (ifd.isRefFrame) {
                item->setBackground(1, QColor(128, 255, 128));
            }
            ui->treeWidget->addTopLevelItem(item);
            QTreeWidget a;
        }

        d->inputFileList.clear();
        ui->progressBar->hide();
        setUnsaved();
    }
}

void MainWindow::removeSelected()
{
    if (ui->treeWidget->selectedItems().size() > 0) {
        foreach (const auto &v, ui->treeWidget->selectedItems()) {
            auto ty = ui->treeWidget->takeTopLevelItem(ui->treeWidget->indexOfTopLevelItem(v));
            delete ty;
        }
    }
}

void MainWindow::selectingFrames()
{
    const auto *currentSelItem = ui->treeWidget->currentItem();
    bool isDurInt = true;

    if (ui->treeWidget->selectedItems().size() > 1) {
        ui->selectedFrameBox->setEnabled(true);
        ui->saveAsRefSpn->setEnabled(false);
        ui->selectedFileLabel->setText(QString("%1 files selected").arg(ui->treeWidget->selectedItems().size()));

        ui->saveAsRefSpn->setValue(-1);
        ui->frameDurationSpn->setValue(-1);
        ui->pageEndChk->setCheckState(Qt::PartiallyChecked);
        ui->frameRefSpinBox->setValue(-1);
        ui->frameXPosSpn->setValue(currentSelItem->data(4, 0).toInt());
        ui->frameYPosSpn->setValue(currentSelItem->data(5, 0).toInt());
        ui->blendModeCmb->setCurrentIndex(5);
        ui->frameNameLine->setText("<unchanged>");

        ui->frameXPosSpn->setEnabled(true);
        ui->frameYPosSpn->setEnabled(true);
    } else if (ui->treeWidget->selectedItems().size() == 1) {
        ui->selectedFrameBox->setEnabled(true);
        ui->saveAsRefSpn->setEnabled(false);
        ui->selectedFileLabel->setText(currentSelItem->data(0, 0).toString());
        ui->saveAsRefSpn->setValue(currentSelItem->data(1, 0).toInt());
        ui->frameDurationSpn->setValue(currentSelItem->data(2, 0).toInt(&isDurInt));
        if (!isDurInt) {
            ui->frameDurationSpn->setValue(1);
            ui->frameDurationSpn->setEnabled(false);
            ui->pageEndChk->setChecked(true);
        } else {
            ui->pageEndChk->setChecked(false);
        }
        // ui->pageEndChk->setChecked(false);
        ui->frameRefSpinBox->setValue(currentSelItem->data(3, 0).toInt());
        ui->frameXPosSpn->setValue(currentSelItem->data(4, 0).toInt());
        ui->frameYPosSpn->setValue(currentSelItem->data(5, 0).toInt());
        ui->frameNameLine->setText(currentSelItem->data(7, 0).toString());
        if (ui->saveAsRefSpn->value() > 0) {
            ui->frameDurationSpn->setEnabled(false);
            ui->frameRefSpinBox->setEnabled(false);
        } else {
            ui->frameDurationSpn->setEnabled(true);
            ui->frameRefSpinBox->setEnabled(true);
        }
        switch (jxfrstch::stringToBlendMode(currentSelItem->data(6, 0).toString())) {
        case JXL_BLEND_BLEND:
            ui->blendModeCmb->setCurrentIndex(0);
            break;
        case JXL_BLEND_REPLACE:
            ui->blendModeCmb->setCurrentIndex(1);
            break;
        case JXL_BLEND_ADD:
            ui->blendModeCmb->setCurrentIndex(2);
            break;
        case JXL_BLEND_MULADD:
            ui->blendModeCmb->setCurrentIndex(3);
            break;
        case JXL_BLEND_MUL:
            ui->blendModeCmb->setCurrentIndex(4);
            break;
        default:
            ui->blendModeCmb->setCurrentIndex(0);
            break;
        }
        if (ui->treeWidget->indexOfTopLevelItem(ui->treeWidget->currentItem()) == 0) {
            ui->frameXPosSpn->setEnabled(false);
            ui->frameYPosSpn->setEnabled(false);
            ui->frameXPosSpn->setValue(0);
            ui->frameYPosSpn->setValue(0);
        } else {
            ui->frameXPosSpn->setEnabled(true);
            ui->frameYPosSpn->setEnabled(true);
        }
    } else {
        ui->selectedFrameBox->setEnabled(false);
        ui->selectedFileLabel->setText("---");
    }

    if (!ui->isAnimatedBox->isChecked()) {
        ui->frameRefSpinBox->setEnabled(false);
        ui->frameDurationSpn->setEnabled(false);
        ui->pageEndChk->setEnabled(false);
        ui->saveAsRefSpn->setEnabled(false);
    } else {
        ui->frameRefSpinBox->setEnabled(ui->saveAsRefSpn->value() <= 0);
        if (isDurInt) {
            ui->frameDurationSpn->setEnabled(ui->saveAsRefSpn->value() <= 0);
        } else {
            ui->frameDurationSpn->setEnabled(false);
        }
        ui->pageEndChk->setEnabled(ui->saveAsRefSpn->value() <= 0);
        ui->saveAsRefSpn->setEnabled(ui->treeWidget->selectedItems().size() == 1);
    }
}

bool MainWindow::saveConfigAs(bool forceDialog)
{
    if (ui->treeWidget->topLevelItemCount() == 0) {
        return false;
    }
    QJsonArray files;
    for (int i = 0; i < ui->treeWidget->topLevelItemCount(); i++) {
        const auto *itm = ui->treeWidget->topLevelItem(i);

        QJsonObject jsobj;

        jsobj["filename"] = itm->data(0, 0).toString();
        jsobj["isRef"] = itm->data(1, 0).toInt();
        bool isDurInt = true;
        int fDur = itm->data(2, 0).toInt(&isDurInt);
        if (!isDurInt) {
            jsobj["frameDur"] = 1;
            jsobj["frameEndP"] = true;
        } else {
            jsobj["frameDur"] = itm->data(2, 0).toInt();
            jsobj["frameEndP"] = false;
        }
        jsobj["frameRef"] = itm->data(3, 0).toInt();
        jsobj["frameXPos"] = itm->data(4, 0).toInt();
        jsobj["frameYPos"] = itm->data(5, 0).toInt();
        jsobj["blend"] = jxfrstch::stringToBlendMode(itm->data(6, 0).toString());
        jsobj["frameName"] = itm->data(7, 0).toString();
        files.append(jsobj);
    }

    QJsonObject sets;
    sets["useAlpha"] = ui->alphaEnableChk->isChecked();
    sets["usePremulAlpha"] = ui->alphaPremulChk->isChecked();
    sets["useLosslessAlpha"] = ui->alphaLosslessChk->isChecked();
    sets["bitdepth"] = ui->bitDepthCmb->currentIndex();
    sets["encDistance"] = ui->distanceSpn->value();
    sets["encEffort"] = ui->effortSpn->value();
    sets["numerator"] = ui->numeratorSpn->value();
    sets["denominator"] = ui->denominatorSpn->value();
    sets["numLoops"] = ui->loopsSpinBox->value();
    sets["useAnimation"] = ui->isAnimatedBox->isChecked();
    sets["colorSpace"] = ui->colorSpaceCmb->currentIndex();
    sets["photonNoise"] = ui->photonNoiseSpn->value();
    sets["autoCrop"] = ui->autoCropChk->isChecked();
    sets["autoCropThr"] = ui->autoCropTreshSpn->value();
    sets["fileList"] = files;

    const QByteArray binsave = QCborValue::fromJsonValue(sets).toCbor();

    const QString tmpfn = [&]() {
        if (forceDialog || d->configSaveFile.isEmpty()) {
            return QFileDialog::getSaveFileName(this,
                                                "Save setting as",
                                                QDir::currentPath(),
                                                "Frame Stitch Config (*.frstch)");
        }
        return d->configSaveFile;
    }();

    if (!tmpfn.isEmpty()) {
        QFile outF(tmpfn);
        outF.open(QIODevice::WriteOnly);
        if (outF.isWritable()) {
            outF.write(binsave);
            d->configSaveFile = tmpfn;
            QFileInfo outFInfo(tmpfn);
            setWindowTitle(QString("%1 - %2").arg(d->windowTitle, outFInfo.fileName()));
            ui->statusBar->showMessage("Config saved");
        }
        outF.close();
    } else {
        return false;
    }
    d->isUnsavedChanges = false;
    return true;
}

bool MainWindow::saveConfig()
{
    if (ui->treeWidget->topLevelItemCount() == 0) {
        return false;
    }
    if (d->configSaveFile.isEmpty()) {
        return saveConfigAs(true);
    } else {
        return saveConfigAs(false);
    }
}

void MainWindow::openConfig()
{
    if (d->isUnsavedChanges) {
        const auto res = QMessageBox::warning(this,
                                              "Warning",
                                              "You have unsaved changes! Would you like to save before making changes?",
                                              QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel);
        if (res == QMessageBox::Yes) {
            if (!saveConfig()) {
                return;
            }
        } else if (res == QMessageBox::Cancel) {
            return;
        }
    }

    const QString tmpfn =
        QFileDialog::getOpenFileName(this, "Open setting", QDir::currentPath(), "Frame Stitch Config (*.frstch)");
    if (tmpfn.isEmpty()) {
        return;
    }
    d->isUnsavedChanges = false;
    openConfig(tmpfn);
}

void MainWindow::openConfig(const QString &tmpfn)
{
    QByteArray binsave;
    if (!tmpfn.isEmpty()) {
        QFile outF(tmpfn);
        outF.open(QIODevice::ReadOnly);
        if (outF.isReadable()) {
            binsave = outF.readAll();
        } else {
            outF.close();
            ui->statusBar->showMessage("Failed to read config file");
            return;
        }
        outF.close();
    }

    if (binsave.isEmpty()) {
        ui->statusBar->showMessage("Failed to read config file");
        return;
    }

    const QJsonObject loadjs = QCborValue::fromCbor(binsave).toMap().toJsonObject();
    if (!loadjs.isEmpty()) {
        const bool useAlpha = loadjs.value("useAlpha").toBool(true);
        const bool usePremulAlpha = loadjs.value("usePremulAlpha").toBool(false);
        const bool useLosslessAlpha = loadjs.value("useLosslessAlpha").toBool(true);
        const int bitdepth = loadjs.value("bitdepth").toInt(0);
        const double encDistance = loadjs.value("encDistance").toDouble(0.0);
        const int encEffort = loadjs.value("encEffort").toInt(1);
        const int numerator = loadjs.value("numerator").toInt(1);
        const int denominator = loadjs.value("denominator").toInt(1);
        const int numLoops = loadjs.value("numLoops").toInt(0);
        const bool useAnimation = loadjs.value("useAnimation").toBool(true);
        const int colorSpace = loadjs.value("colorSpace").toInt(0);
        const double photonNoise = loadjs.value("photonNoise").toDouble(0.0);
        const bool autoCrop = loadjs.value("autoCrop").toBool(false);
        const double autoCropThr = loadjs.value("autoCropThr").toDouble(0.0);

        ui->alphaEnableChk->setChecked(useAlpha);
        ui->alphaPremulChk->setChecked(usePremulAlpha);
        ui->alphaLosslessChk->setChecked(useLosslessAlpha);
        ui->bitDepthCmb->setCurrentIndex(bitdepth);
        ui->distanceSpn->setValue(encDistance);
        ui->effortSpn->setValue(encEffort);
        ui->numeratorSpn->setValue(numerator);
        ui->denominatorSpn->setValue(denominator);
        ui->loopsSpinBox->setValue(numLoops);
        ui->isAnimatedBox->setChecked(useAnimation);
        ui->colorSpaceCmb->setCurrentIndex(colorSpace);
        ui->photonNoiseSpn->setValue(photonNoise);
        ui->autoCropChk->setChecked(autoCrop);
        ui->autoCropTreshSpn->setValue(autoCropThr);

        if (loadjs.value("fileList").isArray()) {
            const QJsonArray farray = loadjs.value("fileList").toArray();
            d->inputFileList.clear();

            ui->treeWidget->clear();
            foreach (const auto &fs, farray) {
                const QJsonObject ff = fs.toObject();
                const QString tmpFile = ff.value("filename").toString();
                const JxlBlendMode tmpBlend = static_cast<JxlBlendMode>(ff.value("blend").toInt(2));
                const int tmpFrameDur = ff.value("frameDur").toInt(1);
                const int tmpFrameRef = ff.value("frameRef").toInt(0);
                const bool tmpFrameEndPage = ff.value("frameEndP").toBool(false);
                const int tmpIsRef = [&]() {
                    if (ff.value("isRef").isBool()) {
                        return ff.value("isRef").toBool(false) ? 1 : 0;
                    }
                    return ff.value("isRef").toInt(0);
                }();
                const int tmpFrameX = ff.value("frameXPos").toInt(0);
                const int tmpFrameY = ff.value("frameYPos").toInt(0);
                const QString tmpFrameName = ff.value("frameName").toString();

                if (!tmpFile.isEmpty()) {

                    jxfrstch::InputFileData ifd;
                    ifd.filename = tmpFile;
                    ifd.blendMode = tmpBlend;
                    ifd.frameDuration = tmpFrameDur;
                    ifd.frameReference = tmpFrameRef;
                    ifd.isRefFrame = tmpIsRef;
                    ifd.frameXPos = tmpFrameX;
                    ifd.frameYPos = tmpFrameY;
                    ifd.frameName = tmpFrameName;
                    ifd.isPageEnd = tmpFrameEndPage;

                    QTreeWidgetItem *item = new QTreeWidgetItem(ui->treeWidget);
                    item->setData(0, 0, ifd.filename);
                    item->setData(1, 0, ifd.isRefFrame);
                    item->setData(2, 0, ifd.frameDuration);
                    if (ifd.isPageEnd)
                        item->setData(2, 0, "END");
                    item->setData(3, 0, ifd.frameReference);
                    item->setData(4, 0, ifd.frameXPos);
                    item->setData(5, 0, ifd.frameYPos);
                    item->setData(6, 0, jxfrstch::blendModeToString(ifd.blendMode));
                    item->setData(7, 0, ifd.frameName);
                    item->setBackground(0, {});
                    item->setFlags(item->flags() & ~Qt::ItemIsDropEnabled);
                    if (ifd.isRefFrame) {
                        item->setBackground(1, QColor(128, 255, 128));
                    }
                    if (ifd.isPageEnd) {
                        item->setBackground(2, QColor(255, 255, 128));
                    }
                    ui->treeWidget->addTopLevelItem(item);
                }
            }
        }
    }

    d->configSaveFile = tmpfn;
    QFileInfo outFInfo(tmpfn);
    setWindowTitle(QString("%1 - %2").arg(d->windowTitle, outFInfo.fileName()));
    ui->statusBar->showMessage("Config loaded");
    ui->progressBar->hide();
}

void MainWindow::currentFrameSettingChanged()
{
    if (ui->treeWidget->topLevelItemCount() == 0 || ui->treeWidget->selectedItems().size() == 0) {
        return;
    }

    const auto selItemList = ui->treeWidget->selectedItems();

    const bool changeFrameDur = (ui->frameDurationSpn->value() >= 0);
    const bool changeFrameRef = (ui->frameRefSpinBox->value() >= 0);
    const bool changeFrameBlend = (ui->blendModeCmb->currentIndex() != 5);
    const bool changeFrameName = (ui->frameNameLine->text() != "<unchanged>");
    const bool changeSaveRef = (ui->saveAsRefSpn->value() >= 0);
    const bool changePageEnd = (ui->pageEndChk->checkState() != Qt::PartiallyChecked);

    foreach (const auto &v, selItemList) {
        if (changeSaveRef) {
            if (ui->saveAsRefSpn->value() > 0) {
                v->setBackground(1, QColor(128, 255, 128));
            } else {
                v->setBackground(1, {});
            }
            v->setData(1, 0, ui->saveAsRefSpn->value());
        }
        if (changeFrameDur)
            v->setData(2, 0, ui->frameDurationSpn->value());
        if (changePageEnd) {
            if (ui->pageEndChk->isChecked()) {
                v->setData(2, 0, "END");
                v->setBackground(2, QColor(255, 255, 128));
            } else {
                v->setBackground(2, {});
            }
        }
        if (changeFrameRef)
            v->setData(3, 0, ui->frameRefSpinBox->value());
        if (v != ui->treeWidget->topLevelItem(0)) {
            v->setData(4, 0, ui->frameXPosSpn->value());
            v->setData(5, 0, ui->frameYPosSpn->value());
        } else {
            v->setData(4, 0, 0);
            v->setData(5, 0, 0);
        }
        if (changeFrameName) {
            v->setData(7, 0, ui->frameNameLine->text());
        }
        if (changeFrameBlend) {
            JxlBlendMode bld;
            switch (ui->blendModeCmb->currentIndex()) {
            case 0:
                bld = JXL_BLEND_BLEND;
                break;
            case 1:
                bld = JXL_BLEND_REPLACE;
                break;
            case 2:
                bld = JXL_BLEND_ADD;
                break;
            case 3:
                bld = JXL_BLEND_MULADD;
                break;
            case 4:
                bld = JXL_BLEND_MUL;
                break;
            default:
                bld = JXL_BLEND_BLEND;
                break;
            }
            v->setData(6, 0, jxfrstch::blendModeToString(bld));
        }
    }
    setUnsaved();
}

void MainWindow::selectOutputFile()
{
    const QString currentFname = ui->outFileLineEdit->text();
    const QString selectedDir = [&]() {
        if (currentFname.isEmpty())
            return QDir::currentPath();
        return QFileInfo(currentFname).dir().absolutePath();
    }();

    const QString tmpFileName = QFileDialog::getSaveFileName(this,
                                                             tr("Open Image"),
                                                             selectedDir,
                                                             tr("JPEG XL Image (*.jxl);;All Files (*)"));
    if (!tmpFileName.isEmpty()) {
        ui->outFileLineEdit->setText(tmpFileName);
    }
}

void MainWindow::doEncode()
{
    d->statLabel->clear();
    if (ui->treeWidget->topLevelItemCount() == 0 || ui->outFileLineEdit->text().isEmpty()) {
        ui->encodeBtn->setText("Encode");
        d->isEncoding = false;
        return;
    }

    const int encEffort = ui->effortSpn->value();
    const int numerator = ui->numeratorSpn->value();
    const int denominator = ui->denominatorSpn->value();

    jxfrstch::EncodeParams params;

    params.alpha = ui->alphaEnableChk->isChecked();
    params.premulAlpha = ui->alphaPremulChk->isChecked();
    params.losslessAlpha = ui->alphaLosslessChk->isChecked();
    params.bitDepth = static_cast<EncodeBitDepth>(ui->bitDepthCmb->currentIndex());
    params.distance = ui->distanceSpn->value();
    params.effort = ui->effortSpn->value();
    params.numerator = ui->numeratorSpn->value();
    params.denominator = ui->denominatorSpn->value();
    params.loops = ui->loopsSpinBox->value();
    params.animation = ui->isAnimatedBox->isChecked();
    params.colorSpace = static_cast<EncodeColorSpace>(ui->colorSpaceCmb->currentIndex());
    params.lossyModular = ui->modularLossyChk->isChecked();
    params.frameTimeMs = (static_cast<double>(denominator * 1000) / static_cast<double>(numerator));
    params.outputFileName = ui->outFileLineEdit->text();
    params.photonNoise = ui->photonNoiseSpn->value();
    params.autoCropFrame = params.animation ? ui->autoCropChk->isChecked() : false;
    params.autoCropFuzzyComparison = ui->autoCropTreshSpn->value();
    params.coalesceJxlInput = ui->autoCropChk ? true : ui->actionCoalesce_JXL_input->isChecked();
    params.chunkedFrame = ui->actionUse_chunked_input->isChecked();

    if (encEffort > 10) {
        const auto diag = QMessageBox::warning(this,
                                               "Caution",
                                               "You have choosen effort >10 which is insanely heavy and slow! You may "
                                               "have to force terminate (End Task) to abort the process "
                                               "since all of the application resources will be directed for encoding, "
                                               "which will result in an unresponsiveness!"
                                               "\n\nAre you really sure want to continue?",
                                               QMessageBox::Yes | QMessageBox::No);
        if (diag == QMessageBox::No) {
            ui->effortSpn->setValue(10);
            ui->encodeBtn->setText("Encode");
            d->isEncoding = false;
            return;
        }
    }

    if (QFileInfo::exists(params.outputFileName)) {
        const auto diag = QMessageBox::warning(this,
                                               "Caution",
                                               "Output file already exists. Do you want to replace it?",
                                               QMessageBox::Yes | QMessageBox::No);
        if (diag == QMessageBox::No) {
            ui->encodeBtn->setText("Encode");
            d->isEncoding = false;
            return;
        }
    }

    ui->progressBar->show();
    ui->progressBar->setMinimum(0);
    ui->progressBar->setValue(0);
    ui->progressBarSub->hide();

    ui->menuBar->setEnabled(false);
    ui->frameListGrp->setEnabled(false);
    ui->globalSettingGrp->setEnabled(false);
    setAcceptDrops(false);

    d->encObj->resetEncoder();
    d->encObj->setEncodeParams(params);

    const int framenum = ui->treeWidget->topLevelItemCount();
    ui->progressBar->setMaximum(framenum);

    for (int i = 0; i < framenum; i++) {
        QTreeWidgetItem *itm = ui->treeWidget->topLevelItem(i);
        itm->setBackground(0, {});
    }

    for (int i = 0; i < framenum; i++) {
        QTreeWidgetItem *itm = ui->treeWidget->topLevelItem(i);
        jxfrstch::InputFileData ind;
        bool isDurInt = true;
        ind.filename = itm->data(0, 0).toString();
        ind.isRefFrame = itm->data(1, 0).toInt();
        ind.frameDuration = itm->data(2, 0).toInt(&isDurInt);
        if (!isDurInt) {
            ind.frameDuration = 1;
            ind.isPageEnd = true;
        } else {
            ind.isPageEnd = false;
        }
        ind.frameReference = itm->data(3, 0).toInt();
        ind.frameXPos = itm->data(4, 0).toInt();
        ind.frameYPos = itm->data(5, 0).toInt();
        ind.blendMode = jxfrstch::stringToBlendMode(itm->data(6, 0).toString());
        ind.frameName = itm->data(7, 0).toString();

        d->encObj->appendInputFiles(ind);
    }

    if (d->encObj->canEncode()) {
        d->encObj->start();
    } else {
        ui->statusBar->showMessage("Encode aborted: unable to read first frame data!");
        ui->encodeBtn->setText("Encode");
        ui->menuBar->setEnabled(true);
        ui->frameListGrp->setEnabled(true);
        ui->globalSettingGrp->setEnabled(true);
        ui->progressBar->hide();
        setAcceptDrops(true);
    }
}
