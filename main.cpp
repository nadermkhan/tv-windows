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
#include <QClipboard>
#include <QGuiApplication>
#include <QScreen>
#include <QScrollBar>
#include <QPainterPath>
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
static const int DEBOUNCE_MS = 150;
static const int OSD_DISPLAY_MS = 3500;
static const int AUTOHIDE_MS = 3000;
static const int MAX_NAME_LEN = 200;
static const int STATUS_CHECK_INTERVAL_MS = 30000;

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

class ChannelDelegate : public QStyledItemDelegate {
    Q_OBJECT
public:
    explicit ChannelDelegate(QObject* parent = nullptr) : QStyledItemDelegate(parent) {}
    void setLogoCache(QHash<QString, QPixmap>* cache) { m_logoCache = cache; }

    QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const override {
        return QSize(172, 100);
    }

    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);

        QRect r = option.rect.adjusted(3, 3, -3, -3);
        QPainterPath path;
        path.addRoundedRect(QRectF(r), 10, 10);

        QColor cardBg(38, 40, 58);
        if (option.state & QStyle::State_Selected) {
            cardBg = QColor(59, 130, 246);
        } else if (option.state & QStyle::State_MouseOver) {
            cardBg = QColor(50, 54, 78);
        }

        painter->fillPath(path, cardBg);
        painter->setPen(QPen(QColor(255, 255, 255, 15), 1));
        painter->drawPath(path);

        QRect iconRect(r.left() + 10, r.top() + 8, 52, 42);
        QString logoUrl = index.data(LogoUrlRole).toString();
        QString name = index.data(NameRole).toString();
        if (name.length() > MAX_NAME_LEN) name = name.left(MAX_NAME_LEN) + "...";
        QString category = index.data(CategoryRole).toString();

        bool drawn = false;
        if (m_logoCache && !logoUrl.isEmpty() && m_logoCache->contains(logoUrl)) {
            QPixmap pm = m_logoCache->value(logoUrl);
            if (!pm.isNull()) {
                QPainterPath clipPath;
                clipPath.addRoundedRect(QRectF(iconRect), 6, 6);
                painter->setClipPath(clipPath);
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
            clipPath.addRoundedRect(QRectF(iconRect), 6, 6);
            int h = name.isEmpty() ? 200 : qAbs(name.at(0).unicode() * 47) % 360;
            QLinearGradient grad(iconRect.topLeft(), iconRect.bottomRight());
            grad.setColorAt(0, QColor::fromHsv(h, 140, 120));
            grad.setColorAt(1, QColor::fromHsv((h + 40) % 360, 120, 90));
            painter->fillPath(clipPath, grad);
            painter->setPen(QColor(255, 255, 255, 220));
            QFont f = painter->font();
            f.setPixelSize(20);
            f.setBold(true);
            painter->setFont(f);
            painter->drawText(iconRect, Qt::AlignCenter, name.isEmpty() ? "?" : name.left(1).toUpper());
        }

        painter->setPen(QColor(240, 240, 245));
        QFont nameFont = painter->font();
        nameFont.setPixelSize(11);
        nameFont.setBold(true);
        painter->setFont(nameFont);
        QRect nameRect(r.left() + 8, r.top() + 56, r.width() - 16, 18);
        QString elidedName = painter->fontMetrics().elidedText(name, Qt::ElideRight, nameRect.width());
        painter->drawText(nameRect, Qt::AlignLeft | Qt::AlignVCenter, elidedName);

        if (!category.isEmpty()) {
            painter->setPen(QColor(148, 163, 184));
            QFont catFont = nameFont;
            catFont.setPixelSize(9);
            catFont.setBold(false);
            painter->setFont(catFont);
            QRect catRect(r.left() + 8, r.top() + 76, r.width() - 16, 14);
            QString elidedCat = painter->fontMetrics().elidedText(category, Qt::ElideRight, catRect.width());
            painter->drawText(catRect, Qt::AlignLeft | Qt::AlignVCenter, elidedCat);
        }

        painter->restore();
    }

