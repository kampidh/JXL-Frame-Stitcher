#ifndef JXLDECODEROBJECT_H
#define JXLDECODEROBJECT_H

#include "jxlutils.h"
#include <QString>

class JXLDecoderObject
{
public:
    JXLDecoderObject(const QString &inputFilename);
    ~JXLDecoderObject();

    void setEncodeParams(const jxfrstch::EncodeParams &params);
    QSize getRootFrameSize();
    QByteArray getIccProfie();

    int imageCount();
    bool haveAnimation();
    bool canRead();
    QImage read();
    QString errorString();
    int nextImageDelay();
    QRect currentImageRect();

private:
    bool decodeJxlMetadata();

    class Private;
    QScopedPointer<Private> d;
};

#endif // JXLDECODEROBJECT_H
