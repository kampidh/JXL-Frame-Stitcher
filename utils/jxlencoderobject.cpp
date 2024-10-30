#include "jxlencoderobject.h"
#include "jxldecoderobject.h"

#include <QColorSpace>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QImageReader>

#include <jxl/color_encoding.h>
#include <jxl/encode_cxx.h>
#include <jxl/resizable_parallel_runner_cxx.h>

#define USE_STREAMING_OUTPUT // need libjxl >= 0.10.0

// let's disable temp file for now (set to never trigger max raw frame size)
#define TEMP_FILE_DIR "./tempframe.bin"
#define MAX_DECODED_BEFORE_TEMPFILE SIZE_MAX

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

    QImage prevFrame;

    QElapsedTimer elt;
    quint64 totalFramesProcessed{0};
    double totalAccumulatedMpps{0.0};
    double totalAccumulatedDecMpps{0.0};

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
    d->isAborted = false;
    d->encodeAbort = false;
    d->abortCompleteFile = true;
    d->idat.clear();
    d->totalFramesProcessed = 0;
    d->totalAccumulatedMpps = 0.0;
    d->totalAccumulatedDecMpps = 0.0;
    d->prevFrame = QImage();
    d->elt.invalidate();

    if (!d->enc || !d->runner) {
        return false;
    }
    JxlEncoderReset(d->enc.get());

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

bool JXLEncoderObject::canEncode()
{
    if (d->idat.isEmpty()) {
        return false;
    }

    emit sigStatusText("Parsing first image information...");
    QCoreApplication::processEvents();

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
        if (!cdec.canRead()) {
            return false;
        }
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
    doEncode();
    cleanupEncoder();
    resetEncoder();
}

