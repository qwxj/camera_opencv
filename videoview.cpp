// =============================================================================
//  videoview.cpp
// =============================================================================

#include "videoview.h"

#include <QColor>
#include <QPainter>
#include <QPaintEvent>

VideoView::VideoView(QWidget *parent)
    : QWidget(parent)
{
    setMinimumSize(480, 320);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setPlaceholder(QStringLiteral("还没有画面 —— 点右边的「打开摄像头」"));
}

void VideoView::setImage(const QImage &image)
{
    m_image = image;
    update();
}

void VideoView::setPlaceholder(const QString &text)
{
    m_image = QImage();
    m_placeholder = text;
    update();
}

QSize VideoView::sizeHint() const
{
    return QSize(960, 540);
}

void VideoView::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.fillRect(rect(), QColor(13, 16, 21));

    if (m_image.isNull()) {
        painter.setPen(QColor(140, 152, 172));
        painter.drawText(rect().adjusted(24, 24, -24, -24),
                         Qt::AlignCenter | Qt::TextWordWrap, m_placeholder);
        return;
    }

    const QSize target = m_image.size().scaled(size(), Qt::KeepAspectRatio);
    const QRect box(QPoint((width() - target.width()) / 2,
                           (height() - target.height()) / 2),
                    target);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.drawImage(box, m_image);
}
