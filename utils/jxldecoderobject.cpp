#include "jxldecoderobject.h"

#include <QColorSpace>
#include <QDebug>
#include <QImage>
#include <QImageReader>
#include <QFile>
#include <QFileInfo>

#include <jxl/decode_cxx.h>
#include <jxl/color_encoding.h>
#include <jxl/resizable_parallel_runner_cxx.h>

class Q_DECL_HIDDEN JXLDecoderObject::Private
{
public:
    bool isJxl{false};
    bool isDecodeable{true};
    bool isCMYK{false};
    bool jxlHasAnim{false};
    bool isLast{false};
    bool readingSet{false};
    double frameDurationMs{0.0};
    int rootWidth{0};
    int rootHeight{0};
    int numFrames{1};
    QSize rootSize{};
    QByteArray rootICC{};
    QRect currentRect{};
    QString errStr{};
    QString inputFileName{};

    jxfrstch::EncodeParams params{};

    QImageReader reader;
    QFile jxlFile;

    JxlDecoderPtr dec;
    JxlResizableParallelRunnerPtr runner;
    QByteArray jxlRawInputData{};
    QByteArray m_rawData{};

    JxlBasicInfo m_info{};
    JxlExtraChannelInfo m_extra{};
    JxlPixelFormat m_pixelFormat{};
    JxlFrameHeader m_header{};
};

JXLDecoderObject::JXLDecoderObject(const QString &inputFilename)
    : d(new Private)
{
    d->inputFileName = inputFilename;
    const QFileInfo fi(d->inputFileName);
    if (fi.suffix().toLower() == "jxl") {
        d->jxlFile.setFileName(inputFilename);
        d->isJxl = true;
        d->dec = JxlDecoderMake(nullptr);
        d->runner = JxlResizableParallelRunnerMake(nullptr);
        d->isDecodeable = decodeJxlMetadata();
    } else {
        d->reader.setFileName(inputFilename);
        d->isJxl = false;
    }
}

JXLDecoderObject::~JXLDecoderObject()
{
    d.reset();
}

