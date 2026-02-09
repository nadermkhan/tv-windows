#include <QApplication>
#include <QMainWindow>
#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSplitter>
#include <QListWidget>
#include <QListView>
#include <QAbstractListModel>
#include <QSortFilterProxyModel>
#include <QLineEdit>
#include <QPushButton>
#include <QLabel>
#include <QStatusBar>
#include <QMessageBox>
#include <QTimer>
#include <QSettings>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>
#include <QVector>
#include <QHash>
#include <QSet>
#include <QPixmap>
#include <QPainter>
#include <QFont>
#include <QColor>
#include <QStyledItemDelegate>
#include <QStyleOptionViewItem>
#include <QModelIndex>
#include <QVariant>
#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QRegularExpression>
#include <QFrame>
#include <QPropertyAnimation>
#include <QParallelAnimationGroup>
#include <QSequentialAnimationGroup>
#include <QClipboard>
#include <QGuiApplication>
#include <QScreen>
#include <QScrollBar>
#include <QPainterPath>
#include <QThread>
#include <QMutex>
#include <QMutexLocker>
#include <QWaitCondition>
#include <QElapsedTimer>
#include <QFontDatabase>
#include <QGraphicsOpacityEffect>
#include <QtGlobal>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

#include <mpv/client.h>

#include <cstring>
#include <algorithm>

static const char* PLAYLIST_URL = "https://m3u.work/jwuF5FPp.m3u";
static const int MAX_DOWNLOAD_SIZE = 10 * 1024 * 1024;
static const int PLAYLIST_TIMEOUT_MS = 15000;
static const int IMAGE_TIMEOUT_MS = 6000;
static const int MAX_CONCURRENT_DOWNLOADS = 8;
static const int DEBOUNCE_MS = 120;
static const int OSD_DISPLAY_MS = 4000;
static const int AUTOHIDE_MS = 3500;
static const int MAX_NAME_LEN = 200;
static const int STATUS_CHECK_MS = 30000;
static const int RETRY_DELAY_MS = 3000;
static const int MAX_RETRIES = 2;
static const int ERROR_DISPLAY_MS = 5000;

// ─── Data Structures ─────────────────────────────────────────────

struct Channel {
    QString name;
    QString category;
    QString logoUrl;
    QString streamUrl;
};

enum ChannelRoles {
    NameRole = Qt::UserRole + 1,
    CategoryRole,
    LogoUrlRole,
    StreamUrlRole,
    IndexRole
};

// ─── M3U Parser Thread ───────────────────────────────────────────

class M3uParserThread : public QThread {
    Q_OBJECT
public:
    explicit M3uParserThread(QObject* parent = nullptr) : QThread(parent) {}

    void parseData(const QByteArray& data) {
        m_data = data;
        start();
    }

signals:
    void parsingFinished(QVector<Channel> channels, QStringList categories);

protected:
    void run() override {
        QVector<Channel> channels;
        QSet<QString> catSet;
        QString text = QString::fromUtf8(m_data);
        QStringList lines = text.split(QRegularExpression("[\\r\\n]+"), QString::SkipEmptyParts);

        if (lines.isEmpty()) {
            emit parsingFinished(channels, QStringList());
            return;
        }

        QRegularExpression reExtInf("^#EXTINF\\s*:\\s*(-?\\d+)\\s*(.*),\\s*(.*)$");
        QRegularExpression reLogo("tvg-logo\\s*=\\s*\"([^\"]*)\"");
        QRegularExpression reGroup("group-title\\s*=\\s*\"([^\"]*)\"");

        Channel pending;
        bool hasPending = false;

        channels.reserve(lines.size() / 2);

        for (int i = 0; i < lines.size(); ++i) {
            QString line = lines[i].trimmed();
            if (line.isEmpty()) continue;

            if (line.startsWith("#EXTINF")) {
                QRegularExpressionMatch match = reExtInf.match(line);
                pending = Channel();
                if (match.hasMatch()) {
                    QString attrs = match.captured(2);
                    pending.name = match.captured(3).trimmed();
                    if (pending.name.length() > MAX_NAME_LEN)
                        pending.name = pending.name.left(MAX_NAME_LEN);

                    QRegularExpressionMatch logoMatch = reLogo.match(attrs);
                    if (logoMatch.hasMatch()) pending.logoUrl = logoMatch.captured(1).trimmed();

                    QRegularExpressionMatch groupMatch = reGroup.match(attrs);
                    if (groupMatch.hasMatch()) pending.category = groupMatch.captured(1).trimmed();
                } else {
                    int commaIdx = line.lastIndexOf(',');
                    if (commaIdx >= 0) {
                        pending.name = line.mid(commaIdx + 1).trimmed();
                        if (pending.name.length() > MAX_NAME_LEN)
                            pending.name = pending.name.left(MAX_NAME_LEN);
                    }
                }
                if (pending.category.isEmpty()) pending.category = "Others";
                if (pending.name.isEmpty()) pending.name = "Unknown";
                hasPending = true;
            } else if (!line.startsWith("#")) {
                if (hasPending) {
                    QUrl streamUrl(line);
                    if (streamUrl.isValid()) {
                        QString scheme = streamUrl.scheme();
                        if (scheme == "http" || scheme == "https" || scheme == "rtsp" ||
                            scheme == "rtmp" || scheme == "mms" || scheme == "mmsh") {
                            pending.streamUrl = line;
                            channels.append(pending);
                            catSet.insert(pending.category);
                        }
                    }
                    hasPending = false;
                }
            }
        }

        QStringList cats = catSet.toList();
        std::sort(cats.begin(), cats.end());
        cats.prepend("All");

        emit parsingFinished(channels, cats);
    }

private:
    QByteArray m_data;
};

// ─── Channel Model ───────────────────────────────────────────────

class ChannelModel : public QAbstractListModel {
    Q_OBJECT
public:
    explicit ChannelModel(QObject* parent = nullptr) : QAbstractListModel(parent) {}

    void setChannels(const QVector<Channel>& ch) {
        beginResetModel();
        m_channels = ch;
        endResetModel();
    }

    int rowCount(const QModelIndex& parent = QModelIndex()) const override {
        if (parent.isValid()) return 0;
        return m_channels.size();
    }

    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override {
        if (!index.isValid() || index.row() < 0 || index.row() >= m_channels.size())
            return QVariant();
        const Channel& ch = m_channels[index.row()];
        switch (role) {
            case Qt::DisplayRole:
            case NameRole: return ch.name;
            case CategoryRole: return ch.category;
            case LogoUrlRole: return ch.logoUrl;
            case StreamUrlRole: return ch.streamUrl;
            case IndexRole: return index.row();
            default: return QVariant();
        }
    }

    QHash<int, QByteArray> roleNames() const override {
        QHash<int, QByteArray> roles;
        roles[NameRole] = "channelName";
        roles[CategoryRole] = "category";
        roles[LogoUrlRole] = "logoUrl";
        roles[StreamUrlRole] = "streamUrl";
        roles[IndexRole] = "channelIndex";
        return roles;
    }

    const QVector<Channel>& channels() const { return m_channels; }

private:
    QVector<Channel> m_channels;
};

// ─── Category Filter Proxy ───────────────────────────────────────

class CategoryFilterProxy : public QSortFilterProxyModel {
    Q_OBJECT
public:
    explicit CategoryFilterProxy(QObject* parent = nullptr) : QSortFilterProxyModel(parent) {}
    void setCategoryFilter(const QString& cat) { m_category = cat; invalidateFilter(); }
    void setSearchFilter(const QString& search) { m_search = search.toLower(); invalidateFilter(); }
    QString categoryFilter() const { return m_category; }

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const override {
        QModelIndex idx = sourceModel()->index(sourceRow, 0, sourceParent);
        if (!idx.isValid()) return false;
        if (!m_category.isEmpty() && m_category != "All") {
            if (idx.data(CategoryRole).toString() != m_category) return false;
        }
        if (!m_search.isEmpty()) {
            if (!idx.data(NameRole).toString().toLower().contains(m_search)) return false;
        }
        return true;
    }

private:
    QString m_category;
    QString m_search;
};

// ─── Smart TV Channel Delegate ───────────────────────────────────

class ChannelDelegate : public QStyledItemDelegate {
    Q_OBJECT
public:
    explicit ChannelDelegate(QObject* parent = nullptr) : QStyledItemDelegate(parent) {}
    void setLogoCache(QHash<QString, QPixmap>* cache) { m_logoCache = cache; }
    void setActiveChannel(const QString& url) { m_activeUrl = url; }

    QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const override {
        return QSize(195, 120);
    }

    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        painter->setRenderHint(QPainter::SmoothPixmapTransform, true);

        QRect r = option.rect.adjusted(4, 4, -4, -4);
        QPainterPath cardPath;
        cardPath.addRoundedRect(QRectF(r), 12, 12);

        QString streamUrl = index.data(StreamUrlRole).toString();
        bool isActive = (!m_activeUrl.isEmpty() && streamUrl == m_activeUrl);
        bool isSelected = option.state & QStyle::State_Selected;
        bool isHovered = option.state & QStyle::State_MouseOver;

