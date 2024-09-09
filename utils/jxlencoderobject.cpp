#include "jxlencoderobject.h"
#include "jxldecoderobject.h"

#include <QColorSpace>
#include <QImageReader>
#include <QFileInfo>

#include <jxl/color_encoding.h>
#include <jxl/encode_cxx.h>
#include <jxl/resizable_parallel_runner_cxx.h>

#define USE_STREAMING_OUTPUT // need libjxl >= 0.10.0

class Q_DECL_HIDDEN JXLEncoderObject::Private
{
public:
    bool isEncoding{false};
    bool encodeAbort{false};
    bool abortCompleteFile{true};
    bool isUnsavedChanges{false};
    bool isAborted{false};

    int rootWidth{0};
    int rootHeight{0};
    QSize rootSize{};
    QByteArray rootICC{};

    jxfrstch::EncodeParams params{};
    QVector<jxfrstch::InputFileData> idat{};

    QObject *parent{nullptr};
    JxlEncoderPtr enc;
    JxlResizableParallelRunnerPtr runner;
};

JXLEncoderObject::JXLEncoderObject(QObject *parent)
    : QThread{parent}
    , d(new Private)
{
    d->parent = parent;
}

JXLEncoderObject::~JXLEncoderObject()
{
    d.reset();
}

void JXLEncoderObject::abortEncode(bool completeFile)
{
    mutex.lock();
    d->encodeAbort = true;
    d->abortCompleteFile = completeFile;
    mutex.unlock();
}

bool JXLEncoderObject::resetEncoder()
{
    if (!d->enc || !d->runner) {
        return false;
    }
    JxlEncoderReset(d->enc.get());

    d->isAborted = false;
    d->encodeAbort = false;
    d->abortCompleteFile = true;
    d->idat.clear();

    return true;
}

bool JXLEncoderObject::cleanupEncoder()
{
    QFileInfo fi(d->params.outputFileName);
    if (!fi.exists()) {
        return true;
    }
    if (fi.size() == 0 || (d->isAborted && !d->abortCompleteFile)) {
        return QFile::remove(fi.absoluteFilePath());
    }
    return true;
}

void JXLEncoderObject::setEncodeParams(const jxfrstch::EncodeParams &params)
{
    d->params = params;
}

void JXLEncoderObject::appendInputFiles(const jxfrstch::InputFileData &ifd)
{
    d->idat.append(ifd);
}

bool JXLEncoderObject::parseFirstImage()
{
    if (d->idat.isEmpty()) {
        return false;
    }

    QFileInfo fst(d->idat.first().filename);

    if (fst.suffix().toLower() != "jxl") {
        QImage firstLayer(d->idat.first().filename);
        if (firstLayer.isNull()) {
            emit sigStatusText("Error: failed to load first image!");
            return false;
        }

        QSize layerSize = firstLayer.size();
        if (!layerSize.isValid()) {
            emit sigStatusText("Error: failed to read first layer size!");
            return false;
        }
        d->rootSize = layerSize;
        d->rootICC = firstLayer.colorSpace().iccProfile();
    } else {
        JXLDecoderObject cdec(fst.absoluteFilePath());
        d->rootSize = cdec.getRootFrameSize();
        d->rootICC = cdec.getIccProfie();
    }

    if (!d->enc) {
        d->enc = JxlEncoderMake(nullptr);
        if (!d->enc) {
            emit sigStatusText("Error: failed to initialize encoder!");
            return false;
        }
    }

    if (!d->runner) {
        d->runner = JxlResizableParallelRunnerMake(nullptr);
        if (!d->runner) {
            emit sigStatusText("Error: failed to initialize runner!");
            return false;
        }
    }

    return true;
}

