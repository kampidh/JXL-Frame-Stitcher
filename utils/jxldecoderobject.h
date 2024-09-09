#ifndef JXLDECODEROBJECT_H
#define JXLDECODEROBJECT_H

#include "jxlutils.h"
#include <QString>

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
