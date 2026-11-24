"""Resolution-independent artwork for the RF3 field edition."""
from pathlib import Path
from math import hypot
from random import Random

from PySide6.QtCore import QPointF, QRectF, Qt
from PySide6.QtGui import QBrush, QColor, QIcon, QLinearGradient, QRadialGradient, QPainter, QPainterPath, QPen, QPixmap, QPolygonF
from PySide6.QtWidgets import QComboBox, QLabel, QStyledItemDelegate


WHITE = "#ffffff"
MAROON = "#771d38"


def polygon(points):
    return QPolygonF([QPointF(x, y) for x, y in points])


def wizard_icon():
    return QIcon(str(Path(__file__).with_name("wizard.ico")))


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
    for mode, color in ((QIcon.Mode.Normal, "#ccc6ca"), (QIcon.Mode.Selected, WHITE)):
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
        painter.setPen(QPen(QColor("#e5dde0" if self.isEnabled() else "#a8a1a6"), 1.6,
                            Qt.PenStyle.SolidLine, Qt.PenCapStyle.RoundCap, Qt.PenJoinStyle.RoundJoin))
        painter.drawPolyline(polygon(((-4,-2), (0,2), (4,-2))))
        painter.end()



def discharge_path(rng, anchors, jitter):
    """Displace midpoints at progressively smaller scales to make a jagged channel."""
    points = [anchors[0]]
    def subdivide(a, b, amplitude):
        dx, dy = b.x() - a.x(), b.y() - a.y()
        length = hypot(dx, dy)
        if length < 6:
            points.append(b)
            return
        t = rng.uniform(.39, .61)
        offset = rng.uniform(-amplitude, amplitude)
        mid = QPointF(a.x() + dx * t - dy / length * offset,
                      a.y() + dy * t + dx / length * offset)
        subdivide(a, mid, amplitude * .57)
        subdivide(mid, b, amplitude * .57)
    for a, b in zip(anchors, anchors[1:]):
        subdivide(a, b, min(jitter, (b - a).manhattanLength() * .22))
    return points


def draw_lightning(painter, hand, tip):
    """Paint a steady branching discharge, anchored at the palm and the title."""
    span = hand.x() - tip.x()
    if span < 8:
        return
    rng = Random(203)
    strength = min(1.0, span / 300)
    height = painter.window().height()
    def location(t, rise):
        return QPointF(hand.x() - t * span,
                       hand.y() + t * (tip.y() - hand.y()) + strength * rise)
    anchors = [location(t, dy) for t, dy in ((0,0),(.09,-4),(.21,2),(.34,-12),
               (.46,-16),(.55,-4),(.67,-13),(.77,-3),(.86,2),(1,0))]
    main = discharge_path(rng, anchors, 6 * strength)
    branches = []
    def add_fork(start, reach, rise, width):
        end = QPointF(max(tip.x() + 3, start.x() - reach),
                      max(4.0, min(height - 4, start.y() + rise)))
        if (end - start).manhattanLength() < 5:
            return []
        points = discharge_path(rng, [start, end], min(9, reach * .22) * strength)
        branches.append((points, width))
        return points

    # Each secondary channel starts at a real junction on its parent. Forks
    # turn toward open space and split again as their current diminishes.
    forks = ((.04,.16,27),(.11,.11,-18),(.19,.27,41),(.25,.11,19),
             (.32,.13,-21),(.39,.22,36),(.49,.10,-23),(.56,.19,31),
             (.64,.14,-28),(.69,.15,23),(.78,.12,-18),(.85,.08,22),(.91,.055,-12))
    for at, length, rise in forks:
        start = main[round(at * (len(main) - 1))]
        fork = add_fork(start, min(length * span, 136), rise * strength, rng.uniform(.8, 1.4))
        if len(fork) < 6:
            continue
        for fraction, direction in ((.38, 1), (.68, -1)):
            joint = fork[round(fraction * (len(fork) - 1))]
            add_fork(joint, min(length * span * rng.uniform(.2,.48), 44),
                     direction * rng.uniform(8,20) * strength, .65)

    def stroke(points, width, start_color, end_color):
        path = QPainterPath(points[0])
        for point in points[1:]:
            path.lineTo(point)
        fade = QLinearGradient(points[0], points[-1])
        fade.setColorAt(0, QColor(start_color))
        fade.setColorAt(1, QColor(end_color))
        painter.setPen(QPen(QBrush(fade), width, Qt.SolidLine, Qt.RoundCap, Qt.RoundJoin))
        painter.drawPath(path)

    painter.save()
    painter.setClipRect(QRectF(tip.x(), 0, span + 2, height), Qt.IntersectClip)
    painter.setBrush(Qt.NoBrush)
    # Broad, low-opacity wine-red glow under a narrow incandescent core.
    for width, color in ((11, "#168e173f"), (6.5, "#438c1841"), (3.5, "#aa9b214d")):
        stroke(main, width, color, color)
    for points, width in branches:
        stroke(points, width + 2, "#559c2750", "#0095294c")
        stroke(points, width, "#ecdf8198", "#249f2e55")
    stroke(main, 2.0, "#f4c98099", "#dfaf4269")
    stroke(main, .85, "#ffffd9df", "#e6f392a4")
    painter.restore()


