#!/usr/bin/env python3
"""Renders the app icon's PNGs from its SVG, after a change to it. Needs PySide6.

    scripts/render-app-icon.py
"""
from pathlib import Path

from PySide6.QtCore import Qt
from PySide6.QtGui import QGuiApplication, QImage, QPainter
from PySide6.QtSvg import QSvgRenderer

NAME = "com.sendspin_audio.Sendspin"
hicolor = Path(__file__).resolve().parent.parent / "data/icons/hicolor"

app = QGuiApplication(["render-app-icon", "-platform", "offscreen"])
svg = QSvgRenderer(str(hicolor / f"scalable/apps/{NAME}.svg"))
for size in (16, 24, 32, 48, 64, 128, 256, 512):
    image = QImage(size, size, QImage.Format.Format_ARGB32)
    image.fill(Qt.GlobalColor.transparent)
    painter = QPainter(image)
    painter.setRenderHint(QPainter.RenderHint.Antialiasing)
    svg.render(painter)
    painter.end()
    out = hicolor / f"{size}x{size}/apps/{NAME}.png"
    out.parent.mkdir(parents=True, exist_ok=True)
    image.save(str(out))