private:
    QHash<QString, QPixmap>* m_logoCache;
};

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
        connect(m_hideTimer, &QTimer::timeout, this, &QWidget::hide);
    }

    void showOsd(const QString& channelName, const QString& category, int index, int total) {
        m_channelName = channelName;
        m_category = category;
        m_index = index;
        m_total = total;
        show();
        raise();
        update();
        m_hideTimer->start(OSD_DISPLAY_MS);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);

        int boxW = qMin(width() - 60, 520);
        int boxH = 90;
        int x = (width() - boxW) / 2;
        int y = height() - boxH - 50;

        QPainterPath bgPath;
        bgPath.addRoundedRect(x, y, boxW, boxH, 16, 16);
        p.fillPath(bgPath, QColor(15, 15, 30, 210));
        p.setPen(QPen(QColor(255, 255, 255, 30), 1));
        p.drawPath(bgPath);

        p.setPen(Qt::NoPen);
        p.setBrush(QColor(99, 102, 241));
        p.drawRoundedRect(x + 16, y + 14, 4, boxH - 28, 2, 2);

        p.setPen(Qt::white);
        QFont f = font();
        f.setPixelSize(20);
        f.setBold(true);
        p.setFont(f);
        QString elidedName = p.fontMetrics().elidedText(m_channelName, Qt::ElideRight, boxW - 50);
        p.drawText(x + 30, y + 16, boxW - 50, 30, Qt::AlignLeft | Qt::AlignVCenter, elidedName);

        f.setPixelSize(13);
        f.setBold(false);
        p.setFont(f);
        p.setPen(QColor(165, 180, 210));
        QString info = m_category;
        if (m_total > 0) info += QString("  |  %1 of %2").arg(m_index + 1).arg(m_total);
        p.drawText(x + 30, y + 50, boxW - 50, 24, Qt::AlignLeft | Qt::AlignVCenter, info);
    }

private:
    QTimer* m_hideTimer;
    QString m_channelName;
    QString m_category;
    int m_index;
    int m_total;
};

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

class StatusIndicator : public QWidget {
    Q_OBJECT
public:
    enum Status { Offline, Connecting, Online };

    explicit StatusIndicator(QWidget* parent = nullptr)
        : QWidget(parent), m_status(Offline), m_dotColor(239, 68, 68), m_pulsePhase(false) {
        setFixedSize(140, 32);
        m_statusText = "Offline";
        m_pulseTimer = new QTimer(this);
        m_pulseTimer->setInterval(1200);
        connect(m_pulseTimer, &QTimer::timeout, this, [this]() {
            m_pulsePhase = !m_pulsePhase;
            update();
        });
    }

    void setStatus(Status s) {
        m_status = s;
        switch (s) {
            case Offline:
                m_dotColor = QColor(239, 68, 68);
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
                m_statusText = "Online";
                m_pulseTimer->stop();
                break;
        }
        update();
    }

    Status status() const { return m_status; }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);

        QPainterPath bg;
        bg.addRoundedRect(QRectF(rect().adjusted(1, 1, -1, -1)), 14, 14);
        p.fillPath(bg, QColor(30, 32, 48, 200));
        p.setPen(QPen(QColor(255, 255, 255, 20), 1));
        p.drawPath(bg);

        int dotX = 16;
        int dotY = height() / 2;
        if (m_status == Connecting && m_pulsePhase) {
            p.setPen(Qt::NoPen);
            QColor pulse = m_dotColor;
            pulse.setAlpha(60);
            p.setBrush(pulse);
            p.drawEllipse(QPoint(dotX, dotY), 8, 8);
        }

        p.setPen(Qt::NoPen);
        p.setBrush(m_dotColor);
        p.drawEllipse(QPoint(dotX, dotY), 5, 5);

        p.setPen(QColor(220, 225, 235));
        QFont f = font();
        f.setPixelSize(11);
        f.setBold(true);
        p.setFont(f);
        p.drawText(QRect(30, 0, width() - 36, height()), Qt::AlignLeft | Qt::AlignVCenter, m_statusText);
    }

