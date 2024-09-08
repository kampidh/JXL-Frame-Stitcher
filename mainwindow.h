#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>

QT_BEGIN_NAMESPACE
namespace Ui {
class MainWindow;
}
QT_END_NAMESPACE

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

private slots:
    void currentFrameSettingChanged();
    void selectOutputFile();
    void doEncode();
    bool saveConfig();
    bool saveConfigAs(bool forceDialog = true);
    void openConfig();
    void openConfig(const QString &tmpfn);
    void addFiles();
    void appendFilesFromList(const QStringList &lst);
    void removeSelected();
    void selectingFrames();
    void resetApp();
    void setUnsaved();
    void resetOrder();

private:
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dropEvent(QDropEvent *event) override;

    class Private;
    QScopedPointer<Private> d;

    Ui::MainWindow *ui;
};
#endif // MAINWINDOW_H