void JXLEncoderObject::run()
{
    if (d->idat.isEmpty()) {
        d->isAborted = true;
        return;
    }

#ifdef USE_STREAMING_OUTPUT
    jxfrstch::JxlOutputProcessor outProcessor;
    if (!outProcessor.SetOutputPath(d->params.outputFileName)) {
        emit sigThrowError("Failed to create output file!");
        d->isAborted = true;
        return;
    }
#endif

    if (JXL_ENC_SUCCESS != JxlEncoderSetParallelRunner(d->enc.get(), JxlResizableParallelRunner, d->runner.get())) {
        emit sigThrowError("JxlEncoderSetParallelRunner failed!");
        d->isAborted = true;
        return;
    }

    JxlResizableParallelRunnerSetThreads(
        d->runner.get(),
        JxlResizableParallelRunnerSuggestThreads(static_cast<uint64_t>(d->rootSize.width()),
                                                 static_cast<uint64_t>(d->rootSize.height())));

#ifdef USE_STREAMING_OUTPUT
    if (JXL_ENC_SUCCESS != JxlEncoderSetOutputProcessor(d->enc.get(), outProcessor.GetOutputProcessor())) {
        emit sigThrowError("JxlEncoderSetOutputProcessor failed!");
        d->isAborted = true;
        return;
    }
#endif

    // Set pixel format
    JxlPixelFormat pixelFormat{};
    switch (d->params.bitDepth) {
    case ENC_BIT_8:
        pixelFormat.data_type = JXL_TYPE_UINT8;
        break;
    case ENC_BIT_16:
        pixelFormat.data_type = JXL_TYPE_UINT16;
        break;
    case ENC_BIT_16F:
        pixelFormat.data_type = JXL_TYPE_FLOAT16;
        break;
    case ENC_BIT_32F:
        pixelFormat.data_type = JXL_TYPE_FLOAT;
        break;
    default:
        emit sigThrowError("Unsupported bit depth!");
        d->isAborted = true;
        return;
        break;
    }
    pixelFormat.num_channels = d->params.alpha ? 4 : 3;

    // Set basic info
    JxlBasicInfo basicInfo{};
    JxlEncoderInitBasicInfo(&basicInfo);
    basicInfo.xsize = static_cast<uint32_t>(d->rootSize.width());
    basicInfo.ysize = static_cast<uint32_t>(d->rootSize.height());
    switch (pixelFormat.data_type) {
    case JXL_TYPE_UINT8:
        basicInfo.bits_per_sample = 8;
        basicInfo.exponent_bits_per_sample = 0;
        if (d->params.alpha) {
            basicInfo.alpha_bits = 8;
            basicInfo.alpha_exponent_bits = 0;
            basicInfo.alpha_premultiplied = d->params.premulAlpha ? JXL_TRUE : JXL_FALSE;
        }
        break;
    case JXL_TYPE_UINT16:
        basicInfo.bits_per_sample = 16;
        basicInfo.exponent_bits_per_sample = 0;
        if (d->params.alpha) {
            basicInfo.alpha_bits = 16;
            basicInfo.alpha_exponent_bits = 0;
            basicInfo.alpha_premultiplied = d->params.premulAlpha ? JXL_TRUE : JXL_FALSE;
        }
        break;
    case JXL_TYPE_FLOAT16:
        basicInfo.bits_per_sample = 16;
        basicInfo.exponent_bits_per_sample = 5;
        if (d->params.alpha) {
            basicInfo.alpha_bits = 16;
            basicInfo.alpha_exponent_bits = 5;
            basicInfo.alpha_premultiplied = d->params.premulAlpha ? JXL_TRUE : JXL_FALSE;
        }
        break;
    case JXL_TYPE_FLOAT:
        basicInfo.bits_per_sample = 32;
        basicInfo.exponent_bits_per_sample = 8;
        if (d->params.alpha) {
            basicInfo.alpha_bits = 32;
            basicInfo.alpha_exponent_bits = 8;
            basicInfo.alpha_premultiplied = d->params.premulAlpha ? JXL_TRUE : JXL_FALSE;
        }
        break;
    default:
        break;
    }
    basicInfo.num_color_channels = 3;
    if (d->params.alpha) {
        basicInfo.num_extra_channels = 1;
    }
    if (d->params.distance > 0.0) {
        basicInfo.uses_original_profile = JXL_FALSE;
    } else {
        basicInfo.uses_original_profile = JXL_TRUE;
    }
    basicInfo.have_animation = d->params.animation ? JXL_TRUE : JXL_FALSE;
    if (d->params.animation) {
        basicInfo.animation.have_timecodes = JXL_FALSE;
        basicInfo.animation.tps_numerator = static_cast<uint32_t>(d->params.numerator);
        basicInfo.animation.tps_denominator = static_cast<uint32_t>(d->params.denominator);
        basicInfo.animation.num_loops = static_cast<uint32_t>(d->params.loops);
    }
    if (JXL_ENC_SUCCESS != JxlEncoderSetBasicInfo(d->enc.get(), &basicInfo)) {
        emit sigThrowError("JxlEncoderSetBasicInfo failed!");
        d->isAborted = true;
        return;
    }

    // Set color space
    if (d->params.colorSpace != ENC_CS_INHERIT_FIRST
        || (d->params.colorSpace == ENC_CS_INHERIT_FIRST && d->rootICC.isEmpty())) {
        JxlColorEncoding cicpDescription{};

        switch (d->params.colorSpace) {
        case ENC_CS_SRGB:
            cicpDescription.transfer_function = JXL_TRANSFER_FUNCTION_SRGB;
            cicpDescription.primaries = JXL_PRIMARIES_SRGB;
            cicpDescription.white_point = JXL_WHITE_POINT_D65;
            break;
        case ENC_CS_SRGB_LINEAR:
            cicpDescription.transfer_function = JXL_TRANSFER_FUNCTION_LINEAR;
            cicpDescription.primaries = JXL_PRIMARIES_SRGB;
            cicpDescription.white_point = JXL_WHITE_POINT_D65;
            break;
        case ENC_CS_P3:
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
        if (JXL_ENC_SUCCESS != JxlEncoderSetColorEncoding(d->enc.get(), &cicpDescription)) {
            emit sigThrowError("JxlEncoderSetColorEncoding failed!");
            d->isAborted = true;
            return;
        }
    } else if (d->params.colorSpace == ENC_CS_INHERIT_FIRST && !d->rootICC.isEmpty()) {
        if (JXL_ENC_SUCCESS
            != JxlEncoderSetICCProfile(d->enc.get(),
                                       reinterpret_cast<const uint8_t *>(d->rootICC.constData()),
                                       static_cast<size_t>(d->rootICC.size()))) {
            emit sigThrowError("JxlEncoderSetICCProfile failed!");
            d->isAborted = true;
            return;
        }
    }

    auto *frameSettings = JxlEncoderFrameSettingsCreate(d->enc.get(), nullptr);
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
            if (d->params.alpha) {
                const double alphadist = d->params.losslessAlpha ? 0.0 : v;
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

        if (d->params.effort > 10) {
            JxlEncoderAllowExpertOptions(d->enc.get());
        }

        if (!setFrameLossless((d->params.distance > 0.0) ? false : true) || !setDistance(d->params.distance)
            || !setSetting(JXL_ENC_FRAME_SETTING_EFFORT, d->params.effort)
            || !setSetting(JXL_ENC_FRAME_SETTING_MODULAR, (d->params.lossyModular ? 1 : -1))) {
            emit sigThrowError("JxlEncoderFrameSettings failed!");
            d->isAborted = true;
            return;
        }
    }

    bool referenceSaved = false;
    auto frameHeader = std::make_unique<JxlFrameHeader>();

    const int framenum = d->idat.size();
    JXLDecoderObject reader;

    for (int i = 0; i < framenum; i++) {
        if (d->encodeAbort && !d->abortCompleteFile) {
            emit sigCurrentMainProgressBar(i, true);
            emit sigEnableSubProgressBar(false, 0);
            emit sigStatusText("Encode aborted!");
            d->isAborted = true;
            return;
        }

        const jxfrstch::InputFileData ind = d->idat.at(i);
        emit sigCurrentMainProgressBar(i, false);

        // QImageReader reader(ind.filename);
        reader.setFileName(ind.filename);

        int imageframenum = 0;
        // const bool isImageAnim = reader.imageCount() > 1 && reader.supportsAnimation();
        const bool isImageAnim = reader.haveAnimation();
        if (isImageAnim || reader.imageCount() > 1) {
            emit sigEnableSubProgressBar(true, reader.imageCount());
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
                const QRect currentFrameRect = reader.currentImageRect();

                if (currentFrame.isNull()) {
                    emit sigThrowError(reader.errorString());
                    d->isAborted = true;
                    return;
                }
                if ((currentFrame.width() != d->rootSize.width() || currentFrame.height() != d->rootSize.height())
                    || ((frameXPos != 0 || frameYPos != 0) && i > 0)) {
                    needCrop = true;
                }

                if ((currentFrameRect.x() != 0 || currentFrameRect.y() != 0) && imageframenum > 0) {
                    needCrop = true;
                    // offset with set position
                    frameXPos += currentFrameRect.x();
                    frameYPos += currentFrameRect.y();
                }

                switch (d->params.bitDepth) {
                case ENC_BIT_8: // u8bpc
                    currentFrame.convertTo(d->params.alpha ? QImage::Format_RGBA8888 : QImage::Format_RGBX8888);
                    break;
                case ENC_BIT_16: // u16bpc
                    currentFrame.convertTo(d->params.alpha ? QImage::Format_RGBA64 : QImage::Format_RGBX64);
                    break;
                case ENC_BIT_16F: // f16bpc
                    currentFrame.convertTo(d->params.alpha ? QImage::Format_RGBA16FPx4 : QImage::Format_RGBX16FPx4);
                    break;
                case ENC_BIT_32F: // f32bpc
                    currentFrame.convertTo(d->params.alpha ? QImage::Format_RGBA32FPx4 : QImage::Format_RGBX32FPx4);
                    break;
                default:
                    emit sigThrowError("Unsupported bit depth!");
                    d->isAborted = true;
                    return;
                    break;
                }

                if (d->params.colorSpace != ENC_CS_RAW) {
                    // treat untagged as sRGB
                    if (!currentFrame.colorSpace().isValid()) {
                        currentFrame.setColorSpace(QColorSpace::SRgb);
                    }
                    switch (d->params.colorSpace) {
                    case ENC_CS_SRGB:
                        currentFrame.convertToColorSpace(QColorSpace::SRgb);
                        break;
                    case ENC_CS_SRGB_LINEAR:
                        currentFrame.convertToColorSpace(QColorSpace::SRgbLinear);
                        break;
                    case ENC_CS_P3:
                        currentFrame.convertToColorSpace(QColorSpace::DisplayP3);
                        break;
                    case ENC_CS_INHERIT_FIRST:
                        if (!d->rootICC.isEmpty()) {
                            currentFrame.convertToColorSpace(QColorSpace::fromIccProfile(d->rootICC));
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
                    switch (d->params.bitDepth) {
                    case ENC_BIT_8:
                        return 1;
                        break;
                    case ENC_BIT_16:
                    case ENC_BIT_16F:
                        return 2;
                        break;
                    case ENC_BIT_32F:
                        return 4;
                    default:
                        return 1;
                        break;
                    }
                }();

                const size_t neededBytes = ((d->params.alpha) ? 4 : 3) * byteSize * frameResolution;
                imagerawdata.resize(neededBytes, 0x0);

                switch (d->params.bitDepth) {
                case ENC_BIT_8:
                    jxfrstch::QImageToBuffer<uint8_t>(currentFrame, imagerawdata, frameResolution, d->params.alpha);
                    break;
                case ENC_BIT_16:
                    jxfrstch::QImageToBuffer<uint16_t>(currentFrame, imagerawdata, frameResolution, d->params.alpha);
                    break;
                case ENC_BIT_16F:
                    jxfrstch::QImageToBuffer<qfloat16>(currentFrame, imagerawdata, frameResolution, d->params.alpha);
                    break;
                case ENC_BIT_32F:
                    jxfrstch::QImageToBuffer<float>(currentFrame, imagerawdata, frameResolution, d->params.alpha);
                    break;
                default:
                    break;
                }
            }

            const uint16_t frameTick = [&]() {
                if (!(isImageAnim || reader.imageCount() > 0) || !reader.canRead()) { // can't read == end of animation or just a single frame
                    return ind.frameDuration;
                } else if (reader.nextImageDelay() == 0) {
                    return static_cast<uint16_t>(0);
                } else {
                    return static_cast<uint16_t>(
                        qRound(qMax(static_cast<float>(reader.nextImageDelay()) / d->params.frameTimeMs, 1.0)));
                }
            }();

            JxlEncoderInitFrameHeader(frameHeader.get());
            frameHeader->duration = (d->params.animation) ? frameTick : 0;
            if (ind.isRefFrame && !referenceSaved) {
                frameHeader->layer_info.save_as_reference = 1;
                referenceSaved = true;
            }
            frameHeader->layer_info.blend_info.blendmode = ind.blendMode;
            if (d->params.alpha) {
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
            QString frameName(ind.frameName);
            if (reader.isJxl()) {
                const JxlFrameHeader hd = reader.getJxlFrameHeader();
                frameHeader->layer_info.blend_info = hd.layer_info.blend_info;
                frameHeader->layer_info.save_as_reference = hd.layer_info.save_as_reference;
                if (!reader.getFrameName().isEmpty()) {
                    if (frameName.isEmpty()) {
                        frameName += reader.getFrameName();
                    } else {
                        frameName += " - " + reader.getFrameName();
                    }
                    if (frameName.toUtf8().size() > 1071) {
                        frameName.truncate(1071);
                    }
                }
            }

            if (JxlEncoderSetFrameHeader(frameSettings, frameHeader.get()) != JXL_ENC_SUCCESS) {
                emit sigThrowError("JxlEncoderSetFrameHeader failed!");
                d->isAborted = true;
                return;
            }

            if (!frameName.isEmpty() && frameName.toUtf8().size() <= 1071) {
                if (JxlEncoderSetFrameName(frameSettings, frameName.toUtf8()) != JXL_ENC_SUCCESS) {
                    emit sigThrowError("JxlEncoderSetFrameName failed!");
                    d->isAborted = true;
                    return;
                }
                // in case warning is needed
                // } else if (ind.frameName.toUtf8().size() > 1071) {
                //     QMessageBox::warning(this,
                //                          "Warning",
                //                          QString("Cannot write name for frame %1, name exceeds 1071 bytes
                //                          limit!\n(Current: %2 bytes)")
                //                              .arg(QString::number(i + 1),
                //                              QString::number(ind.frameName.toUtf8().size())));
            }

            if (JxlEncoderAddImageFrame(frameSettings, &pixelFormat, imagerawdata.constData(), imagerawdata.size())
                != JXL_ENC_SUCCESS) {
                emit sigThrowError("JxlEncoderAddImageFrame failed!");
                d->isAborted = true;
                return;
            }

            const double currentImageSizeKiB = [&]() {
#ifdef USE_STREAMING_OUTPUT
                return static_cast<double>(outProcessor.finalized_position) / 1024.0;
#else
                return 0.0;
#endif
            }();

            if (isImageAnim || reader.imageCount() > 1) {
                emit sigStatusText(QString("Processing frame %1 of %2 (Subframe %3 of %4) | Output file size: %5 KiB")
                                       .arg(QString::number(i + 1),
                                            QString::number(framenum),
                                            QString::number(imageframenum + 1),
                                            QString::number(reader.imageCount()),
                                            QString::number(currentImageSizeKiB)));
                emit sigCurrentSubProgressBar(imageframenum + 1);
            } else {
                emit sigStatusText(
                    QString("Processing frame %1 of %2 | Output file size: %3 KiB")
                        .arg(QString::number(i + 1), QString::number(framenum), QString::number(currentImageSizeKiB)));
            }

            if (d->encodeAbort && d->abortCompleteFile) {
                JxlEncoderCloseInput(d->enc.get());
                JxlEncoderFlushInput(d->enc.get());
                const double finalAbortImageSizeKiB = [&]() {
#ifdef USE_STREAMING_OUTPUT
                    return static_cast<double>(outProcessor.finalized_position) / 1024.0;
#else
                    return 0.0;
#endif
                }();
                emit sigCurrentMainProgressBar(i + 1, true);
                emit sigEnableSubProgressBar(false, 0);
#ifdef USE_STREAMING_OUTPUT
                emit sigStatusText(QString("Encode aborted! Outputting partial image | Final output file size: %1 KiB")
                                       .arg(QString::number(finalAbortImageSizeKiB)));
                d->isAborted = true;
                return;
#else
                emit sigStatusText("Encode aborted!");
                d->isAborted = true;
                return;
#endif
            }

            if (i == framenum - 1 && !reader.canRead()) {
                JxlEncoderCloseInput(d->enc.get());
            }
#ifdef USE_STREAMING_OUTPUT
            JxlEncoderFlushInput(d->enc.get());
#endif
            imageframenum++;
        }
        emit sigCurrentMainProgressBar(i + 1, true);
        emit sigEnableSubProgressBar(false, 0);
    }

#ifndef USE_STREAMING_OUTPUT
    QFile outF(d->params.outputFileName);
    outF.open(QIODevice::WriteOnly);
    if (!outF.isWritable()) {
        outF.close();
        emit sigThrowError("Encode failed: Cannot write to output file!");
        d->isAborted = true;
        return;
    }

    QByteArray compressed(16384, 0x0);
    auto *nextOut = reinterpret_cast<uint8_t *>(compressed.data());
    auto availOut = static_cast<size_t>(compressed.size());
    auto result = JXL_ENC_NEED_MORE_OUTPUT;
    while (result == JXL_ENC_NEED_MORE_OUTPUT) {
        result = JxlEncoderProcessOutput(d->enc.get(), &nextOut, &availOut);
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
        emit sigThrowError("JxlEncoderProcessOutput failed!");
        d->isAborted = true;
        return;
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
        return static_cast<double>(QFileInfo(d->params.outputFileName).size()) / 1024.0;
#endif
    }();

    d->idat.clear();

    emit sigStatusText(
        QString("Encode successful | Final output file size: %1 KiB").arg(QString::number(finalImageSizeKiB)));
    d->isAborted = false;
    return;
}
