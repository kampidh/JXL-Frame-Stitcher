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
#include <QSettings>

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

#define USE_STREAMING_OUTPUT // need libjxl >= 0.10.0

namespace jxfrstch
{
struct InputFileData {
    bool isRefFrame{false};
    uint16_t frameDuration{1};
    uint8_t frameReference{0};
    int16_t frameXPos{0};
    int16_t frameYPos{0};
    JxlBlendMode blendMode{JXL_BLEND_BLEND};
    QString filename{};
    QString frameName{};

    // lexical comparison
    bool operator<(const InputFileData &rhs) const
    {
        return this->filename.compare(rhs.filename) < 0;
    }

    bool operator==(const QString &rhs) const
    {
        return this->filename == rhs;
    }

    bool operator==(const InputFileData &rhs) const
    {
        return this->filename == rhs.filename;
    }
};

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

template<typename T>
void QImageToBuffer(const QImage &img, QByteArray &ba, size_t pxsize, bool alpha)
{
    auto srcPointer = reinterpret_cast<const T *>(img.constBits());
    auto dstPointer = reinterpret_cast<T *>(ba.data());
    const int chan = (alpha) ? 4 : 3;
    for (size_t i = 0; i < pxsize; i++) {
        memcpy(dstPointer, srcPointer, sizeof(T) * chan);
        srcPointer += 4;
        dstPointer += chan;
    }
}
} // namespace jxfrstch

class Q_DECL_HIDDEN MainWindow::Private
{
public:
    bool isEncoding{false};
    bool encodeAbort{false};
    bool isUnsavedChanges{false};
    QString windowTitle{"JXL Frame Stitching"};
    QString configSaveFile{};
    QCollator collator;
    QVector<jxfrstch::InputFileData> inputFileList;
    QList<QTreeWidgetItem *> wdgList;
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

    d->collator.setNumericMode(true);
    ui->treeWidget->setColumnWidth(0, 120);
    ui->treeWidget->setColumnWidth(1, 40);
    ui->treeWidget->setColumnWidth(2, 40);
    ui->treeWidget->setColumnWidth(3, 40);
    ui->treeWidget->setColumnWidth(4, 48);
    ui->treeWidget->setColumnWidth(5, 48);
    ui->treeWidget->setColumnWidth(6, 60);
    ui->progressBarSub->hide();

    connect(ui->clearFilesBtn, &QPushButton::clicked, this, [&]() {
        ui->statusBar->showMessage(
            "Import image frames by drag and dropping into the file list or pressing Add Files...");
        d->inputFileList.clear();
        ui->treeWidget->clear();
    });

    connect(ui->treeWidget, &QTreeWidget::itemSelectionChanged, this, &MainWindow::selectingFrames);

