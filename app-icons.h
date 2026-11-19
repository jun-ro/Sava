#pragma once

#include <QByteArray>
#include <QIcon>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>

namespace sava {

// Rasterized from the provider marks bundled for Co-write.
inline QIcon harnessIcon(const QString &name)
{
    if (name == QLatin1String("Antigravity")) {
        QPixmap image(32, 32);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing, true);

        QPainterPath arch;
        arch.moveTo(6.0, 25.5);
        arch.cubicTo(9.5, 19.0, 12.2, 6.2, 16.0, 6.2);
        arch.cubicTo(19.8, 6.2, 22.5, 19.0, 26.0, 25.5);
        arch.cubicTo(22.2, 21.8, 19.2, 19.2, 16.0, 19.2);
        arch.cubicTo(12.8, 19.2, 9.8, 21.8, 6.0, 25.5);
        arch.closeSubpath();

        QLinearGradient grad(6.0, 25.0, 26.0, 7.0);
        grad.setColorAt(0.0, QColor("#70cdef"));
        grad.setColorAt(0.38, QColor("#76e3b4"));
        grad.setColorAt(0.72, QColor("#ffda70"));
        grad.setColorAt(1.0, QColor("#ff80a7"));
        painter.setPen(Qt::NoPen);
        painter.setBrush(grad);
        painter.drawPath(arch);

        image.setDevicePixelRatio(2);
        return QIcon(image);
    }

    const char *png = nullptr;
    if (name == "Codex") png = "iVBORw0KGgoAAAANSUhEUgAAACAAAAAgCAYAAABzenr0AAAABmJLR0QA/wD/AP+gvaeTAAADcklEQVRYhb2WTWhcVRiGn+/MpKY2oekkdzIOk4nSaBYFFSqI9aeopQhCQYwLa125CBb8QRRR3NUquLCC1oogChoKrbgR3AS1RUFa7KKu6k+xMxObem8zk7amKZnMfV2kSSfJ/Flv8q7OPee93/ucey/fPUaLmvj7zBYz95TBNoF3dbpg4ttZ5z7yvMxvrdaqljUzXBwb666s0QGhoQb+Mti7G7zMG2Y2GxnApfFxrxwrjwJ3tFjvBDANpEFlzE5J9nVo7Yc8z7v0nwAkuVJQ+B54oMXwRjon9Fx3sv/LpQuu3h2Tfn5XROEAKcMOF/386y0DyNnuiMKrtXfCzw1VTyx6BZLcZJB/WmbPIu5eAQCAc1dm4xvT6fRlqHoCQXDmplJQOCrssxUMB0i1t5V3zV84gItnz/bE5Y4A961g8DWF9uj8MA4wG5v9BLhtVcLnNDA/cKWgsBVjR02bmME4APwSabyzdQtDwTN1jcZXCS+7u2LhI6CajeR6ZJK/AIB0TwPvvYVCYa3n3TwOvB0VgMBJis8BQKaBt6+jXS8DXJgK9wF/RsSwuXQ+vxfAin7+MrC2gXkqdDbY09P314Sfe8KwQxFBVCoW2+RA+SbGdS7UWwDdyf7DGMciAojFw8qwAxttajUGF4ayZsAtS8bDzon3gEb/8DCs2AsAE+O5TUKPRQWAyLiu3uxpYfvreQw+7Un1HZNkFrMPudq8IpFxgwFIP7eVAu842J1LLJNt4cxgZ2rALwaFnUgjy3dhH+PCE3XgbwzFa4YlaxPo1ziA2V3lop8rLS9go52pAd/3/Q505Z0aFf5J9PYNN9pkMcilEa/UzrcjC7/jop//A9i4xDIF2gP2ELC9Vg0T+0LTT7XjrdNgD5CumS+3pRrgJHB7/b1ELHEw0Zvd6a5dc3r1svm9TTMvQvWRzOybVcr/IbRwa2dqwIeqI1kQBJ0xTeeADSuRauI4aH9XMvuFmYUL89WmYpB/EjGydD4C/ZhIZu+vtbDoVJzwsgeRvRpxOELv11urudMJPzfksA8Evf87XPZdIpnZZmZqGQDmvom4ph8PYYfN9YcOE+dlxIDNLeafdGv0YFdX/7Im1xSgniTFS8HYm6CXgLYGxpFYu3t+/fq+YqN61/2xXfALt4bSsIztwC0gISvgOKrQfd7dm6nTHRfrX6DBQkNSBf4aAAAAAElFTkSuQmCC";
    if (name == "Claude Code") png = "iVBORw0KGgoAAAANSUhEUgAAACAAAAAgCAYAAABzenr0AAAABmJLR0QA/wD/AP+gvaeTAAABFUlEQVRYhe2VTUoDQRBGXw3jIicQvEBmH1DcjXgGSU4gegKzzHY8gMQbODJXELNzl2XwB1wqOYELZcqFBDLdZEyjTQfSb/cVXVVfUwUFkci2I2bgcdg/8datFs0ub6rlUGo5Um69GRAF49OJt2ZrsvkGFKYKU9fC6+b9aiAryt57Z37gaqD7Wu9nRdn7swHfBDcgz8PB6XJAVa8bD+CshkTgyqWwip4nKqowNus19NNFX11d/yfBRxDcQArcGbFjzz0b/axj5HsnukUZb0ED6xyrygOAiB626VW45lk7AHA/ytO9j91P+JmZqdsMLHborTPfORpNvkxtvg8+guAGrB0AyMnrF2bVKt2GoNUiByaWjkQ2jm+GzmgylAsFdwAAAABJRU5ErkJggg==";
    else if (name == "OpenCode") png = "iVBORw0KGgoAAAANSUhEUgAAACAAAAAgCAYAAABzenr0AAAABmJLR0QA/wD/AP+gvaeTAAAAoElEQVRYhe2WwQkCQQxFf6INiCAe3D4sZK3EOmxEtxDrcDzIglqB+d7dHAYJDGj+MYR5j/APA/x7xBveb9ct9NWFkmxWluvN+XM893ZVuSekj+SLcgCwm7AiId8kBZoLuCX0IpBhseomJfLyHMuJYFWJm18gBVIgBVIgBVIgBar/AwT7x3hh5W61QPMLpEBzAbeEZnKA2jESRNMS+d7v5A2fkSIs62hnWQAAAABJRU5ErkJggg==";
    else if (name == "Pi") png = "iVBORw0KGgoAAAANSUhEUgAAACAAAAAgCAYAAABzenr0AAAABmJLR0QA/wD/AP+gvaeTAAAA9UlEQVRYhe3UIW4CQRTG8f9bJjSBhMAZKnFV1C14PCfYaoKsIXgo2F6gIUjQgFsOABJLLTQNqOVxgR3awjZj5tn58s0vM5kBxyMA+7fukxKEWZfnyX0U26+f1zIGQAlCEfpZAxJNYuAqIMh607+OB3iAAdiayvGkukkLFEiSx/PX0V4htbsB3YdqQZCqJbOaRvVnW8F+2NN7AM6vwAM8wDnA/CJTa74vrE+tHNXFtnaYt+LDvJX6TwjaKTXGA+cn4AEe4Bzw8zNU1qoSuQMI37OXMP4vgPMr8AAPMACiulShkxYIkN3N7aIjUSZpS2dleXNvlnMBzG47/bpnDgoAAAAASUVORK5CYII=";
    if (!png) return {};
    QPixmap image;
    image.loadFromData(QByteArray::fromBase64(png), "PNG");
    image.setDevicePixelRatio(2);
    return QIcon(image);
}

} // namespace sava