class FieldHeader(QLabel):
    """The PCB wizard casts maroon lightning across the shared page header."""
    def __init__(self):
        super().__init__()
        self.setObjectName("heading")
        self.wizard = QPixmap(str(Path(__file__).with_name("RF_Wizard_Shrouded_R3.png")))
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
        background = QLinearGradient(bounds.topLeft(), bounds.topRight())
        background.setColorAt(0, QColor("#500000"))
        background.setColorAt(0.48, QColor("#390910"))
        background.setColorAt(1, QColor("#080609"))
        painter.fillRect(bounds, background)

        # Fit the illustration inside the space left by the actual page title.
        text_end = self.contentsRect().left() + self.fontMetrics().tightBoundingRect(self.text()).right()
        title_end = text_end + 12
        available = self.width() - title_end - 12
        crop = QRectF(0, 100, 1024, 720)
        if available >= 48 and not self.wizard.isNull():
            scale = min(self.height() / crop.height(), available / crop.width())
            painter.setClipRect(QRectF(title_end, 0, available + 12, self.height()),
                                Qt.ClipOperation.IntersectClip)
            origin_x = self.width() - 12 - crop.width() * scale
            painter.save()
            painter.translate(origin_x, self.height() / 2)
            painter.scale(scale, scale)
            painter.translate(0, -crop.center().y())
            painter.setRenderHint(QPainter.RenderHint.SmoothPixmapTransform)
            # Screen blending preserves the white PCB ink on the dark banner.
            painter.setCompositionMode(QPainter.CompositionMode.CompositionMode_Screen)
            painter.drawPixmap(crop, self.wizard, crop)
            # Palm center in the original PCB artwork, mapped with the same transform as the image.
            palm = painter.worldTransform().map(QPointF(201, 417))
            painter.restore()
            # The wizard clip reserves a margin for the title. Let the arc cross
            # that margin and finish just beside the ink of the final letter.
            painter.setClipPath(clip)
            draw_lightning(painter, palm, QPointF(text_end + 2, self.height() * .55))
            radius = 35 * scale
            glow = QRadialGradient(palm, radius)
            glow.setColorAt(0, QColor("#fff4ea"))
            glow.setColorAt(.22, QColor("#ffe3abbd"))
            glow.setColorAt(.55, QColor("#a2be385e"))
            glow.setColorAt(1, QColor("#00992145"))
            painter.setPen(Qt.NoPen)
            painter.setBrush(QBrush(glow))
            painter.drawEllipse(palm, radius, radius)
        painter.end()
        super().paintEvent(event)