    connect(ui->saveAsRefFrameChk, &QCheckBox::toggled, this, [&]() {
        setUnsaved();
        if (ui->saveAsRefFrameChk->isChecked()) {
            ui->frameDurationSpn->setValue(0);
            ui->frameRefSpinBox->setEnabled(false);
            ui->frameDurationSpn->setEnabled(false);
        } else {
            ui->frameDurationSpn->setValue(1);
            ui->frameRefSpinBox->setEnabled(true);
            ui->frameDurationSpn->setEnabled(true);
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
            ui->saveAsRefFrameChk->setEnabled(false);
        } else {
            ui->frameRefSpinBox->setEnabled(!ui->saveAsRefFrameChk->isChecked());
            ui->frameDurationSpn->setEnabled(!ui->saveAsRefFrameChk->isChecked());
            ui->saveAsRefFrameChk->setEnabled(ui->treeWidget->selectedItems().size() == 1);
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
        if (!d->isEncoding) {
            if (ui->treeWidget->topLevelItemCount() > 0) {
                ui->encodeBtn->setText("Abort");
                d->isEncoding = true;
                d->encodeAbort = false;
                doEncode();
            }
        } else {
            ui->encodeBtn->setText("Encode");
            d->encodeAbort = true;
            d->isEncoding = false;
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
}

MainWindow::~MainWindow()
{
    delete ui;
    delete d;
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
            openConfig(finfo.absoluteFilePath());
        } else if (finfo.isFile() || event->mimeData()->urls().count() > 1) {
            QStringList fileList;
            foreach (const QUrl &url, event->mimeData()->urls()) {
                const QFileInfo fileInfo(url.toLocalFile());
                if (fileInfo.isFile()
                    && ui->treeWidget->findItems(fileInfo.absoluteFilePath(), Qt::MatchExactly, 0).isEmpty()
                    && QImageReader::supportedImageFormats().contains(fileInfo.suffix().toLower())) {
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
        ifd.isRefFrame = itm->data(1, 0).toBool();
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
    foreach (const auto &v, QImageReader::supportedImageFormats()) {
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
                    && QImageReader::supportedImageFormats().contains(fileInfo.suffix().toLower())) {
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

    if (ui->treeWidget->selectedItems().size() > 1) {
        ui->selectedFrameBox->setEnabled(true);
        ui->saveAsRefFrameChk->setEnabled(false);
        ui->selectedFileLabel->setText(QString("%1 files selected").arg(ui->treeWidget->selectedItems().size()));

        ui->saveAsRefFrameChk->setChecked(currentSelItem->data(1, 0).toBool());
        ui->frameDurationSpn->setValue(-1);
        ui->frameRefSpinBox->setValue(-1);
        ui->frameXPosSpn->setValue(currentSelItem->data(4, 0).toInt());
        ui->frameYPosSpn->setValue(currentSelItem->data(5, 0).toInt());
        ui->blendModeCmb->setCurrentIndex(5);
        ui->frameNameLine->setText("<unchanged>");

        ui->frameXPosSpn->setEnabled(true);
        ui->frameYPosSpn->setEnabled(true);
    } else if (ui->treeWidget->selectedItems().size() == 1) {
        ui->selectedFrameBox->setEnabled(true);
        ui->saveAsRefFrameChk->setEnabled(true);
        ui->selectedFileLabel->setText(currentSelItem->data(0, 0).toString());
        ui->saveAsRefFrameChk->setChecked(currentSelItem->data(1, 0).toBool());
        ui->frameDurationSpn->setValue(currentSelItem->data(2, 0).toInt());
        ui->frameRefSpinBox->setValue(currentSelItem->data(3, 0).toInt());
        ui->frameXPosSpn->setValue(currentSelItem->data(4, 0).toInt());
        ui->frameYPosSpn->setValue(currentSelItem->data(5, 0).toInt());
        ui->frameNameLine->setText(currentSelItem->data(7, 0).toString());
        if (ui->saveAsRefFrameChk->isChecked()) {
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
        ui->saveAsRefFrameChk->setEnabled(false);
    } else {
        ui->frameRefSpinBox->setEnabled(!ui->saveAsRefFrameChk->isChecked());
        ui->frameDurationSpn->setEnabled(!ui->saveAsRefFrameChk->isChecked());
        ui->saveAsRefFrameChk->setEnabled(ui->treeWidget->selectedItems().size() == 1);
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
        jsobj["isRef"] = itm->data(1, 0).toBool();
        jsobj["frameDur"] = itm->data(2, 0).toInt();
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
    const QString tmpfn =
        QFileDialog::getOpenFileName(this, "Open setting", QDir::currentPath(), "Frame Stitch Config (*.frstch)");
    if (tmpfn.isEmpty()) {
        return;
    }
    openConfig(tmpfn);
}

void MainWindow::openConfig(const QString &tmpfn)
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
                const bool tmpIsRef = ff.value("isRef").toBool(false);
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
    const bool isRefCheck = ui->saveAsRefFrameChk->isChecked();

    const bool changeFrameDur = (ui->frameDurationSpn->value() != -1);
    const bool changeFrameRef = (ui->frameRefSpinBox->value() != -1);
    const bool changeFrameBlend = (ui->blendModeCmb->currentIndex() != 5);
    const bool changeFrameName = (ui->frameNameLine->text() != "<unchanged>");

    if (isRefCheck) {
        for (int i = 0; i < ui->treeWidget->topLevelItemCount(); i++) {
            auto *itm = ui->treeWidget->topLevelItem(i);
            itm->setData(1, 0, false);
            itm->setBackground(1, {});
        }
    }

    foreach (const auto &v, selItemList) {
        if (isRefCheck) {
            v->setBackground(1, QColor(128, 255, 128));
        }
        v->setData(1, 0, isRefCheck);
        if (changeFrameDur)
            v->setData(2, 0, ui->frameDurationSpn->value());
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
    if (ui->treeWidget->topLevelItemCount() == 0 || ui->outFileLineEdit->text().isEmpty()) {
        ui->encodeBtn->setText("Encode");
        d->isEncoding = false;
        return;
    }

    const bool useAlpha = ui->alphaEnableChk->isChecked();
    const bool usePremulAlpha = ui->alphaPremulChk->isChecked();
    const bool useLosslessAlpha = ui->alphaLosslessChk->isChecked();
    const int bitdepth = ui->bitDepthCmb->currentIndex();
    const double encDistance = ui->distanceSpn->value();
    const int encEffort = ui->effortSpn->value();
    const int numerator = ui->numeratorSpn->value();
    const int denominator = ui->denominatorSpn->value();
    const int numLoops = ui->loopsSpinBox->value();
    const bool useAnimation = ui->isAnimatedBox->isChecked();
    const int selColorSpace = ui->colorSpaceCmb->currentIndex();

    const bool useLossyModular = ui->modularLossyChk->isChecked();

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

    const QString outFileName = ui->outFileLineEdit->text();
    const float frameTimeMs = (static_cast<float>(denominator * 1000) / static_cast<float>(numerator));

    if (QFileInfo::exists(outFileName)) {
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

    QImage firstLayer(ui->treeWidget->topLevelItem(0)->data(0, 0).toString());

    if (firstLayer.isNull()) {
        ui->statusBar->showMessage("Error: failed to load first image!");
        ui->encodeBtn->setText("Encode");
        d->isEncoding = false;
        return;
    }

    ui->progressBar->show();
    ui->progressBar->setMinimum(0);
    ui->progressBar->setValue(0);
    ui->progressBarSub->hide();

    ui->menuBar->setEnabled(false);
    ui->frameListGrp->setEnabled(false);
    ui->globalSettingGrp->setEnabled(false);
    setAcceptDrops(false);

    ui->statusBar->showMessage("Begin encoding...");
    QGuiApplication::processEvents();

    const int width = firstLayer.width();
    const int height = firstLayer.height();
    const QRect bounds = firstLayer.rect();

    const QByteArray firstLayerIcc = firstLayer.colorSpace().iccProfile();
    std::vector<uint8_t> sda;

    auto enc = JxlEncoderMake(nullptr);
    auto runner = JxlResizableParallelRunnerMake(nullptr);

#ifdef USE_STREAMING_OUTPUT
    jxfrstch::JxlOutputProcessor outProcessor;
    if (!outProcessor.SetOutputPath(outFileName)) {
        throw QString("Failed to create output file!");
    }
#endif

    try {
        if (JXL_ENC_SUCCESS != JxlEncoderSetParallelRunner(enc.get(), JxlResizableParallelRunner, runner.get())) {
            throw QString("JxlEncoderSetParallelRunner failed");
        }

        JxlResizableParallelRunnerSetThreads(
            runner.get(),
            JxlResizableParallelRunnerSuggestThreads(static_cast<uint64_t>(bounds.width()),
                                                     static_cast<uint64_t>(bounds.height())));

#ifdef USE_STREAMING_OUTPUT
        if (JXL_ENC_SUCCESS != JxlEncoderSetOutputProcessor(enc.get(), outProcessor.GetOutputProcessor())) {
            throw QString("JxlEncoderSetOutputProcessor failed");
        }
#endif
        if (bitdepth > 3) {
            throw QString("Error: unsupported bitdepth");
        }

        const JxlPixelFormat pixelFormat = [&]() {
            JxlPixelFormat pixelFormat{};
            switch (bitdepth) {
            case 0:
                pixelFormat.data_type = JXL_TYPE_UINT8;
                break;
            case 1:
                pixelFormat.data_type = JXL_TYPE_UINT16;
                break;
            case 2:
                pixelFormat.data_type = JXL_TYPE_FLOAT16;
                break;
            case 3:
                pixelFormat.data_type = JXL_TYPE_FLOAT;
                break;
            default:
                break;
            }
            pixelFormat.num_channels = useAlpha ? 4 : 3;
            return pixelFormat;
        }();

        const auto basicInfo = [&]() {
            auto info{std::make_unique<JxlBasicInfo>()};
            JxlEncoderInitBasicInfo(info.get());
            info->xsize = static_cast<uint32_t>(bounds.width());
            info->ysize = static_cast<uint32_t>(bounds.height());
            switch (pixelFormat.data_type) {
            case JXL_TYPE_UINT8:
                info->bits_per_sample = 8;
                info->exponent_bits_per_sample = 0;
                if (useAlpha) {
                    info->alpha_bits = 8;
                    info->alpha_exponent_bits = 0;
                    info->alpha_premultiplied = usePremulAlpha ? JXL_TRUE : JXL_FALSE;
                }
                break;
            case JXL_TYPE_UINT16:
                info->bits_per_sample = 16;
                info->exponent_bits_per_sample = 0;
                if (useAlpha) {
                    info->alpha_bits = 16;
                    info->alpha_exponent_bits = 0;
                    info->alpha_premultiplied = usePremulAlpha ? JXL_TRUE : JXL_FALSE;
                }
                break;
            case JXL_TYPE_FLOAT16:
                info->bits_per_sample = 16;
                info->exponent_bits_per_sample = 5;
                if (useAlpha) {
                    info->alpha_bits = 16;
                    info->alpha_exponent_bits = 5;
                    info->alpha_premultiplied = usePremulAlpha ? JXL_TRUE : JXL_FALSE;
                }
                break;
            case JXL_TYPE_FLOAT:
                info->bits_per_sample = 32;
                info->exponent_bits_per_sample = 8;
                if (useAlpha) {
                    info->alpha_bits = 32;
                    info->alpha_exponent_bits = 8;
                    info->alpha_premultiplied = usePremulAlpha ? JXL_TRUE : JXL_FALSE;
                }
                break;
            default:
                break;
            }

            info->num_color_channels = 3;
            if (useAlpha) {
                info->num_extra_channels = 1;
            }

            if (encDistance > 0.0) {
                info->uses_original_profile = JXL_FALSE;
            } else {
                info->uses_original_profile = JXL_TRUE;
            }
            info->have_animation = useAnimation ? JXL_TRUE : JXL_FALSE;

            if (useAnimation) {
                info->animation.have_timecodes = JXL_FALSE;
                info->animation.tps_numerator = static_cast<uint32_t>(numerator);
                info->animation.tps_denominator = static_cast<uint32_t>(denominator);
                info->animation.num_loops = static_cast<uint32_t>(numLoops);
            }

            return info;
        }();

        if (JXL_ENC_SUCCESS != JxlEncoderSetBasicInfo(enc.get(), basicInfo.get())) {
            throw QString("JxlEncoderSetBasicInfo failed");
        }

        if (selColorSpace != 3 || (selColorSpace == 3 && firstLayerIcc.isEmpty())) {
            JxlColorEncoding cicpDescription{};

            switch (selColorSpace) {
            case 0:
                cicpDescription.transfer_function = JXL_TRANSFER_FUNCTION_SRGB;
                cicpDescription.primaries = JXL_PRIMARIES_SRGB;
                cicpDescription.white_point = JXL_WHITE_POINT_D65;
                break;
            case 1:
                cicpDescription.transfer_function = JXL_TRANSFER_FUNCTION_LINEAR;
                cicpDescription.primaries = JXL_PRIMARIES_SRGB;
                cicpDescription.white_point = JXL_WHITE_POINT_D65;
                break;
            case 2:
                cicpDescription.transfer_function = JXL_TRANSFER_FUNCTION_SRGB;
                cicpDescription.primaries = JXL_PRIMARIES_P3;
                cicpDescription.white_point = JXL_WHITE_POINT_D65;
                break;
            default:
                cicpDescription.transfer_function = JXL_TRANSFER_FUNCTION_SRGB;
                cicpDescription.primaries = JXL_PRIMARIES_SRGB;
                cicpDescription.white_point = JXL_WHITE_POINT_D65;
                break;
            }

            if (JXL_ENC_SUCCESS != JxlEncoderSetColorEncoding(enc.get(), &cicpDescription)) {
                throw QString("JxlEncoderSetColorEncoding failed");
            }
        } else if (selColorSpace == 3 && !firstLayerIcc.isEmpty()) {
            if (JXL_ENC_SUCCESS
                != JxlEncoderSetICCProfile(enc.get(),
                                           reinterpret_cast<const uint8_t *>(firstLayerIcc.constData()),
                                           static_cast<size_t>(firstLayerIcc.size()))) {
                throw QString("JxlEncoderSetICCProfile failed");
            }
        }

        auto *frameSettings = JxlEncoderFrameSettingsCreate(enc.get(), nullptr);
        {
            const auto setFrameLossless = [&](bool v) {
                if (JxlEncoderSetFrameLossless(frameSettings, v ? JXL_TRUE : JXL_FALSE) != JXL_ENC_SUCCESS) {
                    qDebug() << "JxlEncoderSetFrameLossless failed";
                    return false;
                }
                return true;
            };

            const auto setSetting = [&](JxlEncoderFrameSettingId id, int v) {
                if (JxlEncoderFrameSettingsSetOption(frameSettings, id, v) != JXL_ENC_SUCCESS) {
                    qDebug() << "JxlEncoderFrameSettingsSetOption failed";
                    return false;
                }
                return true;
            };

            const auto setDistance = [&](double v) {
                if (JxlEncoderSetFrameDistance(frameSettings, v) != JXL_ENC_SUCCESS) {
                    qDebug() << "JxlEncoderSetFrameDistance failed";
                    return false;
                }
                if (useAlpha) {
                    const double alphadist = useLosslessAlpha ? 0.0 : v;
                    if (JxlEncoderSetExtraChannelDistance(frameSettings, 0, alphadist) != JXL_ENC_SUCCESS) {
                        qDebug() << "JxlEncoderSetExtraChannelDistance (alpha) failed";
                        return false;
                    }
                }
                return true;
            };

            [[maybe_unused]] const auto setSettingFloat = [&](JxlEncoderFrameSettingId id, float v) {
                if (JxlEncoderFrameSettingsSetFloatOption(frameSettings, id, v) != JXL_ENC_SUCCESS) {
                    qDebug() << "JxlEncoderFrameSettingsSetFloatOption failed";
                    return false;
                }
                return true;
            };

            if (encEffort > 10) {
                JxlEncoderAllowExpertOptions(enc.get());
            }

            if (!setFrameLossless((encDistance > 0.0) ? false : true) || !setDistance(encDistance)
                || !setSetting(JXL_ENC_FRAME_SETTING_EFFORT, encEffort)
                || !setSetting(JXL_ENC_FRAME_SETTING_MODULAR, (useLossyModular ? 1 : -1))) {
                throw QString("JxlEncoderFrameSettings failed");
            }
        }

        bool referenceSaved = false;
        auto frameHeader = std::make_unique<JxlFrameHeader>();

        // const int framenum = d->inputFileList.size();
        const int framenum = ui->treeWidget->topLevelItemCount();
        ui->progressBar->setMaximum(framenum);

        for (int i = 0; i < framenum; i++) {
            QTreeWidgetItem *itm = ui->treeWidget->topLevelItem(i);
            itm->setBackground(0, {});
        }

        for (int i = 0; i < framenum; i++) {
            QTreeWidgetItem *itm = ui->treeWidget->topLevelItem(i);
            jxfrstch::InputFileData ind;
            ind.filename = itm->data(0, 0).toString();
            ind.isRefFrame = itm->data(1, 0).toBool();
            ind.frameDuration = itm->data(2, 0).toInt();
            ind.frameReference = itm->data(3, 0).toInt();
            ind.frameXPos = itm->data(4, 0).toInt();
            ind.frameYPos = itm->data(5, 0).toInt();
            ind.blendMode = jxfrstch::stringToBlendMode(itm->data(6, 0).toString());
            ind.frameName = itm->data(7, 0).toString();

            ui->treeWidget->setCurrentItem(itm);
            itm->setBackground(0, QColor(255, 255, 96));
            QGuiApplication::processEvents();

            QImageReader reader(ind.filename);

            int imageframenum = 0;
            const bool isImageAnim = reader.imageCount() > 1 && reader.supportsAnimation();
            if (isImageAnim) {
                ui->progressBarSub->setVisible(true);
                ui->progressBarSub->setMinimum(0);
                ui->progressBarSub->setMaximum(reader.imageCount());
            }
            while (reader.canRead()) {
                int frameXPos = 0;
                int frameYPos = 0;
                if (i > 0) {
                    frameXPos = ind.frameXPos;
                    frameYPos = ind.frameYPos;
                }

                QByteArray imagerawdata;
                bool needCrop = false;
                QSize frameSize;
                size_t frameResolution;
                {
                    QImage currentFrame(reader.read());

                    if (currentFrame.isNull()) {
                        throw reader.errorString();
                        // throw QString("Encode failed: one of the image(s) failed to load!");
                    }
                    if ((currentFrame.width() != width || currentFrame.height() != height)
                        || ((frameXPos != 0 || frameYPos != 0) && i > 0)) {
                        needCrop = true;
                        // throw QString("Encode failed: one of the image(s) have different dimensions!");
                    }

                    switch (bitdepth) {
                    case 0: // u8bpc
                        currentFrame.convertTo(useAlpha ? QImage::Format_RGBA8888 : QImage::Format_RGBX8888);
                        break;
                    case 1: // u16bpc
                        currentFrame.convertTo(useAlpha ? QImage::Format_RGBA64 : QImage::Format_RGBX64);
                        break;
                    case 2: // f16bpc
                        currentFrame.convertTo(useAlpha ? QImage::Format_RGBA16FPx4 : QImage::Format_RGBX16FPx4);
                        break;
                    case 3: // f32bpc
                        currentFrame.convertTo(useAlpha ? QImage::Format_RGBA32FPx4 : QImage::Format_RGBX32FPx4);
                        break;
                    default:
                        break;
                    }

                    if (selColorSpace != 4) {
                        // treat untagged as sRGB
                        if (!currentFrame.colorSpace().isValid()) {
                            currentFrame.setColorSpace(QColorSpace::SRgb);
                        }
                        switch (selColorSpace) {
                        case 0:
                            currentFrame.convertToColorSpace(QColorSpace::SRgb);
                            break;
                        case 1:
                            currentFrame.convertToColorSpace(QColorSpace::SRgbLinear);
                            break;
                        case 2:
                            currentFrame.convertToColorSpace(QColorSpace::DisplayP3);
                            break;
                        case 3:
                            if (!firstLayerIcc.isEmpty()) {
                                currentFrame.convertToColorSpace(QColorSpace::fromIccProfile(firstLayerIcc));
                            } else {
                                currentFrame.convertToColorSpace(QColorSpace::SRgb);
                            }
                            break;
                        default:
                            break;
                        }
                    }

                    frameSize = currentFrame.size();
                    frameResolution = frameSize.width() * frameSize.height();
                    const size_t byteSize = [&]() {
                        switch (bitdepth) {
                        case 0:
                            return 1;
                            break;
                        case 1:
                        case 2:
                            return 2;
                            break;
                        case 3:
                            return 4;
                        default:
                            return 1;
                            break;
                        }
                    }();

                    const size_t neededBytes = ((useAlpha) ? 4 : 3) * byteSize * frameResolution;
                    imagerawdata.resize(neededBytes, 0x0);

                    switch (bitdepth) {
                    case 0:
                        jxfrstch::QImageToBuffer<uint8_t>(currentFrame, imagerawdata, frameResolution, useAlpha);
                        break;
                    case 1:
                        jxfrstch::QImageToBuffer<uint16_t>(currentFrame, imagerawdata, frameResolution, useAlpha);
                        break;
                    case 2:
                        jxfrstch::QImageToBuffer<qfloat16>(currentFrame, imagerawdata, frameResolution, useAlpha);
                        break;
                    case 3:
                        jxfrstch::QImageToBuffer<float>(currentFrame, imagerawdata, frameResolution, useAlpha);
                        break;
                    default:
                        break;
                    }
                }

                const uint16_t frameTick = [&]() {
                    if (!isImageAnim || !reader.canRead()) { // can't read == end of animation or just a single frame
                        return ind.frameDuration;
                    } else {
                        return static_cast<uint16_t>(
                            qRound(qMax(static_cast<float>(reader.nextImageDelay()) / frameTimeMs, 1.0)));
                    }
                }();

                JxlEncoderInitFrameHeader(frameHeader.get());
                frameHeader->duration = (useAnimation) ? frameTick : 0;
                if (ind.isRefFrame && !referenceSaved) {
                    frameHeader->layer_info.save_as_reference = 1;
                    referenceSaved = true;
                }
                frameHeader->layer_info.blend_info.blendmode = ind.blendMode;
                if (useAlpha) {
                    frameHeader->layer_info.blend_info.alpha = 0;
                }
                if (referenceSaved && !ind.isRefFrame) {
                    frameHeader->layer_info.blend_info.source = ind.frameReference;
                }
                if (needCrop) {
                    frameHeader->layer_info.have_crop = JXL_TRUE;
                    frameHeader->layer_info.crop_x0 = static_cast<int32_t>(frameXPos);
                    frameHeader->layer_info.crop_y0 = static_cast<int32_t>(frameYPos);
                    frameHeader->layer_info.xsize = static_cast<uint32_t>(frameSize.width());
                    frameHeader->layer_info.ysize = static_cast<uint32_t>(frameSize.height());
                }

                if (JxlEncoderSetFrameHeader(frameSettings, frameHeader.get()) != JXL_ENC_SUCCESS) {
                    throw QString("JxlEncoderSetFrameHeader failed");
                }

                if (!ind.frameName.isEmpty() && ind.frameName.toUtf8().size() <= 1071) {
                    if (JxlEncoderSetFrameName(frameSettings, ind.frameName.toUtf8()) != JXL_ENC_SUCCESS) {
                        throw QString("JxlEncoderSetFrameName failed");
                    }
                    // in case warning is needed
                // } else if (ind.frameName.toUtf8().size() > 1071) {
                //     QMessageBox::warning(this,
                //                          "Warning",
                //                          QString("Cannot write name for frame %1, name exceeds 1071 bytes limit!\n(Current: %2 bytes)")
                //                              .arg(QString::number(i + 1), QString::number(ind.frameName.toUtf8().size())));
                }

                if (JxlEncoderAddImageFrame(frameSettings, &pixelFormat, imagerawdata.constData(), imagerawdata.size())
                    != JXL_ENC_SUCCESS) {
                    throw QString("JxlEncoderAddImageFrame failed");
                }

                const double currentImageSizeKiB = [&]() {
#ifdef USE_STREAMING_OUTPUT
                    return static_cast<double>(outProcessor.finalized_position) / 1024.0;
#else
                    return 0.0;
#endif
                }();

                if (isImageAnim) {
                    ui->statusBar->showMessage(
                        QString("Processing frame %1 of %2 (Subframe %3 of %4) | Output file size: %5 KiB")
                            .arg(QString::number(i + 1),
                                 QString::number(framenum),
                                 QString::number(imageframenum + 1),
                                 QString::number(reader.imageCount()),
                                 QString::number(currentImageSizeKiB)));
                    ui->progressBarSub->setValue(imageframenum + 1);
                } else {
                    ui->statusBar->showMessage(QString("Processing frame %1 of %2 | Output file size: %3 KiB")
                                                   .arg(QString::number(i + 1),
                                                        QString::number(framenum),
                                                        QString::number(currentImageSizeKiB)));
                }

                if (d->encodeAbort) {
                    JxlEncoderCloseInput(enc.get());
                    JxlEncoderFlushInput(enc.get());
                    throw QString("Encode aborted!");
                }
                QGuiApplication::processEvents();

                if (i == framenum - 1 && !reader.canRead()) {
                    JxlEncoderCloseInput(enc.get());
                }
#ifdef USE_STREAMING_OUTPUT
                JxlEncoderFlushInput(enc.get());
#endif
                imageframenum++;
            }
            ui->progressBarSub->hide();
            itm->setBackground(0, QColor(128, 255, 255));
            ui->progressBar->setValue(i + 1);
            QGuiApplication::processEvents();
        }

#ifndef USE_STREAMING_OUTPUT
        QFile outF(outFileName);
        outF.open(QIODevice::WriteOnly);
        if (!outF.isWritable()) {
            outF.close();
            throw QString("Encode failed: Cannot write to output file");
        }

        QByteArray compressed(16384, 0x0);
        auto *nextOut = reinterpret_cast<uint8_t *>(compressed.data());
        auto availOut = static_cast<size_t>(compressed.size());
        auto result = JXL_ENC_NEED_MORE_OUTPUT;
        while (result == JXL_ENC_NEED_MORE_OUTPUT) {
            result = JxlEncoderProcessOutput(enc.get(), &nextOut, &availOut);
            if (result != JXL_ENC_ERROR) {
                outF.write(compressed.data(), compressed.size() - static_cast<int>(availOut));
            }
            if (result == JXL_ENC_NEED_MORE_OUTPUT) {
                compressed.resize(compressed.size() * 2);
                nextOut = reinterpret_cast<uint8_t *>(compressed.data());
                availOut = static_cast<size_t>(compressed.size());
            }
        }
        if (JXL_ENC_SUCCESS != result) {
            outF.close();
            outF.remove();
            throw QString("JxlEncoderProcessOutput failed");
        }
        outF.close();
#endif

#ifdef USE_STREAMING_OUTPUT
        outProcessor.CloseOutputFile();
#endif
        const double finalImageSizeKiB = [&]() {
#ifdef USE_STREAMING_OUTPUT
            return static_cast<double>(outProcessor.finalized_position) / 1024.0;
#else
            return static_cast<double>(QFileInfo(outFileName).size()) / 1024.0;
#endif
        }();

        ui->statusBar->showMessage(
            QString("Encode successful | Final output file size: %1 KiB").arg(QString::number(finalImageSizeKiB)));
        ui->encodeBtn->setEnabled(true);
        ui->encodeBtn->setText("Encode");
        d->isEncoding = false;
        d->inputFileList.clear();

        ui->menuBar->setEnabled(true);
        ui->frameListGrp->setEnabled(true);
        ui->globalSettingGrp->setEnabled(true);
        setAcceptDrops(true);

    } catch (QString err) {
        if (!d->encodeAbort) {
#ifdef USE_STREAMING_OUTPUT
            outProcessor.CloseOutputFile();
            outProcessor.DeleteOutputFile();
#endif
            QMessageBox::critical(this, "Encoding error", err);
            d->inputFileList.clear();
        }
        ui->statusBar->showMessage(err);
        ui->encodeBtn->setEnabled(true);
        ui->encodeBtn->setText("Encode");
        d->isEncoding = false;
        d->inputFileList.clear();

        ui->menuBar->setEnabled(true);
        ui->frameListGrp->setEnabled(true);
        ui->globalSettingGrp->setEnabled(true);
        setAcceptDrops(true);
    } catch (...) {
#ifdef USE_STREAMING_OUTPUT
        outProcessor.CloseOutputFile();
        outProcessor.DeleteOutputFile();
#endif
        QMessageBox::critical(this, "Encoding error", "Unexpected Error!");
        ui->statusBar->showMessage("Unexpected Error!");
        ui->encodeBtn->setEnabled(true);
        ui->encodeBtn->setText("Encode");
        d->isEncoding = false;
        d->inputFileList.clear();

        ui->menuBar->setEnabled(true);
        ui->frameListGrp->setEnabled(true);
        ui->globalSettingGrp->setEnabled(true);
        setAcceptDrops(true);
    }
}
