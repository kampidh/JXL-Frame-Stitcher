#ifndef JXLENCODEROBJECT_H
#define JXLENCODEROBJECT_H

#include <QObject>
#include <QMutex>
#include <QThread>

#include "jxlutils.h"

class JXLEncoderObject : public QThread
{
    Q_OBJECT
public:
    explicit JXLEncoderObject(QObject *parent = nullptr);
    ~JXLEncoderObject();

    void setEncodeParams(const jxfrstch::EncodeParams &params);
    void appendInputFiles(const jxfrstch::InputFileData &ifd);
    bool parseFirstImage();
    bool doEncode();
    bool resetEncoder();
    bool cleanupEncoder();
    void abortEncode(bool completeFile);

protected:
    void run() override;

signals:
    void sigStatusText(const QString &status);
    void sigCurrentMainProgressBar(const int &progress, const bool &success);
    void sigCurrentSubProgressBar(const int &progress);
    void sigEnableSubProgressBar(const bool &enabled, const int &setMax);
    void sigThrowError(const QString &status);

private:
    class Private;
    QScopedPointer<Private> d;

    QMutex mutex;
};

#endif // JXLENCODEROBJECT_H