private:
    Status m_status;
    QColor m_dotColor;
    QString m_statusText;
    QTimer* m_pulseTimer;
    bool m_pulsePhase;
};

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr) : QMainWindow(parent),
        m_mpv(NULL), m_mpvOk(false), m_nam(NULL), m_logoNam(NULL),
        m_downloadedBytes(0), m_headerBar(NULL), m_leftPanel(NULL),
        m_searchEdit(NULL), m_nowPlayingLabel(NULL), m_channelCountLabel(NULL),
        m_volumeLabel(NULL), m_fullscreenBtn(NULL), m_statusIndicator(NULL),
        m_categoryList(NULL), m_channelView(NULL), m_videoWidget(NULL),
        m_osd(NULL), m_vertSplitter(NULL), m_channelModel(NULL),
        m_proxyModel(NULL), m_delegate(NULL), m_activeLogoDownloads(0),
        m_debounceTimer(NULL), m_autoHideTimer(NULL), m_searchDebounce(NULL),
        m_statusCheckTimer(NULL), m_pendingIndex(0), m_pendingTotal(0),
        m_volume(100), m_muted(false), m_isFullscreen(false)
    {
        setWindowTitle("Live TV Player");
        resize(1280, 720);
        setMinimumSize(900, 550);

        m_nam = new QNetworkAccessManager(this);
        m_logoNam = new QNetworkAccessManager(this);

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
        m_statusCheckTimer->setInterval(STATUS_CHECK_INTERVAL_MS);
        connect(m_statusCheckTimer, &QTimer::timeout, this, &MainWindow::checkOnlineStatus);

        setupUi();
        loadSettings();
        setupMpv();
        applyModernTheme();

        QTimer::singleShot(300, this, [this]() {
            fetchPlaylist(QString::fromLatin1(PLAYLIST_URL));
        });

        m_statusCheckTimer->start();
    }

    ~MainWindow() override {
        saveSettings();
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
                if (m_isFullscreen) exitFullscreen();
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
        if (m_osd) m_osd->setGeometry(m_videoWidget->rect());
    }

    bool eventFilter(QObject* obj, QEvent* event) override {
        if (event->type() == QEvent::MouseMove) resetAutoHide();
        return QMainWindow::eventFilter(obj, event);
    }

private slots:
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
        m_debounceTimer->start();
    }

    void doPlayChannel() {
        if (m_pendingStreamUrl.isEmpty()) return;
        m_statusIndicator->setStatus(StatusIndicator::Connecting);
        playStream(m_pendingStreamUrl);
        m_currentChannelName = m_pendingChannelName;
        m_currentStreamUrl = m_pendingStreamUrl;
        m_nowPlayingLabel->setText("  > " + m_currentChannelName);
        if (m_osd) m_osd->showOsd(m_pendingChannelName, m_pendingCategory, m_pendingIndex, m_pendingTotal);
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
                        m_statusIndicator->setStatus(StatusIndicator::Offline);
                        statusBar()->showMessage("Playback error: " + m_currentChannelName);
                    }
                    break;
                }
                case MPV_EVENT_FILE_LOADED:
                    m_statusIndicator->setStatus(StatusIndicator::Online);
                    statusBar()->showMessage("Playing: " + m_currentChannelName);
                    break;
                default:
                    break;
            }
        }
    }

    void toggleSidebar() {
        if (m_leftPanel->isVisible()) {
            m_leftPanel->hide();
        } else {
            m_leftPanel->show();
        }
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
        if (m_isFullscreen) {
            m_autoHideTimer->start();
        }
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
        if (m_isFullscreen) {
            exitFullscreen();
        } else {
            enterFullscreen();
        }
    }

    void enterFullscreen() {
        m_isFullscreen = true;
        m_savedSplitterState = m_vertSplitter->saveState();
        m_channelView->hide();
        m_leftPanel->hide();
        m_headerBar->hide();
        showFullScreen();
        m_fullscreenBtn->setText("Exit FS");
        m_fullscreenBtn->setToolTip("Exit Fullscreen (F11)");
        m_autoHideTimer->start();
    }

    void exitFullscreen() {
        m_isFullscreen = false;
        m_autoHideTimer->stop();
        showNormal();
        showPanels();
        m_channelView->show();
        if (!m_savedSplitterState.isEmpty()) {
            m_vertSplitter->restoreState(m_savedSplitterState);
        }
        m_fullscreenBtn->setText("Fullscreen");
        m_fullscreenBtn->setToolTip("Fullscreen (F11)");
        setCursor(Qt::ArrowCursor);
    }

