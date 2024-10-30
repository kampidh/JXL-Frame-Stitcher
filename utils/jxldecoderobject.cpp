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

// #define JXL_DECODER_QDEBUG

/* this chunked file loading size is completely arbitrary,
 * define as many as you need.
 */
// metadata loading, read 16KB per chunk
#define METADATA_FILE_CHUNK_SIZE 16384
// frame loading, read 4MB per chunk
#define FRAME_FILE_CHUNK_SIZE 4194304

class Q_DECL_HIDDEN JXLDecoderObject::Private
{
public:
    bool isJxl{false};
    bool isDecodeable{true};
    bool isCMYK{false};
    bool jxlHasAnim{false};
    bool isLast{false};
    bool readingSet{false};
    bool oneShotDecode{false};
    double frameDurationMs{0.0};
    int rootWidth{0};
    int rootHeight{0};
    int numFrames{0};

    QSize rootSize{};
    QByteArray rootICC{};
    QRect currentRect{};
    QString errStr{};
    QString inputFileName{};
    QString inputFileSuffix{};
    QString frameName{};
    QStringList oneShotSuffixes{};

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

JXLDecoderObject::JXLDecoderObject()
    : d(new Private)
{
}

JXLDecoderObject::JXLDecoderObject(const QString &inputFilename)
    : d(new Private)
{
    setFileName(inputFilename);
}

JXLDecoderObject::~JXLDecoderObject()
{
    d.reset();
}

void JXLDecoderObject::setFileName(const QString &inputFilename)
{
    d->inputFileName = inputFilename;
    const QFileInfo fi(d->inputFileName);
    d->inputFileSuffix = fi.suffix().toLower();

    // some files can trigger infinite loop when calling canRead()...
    d->oneShotSuffixes = QStringList{
        "tif",
        "tiff"
    };

    if (d->inputFileSuffix == "jxl") {
        d->isJxl = true;
        resetJxlDecoder();
        d->jxlFile.setFileName(inputFilename);

        d->isDecodeable = decodeJxlMetadata();
    } else {
        d->reader.setFileName(inputFilename);
        d->isJxl = false;
    }
}

void JXLDecoderObject::resetJxlDecoder()
{
    if (d->jxlFile.isOpen()) {
        d->jxlFile.close();
    };
    if (!d->dec)
        d->dec = JxlDecoderMake(nullptr);
    if (!d->runner)
        d->runner = JxlResizableParallelRunnerMake(nullptr);

    JxlDecoderReset(d->dec.get());

    d->isCMYK = false;
    d->jxlHasAnim = false;
    d->isLast = false;
    d->readingSet = false;
    d->oneShotDecode = false;
    d->frameDurationMs = 0.0;
    d->rootWidth = 0;
    d->rootHeight = 0;
    d->numFrames = 0;
    d->rootSize = QSize();
    d->currentRect = QRect();
    d->errStr = QString();
    d->frameName = QString();

    d->rootICC.clear();
    d->jxlRawInputData.clear();
    d->m_rawData.clear();
}

bool JXLDecoderObject::isJxl()
{
    return d->isJxl;
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
    // if (((d->jxlFile.size() / 1024) / 1024) > QImageReader::allocationLimit()) {
    //     d->errStr = "Size too big";
    //     d->jxlFile.close();
    //     return false;
    // }

    d->jxlRawInputData = d->jxlFile.read(METADATA_FILE_CHUNK_SIZE);

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
        != JxlDecoderSubscribeEvents(d->dec.get(), JXL_DEC_BASIC_INFO | JXL_DEC_COLOR_ENCODING | JXL_DEC_FRAME)) {
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
    if (JXL_DEC_SUCCESS != JxlDecoderSetDecompressBoxes(d->dec.get(), JXL_TRUE)) {
        d->errStr = "JxlDecoderSetDecompressBoxes failed";
        return false;
    };

    if (JXL_DEC_SUCCESS != JxlDecoderSetRenderSpotcolors(d->dec.get(), JXL_TRUE)) {
        d->errStr = "JxlDecoderSetRenderSpotcolors failed";
        return false;
    };
    if (JXL_DEC_SUCCESS != JxlDecoderSetCoalescing(d->dec.get(), d->params.coalesceJxlInput ? JXL_TRUE : JXL_FALSE)) {
        d->errStr = "JxlDecoderSetCoalescing failed";
        return false;
    };

    for(;;) {
#ifdef JXL_DECODER_QDEBUG
        qDebug() << "---";
#endif
        JxlDecoderStatus status = JxlDecoderProcessInput(d->dec.get());
#ifdef JXL_DECODER_QDEBUG
        qDebug() << "status:" << Qt::hex << status;
#endif

        if (status == JXL_DEC_ERROR) {
            d->errStr = "Decoder error";
            return false;
        } else if (status == JXL_DEC_NEED_MORE_INPUT) {
            if (d->jxlFile.atEnd()) {
                d->jxlFile.close();
                JxlDecoderCloseInput(d->dec.get());
                JxlDecoderReleaseInput(d->dec.get());
                d->errStr = "Error, already provided all input";
                return false;
            }
            JxlDecoderReleaseInput(d->dec.get());
            d->jxlRawInputData = d->jxlFile.read(METADATA_FILE_CHUNK_SIZE);
            if (JXL_DEC_SUCCESS
                != JxlDecoderSetInput(d->dec.get(),
                                      reinterpret_cast<const uint8_t *>(d->jxlRawInputData.constData()),
                                      static_cast<size_t>(d->jxlRawInputData.size()))) {
                d->errStr = "JxlDecoderSetInput failed";
                return false;
            };
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
#ifdef JXL_DECODER_QDEBUG
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
#endif
            const uint32_t numthreads = [&]() {
                return JxlResizableParallelRunnerSuggestThreads(d->m_info.xsize, d->m_info.ysize);
            }();
#ifdef JXL_DECODER_QDEBUG
            qDebug() << "Threads set:" << numthreads;
#endif
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
        } else if (status == JXL_DEC_FRAME) {
            d->numFrames++;
        } else if (status == JXL_DEC_SUCCESS) {
            d->jxlFile.close();
            JxlDecoderCloseInput(d->dec.get());
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

QSize JXLDecoderObject::getRootFrameSize() const
{
    if (!d->isJxl) {
        return d->reader.size();
    } else if (d->isJxl) {
        return d->rootSize;
    }
    return QSize();
}

QByteArray JXLDecoderObject::getIccProfie() const
{
    if (!d->isJxl) {
        return QImage(d->inputFileName).colorSpace().iccProfile();
    } else if (d->isJxl) {
        return d->rootICC;
    }
    return QByteArray();
}

QSize JXLDecoderObject::size() const
{
    if (!d->isJxl) {
        return d->reader.size();
    } else if (d->isJxl) {
        return d->rootSize;
    }
    return QSize();
}

int JXLDecoderObject::imageCount() const
{
    if (!d->isJxl) {
        return d->reader.imageCount();
    } else if (d->isJxl) {
        return d->numFrames;
    }
    return 1;
}

int JXLDecoderObject::nextImageDelay() const
{
    if (!d->isJxl) {
        return d->reader.nextImageDelay();
    } else if (d->isJxl) {
        return static_cast<int>(d->frameDurationMs * d->m_header.duration);
    }
    return (haveAnimation() ? 1 : 0);
}

bool JXLDecoderObject::haveAnimation() const
{
    if (!d->isJxl) {
        return (d->reader.imageCount() > 1 && d->reader.supportsAnimation());
    } else if (d->isJxl) {
        return d->jxlHasAnim;
    }
    return false;
}

bool JXLDecoderObject::canRead() const
{
    if (!d->isJxl) {
        if (d->oneShotDecode) {
            return false;
        }
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
        if (d->oneShotSuffixes.contains(d->inputFileSuffix)) {
            d->oneShotDecode = true;
        }
        return d->reader.read();
    } else if (d->isJxl) {
        // read full image and frame one by one
        if (d->readingSet) {
            if (!d->jxlFile.open(QIODevice::ReadOnly)) {
                d->errStr = "Failed to open input jxl";
                return QImage();
            }
            d->jxlRawInputData.clear();
            d->jxlRawInputData = d->jxlFile.read(FRAME_FILE_CHUNK_SIZE);

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
            // JxlDecoderCloseInput(d->dec.get());
            if (JXL_DEC_SUCCESS != JxlDecoderSetDecompressBoxes(d->dec.get(), JXL_TRUE)) {
                d->errStr = "JxlDecoderSetDecompressBoxes failed";
                return QImage();
            };

            if (JXL_DEC_SUCCESS != JxlDecoderSetRenderSpotcolors(d->dec.get(), JXL_TRUE)) {
                d->errStr = "JxlDecoderSetRenderSpotcolors failed";
                return QImage();
            };
            if (JXL_DEC_SUCCESS != JxlDecoderSetCoalescing(d->dec.get(), d->params.coalesceJxlInput ? JXL_TRUE : JXL_FALSE)) {
                d->errStr = "JxlDecoderSetCoalescing failed";
                return QImage();
            };
            const uint32_t numthreads = [&]() {
                return JxlResizableParallelRunnerSuggestThreads(d->m_info.xsize, d->m_info.ysize);
            }();
            JxlResizableParallelRunnerSetThreads(d->runner.get(), numthreads);
            d->readingSet = false;
        }

        d->m_rawData.clear();

        bool decodeSuccess = false;
        for(;;) {
#ifdef JXL_DECODER_QDEBUG
            qDebug() << "---";
#endif
            JxlDecoderStatus status = JxlDecoderProcessInput(d->dec.get());
#ifdef JXL_DECODER_QDEBUG
            qDebug() << "status:" << Qt::hex << status;
#endif

            if (status == JXL_DEC_ERROR) {
                d->errStr = "Decoder error";
                break;
            } else if (status == JXL_DEC_NEED_MORE_INPUT) {
                if (d->jxlFile.atEnd()) {
                    d->jxlFile.close();
                    JxlDecoderCloseInput(d->dec.get());
                    JxlDecoderReleaseInput(d->dec.get());
                    d->errStr = "Error, already provided all input";
                    break;
                }
                JxlDecoderReleaseInput(d->dec.get());
                d->jxlRawInputData = d->jxlFile.read(FRAME_FILE_CHUNK_SIZE);
                if (JXL_DEC_SUCCESS
                    != JxlDecoderSetInput(d->dec.get(),
                                          reinterpret_cast<const uint8_t *>(d->jxlRawInputData.constData()),
                                          static_cast<size_t>(d->jxlRawInputData.size()))) {
                    d->errStr = "JxlDecoderSetInput failed";
                    break;
                };
            }  else if (status == JXL_DEC_NEED_IMAGE_OUT_BUFFER) {
                size_t rawSize = 0;
                if (JXL_DEC_SUCCESS != JxlDecoderImageOutBufferSize(d->dec.get(), &d->m_pixelFormat, &rawSize)) {
                    d->errStr = "JxlDecoderImageOutBufferSize failed";
                    break;
                }
                d->m_rawData.resize(static_cast<int>(rawSize));
                if (JXL_DEC_SUCCESS
                    != JxlDecoderSetImageOutBuffer(d->dec.get(),
                                                   &d->m_pixelFormat,
                                                   reinterpret_cast<uint8_t *>(d->m_rawData.data()),
                                                   static_cast<size_t>(d->m_rawData.size()))) {
                    d->errStr = "JxlDecoderSetImageOutBuffer failed";
                    break;
                }
            } else if (status == JXL_DEC_FRAME) {
                if (JXL_DEC_SUCCESS != JxlDecoderGetFrameHeader(d->dec.get(), &d->m_header)) {
                    d->errStr = "JxlDecoderGetFrameHeader failed";
                    break;
                }
                d->isLast = (d->m_header.is_last == JXL_TRUE);

                const uint32_t nameLength = d->m_header.name_length + 1;
                if (nameLength > 0) {
                    QByteArray rawFrameName(nameLength, 0x0);
                    if (JXL_DEC_SUCCESS != JxlDecoderGetFrameName(d->dec.get(), rawFrameName.data(), nameLength)) {
                        d->errStr = "JxlDecoderGetFrameName failed";
                        break;
                    }
                    d->frameName = QString::fromUtf8(rawFrameName);
                } else {
                    d->frameName = QString();
                }
            } else if (status == JXL_DEC_FULL_IMAGE) {
                if (!d->isLast) {
                    decodeSuccess = true;
                    break;
                }
            } else if (status == JXL_DEC_SUCCESS && d->isLast) {
                d->jxlFile.close();
                JxlDecoderCloseInput(d->dec.get());
                JxlDecoderReleaseInput(d->dec.get());
                decodeSuccess = true;
                break;
            }
        }

        if (!decodeSuccess) {
            d->jxlFile.close();
            JxlDecoderCloseInput(d->dec.get());
            JxlDecoderReleaseInput(d->dec.get());
            return QImage();
        }

        d->currentRect = QRect(static_cast<int>(d->m_header.layer_info.crop_x0),
                               static_cast<int>(d->m_header.layer_info.crop_y0),
                               static_cast<int>(d->m_header.layer_info.xsize),
                               static_cast<int>(d->m_header.layer_info.ysize));

        const QImage::Format fmt = [&]() {
            switch (d->params.bitDepth) {
            case ENC_BIT_8:
                return QImage::Format_RGBA8888;
                break;
            case ENC_BIT_16:
                return QImage::Format_RGBA64;
                break;
            case ENC_BIT_16F:
                return QImage::Format_RGBA16FPx4;
                break;
            case ENC_BIT_32F:
                return QImage::Format_RGBA32FPx4;
                break;
            default:
                break;
            }
            return QImage::Format_RGBA8888;
        }();

        QImage buff(d->currentRect.width(), d->currentRect.height(), fmt);

        buff.setColorSpace(QColorSpace::fromIccProfile(d->rootICC));
        memcpy(buff.bits(), d->m_rawData.constData(), d->m_rawData.size());
        d->m_rawData.clear();

        return buff;
    }
    return QImage();
}

JxlFrameHeader JXLDecoderObject::getJxlFrameHeader() const
{
    return d->m_header;
}

QString JXLDecoderObject::getFrameName() const
{
    return d->frameName;
}

QString JXLDecoderObject::errorString() const
{
    if (!d->isJxl) {
        if (d->jxlFile.isOpen()) {
            if (d->jxlFile.isOpen()) {
                d->jxlFile.close();
            };
        }
        return d->reader.errorString();
    } else if (d->isJxl) {
        return d->errStr;
    }
    return QString();
}

QRect JXLDecoderObject::currentImageRect() const
{
    if (!d->isJxl) {
        return d->reader.currentImageRect();
    } else if (d->isJxl) {
        return d->currentRect;
    }
    return QRect();
}
