#ifndef JXLDECODEROBJECT_H
#define JXLDECODEROBJECT_H

#include "jxlutils.h"
#include <QString>

/*
 * A very simple wrapper for QImageReader to add support for decoding JPEG XL images with libjxl
 */
class JXLDecoderObject
{
public:
    JXLDecoderObject();
    JXLDecoderObject(const QString &inputFilename);
    ~JXLDecoderObject();

    void setEncodeParams(const jxfrstch::EncodeParams &params);
    void setFileName(const QString &inputFilename);

    bool isJxl();
    QImage read();
    void resetJxlDecoder();

    QSize size() const;
    int imageCount() const;
    bool haveAnimation() const;
    bool canRead() const;
    QString errorString() const;
    int nextImageDelay() const;
    QRect currentImageRect() const;
    JxlFrameHeader getJxlFrameHeader() const;
    QString getFrameName() const;
    QSize getRootFrameSize() const;
    QByteArray getIccProfie() const;

private:
    bool decodeJxlMetadata();

    class Private;
    QScopedPointer<Private> d;
};

#endif // JXLDECODEROBJECT_H