bool JXLDecoderObject::decodeJxlMetadata()
{
    // read only basic info and color encoding
    if (!d->dec || !d->runner) {
        d->errStr = "No dec or no runner";
        return false;
    }
    if (!d->jxlFile.open(QIODevice::ReadOnly)) {
        d->errStr = "Failed to open input jxl";
        return false;
    }
    if (((d->jxlFile.size() / 1024) / 1024) > QImageReader::allocationLimit()) {
        d->errStr = "Size too big";
        d->jxlFile.close();
        return false;
    }

    d->jxlRawInputData = d->jxlFile.readAll();

    const auto validation = JxlSignatureCheck(reinterpret_cast<const uint8_t *>(d->jxlRawInputData.constData()),
                                              static_cast<size_t>(d->jxlRawInputData.size()));

    switch (validation) {
    case JXL_SIG_NOT_ENOUGH_BYTES:
        d->errStr = "Failed magic byte validation, not enough data";
        return false;
    case JXL_SIG_INVALID:
        d->errStr = "Failed magic byte validation, incorrect format";
        return false;
    default:
        break;
    }

    if (JXL_DEC_SUCCESS
        != JxlDecoderSubscribeEvents(d->dec.get(), JXL_DEC_BASIC_INFO | JXL_DEC_COLOR_ENCODING)) {
        d->errStr = "JxlDecoderSubscribeEvents failed";
        return false;
    }
    if (JXL_DEC_SUCCESS != JxlDecoderSetParallelRunner(d->dec.get(), JxlResizableParallelRunner, d->runner.get())) {
        d->errStr = "JxlDecoderSetParallelRunner failed";
        return false;
    }

    if (JXL_DEC_SUCCESS
        != JxlDecoderSetInput(d->dec.get(),
                              reinterpret_cast<const uint8_t *>(d->jxlRawInputData.constData()),
                              static_cast<size_t>(d->jxlRawInputData.size()))) {
        d->errStr = "JxlDecoderSetInput failed";
        return false;
    };
    JxlDecoderCloseInput(d->dec.get());
    if (JXL_DEC_SUCCESS != JxlDecoderSetDecompressBoxes(d->dec.get(), JXL_TRUE)) {
        d->errStr = "JxlDecoderSetDecompressBoxes failed";
        return false;
    };

    for(;;) {
        // qDebug() << "---";
        JxlDecoderStatus status = JxlDecoderProcessInput(d->dec.get());
        // qDebug() << "status:" << Qt::hex << status;

        if (status == JXL_DEC_ERROR) {
            d->errStr = "Decoder error";
            return false;
        } else if (status == JXL_DEC_NEED_MORE_INPUT) {
            d->errStr = "Error, already provided all input";
            return false;
        } else if (status == JXL_DEC_BASIC_INFO) {
            if (JXL_DEC_SUCCESS != JxlDecoderGetBasicInfo(d->dec.get(), &d->m_info)) {
                d->errStr = "JxlDecoderGetBasicInfo failed";
                return false;
            }

            for (uint32_t i = 0; i < d->m_info.num_extra_channels; i++) {
                if (JXL_DEC_SUCCESS != JxlDecoderGetExtraChannelInfo(d->dec.get(), i, &d->m_extra)) {
                    d->errStr = "JxlDecoderGetExtraChannelInfo failed";
                    break;
                }
                if (d->m_extra.type == JXL_CHANNEL_BLACK) {
                    d->isCMYK = true;
                }
            }

            qDebug() << "Info";
            qDebug() << "Size:" << d->m_info.xsize << "x" << d->m_info.ysize;
            qDebug() << "Depth:" << d->m_info.bits_per_sample << d->m_info.exponent_bits_per_sample;
            qDebug() << "Number of channels:" << d->m_info.num_color_channels;
            qDebug() << "Has alpha" << d->m_info.num_extra_channels << d->m_info.alpha_bits
                     << d->m_info.alpha_exponent_bits;
            qDebug() << "Has animation:" << d->m_info.have_animation << "loops:" << d->m_info.animation.num_loops
                     << "tick:" << d->m_info.animation.tps_numerator << d->m_info.animation.tps_denominator;
            qDebug() << "Original profile?" << d->m_info.uses_original_profile;
            qDebug() << "Has preview?" << d->m_info.have_preview << d->m_info.preview.xsize << "x" << d->m_info.preview.ysize;

            const uint32_t numthreads = [&]() {
                return JxlResizableParallelRunnerSuggestThreads(d->m_info.xsize, d->m_info.ysize);
            }();
            qDebug() << "Threads set:" << numthreads;
            JxlResizableParallelRunnerSetThreads(d->runner.get(), numthreads);

            d->jxlHasAnim = (d->m_info.have_animation == JXL_TRUE);
            d->rootSize = QSize(d->m_info.xsize, d->m_info.ysize);
            d->rootWidth = d->rootSize.width();
            d->rootHeight = d->rootSize.height();
            d->currentRect = QRect(0, 0, d->rootSize.width(), d->rootSize.height());
            if (d->jxlHasAnim && d->m_info.animation.tps_numerator > 0) {
                d->frameDurationMs = (static_cast<double>(d->m_info.animation.tps_denominator * 1000)
                                    / static_cast<double>(d->m_info.animation.tps_numerator));
            }

            // d->m_pixelFormat.data_type = JXL_TYPE_FLOAT;
            switch (d->params.bitDepth) {
            case ENC_BIT_8:
                d->m_pixelFormat.data_type = JXL_TYPE_UINT8;
                break;
            case ENC_BIT_16:
                d->m_pixelFormat.data_type = JXL_TYPE_UINT16;
                break;
            case ENC_BIT_16F:
                d->m_pixelFormat.data_type = JXL_TYPE_FLOAT16;
                break;
            case ENC_BIT_32F:
                d->m_pixelFormat.data_type = JXL_TYPE_FLOAT;
                break;
            default:
                d->m_pixelFormat.data_type = JXL_TYPE_UINT8;
                break;
            }
            d->m_pixelFormat.num_channels = 4;
        } else if (status == JXL_DEC_COLOR_ENCODING) {
            size_t iccSize = 0;

            if (JXL_DEC_SUCCESS
                != JxlDecoderGetICCProfileSize(d->dec.get(),
#if JPEGXL_NUMERIC_VERSION < JPEGXL_COMPUTE_NUMERIC_VERSION(0, 9, 0)
                                               nullptr,
#endif
                                               JXL_COLOR_PROFILE_TARGET_DATA,
                                               &iccSize)) {
                d->errStr = "ICC profile size retrieval failed";
                return false;
            }

            d->rootICC.resize(static_cast<int>(iccSize));

            if (JXL_DEC_SUCCESS
                != JxlDecoderGetColorAsICCProfile(d->dec.get(),
#if JPEGXL_NUMERIC_VERSION < JPEGXL_COMPUTE_NUMERIC_VERSION(0, 9, 0)
                                                  nullptr,
#endif
                                                  JXL_COLOR_PROFILE_TARGET_DATA,
                                                  reinterpret_cast<uint8_t *>(d->rootICC.data()),
                                                  static_cast<size_t>(d->rootICC.size()))) {
                d->errStr = "JxlDecoderGetColorAsICCProfile failed";
                return false;
            }
        } else if (status == JXL_DEC_SUCCESS) {
            JxlDecoderReleaseInput(d->dec.get());
            break;
        }
    }

    JxlDecoderReset(d->dec.get());

    if (d->isCMYK) {
        return false;
    }

    d->readingSet = true;
    return true;
}