bool JXLEncoderObject::doEncode()
{
    if (d->idat.isEmpty()) {
        d->isAborted = true;
        return false;
    }

    emit sigStatusText("Begin encoding...");

#ifdef USE_STREAMING_OUTPUT
    jxfrstch::JxlOutputProcessor outProcessor;
    if (!outProcessor.SetOutputPath(d->params.outputFileName)) {
        emit sigThrowError("Failed to create output file!");
        d->isAborted = true;
        return false;
    }
#endif

    if (JXL_ENC_SUCCESS != JxlEncoderSetParallelRunner(d->enc.get(), JxlResizableParallelRunner, d->runner.get())) {
        emit sigThrowError("JxlEncoderSetParallelRunner failed!");
        d->isAborted = true;
        return false;
    }

    JxlResizableParallelRunnerSetThreads(
        d->runner.get(),
        JxlResizableParallelRunnerSuggestThreads(static_cast<uint64_t>(d->rootSize.width()),
                                                 static_cast<uint64_t>(d->rootSize.height())));

#ifdef USE_STREAMING_OUTPUT
    if (JXL_ENC_SUCCESS != JxlEncoderSetOutputProcessor(d->enc.get(), outProcessor.GetOutputProcessor())) {
        emit sigThrowError("JxlEncoderSetOutputProcessor failed!");
        d->isAborted = true;
        return false;
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
        return false;
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
        return false;
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
            return false;
        }
    } else if (d->params.colorSpace == ENC_CS_INHERIT_FIRST && !d->rootICC.isEmpty()) {
        if (JXL_ENC_SUCCESS
            != JxlEncoderSetICCProfile(d->enc.get(),
                                       reinterpret_cast<const uint8_t *>(d->rootICC.constData()),
                                       static_cast<size_t>(d->rootICC.size()))) {
            emit sigThrowError("JxlEncoderSetICCProfile failed!");
            d->isAborted = true;
            return false;
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
            return false;
        }

        if (d->params.photonNoise > 0.0) {
            if (JxlEncoderFrameSettingsSetFloatOption(frameSettings,
                                                      JXL_ENC_FRAME_SETTING_PHOTON_NOISE,
                                                      static_cast<float>(d->params.photonNoise))
                != JXL_ENC_SUCCESS) {
                qDebug() << "JxlEncoderFrameSettingsSetFloatOption photon noise failed";
                emit sigThrowError("JxlEncoderFrameSettings photon noise failed!");
                d->isAborted = true;
                return false;
            }
        }
    }

    auto frameHeader = std::make_unique<JxlFrameHeader>();

    const int framenum = d->idat.size();
    JXLDecoderObject reader;
    reader.resetJxlDecoder();
    reader.setEncodeParams(d->params);

    bool acResetFrame = true;
    for (int i = 0; i < framenum; i++) {
        if (d->encodeAbort && !d->abortCompleteFile) {
            emit sigCurrentMainProgressBar(i, true);
            emit sigEnableSubProgressBar(false, 0);
            emit sigStatusText("Encode aborted!");
            d->isAborted = true;
            return false;
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
            bool isMassive = false;
            QSize frameSize;
            size_t frameResolution;
            d->elt.start();
            // QImage cFrame;
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
            {
                QImage currentFrame(reader.read());
                QRect currentFrameRect = reader.currentImageRect();
                if (!currentFrameRect.isValid()) {
                    currentFrameRect = currentFrame.rect();
                }

                if (currentFrame.isNull()) {
                    emit sigThrowError(reader.errorString());
                    d->isAborted = true;
                    return false;
                }

                const size_t uncropSize =
                    static_cast<size_t>(currentFrame.width()) * static_cast<size_t>(currentFrame.height());

                if (d->params.autoCropFrame && uncropSize < 50'000'000) {
                    if ((isImageAnim && imageframenum == 0) || (!isImageAnim && i == 0)) {
                        acResetFrame = true;
                        d->prevFrame = currentFrame;
                    } else {
                        /* In short:
                         * Compare 2 QImages and get a QRect where they have differences
                         */
                        QRect cropRect;
                        if (d->prevFrame.size() == currentFrame.size()
                            && d->prevFrame.sizeInBytes() == currentFrame.sizeInBytes()) {
                            QPoint topLeft(currentFrameRect.bottomRight());
                            QPoint bottomRight(0, 0);

                            for (int h = 0; h < currentFrame.height(); h++) {
                                for (int w = 0; w < currentFrame.width(); w++) {
                                    const QPoint cpos(w, h);
                                    const QColor currentPix = currentFrame.pixelColor(cpos);
                                    const QColor prevPix = d->prevFrame.pixelColor(cpos);
                                    const float fuzzycomparison = d->params.autoCropFuzzyComparison;
                                    const bool fuzzy = [&]() {
                                        if (fuzzycomparison > 0.0) {
                                            if (qAbs(currentPix.redF() - prevPix.redF()) > fuzzycomparison)
                                                return true;
                                            if (qAbs(currentPix.greenF() - prevPix.greenF()) > fuzzycomparison)
                                                return true;
                                            if (qAbs(currentPix.blueF() - prevPix.blueF()) > fuzzycomparison)
                                                return true;
                                            if (qAbs(currentPix.alphaF() - prevPix.alphaF()) > fuzzycomparison)
                                                return true;
                                            return false;
                                        } else {
                                            return currentPix != prevPix;
                                        }
                                    }();

                                    if (fuzzy) {
                                        topLeft.setX(qMin(w, topLeft.x()));
                                        topLeft.setY(qMin(h, topLeft.y()));
                                        bottomRight.setX(qMax(w, bottomRight.x()));
                                        bottomRight.setY(qMax(h, bottomRight.y()));
                                    }
                                }
                            }

                            if ((topLeft.x() >= currentFrame.width() - 1 || topLeft.y() >= currentFrame.height() - 1)
                                || (bottomRight.x() < 1 || bottomRight.y() < 1)) {
                                cropRect = QRect(0, 0, 1, 1);
                            } else {
                                cropRect = QRect(topLeft.x(),
                                                 topLeft.y(),
                                                 bottomRight.x() - topLeft.x() + 1,
                                                 bottomRight.y() - topLeft.y() + 1);
                            }

                            if (cropRect != QRect(0, 0, currentFrame.width(), currentFrame.height())) {
                                acResetFrame = false;
                                if (cropRect != QRect(0, 0, 1, 1)) {
                                    currentFrame = currentFrame.copy(cropRect);
                                } else {
                                    /* Fill with single, offscreen transparent pixel if no movement is detected
                                     * Ideally this frame should be skipped and the frame before should be set
                                     * with the correct tick (1+n of skipped frames)
                                     */
                                    currentFrame = QImage(1, 1, currentFrame.format());
                                    currentFrame.fill(Qt::transparent);
                                    topLeft = QPoint(-1, -1);
                                }
                                const QPoint absTopLeft = currentFrameRect.topLeft() + topLeft;
                                currentFrameRect = currentFrame.rect();
                                currentFrameRect.moveTopLeft(absTopLeft);
                            } else {
                                acResetFrame = true;
                                d->prevFrame = currentFrame;
                            }
                        } else {
                            acResetFrame = true;
                            d->prevFrame = currentFrame;
                        }
                    }
                }

                if ((currentFrame.width() != d->rootSize.width() || currentFrame.height() != d->rootSize.height())
                    || ((frameXPos != 0 || frameYPos != 0) && i > 0)) {
                    needCrop = true;
                }
                if (((currentFrameRect.x() != 0 || currentFrameRect.y() != 0) && imageframenum > 0) || !acResetFrame) {
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
                    return false;
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
                frameResolution = static_cast<size_t>(frameSize.width()) * static_cast<size_t>(frameSize.height());
                // qDebug() << "pxsize" << frameResolution;

                const size_t neededBytes = ((d->params.alpha) ? 4 : 3) * byteSize * frameResolution;
                isMassive = (neededBytes > MAX_DECODED_BEFORE_TEMPFILE) && d->params.chunkedFrame;
                // isMassive = true;
                // qDebug() << "bytes" << neededBytes;
                // imagerawdata.resize(neededBytes, 0x0);

                QFile tempFrameFile(TEMP_FILE_DIR);
                QDataStream ds = [&]() {
                    if (isMassive) {
                        emit sigStatusText("Input image too large, saving intermediate to disk...");
                        // qDebug() << "tempfile path";
                        tempFrameFile.open(QIODevice::WriteOnly);
                        return QDataStream(&tempFrameFile);
                    } else {
                        // qDebug() << "memory path";
                        return QDataStream(&imagerawdata, QIODevice::WriteOnly);
                    }
                }();

                switch (d->params.bitDepth) {
                case ENC_BIT_8:
                    jxfrstch::QImageToBuffer<uint8_t>(currentFrame, ds, frameResolution, d->params.alpha);
                    break;
                case ENC_BIT_16:
                    jxfrstch::QImageToBuffer<uint16_t>(currentFrame, ds, frameResolution, d->params.alpha);
                    break;
                case ENC_BIT_16F:
                    jxfrstch::QImageToBuffer<qfloat16>(currentFrame, ds, frameResolution, d->params.alpha);
                    break;
                case ENC_BIT_32F:
                    jxfrstch::QImageToBuffer<float>(currentFrame, ds, frameResolution, d->params.alpha);
                    break;
                default:
                    break;
                }

                if (isMassive) {
                    tempFrameFile.close();
                }

                // qDebug() << "Pixel allocated";
            }

            const uint32_t frameTick = [&]() {
                // what the f-
                if (!(isImageAnim || reader.imageCount() > 0)
                    || !reader.canRead()) { // can't read == end of animation or just a single frame
                    if (isImageAnim) { // if it's end and the image is animated, set to 0
                        return static_cast<uint32_t>(0);
                    }
                    return ind.isPageEnd ? UINT32_MAX : ind.frameDuration; // otherwise set the frame duration
                } else if (reader.nextImageDelay() == 0 || !d->params.animation) {
                    return static_cast<uint32_t>(0);
                } else {
                    return static_cast<uint32_t>(
                        qRound(qMax(static_cast<float>(reader.nextImageDelay()) / d->params.frameTimeMs, 1.0)));
                }
            }();

            JxlEncoderInitFrameHeader(frameHeader.get());
            frameHeader->duration = frameTick;
            frameHeader->layer_info.save_as_reference = static_cast<uint32_t>(ind.isRefFrame);
            frameHeader->layer_info.blend_info.blendmode = ind.blendMode;
            if (d->params.alpha) {
                frameHeader->layer_info.blend_info.alpha = 0;
            }
            frameHeader->layer_info.blend_info.source = static_cast<uint32_t>(ind.frameReference);
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

            if (d->params.autoCropFrame) {
                if (acResetFrame) {
                    frameHeader->layer_info.save_as_reference = 1;
                }
                if (needCrop && !acResetFrame) {
                    frameHeader->layer_info.blend_info.blendmode = JXL_BLEND_BLEND;
                    frameHeader->layer_info.blend_info.source = 1;
                }
            }

            if (JxlEncoderSetFrameHeader(frameSettings, frameHeader.get()) != JXL_ENC_SUCCESS) {
                emit sigThrowError("JxlEncoderSetFrameHeader failed!");
                d->isAborted = true;
                return false;
            }

            if (!frameName.isEmpty() && frameName.toUtf8().size() <= 1071) {
                if (JxlEncoderSetFrameName(frameSettings, frameName.toUtf8()) != JXL_ENC_SUCCESS) {
                    emit sigThrowError("JxlEncoderSetFrameName failed!");
                    d->isAborted = true;
                    return false;
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

            const qint64 decodeNs = d->elt.nsecsElapsed();

            if (!d->params.chunkedFrame) {
                if (JxlEncoderAddImageFrame(frameSettings, &pixelFormat, imagerawdata.constData(), imagerawdata.size())
                    != JXL_ENC_SUCCESS) {
                    emit sigThrowError("JxlEncoderAddImageFrame failed!");
                    d->isAborted = true;
                    return false;
                }
            } else {
                jxfrstch::ChunkedImageFrame ifrm(pixelFormat, byteSize, frameSize);
                QFile tmp(TEMP_FILE_DIR);
                if (isMassive) {
                    tmp.open(QIODevice::ReadOnly);
                    ifrm.inputData(&tmp);
                } else {
                    ifrm.inputData(&imagerawdata);
                }

                if (JxlEncoderAddChunkedFrame(frameSettings,
                                              TO_JXL_BOOL(i == framenum - 1 && !reader.canRead()),
                                              ifrm.getChunkedStruct())
                    != JXL_ENC_SUCCESS) {
                    emit sigThrowError("JxlEncoderAddChunkedFrame failed!");
                    d->isAborted = true;
                    return false;
                }
                if (isMassive) {
                    tmp.close();
                    tmp.remove();
                }
            }

            bool isMb = false;

            const double currentImageSizeKiB = [&]() {
#ifdef USE_STREAMING_OUTPUT
                if (outProcessor.finalized_position > (1024 * 1024 * 10)) {
                    isMb = true;
                    return static_cast<double>(outProcessor.finalized_position) / 1024.0 / 1024.0;
                } else {
                    isMb = false;
                    return static_cast<double>(outProcessor.finalized_position) / 1024.0;
                }
#else
                return 0.0;
#endif
            }();

            if (isImageAnim || reader.imageCount() > 1) {
                emit sigStatusText(QString("Processing frame %1 of %2 (Subframe %3 of %4) | Output file size: %5 %6")
                                       .arg(QString::number(i + 1),
                                            QString::number(framenum),
                                            QString::number(imageframenum + 1),
                                            QString::number(reader.imageCount()),
                                            QString::number(currentImageSizeKiB),
                                            isMb ? "MiB" : "KiB"));
                emit sigCurrentSubProgressBar(imageframenum + 1);
            } else {
                emit sigStatusText(QString("Processing frame %1 of %2 | Output file size: %3 %4")
                                       .arg(QString::number(i + 1),
                                            QString::number(framenum),
                                            QString::number(currentImageSizeKiB),
                                            isMb ? "MiB" : "KiB"));
            }

            d->totalFramesProcessed++;

            if (d->encodeAbort && d->abortCompleteFile) {
                if (!d->params.chunkedFrame) {
                    JxlEncoderCloseInput(d->enc.get());
                    JxlEncoderFlushInput(d->enc.get());
                }
                const double finalAbortImageSizeKiB = [&]() {
#ifdef USE_STREAMING_OUTPUT
                    if (outProcessor.finalized_position > (1024 * 1024 * 10)) {
                        isMb = true;
                        return static_cast<double>(outProcessor.finalized_position) / 1024.0 / 1024.0;
                    } else {
                        isMb = false;
                        return static_cast<double>(outProcessor.finalized_position) / 1024.0;
                    }
#else
                    return 0.0;
#endif
                }();
                emit sigCurrentMainProgressBar(i + 1, true);
                emit sigEnableSubProgressBar(false, 0);
#ifdef USE_STREAMING_OUTPUT
                emit sigStatusText(QString("Encode aborted! Outputting partial image | Final output file size: %1 %2")
                                       .arg(QString::number(finalAbortImageSizeKiB), isMb ? "MiB" : "KiB"));
                emit sigSpeedStats(
                    QString("%1 frame(s) processed | Dec: %2 MP/s | Enc: %3 MP/s")
                        .arg(QString::number(d->totalFramesProcessed),
                             QString::number(d->totalAccumulatedDecMpps / static_cast<double>(d->totalFramesProcessed),
                                             'g',
                                             4),
                             QString::number(d->totalAccumulatedMpps / static_cast<double>(d->totalFramesProcessed),
                                             'g',
                                             4)));
                d->isAborted = true;
                return false;
#else
                emit sigStatusText("Encode aborted!");
                d->isAborted = true;
                return false;
#endif
            }

            if (i == framenum - 1 && !reader.canRead()) {
                if (!d->params.chunkedFrame) {
                    JxlEncoderCloseInput(d->enc.get());
                }
            }
#ifdef USE_STREAMING_OUTPUT
            if (!d->params.chunkedFrame) {
                JxlEncoderFlushInput(d->enc.get());
            }
#endif
            const qint64 encodeNs = d->elt.nsecsElapsed() - decodeNs;
            const double decNstoSec = static_cast<double>(decodeNs) / 1.0e9;
            const double encNstoSec = static_cast<double>(encodeNs) / 1.0e9;

            const double decmpps = [&]() {
                if (d->elt.isValid() && decNstoSec > 0) {
                    const size_t frameres = frameResolution;
                    return static_cast<double>((static_cast<double>(frameResolution) / 1000000.0) / decNstoSec);
                } else {
                    return 0.0;
                }
            }();

            const double mpps = [&]() {
                if (d->elt.isValid() && encNstoSec > 0) {
                    const size_t frameres = frameResolution;
                    return static_cast<double>((static_cast<double>(frameResolution) / 1000000.0) / encNstoSec);
                } else {
                    return 0.0;
                }
            }();

            d->totalAccumulatedMpps += mpps;
            d->totalAccumulatedDecMpps += decmpps;

            emit sigSpeedStats(QString("Dec: %1 MP/s | Enc: %2 MP/s")
                                   .arg(QString::number(decmpps, 'g', 4), QString::number(mpps, 'g', 4)));

            imageframenum++;
        }
        emit sigCurrentMainProgressBar(i + 1, true);
        emit sigEnableSubProgressBar(false, 0);
    }

    d->elt.invalidate();

#ifndef USE_STREAMING_OUTPUT
    QFile outF(d->params.outputFileName);
    outF.open(QIODevice::WriteOnly);
    if (!outF.isWritable()) {
        outF.close();
        emit sigThrowError("Encode failed: Cannot write to output file!");
        d->isAborted = true;
        return false;
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
        return false;
    }
    outF.close();
#endif

#ifdef USE_STREAMING_OUTPUT
    outProcessor.CloseOutputFile();
#endif
    bool isMb = false;
    const double finalImageSizeKiB = [&]() {
#ifdef USE_STREAMING_OUTPUT
        if (outProcessor.finalized_position > (1024 * 1024 * 10)) {
            isMb = true;
            return static_cast<double>(outProcessor.finalized_position) / 1024.0 / 1024.0;
        } else {
            isMb = false;
            return static_cast<double>(outProcessor.finalized_position) / 1024.0;
        }
#else
        if (QFileInfo(d->params.outputFileName).size() > (1024 * 1024 * 10)) {
            isMb = true;
            return static_cast<double>(QFileInfo(d->params.outputFileName).size()) / 1024.0 / 1024.0;
        } else {
            isMb = false;
            return static_cast<double>(QFileInfo(d->params.outputFileName).size()) / 1024.0;
        }
#endif
    }();

    d->idat.clear();

    emit sigStatusText(QString("Encode successful | Final output file size: %1 %2")
                           .arg(QString::number(finalImageSizeKiB), isMb ? "MiB" : "KiB"));
    emit sigSpeedStats(
        QString("%1 frame(s) processed | Dec: %2 MP/s | Enc: %3 MP/s")
            .arg(QString::number(d->totalFramesProcessed),
                 QString::number(d->totalAccumulatedDecMpps / static_cast<double>(d->totalFramesProcessed), 'g', 4),
                 QString::number(d->totalAccumulatedMpps / static_cast<double>(d->totalFramesProcessed), 'g', 4)));
    d->isAborted = false;
    return true;
}