        // Card background with gradient
        QLinearGradient cardGrad(r.topLeft(), r.bottomRight());
        if (isActive) {
            cardGrad.setColorAt(0, QColor(30, 64, 120));
            cardGrad.setColorAt(1, QColor(20, 45, 90));
        } else if (isSelected) {
            cardGrad.setColorAt(0, QColor(55, 65, 110));
            cardGrad.setColorAt(1, QColor(40, 50, 90));
        } else if (isHovered) {
            cardGrad.setColorAt(0, QColor(42, 46, 72));
            cardGrad.setColorAt(1, QColor(35, 38, 62));
        } else {
            cardGrad.setColorAt(0, QColor(30, 33, 52));
            cardGrad.setColorAt(1, QColor(26, 28, 46));
        }
        painter->fillPath(cardPath, cardGrad);

        // Glow border for active
        if (isActive) {
            QPen glowPen(QColor(99, 140, 255, 160), 2);
            painter->setPen(glowPen);
            painter->drawPath(cardPath);

            // Live indicator dot
            painter->setPen(Qt::NoPen);
            painter->setBrush(QColor(34, 197, 94));
            painter->drawEllipse(r.right() - 14, r.top() + 8, 8, 8);
        } else if (isHovered) {
            painter->setPen(QPen(QColor(255, 255, 255, 25), 1));
            painter->drawPath(cardPath);
        } else {
            painter->setPen(QPen(QColor(255, 255, 255, 8), 1));
            painter->drawPath(cardPath);
        }

        // Logo
        QRect iconRect(r.left() + 14, r.top() + 12, 56, 44);
        QString logoUrl = index.data(LogoUrlRole).toString();
        QString name = index.data(NameRole).toString();
        if (name.length() > MAX_NAME_LEN) name = name.left(MAX_NAME_LEN) + "...";
        QString category = index.data(CategoryRole).toString();

        bool drawn = false;
        if (m_logoCache && !logoUrl.isEmpty() && m_logoCache->contains(logoUrl)) {
            const QPixmap& pm = m_logoCache->value(logoUrl);
            if (!pm.isNull()) {
                QPainterPath clipPath;
                clipPath.addRoundedRect(QRectF(iconRect), 8, 8);
                painter->setClipPath(clipPath);

                // Dark background behind logo
                painter->fillRect(iconRect, QColor(15, 15, 25));

                QPixmap scaled = pm.scaled(iconRect.size(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
                int dx = iconRect.left() + (iconRect.width() - scaled.width()) / 2;
                int dy = iconRect.top() + (iconRect.height() - scaled.height()) / 2;
                painter->drawPixmap(dx, dy, scaled);
                painter->setClipping(false);
                drawn = true;
            }
        }

        if (!drawn) {
            QPainterPath clipPath;
            clipPath.addRoundedRect(QRectF(iconRect), 8, 8);
            int h = name.isEmpty() ? 200 : qAbs(name.at(0).unicode() * 47 + name.length() * 13) % 360;
            QLinearGradient grad(iconRect.topLeft(), iconRect.bottomRight());
            grad.setColorAt(0, QColor::fromHsv(h, 130, 110));
            grad.setColorAt(1, QColor::fromHsv((h + 35) % 360, 110, 85));
            painter->fillPath(clipPath, grad);
            painter->setPen(QColor(255, 255, 255, 230));
            QFont f = painter->font();
            f.setPixelSize(22);
            f.setBold(true);
            painter->setFont(f);
            painter->drawText(iconRect, Qt::AlignCenter, name.isEmpty() ? "?" : name.left(1).toUpper());
        }

        // Channel number badge
        int chNum = index.data(IndexRole).toInt() + 1;
        if (chNum > 0 && chNum <= 9999) {
            QFont numFont = painter->font();
            numFont.setPixelSize(9);
            numFont.setBold(true);
            painter->setFont(numFont);
            QString numStr = QString::number(chNum);
            int numW = painter->fontMetrics().horizontalAdvance(numStr) + 8;
            QRect numRect(r.left() + 14, r.top() + 60, numW, 16);
            QPainterPath numPath;
            numPath.addRoundedRect(QRectF(numRect), 4, 4);
            painter->fillPath(numPath, QColor(0, 0, 0, 120));
            painter->setPen(QColor(180, 190, 210));
            painter->drawText(numRect, Qt::AlignCenter, numStr);
        }

        // Channel name
        painter->setPen(QColor(240, 243, 248));
        QFont nameFont = painter->font();
        nameFont.setPixelSize(12);
        nameFont.setBold(true);
        painter->setFont(nameFont);
        QRect nameRect(r.left() + 10, r.top() + 72, r.width() - 20, 20);
        QString elidedName = painter->fontMetrics().elidedText(name, Qt::ElideRight, nameRect.width());
        painter->drawText(nameRect, Qt::AlignLeft | Qt::AlignVCenter, elidedName);

        // Category label
        if (!category.isEmpty()) {
            QFont catFont = nameFont;
            catFont.setPixelSize(10);
            catFont.setBold(false);
            painter->setFont(catFont);

            // Category pill background
            int catTextW = painter->fontMetrics().horizontalAdvance(category);
            int pillW = qMin(catTextW + 12, r.width() - 20);
            QRect catPillRect(r.left() + 10, r.top() + 94, pillW, 16);
            QPainterPath catPillPath;
            catPillPath.addRoundedRect(QRectF(catPillRect), 4, 4);
            painter->fillPath(catPillPath, QColor(99, 102, 241, 40));
            painter->setPen(QColor(165, 170, 220));
            QString elidedCat = painter->fontMetrics().elidedText(category, Qt::ElideRight, pillW - 10);
            painter->drawText(catPillRect, Qt::AlignCenter, elidedCat);
        }

        painter->restore();
    }

private:
    QHash<QString, QPixmap>* m_logoCache;
    QString m_activeUrl;
};

// ─── Error Overlay Widget ────────────────────────────────────────

class ErrorOverlay : public QWidget {
    Q_OBJECT
public:
    explicit ErrorOverlay(QWidget* parent = nullptr) : QWidget(parent) {
        setAttribute(Qt::WA_TransparentForMouseEvents, false);
        setAttribute(Qt::WA_NoSystemBackground);
        setAutoFillBackground(false);
        hide();

        m_hideTimer = new QTimer(this);
        m_hideTimer->setSingleShot(true);
        connect(m_hideTimer, &QTimer::timeout, this, &ErrorOverlay::fadeOut);

        m_opacityEffect = new QGraphicsOpacityEffect(this);
        m_opacityEffect->setOpacity(1.0);
        setGraphicsEffect(m_opacityEffect);
    }

    void showError(const QString& channelName) {
        m_channelName = channelName;
        m_opacityEffect->setOpacity(1.0);
        show();
        raise();
        update();
        m_hideTimer->start(ERROR_DISPLAY_MS);
    }

    void fadeOut() {
        QPropertyAnimation* anim = new QPropertyAnimation(m_opacityEffect, "opacity", this);
        anim->setDuration(600);
        anim->setStartValue(1.0);
        anim->setEndValue(0.0);
        connect(anim, &QPropertyAnimation::finished, this, &QWidget::hide);
        anim->start(QAbstractAnimation::DeleteWhenStopped);
    }

signals:
    void retryClicked();
    void dismissClicked();

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);

        // Semi-transparent backdrop
        p.fillRect(rect(), QColor(0, 0, 0, 140));

        // Dialog card
        int cardW = qMin(width() - 60, 440);
        int cardH = 220;
        int cx = (width() - cardW) / 2;
        int cy = (height() - cardH) / 2;

        QPainterPath cardPath;
        cardPath.addRoundedRect(cx, cy, cardW, cardH, 20, 20);

        QLinearGradient cardGrad(cx, cy, cx, cy + cardH);
        cardGrad.setColorAt(0, QColor(35, 30, 50));
        cardGrad.setColorAt(1, QColor(25, 22, 40));
        p.fillPath(cardPath, cardGrad);
        p.setPen(QPen(QColor(255, 255, 255, 15), 1));
        p.drawPath(cardPath);