void JXLDecoderObject::setEncodeParams(const jxfrstch::EncodeParams &params)
{
    d->params = params;
}

QSize JXLDecoderObject::getRootFrameSize()
{
    if (!d->isJxl) {
        return d->reader.size();
    } else if (d->isJxl) {
        return d->rootSize;
    }
    return QSize();
}

QByteArray JXLDecoderObject::getIccProfie()
{
    if (!d->isJxl) {
        return QImage(d->inputFileName).colorSpace().iccProfile();
    } else if (d->isJxl) {
        return d->rootICC;
    }
    return QByteArray();
}

int JXLDecoderObject::imageCount()
{
    if (!d->isJxl) {
        return d->reader.imageCount();
    } else if (d->isJxl) {
        // unknown,
        return 0;
    }
    return 1;
}

int JXLDecoderObject::nextImageDelay()
{
    if (!d->isJxl) {
        return d->reader.nextImageDelay();
    } else if (d->isJxl) {
        return static_cast<int>(d->frameDurationMs * d->m_header.duration);
    }
    return (haveAnimation() ? 1 : 0);
}

bool JXLDecoderObject::haveAnimation()
{
    if (!d->isJxl) {
        return (d->reader.imageCount() > 1 && d->reader.supportsAnimation());
    } else if (d->isJxl) {
        return d->jxlHasAnim;
    }
    return false;
}

bool JXLDecoderObject::canRead()
{
    if (!d->isJxl) {
        return d->reader.canRead();
    } else if (d->isJxl) {
        if (!d->isDecodeable) {
            return false;
        }
        return !d->isLast;
    }
    return false;
}

