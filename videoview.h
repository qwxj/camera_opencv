// =============================================================================
//  videoview.h —— 等比缩放显示 QImage 的显示控件
//
//  用 paintEvent 自己画，比「QLabel + 每帧 setPixmap(缩放后的图)」
//  少一次全图拷贝，窗口拉伸时也不会变形。
// =============================================================================

#ifndef VIDEOVIEW_H
#define VIDEOVIEW_H

#include <QImage>
#include <QString>
#include <QWidget>

class VideoView : public QWidget
{
    Q_OBJECT

public:
    explicit VideoView(QWidget *parent = nullptr);

    void setImage(const QImage &image);
    void setPlaceholder(const QString &text);

    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QImage  m_image;
    QString m_placeholder;
};

#endif // VIDEOVIEW_H