private:
    void setupUi() {
        QWidget* central = new QWidget(this);
        setCentralWidget(central);
        QVBoxLayout* rootLayout = new QVBoxLayout(central);
        rootLayout->setContentsMargins(0, 0, 0, 0);
        rootLayout->setSpacing(0);

        m_headerBar = new QWidget(central);
        m_headerBar->setFixedHeight(52);
        m_headerBar->setObjectName("headerBar");
        QHBoxLayout* headerLayout = new QHBoxLayout(m_headerBar);
        headerLayout->setContentsMargins(16, 0, 16, 0);
        headerLayout->setSpacing(12);

        QLabel* appTitle = new QLabel("Live TV", m_headerBar);
        appTitle->setObjectName("appTitle");
        headerLayout->addWidget(appTitle);

        headerLayout->addSpacing(12);

        m_searchEdit = new QLineEdit(m_headerBar);
        m_searchEdit->setPlaceholderText("Search channels...");
        m_searchEdit->setObjectName("searchEdit");
        m_searchEdit->setMaximumWidth(320);
        m_searchEdit->setMinimumWidth(180);
        m_searchEdit->setClearButtonEnabled(true);
        connect(m_searchEdit, &QLineEdit::textChanged, this, &MainWindow::onSearchChanged);
        headerLayout->addWidget(m_searchEdit);

        headerLayout->addStretch();

        m_nowPlayingLabel = new QLabel("  No channel selected", m_headerBar);
        m_nowPlayingLabel->setObjectName("nowPlaying");
        headerLayout->addWidget(m_nowPlayingLabel);

        headerLayout->addStretch();

        m_channelCountLabel = new QLabel("0 channels", m_headerBar);
        m_channelCountLabel->setObjectName("channelCount");
        headerLayout->addWidget(m_channelCountLabel);

        headerLayout->addSpacing(8);

        m_statusIndicator = new StatusIndicator(m_headerBar);
        m_statusIndicator->setStatus(StatusIndicator::Offline);
        headerLayout->addWidget(m_statusIndicator);

        headerLayout->addSpacing(8);

        QPushButton* volDown = new QPushButton("Vol-", m_headerBar);
        volDown->setObjectName("iconBtn");
        volDown->setFixedSize(40, 32);
        volDown->setToolTip("Volume Down (Left Arrow)");
        connect(volDown, &QPushButton::clicked, this, [this]() { changeVolume(-5); });
        headerLayout->addWidget(volDown);

        m_volumeLabel = new QLabel("100%", m_headerBar);
        m_volumeLabel->setObjectName("volumeLabel");
        m_volumeLabel->setFixedWidth(44);
        m_volumeLabel->setAlignment(Qt::AlignCenter);
        headerLayout->addWidget(m_volumeLabel);

        QPushButton* volUp = new QPushButton("Vol+", m_headerBar);
        volUp->setObjectName("iconBtn");
        volUp->setFixedSize(40, 32);
        volUp->setToolTip("Volume Up (Right Arrow)");
        connect(volUp, &QPushButton::clicked, this, [this]() { changeVolume(5); });
        headerLayout->addWidget(volUp);

        headerLayout->addSpacing(4);

        QPushButton* muteBtn = new QPushButton("Mute", m_headerBar);
        muteBtn->setObjectName("iconBtn");
        muteBtn->setFixedSize(44, 32);
        muteBtn->setToolTip("Toggle Mute (M)");
        connect(muteBtn, &QPushButton::clicked, this, [this]() { toggleMute(); });
        headerLayout->addWidget(muteBtn);

        headerLayout->addSpacing(4);

        m_fullscreenBtn = new QPushButton("Fullscreen", m_headerBar);
        m_fullscreenBtn->setObjectName("iconBtn");
        m_fullscreenBtn->setFixedHeight(32);
        m_fullscreenBtn->setToolTip("Fullscreen (F11)");
        connect(m_fullscreenBtn, &QPushButton::clicked, this, &MainWindow::toggleFullscreen);
        headerLayout->addWidget(m_fullscreenBtn);

        rootLayout->addWidget(m_headerBar);

        QFrame* sep = new QFrame(central);
        sep->setFrameShape(QFrame::HLine);
        sep->setObjectName("headerSep");
        sep->setFixedHeight(1);
        rootLayout->addWidget(sep);

        QSplitter* hSplitter = new QSplitter(Qt::Horizontal, central);
        hSplitter->setObjectName("mainSplitter");

        m_leftPanel = new QWidget(hSplitter);
        m_leftPanel->setObjectName("leftPanel");
        m_leftPanel->setMinimumWidth(170);
        m_leftPanel->setMaximumWidth(240);
        QVBoxLayout* leftLayout = new QVBoxLayout(m_leftPanel);
        leftLayout->setContentsMargins(8, 12, 4, 8);
        leftLayout->setSpacing(6);

        QLabel* catLabel = new QLabel("Categories", m_leftPanel);
        catLabel->setObjectName("sectionTitle");
        leftLayout->addWidget(catLabel);

        m_categoryList = new QListWidget(m_leftPanel);
        m_categoryList->setObjectName("categoryList");
        connect(m_categoryList, &QListWidget::currentRowChanged, this, &MainWindow::onCategoryChanged);
        leftLayout->addWidget(m_categoryList);

        QPushButton* refreshBtn = new QPushButton("Refresh Playlist", m_leftPanel);
        refreshBtn->setObjectName("refreshBtn");
        connect(refreshBtn, &QPushButton::clicked, this, [this]() {
            fetchPlaylist(QString::fromLatin1(PLAYLIST_URL));
        });
        leftLayout->addWidget(refreshBtn);

        hSplitter->addWidget(m_leftPanel);

        QWidget* rightPanel = new QWidget(hSplitter);
        QVBoxLayout* rightLayout = new QVBoxLayout(rightPanel);
        rightLayout->setContentsMargins(0, 0, 0, 0);
        rightLayout->setSpacing(0);

        m_vertSplitter = new QSplitter(Qt::Vertical, rightPanel);

        m_videoWidget = new VideoWidget(m_vertSplitter);
        m_videoWidget->installEventFilter(this);
        connect(m_videoWidget, &VideoWidget::doubleClicked, this, &MainWindow::toggleFullscreen);

        m_vertSplitter->addWidget(m_videoWidget);

        m_channelModel = new ChannelModel(this);
        m_proxyModel = new CategoryFilterProxy(this);
        m_proxyModel->setSourceModel(m_channelModel);

        m_channelView = new QListView(m_vertSplitter);
        m_channelView->setModel(m_proxyModel);
        m_channelView->setViewMode(QListView::IconMode);
        m_channelView->setResizeMode(QListView::Adjust);
        m_channelView->setMovement(QListView::Static);
        m_channelView->setSpacing(6);
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
        hSplitter->setSizes(QList<int>() << 200 << 1080);

        rootLayout->addWidget(hSplitter, 1);

        m_osd = new OsdWidget(m_videoWidget);
        m_osd->setGeometry(m_videoWidget->rect());

        statusBar()->showMessage("Loading playlist...");
    }

    void applyModernTheme() {
        QString style =
            "* {"
            "  font-family: 'Segoe UI', 'Helvetica Neue', Arial, sans-serif;"
            "}"
            "QMainWindow, QWidget {"
            "  background-color: #0f0f1a;"
            "  color: #e2e8f0;"
            "}"
            "#headerBar {"
            "  background-color: #1a1a2e;"
            "}"
            "#appTitle {"
            "  font-size: 16px;"
            "  font-weight: bold;"
            "  color: #f1f5f9;"
            "}"
            "#searchEdit {"
            "  background-color: #1e1e35;"
            "  color: #e2e8f0;"
            "  border: 1px solid rgba(255,255,255,25);"
            "  border-radius: 8px;"
            "  padding: 6px 12px;"
            "  font-size: 13px;"
            "}"
            "#searchEdit:focus {"
            "  border: 1px solid #6366f1;"
            "  background-color: #222240;"
            "}"
            "#nowPlaying {"
            "  color: #a5b4fc;"
            "  font-size: 12px;"
            "  font-weight: bold;"
            "}"
            "#channelCount {"
            "  color: #64748b;"
            "  font-size: 11px;"
            "}"
            "#volumeLabel {"
            "  color: #94a3b8;"
            "  font-size: 11px;"
            "  font-weight: bold;"
            "}"
            "#iconBtn {"
            "  background: rgba(255,255,255,12);"
            "  border: 1px solid rgba(255,255,255,20);"
            "  border-radius: 6px;"
            "  color: #e2e8f0;"
            "  font-size: 12px;"
            "  padding: 2px 8px;"
            "}"
            "#iconBtn:hover {"
            "  background: rgba(99,102,241,80);"
            "  border-color: rgba(99,102,241,130);"
            "}"
            "#iconBtn:pressed {"
            "  background: rgba(99,102,241,130);"
            "}"
            "#headerSep {"
            "  background-color: rgba(255,255,255,10);"
            "  border: none;"
            "}"
            "#leftPanel {"
            "  background-color: #12121f;"
            "}"
            "#sectionTitle {"
            "  font-weight: bold;"
            "  font-size: 13px;"
            "  color: #94a3b8;"
            "  padding: 4px 8px;"
            "}"
            "#categoryList {"
            "  background-color: transparent;"
            "  border: none;"
            "  outline: none;"
            "  font-size: 13px;"
            "}"
            "#categoryList::item {"
            "  padding: 8px 12px;"
            "  border-radius: 8px;"
            "  margin: 1px 4px;"
            "  color: #cbd5e1;"
            "}"
            "#categoryList::item:selected {"
            "  background-color: rgba(99,102,241,90);"
            "  color: #e0e7ff;"
            "}"
            "#categoryList::item:hover {"
            "  background-color: rgba(255,255,255,10);"
            "}"
            "#refreshBtn {"
            "  background: rgba(99,102,241,40);"
            "  border: 1px solid rgba(99,102,241,65);"
            "  border-radius: 8px;"
            "  color: #a5b4fc;"
            "  padding: 8px;"
            "  font-size: 12px;"
            "  font-weight: bold;"
            "}"
            "#refreshBtn:hover {"
            "  background: rgba(99,102,241,80);"
            "  color: #e0e7ff;"
            "}"
            "#channelGrid {"
            "  background-color: #0f0f1a;"
            "  border: none;"
            "}"
            "QSplitter::handle {"
            "  background-color: rgba(255,255,255,10);"
            "}"
            "QSplitter::handle:horizontal { width: 1px; }"
            "QSplitter::handle:vertical { height: 4px; }"
            "QSplitter::handle:hover {"
            "  background-color: rgba(99,102,241,100);"
            "}"
            "QStatusBar {"
            "  background-color: #0a0a16;"
            "  color: #64748b;"
            "  font-size: 11px;"
            "  padding: 2px 12px;"
            "}"
            "QScrollBar:vertical {"
            "  background: transparent;"
            "  width: 8px;"
            "  margin: 0;"
            "}"
            "QScrollBar::handle:vertical {"
            "  background: rgba(148,163,184,50);"
            "  border-radius: 4px;"
            "  min-height: 30px;"
            "}"
            "QScrollBar::handle:vertical:hover {"
            "  background: rgba(148,163,184,90);"
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
            QMessageBox::critical(this, "Error", "Failed to create mpv instance.");
            m_mpvOk = false;
            return;
        }

        // Video output
        mpv_set_option_string(m_mpv, "vo", "gpu");
        mpv_set_option_string(m_mpv, "hwdec", "auto-safe");

        // Audio output - try multiple backends