        // Error icon circle
        int iconCx = cx + cardW / 2;
        int iconCy = cy + 50;
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(239, 68, 68, 30));
        p.drawEllipse(QPoint(iconCx, iconCy), 28, 28);
        p.setBrush(QColor(239, 68, 68));
        p.drawEllipse(QPoint(iconCx, iconCy), 18, 18);

        // X mark
        p.setPen(QPen(Qt::white, 3, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(iconCx - 6, iconCy - 6, iconCx + 6, iconCy + 6);
        p.drawLine(iconCx + 6, iconCy - 6, iconCx - 6, iconCy + 6);

        // Title
        p.setPen(Qt::white);
        QFont f = font();
        f.setPixelSize(17);
        f.setBold(true);
        p.setFont(f);
        p.drawText(cx, cy + 80, cardW, 28, Qt::AlignCenter, "Channel Unavailable");

        // Subtitle
        f.setPixelSize(12);
        f.setBold(false);
        p.setFont(f);
        p.setPen(QColor(160, 165, 185));
        QString msg = m_channelName.isEmpty()
            ? "This channel is not available right now."
            : "\"" + p.fontMetrics().elidedText(m_channelName, Qt::ElideRight, cardW - 60) + "\" is not available right now.";
        p.drawText(cx + 20, cy + 110, cardW - 40, 20, Qt::AlignCenter, msg);
        p.drawText(cx + 20, cy + 130, cardW - 40, 20, Qt::AlignCenter, "Please try another channel or retry later.");

        // Buttons
        int btnW = 100;
        int btnH = 34;
        int btnY = cy + cardH - 55;
        int gap = 16;
        int totalBtnW = btnW * 2 + gap;
        int btnStartX = cx + (cardW - totalBtnW) / 2;

        // Retry button
        m_retryRect = QRect(btnStartX, btnY, btnW, btnH);
        QPainterPath retryPath;
        retryPath.addRoundedRect(QRectF(m_retryRect), 8, 8);
        p.fillPath(retryPath, QColor(99, 102, 241));
        p.setPen(Qt::white);
        f.setPixelSize(12);
        f.setBold(true);
        p.setFont(f);
        p.drawText(m_retryRect, Qt::AlignCenter, "Retry");

        // Dismiss button
        m_dismissRect = QRect(btnStartX + btnW + gap, btnY, btnW, btnH);
        QPainterPath dismissPath;
        dismissPath.addRoundedRect(QRectF(m_dismissRect), 8, 8);
        p.fillPath(dismissPath, QColor(60, 60, 80));
        p.setPen(QPen(QColor(255, 255, 255, 30), 1));
        p.drawPath(dismissPath);
        p.setPen(QColor(200, 200, 220));
        p.drawText(m_dismissRect, Qt::AlignCenter, "Dismiss");
    }

    void mousePressEvent(QMouseEvent* event) override {
        QPoint pos = event->pos();
        if (m_retryRect.contains(pos)) {
            hide();
            emit retryClicked();
        } else if (m_dismissRect.contains(pos)) {
            fadeOut();
            emit dismissClicked();
        }
    }

private:
    QTimer* m_hideTimer;
    QGraphicsOpacityEffect* m_opacityEffect;
    QString m_channelName;
    QRect m_retryRect;
    QRect m_dismissRect;
};

// ─── OSD Widget ──────────────────────────────────────────────────

class OsdWidget : public QWidget {
    Q_OBJECT
public:
    explicit OsdWidget(QWidget* parent = nullptr) : QWidget(parent) {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
        setAutoFillBackground(false);
        hide();
        m_hideTimer = new QTimer(this);
        m_hideTimer->setSingleShot(true);

        m_opacityEffect = new QGraphicsOpacityEffect(this);
        m_opacityEffect->setOpacity(1.0);
        setGraphicsEffect(m_opacityEffect);

        connect(m_hideTimer, &QTimer::timeout, this, &OsdWidget::fadeOut);
    }

    void showOsd(const QString& channelName, const QString& category, int index, int total, int volume) {
        m_channelName = channelName;
        m_category = category;
        m_index = index;
        m_total = total;
        m_volume = volume;
        m_opacityEffect->setOpacity(1.0);
        show();
        raise();
        update();
        m_hideTimer->start(OSD_DISPLAY_MS);
    }

    void showVolumeOsd(int volume) {
        m_showVolumeOnly = true;
        m_volume = volume;
        m_opacityEffect->setOpacity(1.0);
        show();
        raise();
        update();
        m_hideTimer->start(1500);
    }

    void fadeOut() {
        QPropertyAnimation* anim = new QPropertyAnimation(m_opacityEffect, "opacity", this);
        anim->setDuration(500);
        anim->setStartValue(1.0);
        anim->setEndValue(0.0);
        connect(anim, &QPropertyAnimation::finished, this, [this]() {
            hide();
            m_showVolumeOnly = false;
        });
        anim->start(QAbstractAnimation::DeleteWhenStopped);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);

        if (m_showVolumeOnly) {
            paintVolumeBar(p);
            return;
        }

        // Channel info OSD
        int boxW = qMin(width() - 60, 560);
        int boxH = 100;
        int x = (width() - boxW) / 2;
        int y = height() - boxH - 50;

        QPainterPath bgPath;
        bgPath.addRoundedRect(x, y, boxW, boxH, 18, 18);

        QLinearGradient bgGrad(x, y, x + boxW, y + boxH);
        bgGrad.setColorAt(0, QColor(15, 15, 35, 220));
        bgGrad.setColorAt(1, QColor(10, 10, 25, 220));
        p.fillPath(bgPath, bgGrad);
        p.setPen(QPen(QColor(255, 255, 255, 20), 1));
        p.drawPath(bgPath);

        // Accent bar
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(99, 102, 241));
        p.drawRoundedRect(x + 18, y + 16, 4, boxH - 32, 2, 2);

        // Channel number
        QFont f = font();
        f.setPixelSize(28);
        f.setBold(true);
        p.setFont(f);
        p.setPen(QColor(99, 140, 255));
        QString chNum = QString::number(m_index + 1);
        int numW = p.fontMetrics().horizontalAdvance(chNum);
        p.drawText(x + 32, y + 14, numW + 10, 36, Qt::AlignLeft | Qt::AlignVCenter, chNum);

        // Channel name
        p.setPen(Qt::white);
        f.setPixelSize(19);
        p.setFont(f);
        int nameX = x + 32 + numW + 16;
        QString elidedName = p.fontMetrics().elidedText(m_channelName, Qt::ElideRight, boxW - nameX + x - 20);
        p.drawText(nameX, y + 14, boxW - (nameX - x) - 20, 36, Qt::AlignLeft | Qt::AlignVCenter, elidedName);

        // Info line
        f.setPixelSize(12);
        f.setBold(false);
        p.setFont(f);
        p.setPen(QColor(150, 165, 200));
        QString info = m_category;
        if (m_total > 0) info += QString("  |  %1 of %2 channels").arg(m_index + 1).arg(m_total);
        p.drawText(x + 32, y + 54, boxW - 60, 22, Qt::AlignLeft | Qt::AlignVCenter, info);

        // Volume bar at bottom of OSD
        int barX = x + 32;
        int barY = y + boxH - 18;
        int barW = boxW - 64;
        int barH = 4;
        QPainterPath barBg;
        barBg.addRoundedRect(barX, barY, barW, barH, 2, 2);
        p.fillPath(barBg, QColor(255, 255, 255, 25));
        int fillW = (int)(barW * qBound(0, m_volume, 150) / 150.0);
        if (fillW > 0) {
            QPainterPath barFill;
            barFill.addRoundedRect(barX, barY, fillW, barH, 2, 2);
            p.fillPath(barFill, QColor(99, 102, 241));
        }
    }

    void paintVolumeBar(QPainter& p) {
        int boxW = 220;
        int boxH = 60;
        int x = (width() - boxW) / 2;
        int y = height() - boxH - 50;

        QPainterPath bg;
        bg.addRoundedRect(x, y, boxW, boxH, 14, 14);
        p.fillPath(bg, QColor(15, 15, 35, 220));
        p.setPen(QPen(QColor(255, 255, 255, 20), 1));
        p.drawPath(bg);

        // Volume text
        QFont f = font();
        f.setPixelSize(14);
        f.setBold(true);
        p.setFont(f);
        p.setPen(Qt::white);
        p.drawText(x, y + 6, boxW, 24, Qt::AlignCenter, QString("Volume: %1%").arg(m_volume));

        // Bar
        int barX = x + 20;
        int barY = y + 36;
        int barW = boxW - 40;
        int barH = 6;
        QPainterPath barBg;
        barBg.addRoundedRect(barX, barY, barW, barH, 3, 3);
        p.fillPath(barBg, QColor(255, 255, 255, 30));
        int fillW = (int)(barW * qBound(0, m_volume, 150) / 150.0);
        if (fillW > 0) {
            QPainterPath barFill;
            barFill.addRoundedRect(barX, barY, fillW, barH, 3, 3);
            QLinearGradient fillGrad(barX, barY, barX + fillW, barY);
            fillGrad.setColorAt(0, QColor(99, 102, 241));
            fillGrad.setColorAt(1, QColor(139, 92, 246));
            p.fillPath(barFill, fillGrad);
        }
    }

private:
    QTimer* m_hideTimer;
    QGraphicsOpacityEffect* m_opacityEffect;
    QString m_channelName;
    QString m_category;
    int m_index;
    int m_total;
    int m_volume;
    bool m_showVolumeOnly;
};

// ─── Video Widget ────────────────────────────────────────────────

