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
#include <QMenuBar>
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
#include <QGraphicsOpacityEffect>
#include <QPropertyAnimation>
#include <QClipboard>
#include <QGuiApplication>
#include <QScreen>
#include <QScrollBar>
#include <QCache>
#include <QtGlobal>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

#include <mpv/client.h>

#include <cstring>
#include <algorithm>

static const char* DEFAULT_PLAYLIST_URL = "https://m3u.work/jwuF5FPp.m3u";
static const int MAX_DOWNLOAD_SIZE = 10 * 1024 * 1024;
static const int PLAYLIST_TIMEOUT_MS = 12000;
static const int IMAGE_TIMEOUT_MS = 6000;
static const int MAX_CONCURRENT_DOWNLOADS = 6;
static const int DEBOUNCE_MS = 150;
static const int OSD_DISPLAY_MS = 3000;
static const int AUTOHIDE_MS = 2500;
static const int MAX_NAME_LEN = 200;

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
            case NameRole:
                return ch.name;
            case CategoryRole:
                return ch.category;
            case LogoUrlRole:
                return ch.logoUrl;
            case StreamUrlRole:
                return ch.streamUrl;
            case IndexRole:
                return index.row();
            case Qt::DecorationRole:
                return QVariant();
            default:
                return QVariant();
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

    void setCategoryFilter(const QString& cat) {
        m_category = cat;
        invalidateFilter();
    }

    void setSearchFilter(const QString& search) {
        m_search = search.toLower();
        invalidateFilter();
    }

    QString categoryFilter() const { return m_category; }
    QString searchFilter() const { return m_search; }

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const override {
        QModelIndex idx = sourceModel()->index(sourceRow, 0, sourceParent);
        if (!idx.isValid()) return false;

        if (!m_category.isEmpty() && m_category != "All") {
            QString cat = idx.data(CategoryRole).toString();
            if (cat != m_category) return false;
        }

        if (!m_search.isEmpty()) {
            QString name = idx.data(NameRole).toString().toLower();
            if (!name.contains(m_search)) return false;
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
        return QSize(180, 80);
    }

    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        painter->save();

        if (option.state & QStyle::State_Selected) {
            painter->fillRect(option.rect, QColor(0, 120, 215));
        } else if (option.state & QStyle::State_MouseOver) {
            painter->fillRect(option.rect, QColor(60, 60, 80));
        } else {
            painter->fillRect(option.rect, QColor(40, 40, 55));
        }

        painter->setPen(QColor(80, 80, 100));
        painter->drawRect(option.rect.adjusted(0, 0, -1, -1));

        QRect iconRect(option.rect.left() + 5, option.rect.top() + 5, 60, 45);
        QString logoUrl = index.data(LogoUrlRole).toString();
        QString name = index.data(NameRole).toString();
        if (name.length() > MAX_NAME_LEN) name = name.left(MAX_NAME_LEN) + "...";

        bool drawn = false;
        if (m_logoCache && !logoUrl.isEmpty() && m_logoCache->contains(logoUrl)) {
            QPixmap pm = m_logoCache->value(logoUrl);
            if (!pm.isNull()) {
                painter->drawPixmap(iconRect, pm.scaled(iconRect.size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
                drawn = true;
            }
        }

        if (!drawn) {
            QColor bgColor;
            if (!name.isEmpty()) {
                int h = qAbs(name.at(0).unicode() * 37) % 360;
                bgColor = QColor::fromHsv(h, 120, 100);
            } else {
                bgColor = QColor(80, 80, 80);
            }
            painter->fillRect(iconRect, bgColor);
            painter->setPen(Qt::white);
            QFont f = painter->font();
            f.setPixelSize(20);
            f.setBold(true);
            painter->setFont(f);
            QString letter = name.isEmpty() ? "?" : name.left(1).toUpper();
            painter->drawText(iconRect, Qt::AlignCenter, letter);
        }

        painter->setPen(Qt::white);
        QFont textFont = painter->font();
        textFont.setPixelSize(12);
        textFont.setBold(false);
        painter->setFont(textFont);

        QRect textRect(option.rect.left() + 5, option.rect.top() + 55, option.rect.width() - 10, 20);
        QString elidedName = painter->fontMetrics().elidedText(name, Qt::ElideRight, textRect.width());
        painter->drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter, elidedName);

        painter->restore();
    }

private:
    QHash<QString, QPixmap>* m_logoCache = nullptr;
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
        connect(m_hideTimer, &QTimer::timeout, this, &OsdWidget::hide);
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

        int boxW = qMin(width() - 40, 500);
        int boxH = 80;
        int x = (width() - boxW) / 2;
        int y = height() - boxH - 40;

        p.setBrush(QColor(0, 0, 0, 200));
        p.setPen(Qt::NoPen);
        p.drawRoundedRect(x, y, boxW, boxH, 10, 10);

        p.setPen(Qt::white);
        QFont f = font();
        f.setPixelSize(22);
        f.setBold(true);
        p.setFont(f);
        QString elidedName = p.fontMetrics().elidedText(m_channelName, Qt::ElideRight, boxW - 20);
        p.drawText(x + 10, y + 10, boxW - 20, 30, Qt::AlignLeft | Qt::AlignVCenter, elidedName);

        f.setPixelSize(14);
        f.setBold(false);
        p.setFont(f);
        p.setPen(QColor(180, 180, 200));
        QString info = m_category;
        if (m_total > 0) {
            info += QString("  |  %1/%2").arg(m_index + 1).arg(m_total);
        }
        p.drawText(x + 10, y + 45, boxW - 20, 25, Qt::AlignLeft | Qt::AlignVCenter, info);
    }