#ifdef Q_OS_WIN
        mpv_set_option_string(m_mpv, "ao", "wasapi,sdl,openal");
#elif defined(Q_OS_LINUX)
        mpv_set_option_string(m_mpv, "ao", "pulse,alsa,sdl");
#elif defined(Q_OS_MAC)
        mpv_set_option_string(m_mpv, "ao", "coreaudio,sdl");
#else
        mpv_set_option_string(m_mpv, "ao", "auto");
#endif

        // Audio settings - ensure audio is not disabled
        mpv_set_option_string(m_mpv, "audio", "yes");
        mpv_set_option_string(m_mpv, "mute", "no");

        // Set initial volume before init
        QByteArray volStr = QString::number(m_volume).toUtf8();
        mpv_set_option_string(m_mpv, "volume", volStr.constData());

        // General playback
        mpv_set_option_string(m_mpv, "keep-open", "yes");
        mpv_set_option_string(m_mpv, "idle", "yes");
        mpv_set_option_string(m_mpv, "input-default-bindings", "no");
        mpv_set_option_string(m_mpv, "input-vo-keyboard", "no");
        mpv_set_option_string(m_mpv, "osc", "no");
        mpv_set_option_string(m_mpv, "osd-level", "0");

        // Network/cache
        mpv_set_option_string(m_mpv, "cache", "yes");
        mpv_set_option_string(m_mpv, "demuxer-max-bytes", "50MiB");
        mpv_set_option_string(m_mpv, "demuxer-max-back-bytes", "10MiB");
        mpv_set_option_string(m_mpv, "cache-secs", "10");
        mpv_set_option_string(m_mpv, "network-timeout", "15");

        // Embed video into our widget
        int64_t wid = static_cast<int64_t>(m_videoWidget->winId());
        mpv_set_option(m_mpv, "wid", MPV_FORMAT_INT64, &wid);

        int err = mpv_initialize(m_mpv);
        if (err < 0) {
            QMessageBox::critical(this, "Error", QString("mpv init failed: %1").arg(mpv_error_string(err)));
            mpv_terminate_destroy(m_mpv);
            m_mpv = NULL;
            m_mpvOk = false;
            return;
        }

        // Set properties AFTER initialization
        // Volume must be set as property after init for reliable behavior
        int64_t vol = m_volume;
        mpv_set_property(m_mpv, "volume", MPV_FORMAT_INT64, &vol);

        // Ensure audio is not muted
        int muteFlag = m_muted ? 1 : 0;
        mpv_set_property(m_mpv, "mute", MPV_FORMAT_FLAG, &muteFlag);

        // Set audio device to auto
        mpv_set_property_string(m_mpv, "audio-device", "auto");

        // Observe audio properties for debugging
        mpv_observe_property(m_mpv, 0, "volume", MPV_FORMAT_DOUBLE);
        mpv_observe_property(m_mpv, 0, "mute", MPV_FORMAT_FLAG);
        mpv_observe_property(m_mpv, 0, "audio-device", MPV_FORMAT_STRING);

        mpv_set_wakeup_callback(m_mpv, [](void* ctx) {
            QMetaObject::invokeMethod(static_cast<MainWindow*>(ctx), "onMpvWakeup", Qt::QueuedConnection);
        }, this);

        m_mpvOk = true;
    }

    void loadSettings() {
        QSettings s("LiveTVPlayer", "LiveTVPlayer");
        m_currentCategory = s.value("lastCategory", "All").toString();
        m_volume = s.value("volume", 100).toInt();
        if (m_volume < 0) m_volume = 0;
        if (m_volume > 150) m_volume = 150;
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
                m_statusIndicator->setStatus(StatusIndicator::Offline);
                statusBar()->showMessage("Failed to load playlist - check connection");
                reply->deleteLater();
                return;
            }

            QByteArray data = reply->readAll();
            reply->deleteLater();

            if (data.size() > MAX_DOWNLOAD_SIZE) {
                statusBar()->showMessage("Playlist too large.");
                return;
            }

            parseM3u(data);
        });
    }

    void parseM3u(const QByteArray& data) {
        QVector<Channel> channels;
        QString text = QString::fromUtf8(data);
        QStringList lines = text.split(QRegularExpression("[\\r\\n]+"), QString::SkipEmptyParts);

        if (lines.isEmpty()) {
            statusBar()->showMessage("Empty playlist.");
            m_statusIndicator->setStatus(StatusIndicator::Offline);
            return;
        }

        QRegularExpression reExtInf("^#EXTINF\\s*:\\s*(-?\\d+)\\s*(.*),\\s*(.*)$");
        QRegularExpression reLogo("tvg-logo\\s*=\\s*\"([^\"]*)\"");
        QRegularExpression reGroup("group-title\\s*=\\s*\"([^\"]*)\"");

        Channel pending;
        bool hasPending = false;

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
                        }
                    }
                    hasPending = false;
                }
            }
        }

        if (channels.isEmpty()) {
            statusBar()->showMessage("No valid channels found.");
            m_statusIndicator->setStatus(StatusIndicator::Offline);
            return;
        }

        m_channelModel->setChannels(channels);

        QSet<QString> catSet;
        for (int i = 0; i < channels.size(); ++i) {
            catSet.insert(channels[i].category);
        }
        QStringList cats = catSet.toList();
        std::sort(cats.begin(), cats.end());
        cats.prepend("All");

        m_categoryList->blockSignals(true);
        m_categoryList->clear();
        for (int i = 0; i < cats.size(); ++i) {
            m_categoryList->addItem(cats[i]);
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

        statusBar()->showMessage(QString("Loaded %1 channels in %2 categories").arg(channels.size()).arg(cats.size() - 1));
        if (m_currentStreamUrl.isEmpty()) {
            m_statusIndicator->setStatus(StatusIndicator::Online);
        }
        updateChannelCount();
        scheduleLogoDownloads();
    }

    void updateChannelCount() {
        int count = m_proxyModel ? m_proxyModel->rowCount() : 0;
        m_channelCountLabel->setText(QString("%1 channel%2").arg(count).arg(count != 1 ? "s" : ""));
    }

    void updateVolumeLabel() {
        if (m_volumeLabel) {
            m_volumeLabel->setText(QString("%1%").arg(m_volume));
        }
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
                        m_logoPixmaps.insert(urlStr, pm.scaled(52, 42, Qt::KeepAspectRatio, Qt::SmoothTransformation));
                    }
                }
            }

            reply->deleteLater();
            downloadNextLogos();

            if (m_channelView && m_channelView->viewport()) {
                m_channelView->viewport()->update();
            }
        });
    }

    void playStream(const QString& url) {
        if (!m_mpvOk || !m_mpv || url.isEmpty()) {
            statusBar()->showMessage("Playback unavailable.");
            return;
        }

        // Ensure audio is enabled before each playback
        int muteOff = 0;
        if (!m_muted) {
            mpv_set_property(m_mpv, "mute", MPV_FORMAT_FLAG, &muteOff);
        }

        // Re-apply volume before playback
        int64_t vol = m_volume;
        mpv_set_property(m_mpv, "volume", MPV_FORMAT_INT64, &vol);

        QByteArray urlBytes = url.toUtf8();
        const char* cmd[] = {"loadfile", urlBytes.constData(), "replace", NULL};
        int err = mpv_command(m_mpv, cmd);
        if (err < 0) {
            statusBar()->showMessage(QString("mpv error: %1").arg(mpv_error_string(err)));
            m_statusIndicator->setStatus(StatusIndicator::Offline);
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

            // If changing volume, unmute
            if (m_muted && delta > 0) {
                m_muted = false;
                int muteFlag = 0;
                mpv_set_property(m_mpv, "mute", MPV_FORMAT_FLAG, &muteFlag);
            }
        }
        updateVolumeLabel();
        statusBar()->showMessage(QString("Volume: %1%").arg(m_volume), 2000);
    }

    void toggleMute() {
        m_muted = !m_muted;
        if (m_mpv && m_mpvOk) {
            int muteFlag = m_muted ? 1 : 0;
            mpv_set_property(m_mpv, "mute", MPV_FORMAT_FLAG, &muteFlag);
        }
        statusBar()->showMessage(m_muted ? "Muted" : "Unmuted", 2000);
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
    QSplitter* m_vertSplitter;

    ChannelModel* m_channelModel;
    CategoryFilterProxy* m_proxyModel;
    ChannelDelegate* m_delegate;

    QHash<QString, QPixmap> m_logoPixmaps;
    QStringList m_logoPending;
    int m_activeLogoDownloads;

    QTimer* m_debounceTimer;
    QTimer* m_autoHideTimer;
    QTimer* m_searchDebounce;
    QTimer* m_statusCheckTimer;

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