QImage JXLDecoderObject::read()
{
    if (!d->isJxl) {
        return d->reader.read();
    } else if (d->isJxl) {
        // read full image and frame one by one
        if (d->readingSet) {
            if (JXL_DEC_SUCCESS
                != JxlDecoderSubscribeEvents(d->dec.get(), JXL_DEC_FULL_IMAGE | JXL_DEC_FRAME)) {
                d->errStr = "JxlDecoderSubscribeEvents failed";
                return QImage();
            }
            if (JXL_DEC_SUCCESS != JxlDecoderSetParallelRunner(d->dec.get(), JxlResizableParallelRunner, d->runner.get())) {
                d->errStr = "JxlDecoderSetParallelRunner failed";
                return QImage();
            }

            if (JXL_DEC_SUCCESS
                != JxlDecoderSetInput(d->dec.get(),
                                      reinterpret_cast<const uint8_t *>(d->jxlRawInputData.constData()),
                                      static_cast<size_t>(d->jxlRawInputData.size()))) {
                d->errStr = "JxlDecoderSetInput failed";
                return QImage();
            };
            JxlDecoderCloseInput(d->dec.get());
            if (JXL_DEC_SUCCESS != JxlDecoderSetDecompressBoxes(d->dec.get(), JXL_TRUE)) {
                d->errStr = "JxlDecoderSetDecompressBoxes failed";
                return QImage();
            };
            d->readingSet = false;
        }

        d->m_rawData.clear();

        for(;;) {
            JxlDecoderStatus status = JxlDecoderProcessInput(d->dec.get());

            if (status == JXL_DEC_ERROR) {
                d->errStr = "Decoder error";
                return QImage();
            } else if (status == JXL_DEC_NEED_MORE_INPUT) {
                d->errStr = "Error, already provided all input";
                return QImage();
            }  else if (status == JXL_DEC_NEED_IMAGE_OUT_BUFFER) {
                size_t rawSize = 0;
                if (JXL_DEC_SUCCESS != JxlDecoderImageOutBufferSize(d->dec.get(), &d->m_pixelFormat, &rawSize)) {
                    d->errStr = "JxlDecoderImageOutBufferSize failed";
                    return QImage();
                }
                d->m_rawData.resize(static_cast<int>(rawSize));
                if (JXL_DEC_SUCCESS
                    != JxlDecoderSetImageOutBuffer(d->dec.get(),
                                                   &d->m_pixelFormat,
                                                   reinterpret_cast<uint8_t *>(d->m_rawData.data()),
                                                   static_cast<size_t>(d->m_rawData.size()))) {
                    d->errStr = "JxlDecoderSetImageOutBuffer failed";
                    return QImage();
                }
            } else if (status == JXL_DEC_FRAME) {
                if (JXL_DEC_SUCCESS != JxlDecoderGetFrameHeader(d->dec.get(), &d->m_header)) {
                    d->errStr = "JxlDecoderGetFrameHeader failed";
                    return QImage();
                }
                d->isLast = (d->m_header.is_last == JXL_TRUE);
            } else if (status == JXL_DEC_FULL_IMAGE) {
                if (!d->isLast) {
                    break;
                }
            } else if (status == JXL_DEC_SUCCESS && d->isLast) {
                JxlDecoderReleaseInput(d->dec.get());
                break;
            }
        }

        QImage buff(d->rootSize, QImage::Format_RGBA8888);
        switch (d->params.bitDepth) {
        case ENC_BIT_8:
            buff.convertTo(QImage::Format_RGBA8888);
            break;
        case ENC_BIT_16:
            buff.convertTo(QImage::Format_RGBA64);
            break;
        case ENC_BIT_16F:
            buff.convertTo(QImage::Format_RGBA16FPx4);
            break;
        case ENC_BIT_32F:
            buff.convertTo(QImage::Format_RGBA32FPx4);
            break;
        default:
            break;
        }
        buff.setColorSpace(QColorSpace::fromIccProfile(d->rootICC));
        memcpy(buff.bits(), d->m_rawData.constData(), d->m_rawData.size());
        d->m_rawData.clear();

        return buff;
    }
    return QImage();
}

QString JXLDecoderObject::errorString()
{
    if (!d->isJxl) {
        return d->reader.errorString();
    } else if (d->isJxl) {
        return d->errStr;
    }
    return QString();
}

QRect JXLDecoderObject::currentImageRect()
{
    if (!d->isJxl) {
        return d->reader.currentImageRect();
    } else if (d->isJxl) {
        return d->currentRect;
    }
    return QRect();
}