private:
    QTimer* m_hideTimer;
    QString m_channelName;
    QString m_category;
    int m_index = 0;
    int m_total = 0;
};

class VideoWidget : public QWidget {
    Q_OBJECT
public:
    explicit VideoWidget(QWidget* parent = nullptr) : QWidget(parent) {
        setAttribute(Qt::WA_DontCreateNativeAncestors);
        setAttribute(Qt::WA_NativeWindow);
        setMinimumSize(320, 240);
        setStyleSheet("background-color: black;");
        setFocusPolicy(Qt::NoFocus);
    }
};

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr) : QMainWindow(parent) {
        setWindowTitle("Live TV Player");
        resize(1280, 720);
        setMinimumSize(800, 500);

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

        setupUi();
        setupMpv();
        loadSettings();
        applyDarkTheme();
    }

    ~MainWindow() override {
        saveSettings();
        if (m_mpv) {
            mpv_terminate_destroy(m_mpv);
            m_mpv = nullptr;
        }
    }

protected:
    void keyPressEvent(QKeyEvent* event) override {
        resetAutoHide();

        switch (event->key()) {
            case Qt::Key_F11:
                toggleFullscreen();
                break;
            case Qt::Key_Escape:
                if (isFullScreen()) {
                    showNormal();
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
            case Qt::Key_Return:
            case Qt::Key_Enter:
                toggleTvMode();
                break;
            default:
                QMainWindow::keyPressEvent(event);
                break;
        }
    }

    void mouseMoveEvent(QMouseEvent* event) override {
        QMainWindow::mouseMoveEvent(event);
        resetAutoHide();
    }

    void resizeEvent(QResizeEvent* event) override {
        QMainWindow::resizeEvent(event);
        if (m_osd) {
            m_osd->setGeometry(m_videoWidget->rect());
        }
    }

    bool eventFilter(QObject* obj, QEvent* event) override {
        if (event->type() == QEvent::MouseMove) {
            resetAutoHide();
        }
        return QMainWindow::eventFilter(obj, event);
    }

private slots:
    void onLoadClicked() {
        QString url = m_urlEdit->text().trimmed();
        if (url.isEmpty()) {
            QMessageBox::warning(this, "Error", "Please enter a playlist URL.");
            return;
        }
        fetchPlaylist(url);
    }

    void onPasteClicked() {
        QClipboard* cb = QGuiApplication::clipboard();
        if (cb) {
            QString text = cb->text().trimmed();
            if (!text.isEmpty()) {
                m_urlEdit->setText(text);
            }
        }
    }

    void onDefaultClicked() {
        m_urlEdit->setText(DEFAULT_PLAYLIST_URL);
    }

    void onCategoryChanged(int row) {
        if (row < 0 || row >= m_categoryList->count()) return;
        QString cat = m_categoryList->item(row)->text();
        m_proxyModel->setCategoryFilter(cat);
        m_currentCategory = cat;
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
        playStream(m_pendingStreamUrl);
        m_currentChannelName = m_pendingChannelName;
        m_currentStreamUrl = m_pendingStreamUrl;
        statusBar()->showMessage(m_currentChannelName);
        if (m_osd) {
            m_osd->showOsd(m_pendingChannelName, m_pendingCategory, m_pendingIndex, m_pendingTotal);
        }
    }

    void onSearchChanged(const QString&) {
        m_searchDebounce->start();
    }

    void applySearch() {
        m_proxyModel->setSearchFilter(m_searchEdit->text().trimmed());
    }

    void onMpvWakeup() {
        while (m_mpv) {
            mpv_event* event = mpv_wait_event(m_mpv, 0);
            if (!event || event->event_id == MPV_EVENT_NONE)
                break;
            switch (event->event_id) {
                case MPV_EVENT_SHUTDOWN:
                    break;
                case MPV_EVENT_END_FILE: {
                    mpv_event_end_file* ef = static_cast<mpv_event_end_file*>(event->data);
                    if (ef && ef->reason == MPV_END_FILE_REASON_ERROR) {
                        statusBar()->showMessage("Playback error: " + m_currentChannelName);
                    }
                    break;
                }
                case MPV_EVENT_FILE_LOADED:
                    statusBar()->showMessage("Playing: " + m_currentChannelName);
                    break;
                default:
                    break;
            }
        }
    }

    void toggleTvMode() {
        m_tvMode = !m_tvMode;
        if (m_tvMode) {
            showPanels();
            resetAutoHide();
        } else {
            m_autoHideTimer->stop();
            showPanels();
        }
    }

    void hidePanels() {
        if (!m_tvMode) return;
        if (m_leftPanel) m_leftPanel->hide();
        if (m_topBar) m_topBar->hide();
    }

    void showPanels() {
        if (m_leftPanel) m_leftPanel->show();
        if (m_topBar) m_topBar->show();
    }

    void resetAutoHide() {
        if (m_tvMode) {
            showPanels();
            m_autoHideTimer->start();
        }
    }

private:
    void setupUi() {
        QWidget* centralWidget = new QWidget(this);
        setCentralWidget(centralWidget);

        QVBoxLayout* mainLayout = new QVBoxLayout(centralWidget);
        mainLayout->setContentsMargins(0, 0, 0, 0);
        mainLayout->setSpacing(0);

        m_topBar = new QWidget(centralWidget);
        QHBoxLayout* topLayout = new QHBoxLayout(m_topBar);
        topLayout->setContentsMargins(5, 5, 5, 5);

        m_urlEdit = new QLineEdit(m_topBar);
        m_urlEdit->setPlaceholderText("Enter M3U playlist URL...");
        m_urlEdit->setMinimumWidth(300);
        topLayout->addWidget(m_urlEdit, 1);

        QPushButton* loadBtn = new QPushButton("Load", m_topBar);
        connect(loadBtn, &QPushButton::clicked, this, &MainWindow::onLoadClicked);
        topLayout->addWidget(loadBtn);

        QPushButton* pasteBtn = new QPushButton("Paste", m_topBar);
        connect(pasteBtn, &QPushButton::clicked, this, &MainWindow::onPasteClicked);
        topLayout->addWidget(pasteBtn);

        QPushButton* defaultBtn = new QPushButton("Default", m_topBar);
        connect(defaultBtn, &QPushButton::clicked, this, &MainWindow::onDefaultClicked);
        topLayout->addWidget(defaultBtn);

        m_searchEdit = new QLineEdit(m_topBar);
        m_searchEdit->setPlaceholderText("Search channels...");
        m_searchEdit->setMaximumWidth(250);
        connect(m_searchEdit, &QLineEdit::textChanged, this, &MainWindow::onSearchChanged);
        topLayout->addWidget(m_searchEdit);

        mainLayout->addWidget(m_topBar);

        QSplitter* splitter = new QSplitter(Qt::Horizontal, centralWidget);

        m_leftPanel = new QWidget(splitter);
        QVBoxLayout* leftLayout = new QVBoxLayout(m_leftPanel);
        leftLayout->setContentsMargins(2, 2, 2, 2);

        QLabel* catLabel = new QLabel("Categories", m_leftPanel);
        catLabel->setStyleSheet("font-weight: bold; font-size: 14px; padding: 4px;");
        leftLayout->addWidget(catLabel);

        m_categoryList = new QListWidget(m_leftPanel);
        m_categoryList->setMaximumWidth(220);
        m_categoryList->setMinimumWidth(150);
        connect(m_categoryList, &QListWidget::currentRowChanged, this, &MainWindow::onCategoryChanged);
        leftLayout->addWidget(m_categoryList);

        splitter->addWidget(m_leftPanel);

        QWidget* rightPanel = new QWidget(splitter);
        QVBoxLayout* rightLayout = new QVBoxLayout(rightPanel);
        rightLayout->setContentsMargins(0, 0, 0, 0);
        rightLayout->setSpacing(0);

        m_channelModel = new ChannelModel(this);
        m_proxyModel = new CategoryFilterProxy(this);
        m_proxyModel->setSourceModel(m_channelModel);

        m_channelView = new QListView(rightPanel);
        m_channelView->setModel(m_proxyModel);
        m_channelView->setViewMode(QListView::IconMode);
        m_channelView->setResizeMode(QListView::Adjust);
        m_channelView->setMovement(QListView::Static);
        m_channelView->setSpacing(4);
        m_channelView->setUniformItemSizes(true);
        m_channelView->setWrapping(true);
        m_channelView->setSelectionMode(QAbstractItemView::SingleSelection);
        m_channelView->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
        m_channelView->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

        m_delegate = new ChannelDelegate(this);
        m_delegate->setLogoCache(&m_logoPixmaps);
        m_channelView->setItemDelegate(m_delegate);

        connect(m_channelView, &QListView::clicked, this, &MainWindow::onChannelClicked);
        connect(m_channelView, &QListView::activated, this, &MainWindow::onChannelClicked);

        QSplitter* vertSplitter = new QSplitter(Qt::Vertical, rightPanel);

        m_videoWidget = new VideoWidget(vertSplitter);
        m_videoWidget->installEventFilter(this);

        vertSplitter->addWidget(m_videoWidget);
        vertSplitter->addWidget(m_channelView);
        vertSplitter->setStretchFactor(0, 3);
        vertSplitter->setStretchFactor(1, 2);

        rightLayout->addWidget(vertSplitter);

        splitter->addWidget(rightPanel);
        splitter->setStretchFactor(0, 0);
        splitter->setStretchFactor(1, 1);
        splitter->setSizes(QList<int>() << 200 << 1080);

        mainLayout->addWidget(splitter, 1);

        m_osd = new OsdWidget(m_videoWidget);
        m_osd->setGeometry(m_videoWidget->rect());

        statusBar()->showMessage("Ready. Enter a playlist URL and click Load.");
        statusBar()->setStyleSheet("color: #ccc;");
    }

    void applyDarkTheme() {
        QString style =
            "QMainWindow, QWidget { background-color: #1e1e2e; color: #e0e0e0; }"
            "QLineEdit { background-color: #2a2a3e; color: #e0e0e0; border: 1px solid #444; border-radius: 4px; padding: 4px 8px; }"
            "QPushButton { background-color: #3a3a5e; color: #e0e0e0; border: 1px solid #555; border-radius: 4px; padding: 5px 12px; }"
            "QPushButton:hover { background-color: #4a4a7e; }"
            "QPushButton:pressed { background-color: #2a2a4e; }"
            "QListWidget { background-color: #252538; color: #e0e0e0; border: 1px solid #333; }"
            "QListWidget::item { padding: 6px; }"
            "QListWidget::item:selected { background-color: #0078d7; }"
            "QListWidget::item:hover { background-color: #353550; }"
            "QListView { background-color: #1a1a2e; border: 1px solid #333; }"
            "QSplitter::handle { background-color: #333; width: 3px; }"
            "QStatusBar { background-color: #151525; color: #aaa; }"
            "QLabel { color: #e0e0e0; }"
            "QScrollBar:vertical { background: #1a1a2e; width: 10px; }"
            "QScrollBar::handle:vertical { background: #444; border-radius: 5px; min-height: 20px; }"
            "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }";
        qApp->setStyleSheet(style);
    }

    void setupMpv() {
        m_mpv = mpv_create();
        if (!m_mpv) {
            QMessageBox::critical(this, "Error", "Failed to create mpv instance. Playback will be disabled.");
            m_mpvOk = false;
            return;
        }

        mpv_set_option_string(m_mpv, "hwdec", "auto");
        mpv_set_option_string(m_mpv, "vo", "gpu");
        mpv_set_option_string(m_mpv, "keep-open", "yes");
        mpv_set_option_string(m_mpv, "idle", "yes");
        mpv_set_option_string(m_mpv, "input-default-bindings", "no");
        mpv_set_option_string(m_mpv, "input-vo-keyboard", "no");
        mpv_set_option_string(m_mpv, "osc", "no");
        mpv_set_option_string(m_mpv, "osd-level", "0");
        mpv_set_option_string(m_mpv, "cache", "yes");
        mpv_set_option_string(m_mpv, "demuxer-max-bytes", "50MiB");
        mpv_set_option_string(m_mpv, "demuxer-max-back-bytes", "10MiB");
        mpv_set_option_string(m_mpv, "cache-secs", "10");
        mpv_set_option_string(m_mpv, "network-timeout", "15");

        int64_t wid = static_cast<int64_t>(m_videoWidget->winId());
        mpv_set_option(m_mpv, "wid", MPV_FORMAT_INT64, &wid);

        int err = mpv_initialize(m_mpv);
        if (err < 0) {
            QMessageBox::critical(this, "Error",
                QString("Failed to initialize mpv: %1").arg(mpv_error_string(err)));
            mpv_terminate_destroy(m_mpv);
            m_mpv = nullptr;
            m_mpvOk = false;
            return;
        }

        mpv_set_wakeup_callback(m_mpv, [](void* ctx) {
            QMetaObject::invokeMethod(static_cast<MainWindow*>(ctx), "onMpvWakeup", Qt::QueuedConnection);
        }, this);

        m_mpvOk = true;

        if (m_volume >= 0) {
            QString volStr = QString::number(m_volume);
            mpv_set_property_string(m_mpv, "volume", volStr.toUtf8().constData());
        }
        if (m_muted) {
            mpv_set_property_string(m_mpv, "mute", "yes");
        }
    }

    void loadSettings() {
        QSettings s("LiveTVPlayer", "LiveTVPlayer");
        QString lastUrl = s.value("lastUrl", DEFAULT_PLAYLIST_URL).toString();
        m_urlEdit->setText(lastUrl);
        m_currentCategory = s.value("lastCategory", "All").toString();
        m_volume = s.value("volume", 100).toInt();
        m_muted = s.value("muted", false).toBool();
        m_tvMode = s.value("tvMode", false).toBool();
        m_lastStreamUrl = s.value("lastStream", "").toString();
    }

    void saveSettings() {
        QSettings s("LiveTVPlayer", "LiveTVPlayer");
        s.setValue("lastUrl", m_urlEdit->text().trimmed());
        s.setValue("lastCategory", m_currentCategory);
        s.setValue("volume", m_volume);
        s.setValue("muted", m_muted);
        s.setValue("tvMode", m_tvMode);
        if (!m_currentStreamUrl.isEmpty()) {
            s.setValue("lastStream", m_currentStreamUrl);
        }
    }

    void fetchPlaylist(const QString& urlStr) {
        QUrl url(urlStr);
        if (!url.isValid() || (url.scheme() != "http" && url.scheme() != "https")) {
            QMessageBox::warning(this, "Invalid URL", "URL must use http or https scheme.");
            return;
        }

        statusBar()->showMessage("Loading playlist...");

        QNetworkRequest req(url);
        req.setRawHeader("User-Agent", "LiveTVPlayer/1.0");
        req.setAttribute(QNetworkRequest::FollowRedirectsAttribute, true);

        QNetworkReply* reply = m_nam->get(req);

        QTimer* timeout = new QTimer(this);
        timeout->setSingleShot(true);
        connect(timeout, &QTimer::timeout, this, [reply, timeout]() {
            if (reply && reply->isRunning()) {
                reply->abort();
            }
            timeout->deleteLater();
        });
        timeout->start(PLAYLIST_TIMEOUT_MS);

        m_downloadedBytes = 0;

        connect(reply, &QNetworkReply::readyRead, this, [this, reply]() {
            m_downloadedBytes += reply->bytesAvailable();
            if (m_downloadedBytes > MAX_DOWNLOAD_SIZE) {
                reply->abort();
            }
        });

        connect(reply, &QNetworkReply::finished, this, [this, reply, timeout]() {
            timeout->stop();
            timeout->deleteLater();

            if (reply->error() != QNetworkReply::NoError) {
                statusBar()->showMessage("Failed to load playlist.");
                QMessageBox::warning(this, "Network Error",
                    QString("Failed to download playlist:\n%1").arg(reply->errorString()));
                reply->deleteLater();
                return;
            }

            QByteArray data = reply->readAll();
            reply->deleteLater();

            if (data.size() > MAX_DOWNLOAD_SIZE) {
                QMessageBox::warning(this, "Error", "Playlist too large.");
                return;
            }

            parseM3u(data);
        });
    }

    void parseM3u(const QByteArray& data) {
        QVector<Channel> channels;
        QString text = QString::fromUtf8(data);
        QStringList lines = text.split(QRegularExpression("[\\r\\n]+"), Qt::SkipEmptyParts);

        if (lines.isEmpty()) {
            statusBar()->showMessage("Empty playlist.");
            return;
        }

        QRegularExpression reExtInf("#EXTINF\\s*:\\s*(-?\\d+)\\s*(.*),\\s*(.*)");
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
                    if (logoMatch.hasMatch()) {
                        pending.logoUrl = logoMatch.captured(1).trimmed();
                    }

                    QRegularExpressionMatch groupMatch = reGroup.match(attrs);
                    if (groupMatch.hasMatch()) {
                        pending.category = groupMatch.captured(1).trimmed();
                    }
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
                    if (streamUrl.isValid() && (streamUrl.scheme() == "http" || streamUrl.scheme() == "https" || streamUrl.scheme() == "rtsp" || streamUrl.scheme() == "rtmp" || streamUrl.scheme() == "mms" || streamUrl.scheme() == "mmsh")) {
                        pending.streamUrl = line;
                        channels.append(pending);
                    }
                    hasPending = false;
                }
            }
        }

        if (channels.isEmpty()) {
            statusBar()->showMessage("No valid channels found in playlist.");
            QMessageBox::information(this, "Info", "No valid channels found in the playlist.");
            return;
        }

        m_channelModel->setChannels(channels);

        QSet<QString> catSet;
        for (const auto& ch : channels) {
            catSet.insert(ch.category);
        }
        QStringList cats = catSet.values();
        std::sort(cats.begin(), cats.end());
        cats.prepend("All");

        m_categoryList->blockSignals(true);
        m_categoryList->clear();
        for (const auto& c : cats) {
            m_categoryList->addItem(c);
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

        statusBar()->showMessage(QString("Loaded %1 channels in %2 categories.").arg(channels.size()).arg(cats.size() - 1));

        scheduleLogoDownloads();
    }

    void scheduleLogoDownloads() {
        m_logoPending.clear();
        m_activeLogoDownloads = 0;

        const QVector<Channel>& channels = m_channelModel->channels();
        QSet<QString> queued;
        for (const auto& ch : channels) {
            if (!ch.logoUrl.isEmpty() && !m_logoPixmaps.contains(ch.logoUrl) && !queued.contains(ch.logoUrl)) {
                QUrl u(ch.logoUrl);
                if (u.isValid() && (u.scheme() == "http" || u.scheme() == "https")) {
                    m_logoPending.append(ch.logoUrl);
                    queued.insert(ch.logoUrl);
                }
            }
        }

        downloadNextLogos();
    }

    void downloadNextLogos() {
        while (m_activeLogoDownloads < MAX_CONCURRENT_DOWNLOADS && !m_logoPending.isEmpty()) {
            QString url = m_logoPending.takeFirst();
            downloadLogo(url);
        }
    }

    void downloadLogo(const QString& urlStr) {
        QUrl url(urlStr);
        if (!url.isValid()) return;

        QNetworkRequest req(url);
        req.setRawHeader("User-Agent", "LiveTVPlayer/1.0");
        req.setAttribute(QNetworkRequest::FollowRedirectsAttribute, true);

        QNetworkReply* reply = m_logoNam->get(req);
        m_activeLogoDownloads++;

        QTimer* timeout = new QTimer(this);
        timeout->setSingleShot(true);
        connect(timeout, &QTimer::timeout, this, [reply, timeout]() {
            if (reply && reply->isRunning()) {
                reply->abort();
            }
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
                        m_logoPixmaps.insert(urlStr, pm.scaled(60, 45, Qt::KeepAspectRatio, Qt::SmoothTransformation));
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
        if (!m_mpvOk || !m_mpv) {
            statusBar()->showMessage("Playback unavailable (mpv not initialized).");
            return;
        }

        if (url.isEmpty()) return;

        QByteArray urlBytes = url.toUtf8();
        const char* cmd[] = {"loadfile", urlBytes.constData(), "replace", nullptr};
        int err = mpv_command(m_mpv, cmd);
        if (err < 0) {
            statusBar()->showMessage(QString("mpv error: %1").arg(mpv_error_string(err)));
        }
    }

    void toggleFullscreen() {
        if (isFullScreen()) {
            showNormal();
        } else {
            showFullScreen();
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
        onChannelClicked(idx);
    }

    void changeVolume(int delta) {
        m_volume = qBound(0, m_volume + delta, 150);
        if (m_mpv && m_mpvOk) {
            QString volStr = QString::number(m_volume);
            mpv_set_property_string(m_mpv, "volume", volStr.toUtf8().constData());
        }
        statusBar()->showMessage(QString("Volume: %1%").arg(m_volume), 2000);
    }

    void toggleMute() {
        m_muted = !m_muted;
        if (m_mpv && m_mpvOk) {
            mpv_set_property_string(m_mpv, "mute", m_muted ? "yes" : "no");
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

    mpv_handle* m_mpv = nullptr;
    bool m_mpvOk = false;

    QNetworkAccessManager* m_nam = nullptr;
    QNetworkAccessManager* m_logoNam = nullptr;
    int m_downloadedBytes = 0;

    QWidget* m_topBar = nullptr;
    QWidget* m_leftPanel = nullptr;
    QLineEdit* m_urlEdit = nullptr;
    QLineEdit* m_searchEdit = nullptr;
    QListWidget* m_categoryList = nullptr;
    QListView* m_channelView = nullptr;
    VideoWidget* m_videoWidget = nullptr;
    OsdWidget* m_osd = nullptr;

    ChannelModel* m_channelModel = nullptr;
    CategoryFilterProxy* m_proxyModel = nullptr;
    ChannelDelegate* m_delegate = nullptr;

    QHash<QString, QPixmap> m_logoPixmaps;
    QStringList m_logoPending;
    int m_activeLogoDownloads = 0;

    QTimer* m_debounceTimer = nullptr;
    QTimer* m_autoHideTimer = nullptr;
    QTimer* m_searchDebounce = nullptr;

    QString m_pendingStreamUrl;
    QString m_pendingChannelName;
    QString m_pendingCategory;
    int m_pendingIndex = 0;
    int m_pendingTotal = 0;

    QString m_currentChannelName;
    QString m_currentStreamUrl;
    QString m_currentCategory;
    QString m_lastStreamUrl;

    int m_volume = 100;
    bool m_muted = false;
    bool m_tvMode = false;
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