class VideoWidget : public QWidget {
    Q_OBJECT
public:
    explicit VideoWidget(QWidget* parent = nullptr) : QWidget(parent) {
        setAttribute(Qt::WA_DontCreateNativeAncestors);
        setAttribute(Qt::WA_NativeWindow);
        setMinimumSize(320, 240);
        setStyleSheet("background-color: #000;");
        setFocusPolicy(Qt::NoFocus);
        setMouseTracking(true);
    }

signals:
    void doubleClicked();

protected:
    void mouseDoubleClickEvent(QMouseEvent* event) override {
        emit doubleClicked();
        QWidget::mouseDoubleClickEvent(event);
    }
};

// ─── Status Indicator ────────────────────────────────────────────

class StatusIndicator : public QWidget {
    Q_OBJECT
public:
    enum Status { Offline, Connecting, Online, Error };

    explicit StatusIndicator(QWidget* parent = nullptr)
        : QWidget(parent), m_status(Offline), m_dotColor(239, 68, 68), m_pulsePhase(false) {
        setFixedSize(140, 34);
        m_statusText = "Offline";
        m_pulseTimer = new QTimer(this);
        m_pulseTimer->setInterval(800);
        connect(m_pulseTimer, &QTimer::timeout, this, [this]() {
            m_pulsePhase = !m_pulsePhase;
            update();
        });
    }

    void setStatus(Status s) {
        m_status = s;
        switch (s) {
            case Offline:
                m_dotColor = QColor(120, 120, 140);
                m_statusText = "Offline";
                m_pulseTimer->stop();
                break;
            case Connecting:
                m_dotColor = QColor(251, 191, 36);
                m_statusText = "Connecting...";
                m_pulseTimer->start();
                break;
            case Online:
                m_dotColor = QColor(34, 197, 94);
                m_statusText = "Live";
                m_pulseTimer->stop();
                break;
            case Error:
                m_dotColor = QColor(239, 68, 68);
                m_statusText = "Error";
                m_pulseTimer->stop();
                break;
        }
        m_pulsePhase = false;
        update();
    }

    Status status() const { return m_status; }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);

        QPainterPath bg;
        bg.addRoundedRect(QRectF(rect().adjusted(1, 2, -1, -2)), 15, 15);
        p.fillPath(bg, QColor(20, 22, 38, 220));
        p.setPen(QPen(QColor(255, 255, 255, 12), 1));
        p.drawPath(bg);

        int dotX = 18;
        int dotY = height() / 2;

        // Outer pulse ring
        if ((m_status == Connecting || m_status == Online) && m_pulsePhase) {
            p.setPen(Qt::NoPen);
            QColor pulse = m_dotColor;
            pulse.setAlpha(40);
            p.setBrush(pulse);
            p.drawEllipse(QPoint(dotX, dotY), 9, 9);
        }

        // Inner dot with slight glow
        QRadialGradient dotGlow(dotX, dotY, 8);
        dotGlow.setColorAt(0, m_dotColor);
        dotGlow.setColorAt(0.6, m_dotColor);
        QColor outer = m_dotColor;
        outer.setAlpha(0);
        dotGlow.setColorAt(1.0, outer);
        p.setPen(Qt::NoPen);
        p.setBrush(dotGlow);
        p.drawEllipse(QPoint(dotX, dotY), 8, 8);

        // Solid center
        p.setBrush(m_dotColor);
        p.drawEllipse(QPoint(dotX, dotY), 4, 4);

        p.setPen(QColor(210, 215, 230));
        QFont f = font();
        f.setPixelSize(11);
        f.setBold(true);
        p.setFont(f);
        p.drawText(QRect(32, 0, width() - 38, height()), Qt::AlignLeft | Qt::AlignVCenter, m_statusText);
    }

private:
    Status m_status;
    QColor m_dotColor;
    QString m_statusText;
    QTimer* m_pulseTimer;
    bool m_pulsePhase;
};

// ─── Loading Spinner Widget ──────────────────────────────────────

class LoadingSpinner : public QWidget {
    Q_OBJECT
public:
    explicit LoadingSpinner(QWidget* parent = nullptr) : QWidget(parent), m_angle(0) {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
        setFixedSize(80, 80);
        hide();
        m_timer = new QTimer(this);
        m_timer->setInterval(30);
        connect(m_timer, &QTimer::timeout, this, [this]() {
            m_angle = (m_angle + 8) % 360;
            update();
        });
    }

    void startSpinning() { show(); raise(); m_timer->start(); }
    void stopSpinning() { m_timer->stop(); hide(); }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.translate(width() / 2, height() / 2);
        p.rotate(m_angle);

        QPen pen(QColor(99, 102, 241), 3, Qt::SolidLine, Qt::RoundCap);
        p.setPen(pen);
        p.drawArc(-15, -15, 30, 30, 0, 270 * 16);
    }

private:
    QTimer* m_timer;
    int m_angle;
};

// ─── Main Window ─────────────────────────────────────────────────

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr) : QMainWindow(parent),
        m_mpv(NULL), m_mpvOk(false), m_nam(NULL), m_logoNam(NULL),
        m_downloadedBytes(0), m_headerBar(NULL), m_leftPanel(NULL),
        m_searchEdit(NULL), m_nowPlayingLabel(NULL), m_channelCountLabel(NULL),
        m_volumeLabel(NULL), m_fullscreenBtn(NULL), m_statusIndicator(NULL),
        m_categoryList(NULL), m_channelView(NULL), m_videoWidget(NULL),
        m_osd(NULL), m_errorOverlay(NULL), m_loadingSpinner(NULL),
        m_vertSplitter(NULL), m_channelModel(NULL),
        m_proxyModel(NULL), m_delegate(NULL), m_parserThread(NULL),
        m_activeLogoDownloads(0),
        m_debounceTimer(NULL), m_autoHideTimer(NULL), m_searchDebounce(NULL),
        m_statusCheckTimer(NULL), m_retryTimer(NULL),
        m_pendingIndex(0), m_pendingTotal(0),
        m_volume(100), m_muted(false), m_isFullscreen(false),
        m_retryCount(0)
    {
        setWindowTitle("Live TV Player");
        resize(1280, 720);
        setMinimumSize(900, 550);
        setMouseTracking(true);

        m_nam = new QNetworkAccessManager(this);
        m_logoNam = new QNetworkAccessManager(this);

        m_parserThread = new M3uParserThread(this);
        connect(m_parserThread, &M3uParserThread::parsingFinished,
                this, &MainWindow::onParsingFinished, Qt::QueuedConnection);

        m_debounceTimer = new QTimer(this);
        m_debounceTimer->setSingleShot(true);
        m_debounceTimer->setInterval(DEBOUNCE_MS);
        connect(m_debounceTimer, &QTimer::timeout, this, &MainWindow::doPlayChannel);

        m_autoHideTimer = new QTimer(this);
        m_autoHideTimer->setSingleShot(true);
        m_autoHideTimer->setInterval(AUTOHIDE_MS);
        connect(m_autoHideTimer, &QTimer::timeout, this, &MainWindow::hidePanels);

        m_searchDebounce = new QTimer(this);
        m_searchDebounce->setSingleShot(true);
        m_searchDebounce->setInterval(200);
        connect(m_searchDebounce, &QTimer::timeout, this, &MainWindow::applySearch);

        m_statusCheckTimer = new QTimer(this);
        m_statusCheckTimer->setInterval(STATUS_CHECK_MS);
        connect(m_statusCheckTimer, &QTimer::timeout, this, &MainWindow::checkOnlineStatus);

        m_retryTimer = new QTimer(this);
        m_retryTimer->setSingleShot(true);
        connect(m_retryTimer, &QTimer::timeout, this, &MainWindow::retryCurrentChannel);

        setupUi();
        loadSettings();
        setupMpv();
        applyModernTheme();

        QTimer::singleShot(200, this, [this]() {
            fetchPlaylist(QString::fromLatin1(PLAYLIST_URL));
        });

        m_statusCheckTimer->start();
    }

    ~MainWindow() override {
        saveSettings();
        if (m_parserThread && m_parserThread->isRunning()) {
            m_parserThread->quit();
            m_parserThread->wait(2000);
        }
        if (m_mpv) {
            mpv_terminate_destroy(m_mpv);
            m_mpv = NULL;
        }
    }

protected:
    void keyPressEvent(QKeyEvent* event) override {
        resetAutoHide();
        switch (event->key()) {
            case Qt::Key_F11:
            case Qt::Key_F:
                toggleFullscreen();
                break;
            case Qt::Key_Escape:
                if (m_errorOverlay && m_errorOverlay->isVisible()) {
                    m_errorOverlay->fadeOut();
                } else if (m_isFullscreen) {
                    exitFullscreen();
                }
                break;
            case Qt::Key_Up:
                zapChannel(-1);
                break;
            case Qt::Key_Down:
                zapChannel(1);
                break;
            case Qt::Key_Left:
                changeVolume(-5);
                break;
            case Qt::Key_Right:
                changeVolume(5);
                break;
            case Qt::Key_M:
                toggleMute();
                break;
            case Qt::Key_Space:
                togglePause();
                break;
            case Qt::Key_Tab:
                toggleSidebar();
                break;
            case Qt::Key_R:
                retryCurrentChannel();
                break;
            default:
                QMainWindow::keyPressEvent(event);
        }
    }

    void mouseMoveEvent(QMouseEvent* event) override {
        QMainWindow::mouseMoveEvent(event);
        resetAutoHide();
    }

    void resizeEvent(QResizeEvent* event) override {
        QMainWindow::resizeEvent(event);
        updateOverlayGeometry();
    }

    bool eventFilter(QObject* obj, QEvent* event) override {
        if (event->type() == QEvent::MouseMove) resetAutoHide();
        return QMainWindow::eventFilter(obj, event);
    }

