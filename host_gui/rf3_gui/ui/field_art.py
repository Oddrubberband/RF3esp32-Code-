"""Resolution-independent artwork for the RF3 field edition."""
from PySide6.QtCore import QPointF, QRectF, Qt
from PySide6.QtGui import QColor, QIcon, QPainter, QPainterPath, QPen, QPixmap, QPolygonF
from PySide6.QtWidgets import QComboBox, QLabel, QStyledItemDelegate


INK = "#212421"
APRICOT = "#ffac74"


def polygon(points):
    return QPolygonF([QPointF(x, y) for x, y in points])


def mark(size=40):
    pixmap = QPixmap(size * 2, size * 2)
    pixmap.setDevicePixelRatio(2)
    pixmap.fill(Qt.GlobalColor.transparent)
    painter = QPainter(pixmap)
    painter.setRenderHint(QPainter.RenderHint.Antialiasing)
    painter.scale(size / 40, size / 40)
    painter.setPen(Qt.PenStyle.NoPen)
    painter.setBrush(QColor(APRICOT))
    painter.drawRoundedRect(QRectF(0, 0, 40, 40), 10, 10)
    painter.setBrush(QColor(INK))
    for x, y, height in ((7, 19, 13), (16, 11, 21), (25, 7, 18)):
        painter.drawPolygon(polygon(((x, y+4), (x+7, y), (x+7, y+height-4), (x, y+height))))
    painter.end()
    return pixmap


# Each glyph uses the same 24-unit grid and stroke weight.
GLYPHS = {
    "Live metrics": [[(3,3),(3,20),(22,20)],[(6,15),(10,10),(13,13),(20,5)]],
    "File transfer": [[(3,8),(19,8),(15,4)],[(21,16),(5,16),(9,20)]],
    "Board files": [[(3,7),(9,7),(11,10),(21,10),(21,19),(3,19),(3,7)]],
    "Text and CW": [[(3,5),(21,5),(21,16),(11,16),(6,20),(6,16),(3,16),(3,5)],[(7,10),(16,10)]],
    "Radio settings": [[(3,6),(10,6)],[(14,6),(21,6)],[(12,3),(12,9)],[(3,16),(6,16)],[(10,16),(21,16)],[(8,13),(8,19)]],
    "Connection": [[(3,16),(3,12),(9,6),(13,6)],[(21,8),(21,12),(15,18),(11,18)],[(8,16),(16,8)]],
    "Device status": [[(2,12),(7,12),(10,5),(14,19),(17,12),(22,12)]],
    "Diagnostics": [[(4,6),(10,12),(4,18)],[(13,18),(21,18)]],
}


def nav_icon(name):
    icon = QIcon()
    for mode, color in ((QIcon.Mode.Normal, "#cfcec5"), (QIcon.Mode.Selected, INK)):
        pixmap = QPixmap(40, 40)
        pixmap.setDevicePixelRatio(2)
        pixmap.fill(Qt.GlobalColor.transparent)
        painter = QPainter(pixmap)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        painter.scale(20 / 24, 20 / 24)
        painter.setPen(QPen(QColor(color), 1.8, Qt.PenStyle.SolidLine,
                            Qt.PenCapStyle.RoundCap, Qt.PenJoinStyle.RoundJoin))
        for points in GLYPHS[name]:
            painter.drawPolyline(polygon(points))
        painter.end()
        icon.addPixmap(pixmap, mode)
    return icon


class FieldComboBox(QComboBox):
    """Keep native selection and keyboard handling with a visible disclosure mark."""
    def __init__(self, parent=None):
        super().__init__(parent)
        self.setItemDelegate(QStyledItemDelegate(self))

    def paintEvent(self, event):
        super().paintEvent(event)
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        painter.translate(self.width() - 13, self.height() / 2)
        painter.setPen(QPen(QColor("#50584e" if self.isEnabled() else "#777c72"), 1.6,
                            Qt.PenStyle.SolidLine, Qt.PenCapStyle.RoundCap, Qt.PenJoinStyle.RoundJoin))
        painter.drawPolyline(polygon(((-4,-2), (0,2), (4,-2))))
        painter.end()


class FieldHeader(QLabel):
    """A printed antenna motif. The illustration has no telemetry meaning."""
    def __init__(self):
        super().__init__()
        self.setObjectName("heading")
        self.setMinimumHeight(98)
        self.setContentsMargins(24, 0, 8, 0)
        self.setAlignment(Qt.AlignmentFlag.AlignVCenter | Qt.AlignmentFlag.AlignLeft)

    def paintEvent(self, event):
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        bounds = QRectF(self.rect())
        clip = QPainterPath()
        clip.addRoundedRect(bounds, 12, 12)
        painter.setClipPath(clip)
        painter.fillRect(bounds, QColor("#efd6af"))
        # Keep the title clear when the window is narrow or fonts are enlarged.
        title_width = self.fontMetrics().horizontalAdvance(self.text()) + 48
        art_width = min(368, max(0, self.width() - title_width))
        if art_width:
            painter.save()
            painter.setClipRect(QRectF(self.width()-art_width, 0, art_width, self.height()),
                                Qt.ClipOperation.IntersectClip)
            painter.translate(self.width(), self.height())
            painter.scale(self.height()/110, self.height()/110)
            # Offset copper disc and nesting transmission arcs recall printed radio manuals.
            painter.setPen(Qt.PenStyle.NoPen)
            painter.setBrush(QColor("#e58452"))
            painter.drawEllipse(QPointF(-87, 8), 72, 72)
            painter.setBrush(Qt.BrushStyle.NoBrush)
            for radius in range(21, 272, 13):
                pen = QPen(QColor("#4e4c40"), 1.05 if radius % 2 else 1.7)
                painter.setPen(pen)
                painter.drawEllipse(QPointF(-36, 39), radius * 1.22, radius)
            painter.setPen(Qt.PenStyle.NoPen)
            painter.setBrush(QColor(INK))
            painter.drawPolygon(polygon(((-95,110), (-46,-64), (-37,-67), (-63,110))))
            painter.drawEllipse(QPointF(-42,-68), 6, 6)
            painter.restore()
        painter.end()
        super().paintEvent(event)