private slots:
    void onParsingFinished(QVector<Channel> channels, QStringList categories) {
        if (m_loadingSpinner) m_loadingSpinner->stopSpinning();

        if (channels.isEmpty()) {
            statusBar()->showMessage("No valid channels found.");
            m_statusIndicator->setStatus(StatusIndicator::Offline);
            return;
        }

        m_channelModel->setChannels(channels);

        m_categoryList->blockSignals(true);
        m_categoryList->clear();
        for (int i = 0; i < categories.size(); ++i) {
            m_categoryList->addItem(categories[i]);
        }
        m_categoryList->blockSignals(false);

        int catIdx = 0;
        if (!m_currentCategory.isEmpty()) {
            for (int i = 0; i < m_categoryList->count(); ++i) {
                if (m_categoryList->item(i)->text() == m_currentCategory) {
                    catIdx = i;
                    break;
                }
            }
        }
        m_categoryList->setCurrentRow(catIdx);
        onCategoryChanged(catIdx);

        statusBar()->showMessage(QString("Loaded %1 channels | %2 categories").arg(channels.size()).arg(categories.size() - 1));
        if (m_currentStreamUrl.isEmpty()) {
            m_statusIndicator->setStatus(StatusIndicator::Online);
        }
        updateChannelCount();
        scheduleLogoDownloads();
    }

    void onCategoryChanged(int row) {
        if (row < 0 || row >= m_categoryList->count()) return;
        QString cat = m_categoryList->item(row)->text();
        m_proxyModel->setCategoryFilter(cat);
        m_currentCategory = cat;
        updateChannelCount();
    }

    void onChannelClicked(const QModelIndex& index) {
        if (!index.isValid()) return;
        m_pendingStreamUrl = index.data(StreamUrlRole).toString();
        m_pendingChannelName = index.data(NameRole).toString();
        m_pendingCategory = index.data(CategoryRole).toString();
        m_pendingIndex = m_channelView->currentIndex().row();
        m_pendingTotal = m_proxyModel->rowCount();
        m_retryCount = 0;
        m_debounceTimer->start();
    }

    void doPlayChannel() {
        if (m_pendingStreamUrl.isEmpty()) return;
        if (m_errorOverlay && m_errorOverlay->isVisible()) m_errorOverlay->hide();
        m_statusIndicator->setStatus(StatusIndicator::Connecting);
        if (m_loadingSpinner) m_loadingSpinner->startSpinning();
        playStream(m_pendingStreamUrl);
        m_currentChannelName = m_pendingChannelName;
        m_currentStreamUrl = m_pendingStreamUrl;
        m_nowPlayingLabel->setText("  > " + m_currentChannelName);

        if (m_delegate) {
            m_delegate->setActiveChannel(m_currentStreamUrl);
            if (m_channelView && m_channelView->viewport())
                m_channelView->viewport()->update();
        }

        if (m_osd) m_osd->showOsd(m_pendingChannelName, m_pendingCategory, m_pendingIndex, m_pendingTotal, m_volume);
    }

    void retryCurrentChannel() {
        if (m_currentStreamUrl.isEmpty()) return;
        m_pendingStreamUrl = m_currentStreamUrl;
        m_pendingChannelName = m_currentChannelName;
        m_retryCount = 0;
        if (m_errorOverlay && m_errorOverlay->isVisible()) m_errorOverlay->hide();
        m_statusIndicator->setStatus(StatusIndicator::Connecting);
        if (m_loadingSpinner) m_loadingSpinner->startSpinning();
        playStream(m_currentStreamUrl);
        statusBar()->showMessage("Retrying: " + m_currentChannelName);
    }

    void onSearchChanged(const QString&) { m_searchDebounce->start(); }

    void applySearch() {
        m_proxyModel->setSearchFilter(m_searchEdit->text().trimmed());
        updateChannelCount();
    }

    void onMpvWakeup() {
        while (m_mpv) {
            mpv_event* event = mpv_wait_event(m_mpv, 0);
            if (!event || event->event_id == MPV_EVENT_NONE) break;
            switch (event->event_id) {
                case MPV_EVENT_SHUTDOWN:
                    break;
                case MPV_EVENT_END_FILE: {
                    mpv_event_end_file* ef = static_cast<mpv_event_end_file*>(event->data);
                    if (ef && ef->reason == MPV_END_FILE_REASON_ERROR) {
                        if (m_loadingSpinner) m_loadingSpinner->stopSpinning();

                        if (m_retryCount < MAX_RETRIES) {
                            m_retryCount++;
                            statusBar()->showMessage(QString("Retry %1/%2: %3").arg(m_retryCount).arg(MAX_RETRIES).arg(m_currentChannelName));
                            m_retryTimer->start(RETRY_DELAY_MS);
                        } else {
                            m_statusIndicator->setStatus(StatusIndicator::Error);
                            statusBar()->showMessage("Channel unavailable: " + m_currentChannelName);
                            showPlaybackError(m_currentChannelName);
                        }
                    }
                    break;
                }
                case MPV_EVENT_FILE_LOADED:
                    if (m_loadingSpinner) m_loadingSpinner->stopSpinning();
                    m_retryCount = 0;
                    m_statusIndicator->setStatus(StatusIndicator::Online);
                    statusBar()->showMessage("Playing: " + m_currentChannelName);
                    break;
                case MPV_EVENT_PROPERTY_CHANGE:
                    break;
                default:
                    break;
            }
        }
    }

    void toggleSidebar() {
        m_leftPanel->setVisible(!m_leftPanel->isVisible());
    }

    void hidePanels() {
        if (!m_isFullscreen) return;
        if (m_leftPanel) m_leftPanel->hide();
        if (m_headerBar) m_headerBar->hide();
        if (m_channelView) m_channelView->hide();
        setCursor(Qt::BlankCursor);
    }

    void showPanels() {
        if (m_leftPanel) m_leftPanel->show();
        if (m_headerBar) m_headerBar->show();
        if (m_channelView) m_channelView->show();
        setCursor(Qt::ArrowCursor);
    }

    void resetAutoHide() {
        showPanels();
        if (m_isFullscreen) m_autoHideTimer->start();
    }

    void checkOnlineStatus() {
        QUrl checkUrl(QString::fromLatin1(PLAYLIST_URL));
        QNetworkRequest req;
        req.setUrl(checkUrl);
        req.setRawHeader("User-Agent", "LiveTVPlayer/2.0");
        QNetworkReply* reply = m_nam->head(req);
        connect(reply, &QNetworkReply::finished, this, [reply]() {
            reply->deleteLater();
        });
    }

    void toggleFullscreen() {
        if (m_isFullscreen) exitFullscreen();
        else enterFullscreen();
    }

    void enterFullscreen() {
        m_isFullscreen = true;
        m_savedSplitterState = m_vertSplitter->saveState();
        m_channelView->hide();
        m_leftPanel->hide();
        m_headerBar->hide();
        showFullScreen();
        m_fullscreenBtn->setText("Exit FS");
        m_autoHideTimer->start();
        updateOverlayGeometry();
    }

    void exitFullscreen() {
        m_isFullscreen = false;
        m_autoHideTimer->stop();
        showNormal();
        showPanels();
        m_channelView->show();
        if (!m_savedSplitterState.isEmpty())
            m_vertSplitter->restoreState(m_savedSplitterState);
        m_fullscreenBtn->setText("Fullscreen");
        setCursor(Qt::ArrowCursor);
        updateOverlayGeometry();
    }

private:
    void updateOverlayGeometry() {
        if (m_osd) m_osd->setGeometry(m_videoWidget->rect());
        if (m_errorOverlay) m_errorOverlay->setGeometry(m_videoWidget->rect());
        if (m_loadingSpinner) {
            int sx = (m_videoWidget->width() - m_loadingSpinner->width()) / 2;
            int sy = (m_videoWidget->height() - m_loadingSpinner->height()) / 2;
            m_loadingSpinner->move(sx, sy);
        }
    }

    void showPlaybackError(const QString& channelName) {
        if (m_errorOverlay) {
            m_errorOverlay->setGeometry(m_videoWidget->rect());
            m_errorOverlay->showError(channelName);
        }
    }

    void setupUi() {
        QWidget* central = new QWidget(this);
        setCentralWidget(central);
        QVBoxLayout* rootLayout = new QVBoxLayout(central);
        rootLayout->setContentsMargins(0, 0, 0, 0);
        rootLayout->setSpacing(0);

        // ── Header Bar ──
        m_headerBar = new QWidget(central);
        m_headerBar->setFixedHeight(54);
        m_headerBar->setObjectName("headerBar");
        QHBoxLayout* headerLayout = new QHBoxLayout(m_headerBar);
        headerLayout->setContentsMargins(20, 0, 20, 0);
        headerLayout->setSpacing(14);

        QLabel* appTitle = new QLabel("LIVE TV", m_headerBar);
        appTitle->setObjectName("appTitle");
        headerLayout->addWidget(appTitle);

        headerLayout->addSpacing(16);

        m_searchEdit = new QLineEdit(m_headerBar);
        m_searchEdit->setPlaceholderText("Search channels...");
        m_searchEdit->setObjectName("searchEdit");
        m_searchEdit->setMaximumWidth(340);
        m_searchEdit->setMinimumWidth(200);
        m_searchEdit->setClearButtonEnabled(true);
        connect(m_searchEdit, &QLineEdit::textChanged, this, &MainWindow::onSearchChanged);
        headerLayout->addWidget(m_searchEdit);

        headerLayout->addStretch();

        m_nowPlayingLabel = new QLabel("No channel selected", m_headerBar);
        m_nowPlayingLabel->setObjectName("nowPlaying");
        m_nowPlayingLabel->setMaximumWidth(300);
        headerLayout->addWidget(m_nowPlayingLabel);

        headerLayout->addStretch();

        m_channelCountLabel = new QLabel("0 channels", m_headerBar);
        m_channelCountLabel->setObjectName("channelCount");
        headerLayout->addWidget(m_channelCountLabel);

        headerLayout->addSpacing(10);

        m_statusIndicator = new StatusIndicator(m_headerBar);
        m_statusIndicator->setStatus(StatusIndicator::Offline);
        headerLayout->addWidget(m_statusIndicator);

        headerLayout->addSpacing(10);

        QPushButton* volDown = new QPushButton("Vol -", m_headerBar);
        volDown->setObjectName("headerBtn");
        volDown->setFixedSize(48, 32);
        volDown->setToolTip("Volume Down (Left Arrow)");
        connect(volDown, &QPushButton::clicked, this, [this]() { changeVolume(-5); });
        headerLayout->addWidget(volDown);

        m_volumeLabel = new QLabel("100%", m_headerBar);
        m_volumeLabel->setObjectName("volumeLabel");
        m_volumeLabel->setFixedWidth(44);
        m_volumeLabel->setAlignment(Qt::AlignCenter);
        headerLayout->addWidget(m_volumeLabel);

        QPushButton* volUp = new QPushButton("Vol +", m_headerBar);
        volUp->setObjectName("headerBtn");
        volUp->setFixedSize(48, 32);
        volUp->setToolTip("Volume Up (Right Arrow)");
        connect(volUp, &QPushButton::clicked, this, [this]() { changeVolume(5); });
        headerLayout->addWidget(volUp);

        headerLayout->addSpacing(6);

        QPushButton* muteBtn = new QPushButton("Mute", m_headerBar);
        muteBtn->setObjectName("headerBtn");
        muteBtn->setFixedSize(48, 32);
        muteBtn->setToolTip("Toggle Mute (M)");
        connect(muteBtn, &QPushButton::clicked, this, [this]() { toggleMute(); });
        headerLayout->addWidget(muteBtn);

        headerLayout->addSpacing(6);

        m_fullscreenBtn = new QPushButton("Fullscreen", m_headerBar);
        m_fullscreenBtn->setObjectName("headerBtn");
        m_fullscreenBtn->setFixedHeight(32);
        m_fullscreenBtn->setToolTip("Fullscreen (F11 / F)");
        connect(m_fullscreenBtn, &QPushButton::clicked, this, &MainWindow::toggleFullscreen);
        headerLayout->addWidget(m_fullscreenBtn);

        rootLayout->addWidget(m_headerBar);

        // ── Main Content ──
        QSplitter* hSplitter = new QSplitter(Qt::Horizontal, central);
        hSplitter->setObjectName("mainSplitter");
        hSplitter->setHandleWidth(1);

        // Left panel
        m_leftPanel = new QWidget(hSplitter);
        m_leftPanel->setObjectName("leftPanel");
        m_leftPanel->setMinimumWidth(180);
        m_leftPanel->setMaximumWidth(250);
        QVBoxLayout* leftLayout = new QVBoxLayout(m_leftPanel);
        leftLayout->setContentsMargins(10, 14, 6, 10);
        leftLayout->setSpacing(8);

        QLabel* catLabel = new QLabel("CATEGORIES", m_leftPanel);
        catLabel->setObjectName("sectionTitle");
        leftLayout->addWidget(catLabel);

        m_categoryList = new QListWidget(m_leftPanel);
        m_categoryList->setObjectName("categoryList");
        connect(m_categoryList, &QListWidget::currentRowChanged, this, &MainWindow::onCategoryChanged);
        leftLayout->addWidget(m_categoryList);

        QPushButton* refreshBtn = new QPushButton("Refresh", m_leftPanel);
        refreshBtn->setObjectName("refreshBtn");
        connect(refreshBtn, &QPushButton::clicked, this, [this]() {
            fetchPlaylist(QString::fromLatin1(PLAYLIST_URL));
        });
        leftLayout->addWidget(refreshBtn);

        hSplitter->addWidget(m_leftPanel);

        // Right panel
        QWidget* rightPanel = new QWidget(hSplitter);
        QVBoxLayout* rightLayout = new QVBoxLayout(rightPanel);
        rightLayout->setContentsMargins(0, 0, 0, 0);
        rightLayout->setSpacing(0);

        m_vertSplitter = new QSplitter(Qt::Vertical, rightPanel);
        m_vertSplitter->setHandleWidth(3);

        m_videoWidget = new VideoWidget(m_vertSplitter);
        m_videoWidget->installEventFilter(this);
        connect(m_videoWidget, &VideoWidget::doubleClicked, this, &MainWindow::toggleFullscreen);
        m_vertSplitter->addWidget(m_videoWidget);

        // Channel grid
        m_channelModel = new ChannelModel(this);
        m_proxyModel = new CategoryFilterProxy(this);
        m_proxyModel->setSourceModel(m_channelModel);

        m_channelView = new QListView(m_vertSplitter);
        m_channelView->setModel(m_proxyModel);
        m_channelView->setViewMode(QListView::IconMode);
        m_channelView->setResizeMode(QListView::Adjust);
        m_channelView->setMovement(QListView::Static);
        m_channelView->setSpacing(5);
        m_channelView->setUniformItemSizes(true);
        m_channelView->setWrapping(true);
        m_channelView->setSelectionMode(QAbstractItemView::SingleSelection);
        m_channelView->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
        m_channelView->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        m_channelView->setObjectName("channelGrid");

        m_delegate = new ChannelDelegate(this);
        m_delegate->setLogoCache(&m_logoPixmaps);
        m_channelView->setItemDelegate(m_delegate);

        connect(m_channelView, &QListView::clicked, this, &MainWindow::onChannelClicked);
        connect(m_channelView, &QListView::activated, this, &MainWindow::onChannelClicked);

        m_vertSplitter->addWidget(m_channelView);
        m_vertSplitter->setStretchFactor(0, 3);
        m_vertSplitter->setStretchFactor(1, 2);

        rightLayout->addWidget(m_vertSplitter);

        hSplitter->addWidget(rightPanel);
        hSplitter->setStretchFactor(0, 0);
        hSplitter->setStretchFactor(1, 1);
        hSplitter->setSizes(QList<int>() << 210 << 1070);

        rootLayout->addWidget(hSplitter, 1);

        // Overlays (children of video widget)
        m_osd = new OsdWidget(m_videoWidget);
        m_errorOverlay = new ErrorOverlay(m_videoWidget);
        m_loadingSpinner = new LoadingSpinner(m_videoWidget);

        connect(m_errorOverlay, &ErrorOverlay::retryClicked, this, &MainWindow::retryCurrentChannel);
        connect(m_errorOverlay, &ErrorOverlay::dismissClicked, this, [this]() {
            statusBar()->showMessage("Ready");
        });

        statusBar()->showMessage("Starting up...");
    }

    void applyModernTheme() {
        QString style =
            "* {"
            "  font-family: 'Segoe UI', 'Helvetica Neue', Arial, sans-serif;"
            "}"
            "QMainWindow, QWidget {"
            "  background-color: #0c0c18;"
            "  color: #e2e8f0;"
            "}"

            /* Header */
            "#headerBar {"
            "  background-color: #111122;"
            "}"
            "#appTitle {"
            "  font-size: 15px;"
            "  font-weight: bold;"
            "  color: #818cf8;"
            "  letter-spacing: 2px;"
            "}"
            "#searchEdit {"
            "  background-color: #181830;"
            "  color: #e2e8f0;"
            "  border: 1px solid rgba(255,255,255,18);"
            "  border-radius: 10px;"
            "  padding: 7px 14px;"
            "  font-size: 13px;"
            "}"
            "#searchEdit:focus {"
            "  border: 1px solid #6366f1;"
            "  background-color: #1c1c38;"
            "}"
            "#nowPlaying {"
            "  color: #a5b4fc;"
            "  font-size: 12px;"
            "  font-weight: bold;"
            "}"
            "#channelCount {"
            "  color: #4b5580;"
            "  font-size: 11px;"
            "}"
            "#volumeLabel {"
            "  color: #94a3b8;"
            "  font-size: 11px;"
            "  font-weight: bold;"
            "}"
            "#headerBtn {"
            "  background: rgba(255,255,255,6);"
            "  border: 1px solid rgba(255,255,255,10);"
            "  border-radius: 8px;"
            "  color: #c0c8e0;"
            "  font-size: 11px;"
            "  font-weight: bold;"
            "  padding: 2px 10px;"
            "}"
            "#headerBtn:hover {"
            "  background: rgba(99,102,241,50);"
            "  border-color: rgba(99,102,241,80);"
            "  color: #e0e7ff;"
            "}"
            "#headerBtn:pressed {"
            "  background: rgba(99,102,241,90);"
            "}"

            /* Left Panel */
            "#leftPanel {"
            "  background-color: #0e0e1c;"
            "}"
            "#sectionTitle {"
            "  font-weight: bold;"
            "  font-size: 11px;"
            "  color: #4b5580;"
            "  letter-spacing: 2px;"
            "  padding: 4px 8px;"
            "}"
            "#categoryList {"
            "  background-color: transparent;"
            "  border: none;"
            "  outline: none;"
            "  font-size: 13px;"
            "}"
            "#categoryList::item {"
            "  padding: 10px 14px;"
            "  border-radius: 10px;"
            "  margin: 2px 4px;"
            "  color: #8890b0;"
            "}"
            "#categoryList::item:selected {"
            "  background-color: rgba(99,102,241,25);"
            "  color: #c7d2fe;"
            "  border-left: 3px solid #6366f1;"
            "}"
            "#categoryList::item:hover {"
            "  background-color: rgba(255,255,255,4);"
            "  color: #c0c8e0;"
            "}"
            "#refreshBtn {"
            "  background: rgba(99,102,241,18);"
            "  border: 1px solid rgba(99,102,241,30);"
            "  border-radius: 10px;"
            "  color: #818cf8;"
            "  padding: 10px;"
            "  font-size: 12px;"
            "  font-weight: bold;"
            "}"
            "#refreshBtn:hover {"
            "  background: rgba(99,102,241,40);"
            "  color: #c7d2fe;"
            "}"

            /* Channel Grid */
            "#channelGrid {"
            "  background-color: #0c0c18;"
            "  border: none;"
            "}"

            /* Splitter */
            "QSplitter::handle {"
            "  background-color: rgba(255,255,255,5);"
            "}"
            "QSplitter::handle:horizontal { width: 1px; }"
            "QSplitter::handle:vertical { height: 3px; }"
            "QSplitter::handle:hover {"
            "  background-color: rgba(99,102,241,60);"
            "}"

            /* Status Bar */
            "QStatusBar {"
            "  background-color: #080812;"
            "  color: #3b4470;"
            "  font-size: 11px;"
            "  padding: 2px 16px;"
            "  border-top: 1px solid rgba(255,255,255,4);"
            "}"

            /* Scrollbars */
            "QScrollBar:vertical {"
            "  background: transparent;"
            "  width: 6px;"
            "  margin: 0;"
            "}"
            "QScrollBar::handle:vertical {"
            "  background: rgba(99,102,241,30);"
            "  border-radius: 3px;"
            "  min-height: 40px;"
            "}"
            "QScrollBar::handle:vertical:hover {"
            "  background: rgba(99,102,241,60);"
            "}"
            "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {"
            "  height: 0;"
            "}"
            "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical {"
            "  background: transparent;"
            "}";
        qApp->setStyleSheet(style);
    }

    void setupMpv() {
        m_mpv = mpv_create();
        if (!m_mpv) {
            m_mpvOk = false;
            return;
        }

        mpv_set_option_string(m_mpv, "vo", "gpu");
        mpv_set_option_string(m_mpv, "hwdec", "auto-safe");
        mpv_set_option_string(m_mpv, "gpu-context", "auto");

#ifdef Q_OS_WIN
        mpv_set_option_string(m_mpv, "ao", "wasapi,sdl,openal");
#elif defined(Q_OS_LINUX)
        mpv_set_option_string(m_mpv, "ao", "pulse,alsa,sdl");
#elif defined(Q_OS_MAC)
        mpv_set_option_string(m_mpv, "ao", "coreaudio,sdl");
#else
        mpv_set_option_string(m_mpv, "ao", "auto");
#endif

        mpv_set_option_string(m_mpv, "audio", "yes");
        mpv_set_option_string(m_mpv, "mute", "no");

        QByteArray volStr = QString::number(m_volume).toUtf8();
        mpv_set_option_string(m_mpv, "volume", volStr.constData());

        mpv_set_option_string(m_mpv, "keep-open", "yes");
        mpv_set_option_string(m_mpv, "idle", "yes");
        mpv_set_option_string(m_mpv, "input-default-bindings", "no");
        mpv_set_option_string(m_mpv, "input-vo-keyboard", "no");
        mpv_set_option_string(m_mpv, "osc", "no");
        mpv_set_option_string(m_mpv, "osd-level", "0");

        mpv_set_option_string(m_mpv, "cache", "yes");
        mpv_set_option_string(m_mpv, "demuxer-max-bytes", "80MiB");
        mpv_set_option_string(m_mpv, "demuxer-max-back-bytes", "20MiB");
        mpv_set_option_string(m_mpv, "cache-secs", "15");
        mpv_set_option_string(m_mpv, "network-timeout", "15");
        mpv_set_option_string(m_mpv, "stream-buffer-size", "2MiB");

        // Faster startup
        mpv_set_option_string(m_mpv, "demuxer-lavf-analyzeduration", "2");
        mpv_set_option_string(m_mpv, "demuxer-lavf-probesize", "500000");
        mpv_set_option_string(m_mpv, "untimed", "no");

        int64_t wid = static_cast<int64_t>(m_videoWidget->winId());
        mpv_set_option(m_mpv, "wid", MPV_FORMAT_INT64, &wid);

        int err = mpv_initialize(m_mpv);
        if (err < 0) {
            mpv_terminate_destroy(m_mpv);
            m_mpv = NULL;
            m_mpvOk = false;
            return;
        }

        // Post-init property setting
        int64_t vol = m_volume;
        mpv_set_property(m_mpv, "volume", MPV_FORMAT_INT64, &vol);
        int muteFlag = m_muted ? 1 : 0;
        mpv_set_property(m_mpv, "mute", MPV_FORMAT_FLAG, &muteFlag);
        mpv_set_property_string(m_mpv, "audio-device", "auto");

        mpv_observe_property(m_mpv, 0, "volume", MPV_FORMAT_DOUBLE);
        mpv_observe_property(m_mpv, 0, "mute", MPV_FORMAT_FLAG);

        mpv_set_wakeup_callback(m_mpv, [](void* ctx) {
            QMetaObject::invokeMethod(static_cast<MainWindow*>(ctx), "onMpvWakeup", Qt::QueuedConnection);
        }, this);

        m_mpvOk = true;
    }

    void loadSettings() {
        QSettings s("LiveTVPlayer", "LiveTVPlayer");
        m_currentCategory = s.value("lastCategory", "All").toString();
        m_volume = qBound(0, s.value("volume", 100).toInt(), 150);
        m_muted = s.value("muted", false).toBool();
        m_lastStreamUrl = s.value("lastStream", "").toString();
        updateVolumeLabel();
    }

    void saveSettings() {
        QSettings s("LiveTVPlayer", "LiveTVPlayer");
        s.setValue("lastCategory", m_currentCategory);
        s.setValue("volume", m_volume);
        s.setValue("muted", m_muted);
        if (!m_currentStreamUrl.isEmpty()) s.setValue("lastStream", m_currentStreamUrl);
    }

    void fetchPlaylist(const QString& urlStr) {
        QUrl url(urlStr);
        if (!url.isValid()) return;

        m_statusIndicator->setStatus(StatusIndicator::Connecting);
        statusBar()->showMessage("Loading playlist...");
        if (m_loadingSpinner) m_loadingSpinner->startSpinning();

        QNetworkRequest req;
        req.setUrl(url);
        req.setRawHeader("User-Agent", "LiveTVPlayer/2.0");

        QNetworkReply* reply = m_nam->get(req);

        QTimer* timeout = new QTimer(this);
        timeout->setSingleShot(true);
        connect(timeout, &QTimer::timeout, this, [reply, timeout]() {
            if (reply && reply->isRunning()) reply->abort();
            timeout->deleteLater();
        });
        timeout->start(PLAYLIST_TIMEOUT_MS);

        m_downloadedBytes = 0;
        connect(reply, &QNetworkReply::readyRead, this, [this, reply]() {
            m_downloadedBytes += reply->bytesAvailable();
            if (m_downloadedBytes > MAX_DOWNLOAD_SIZE) reply->abort();
        });

        connect(reply, &QNetworkReply::finished, this, [this, reply, timeout]() {
            timeout->stop();
            timeout->deleteLater();

            if (reply->error() != QNetworkReply::NoError) {
                if (m_loadingSpinner) m_loadingSpinner->stopSpinning();
                m_statusIndicator->setStatus(StatusIndicator::Offline);
                statusBar()->showMessage("Failed to load playlist - check connection");
                reply->deleteLater();
                return;
            }

            QByteArray data = reply->readAll();
            reply->deleteLater();

            if (data.size() > MAX_DOWNLOAD_SIZE) {
                if (m_loadingSpinner) m_loadingSpinner->stopSpinning();
                statusBar()->showMessage("Playlist too large.");
                return;
            }

            statusBar()->showMessage("Parsing playlist...");
            m_parserThread->parseData(data);
        });
    }

    void updateChannelCount() {
        int count = m_proxyModel ? m_proxyModel->rowCount() : 0;
        m_channelCountLabel->setText(QString("%1 channel%2").arg(count).arg(count != 1 ? "s" : ""));
    }

    void updateVolumeLabel() {
        if (m_volumeLabel) m_volumeLabel->setText(QString("%1%").arg(m_volume));
    }

    void scheduleLogoDownloads() {
        m_logoPending.clear();
        m_activeLogoDownloads = 0;

        const QVector<Channel>& ch = m_channelModel->channels();
        QSet<QString> queued;
        for (int i = 0; i < ch.size(); ++i) {
            const QString& logoUrl = ch[i].logoUrl;
            if (!logoUrl.isEmpty() && !m_logoPixmaps.contains(logoUrl) && !queued.contains(logoUrl)) {
                QUrl u(logoUrl);
                if (u.isValid() && (u.scheme() == "http" || u.scheme() == "https")) {
                    m_logoPending.append(logoUrl);
                    queued.insert(logoUrl);
                }
            }
        }
        downloadNextLogos();
    }

    void downloadNextLogos() {
        while (m_activeLogoDownloads < MAX_CONCURRENT_DOWNLOADS && !m_logoPending.isEmpty()) {
            downloadLogo(m_logoPending.takeFirst());
        }
    }

    void downloadLogo(const QString& urlStr) {
        QUrl url(urlStr);
        if (!url.isValid()) return;

        QNetworkRequest req;
        req.setUrl(url);
        req.setRawHeader("User-Agent", "LiveTVPlayer/2.0");

        QNetworkReply* reply = m_logoNam->get(req);
        m_activeLogoDownloads++;

        QTimer* timeout = new QTimer(this);
        timeout->setSingleShot(true);
        connect(timeout, &QTimer::timeout, this, [reply, timeout]() {
            if (reply && reply->isRunning()) reply->abort();
            timeout->deleteLater();
        });
        timeout->start(IMAGE_TIMEOUT_MS);

        connect(reply, &QNetworkReply::finished, this, [this, reply, urlStr, timeout]() {
            timeout->stop();
            timeout->deleteLater();
            m_activeLogoDownloads--;

            if (reply->error() == QNetworkReply::NoError) {
                QByteArray imgData = reply->readAll();
                if (imgData.size() < 2 * 1024 * 1024 && !imgData.isEmpty()) {
                    QPixmap pm;
                    if (pm.loadFromData(imgData)) {
                        m_logoPixmaps.insert(urlStr, pm.scaled(56, 44, Qt::KeepAspectRatio, Qt::SmoothTransformation));
                    }
                }
            }

            reply->deleteLater();
            downloadNextLogos();

            if (m_channelView && m_channelView->viewport())
                m_channelView->viewport()->update();
        });
    }

    void playStream(const QString& url) {
        if (!m_mpvOk || !m_mpv || url.isEmpty()) {
            statusBar()->showMessage("Playback unavailable.");
            return;
        }

        if (!m_muted) {
            int muteOff = 0;
            mpv_set_property(m_mpv, "mute", MPV_FORMAT_FLAG, &muteOff);
        }
        int64_t vol = m_volume;
        mpv_set_property(m_mpv, "volume", MPV_FORMAT_INT64, &vol);

        QByteArray urlBytes = url.toUtf8();
        const char* cmd[] = {"loadfile", urlBytes.constData(), "replace", NULL};
        int err = mpv_command(m_mpv, cmd);
        if (err < 0) {
            statusBar()->showMessage(QString("mpv error: %1").arg(mpv_error_string(err)));
            m_statusIndicator->setStatus(StatusIndicator::Error);
            if (m_loadingSpinner) m_loadingSpinner->stopSpinning();
        }
    }

    void zapChannel(int direction) {
        if (!m_proxyModel || m_proxyModel->rowCount() == 0) return;
        int current = m_channelView->currentIndex().row();
        if (current < 0) current = 0;
        int next = current + direction;
        if (next < 0) next = m_proxyModel->rowCount() - 1;
        if (next >= m_proxyModel->rowCount()) next = 0;
        QModelIndex idx = m_proxyModel->index(next, 0);
        m_channelView->setCurrentIndex(idx);
        m_channelView->scrollTo(idx);
        onChannelClicked(idx);
    }

    void changeVolume(int delta) {
        m_volume = qBound(0, m_volume + delta, 150);
        if (m_mpv && m_mpvOk) {
            int64_t vol = m_volume;
            mpv_set_property(m_mpv, "volume", MPV_FORMAT_INT64, &vol);
            if (m_muted && delta > 0) {
                m_muted = false;
                int muteFlag = 0;
                mpv_set_property(m_mpv, "mute", MPV_FORMAT_FLAG, &muteFlag);
            }
        }
        updateVolumeLabel();
        if (m_osd) m_osd->showVolumeOsd(m_volume);
        statusBar()->showMessage(QString("Volume: %1%").arg(m_volume), 1500);
    }

    void toggleMute() {
        m_muted = !m_muted;
        if (m_mpv && m_mpvOk) {
            int muteFlag = m_muted ? 1 : 0;
            mpv_set_property(m_mpv, "mute", MPV_FORMAT_FLAG, &muteFlag);
        }
        statusBar()->showMessage(m_muted ? "Muted" : "Unmuted", 2000);
        if (m_osd) m_osd->showVolumeOsd(m_muted ? 0 : m_volume);
    }

    void togglePause() {
        if (!m_mpv || !m_mpvOk) return;
        int pause = 0;
        mpv_get_property(m_mpv, "pause", MPV_FORMAT_FLAG, &pause);
        pause = !pause;
        mpv_set_property(m_mpv, "pause", MPV_FORMAT_FLAG, &pause);
        statusBar()->showMessage(pause ? "Paused" : "Playing", 2000);
    }

    mpv_handle* m_mpv;
    bool m_mpvOk;

    QNetworkAccessManager* m_nam;
    QNetworkAccessManager* m_logoNam;
    int m_downloadedBytes;

    QWidget* m_headerBar;
    QWidget* m_leftPanel;
    QLineEdit* m_searchEdit;
    QLabel* m_nowPlayingLabel;
    QLabel* m_channelCountLabel;
    QLabel* m_volumeLabel;
    QPushButton* m_fullscreenBtn;
    StatusIndicator* m_statusIndicator;
    QListWidget* m_categoryList;
    QListView* m_channelView;
    VideoWidget* m_videoWidget;
    OsdWidget* m_osd;
    ErrorOverlay* m_errorOverlay;
    LoadingSpinner* m_loadingSpinner;
    QSplitter* m_vertSplitter;

    ChannelModel* m_channelModel;
    CategoryFilterProxy* m_proxyModel;
    ChannelDelegate* m_delegate;
    M3uParserThread* m_parserThread;

    QHash<QString, QPixmap> m_logoPixmaps;
    QStringList m_logoPending;
    int m_activeLogoDownloads;

    QTimer* m_debounceTimer;
    QTimer* m_autoHideTimer;
    QTimer* m_searchDebounce;
    QTimer* m_statusCheckTimer;
    QTimer* m_retryTimer;

    QString m_pendingStreamUrl;
    QString m_pendingChannelName;
    QString m_pendingCategory;
    int m_pendingIndex;
    int m_pendingTotal;

    QString m_currentChannelName;
    QString m_currentStreamUrl;
    QString m_currentCategory;
    QString m_lastStreamUrl;

    int m_volume;
    bool m_muted;
    bool m_isFullscreen;
    int m_retryCount;
    QByteArray m_savedSplitterState;
};

#include "main.moc"

#ifdef Q_OS_WIN
#include <QtPlugin>
Q_IMPORT_PLUGIN(QWindowsIntegrationPlugin)
#endif

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    app.setApplicationName("LiveTVPlayer");
    app.setOrganizationName("LiveTVPlayer");

    MainWindow w;
    w.show();

    return app.exec();
}
