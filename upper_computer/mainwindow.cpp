#include "mainwindow.h"

#include <QComboBox>
#include <QCheckBox>
#include <QDateTime>
#include <QDir>
#include <QFormLayout>
#include <QCloseEvent>
#include <QFile>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonParseError>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QNetworkInterface>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSaveFile>
#include <QRandomGenerator>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSerialPort>
#include <QSerialPortInfo>
#include <QSettings>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTableWidget>
#include <QHeaderView>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUuid>
#include <QVBoxLayout>

#include <cstdlib>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cwchar>
#endif

namespace {
constexpr int kProtocolVersion = 1;
constexpr int kMaximumLine = 4096;
constexpr int kPresentationResultCache = 64;

bool sendPresentationKey(bool nextPage, QString *reason)
{
#ifdef Q_OS_WIN
    HWND window = GetForegroundWindow();
    if(window == nullptr) {
        if(reason) *reason = "foreground_not_slideshow";
        return false;
    }

    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    wchar_t path[1024] = {};
    DWORD pathSize = DWORD(sizeof(path) / sizeof(path[0]));
    bool supportedProcess = false;
    if(process != nullptr) {
        if(QueryFullProcessImageNameW(process, 0, path, &pathSize)) {
            const wchar_t *name = std::wcsrchr(path, L'\\');
            name = name != nullptr ? name + 1 : path;
            supportedProcess = _wcsicmp(name, L"wps.exe") == 0 ||
                               _wcsicmp(name, L"wpp.exe") == 0 ||
                               _wcsicmp(name, L"powerpnt.exe") == 0;
        }
        CloseHandle(process);
    }

    RECT windowRect = {};
    MONITORINFO monitorInfo = {};
    monitorInfo.cbSize = sizeof(MONITORINFO);
    HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
    bool fullScreen = GetWindowRect(window, &windowRect) &&
                      GetMonitorInfoW(monitor, &monitorInfo) &&
                      std::abs(windowRect.left - monitorInfo.rcMonitor.left) <= 16 &&
                      std::abs(windowRect.top - monitorInfo.rcMonitor.top) <= 16 &&
                      std::abs(windowRect.right - monitorInfo.rcMonitor.right) <= 16 &&
                      std::abs(windowRect.bottom - monitorInfo.rcMonitor.bottom) <= 16;

    if(!supportedProcess || !fullScreen) {
        if(reason) *reason = "foreground_not_slideshow";
        return false;
    }

    const WORD virtualKey = nextPage ? VK_NEXT : VK_PRIOR;
    INPUT inputs[2] = {};
    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki.wVk = virtualKey;
    inputs[1] = inputs[0];
    inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;
    if(SendInput(2, inputs, sizeof(INPUT)) != 2) {
        if(reason) *reason = "send_input_failed";
        return false;
    }
    if(reason) *reason = nextPage ? "next" : "previous";
    return true;
#else
    Q_UNUSED(nextPage);
    if(reason) *reason = "unsupported_platform";
    return false;
#endif
}
}

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent),
      m_serial(new QSerialPort(this)),
      m_server(new QTcpServer(this)),
      m_heartbeatTimer(new QTimer(this)),
      m_focusTimer(new QTimer(this))
{
    buildUi();
    setWindowTitle("ESP-WATCH 上位机");
    resize(960, 720);

    connect(m_serial, &QSerialPort::readyRead, this, &MainWindow::consumeSerialData);
    connect(m_server, &QTcpServer::newConnection, this, &MainWindow::acceptClient);
    connect(m_heartbeatTimer, &QTimer::timeout, this, &MainWindow::heartbeatTick);
    m_heartbeatTimer->setInterval(5000);
    connect(m_focusTimer, &QTimer::timeout, this, &MainWindow::focusTick);
    m_focusTimer->setInterval(250);
    m_focusTimer->start();

    QSettings settings;
    m_hostPort->setValue(settings.value("network/port", 8765).toInt());
    m_pairToken->setText(settings.value("network/token").toString());
    if(m_pairToken->text().isEmpty()) {
        m_pairToken->setText(createPairToken());
    }

    refreshSerialPorts();
    setSerialControlsEnabled(false);
    setWirelessStatus("服务未启动", "#777777");
    updatePresentationAvailability();
    loadFocusHistory();
    refreshFocusUi();
    m_addressHint->setText(localIpv4Text());
    appendLog("程序已启动。先开启电脑热点，再通过 USB 写入热点和无线服务配置。");
}

MainWindow::~MainWindow()
{
    if(m_serial->isOpen()) {
        m_serial->close();
    }
    disconnectClient();
    m_server->close();
}

void MainWindow::buildUi()
{
    auto *tabs = new QTabWidget(this);
    tabs->addTab(buildConnectionPage(), "设备连接");
    tabs->addTab(buildFocusPage(), "专注任务");
    tabs->addTab(buildPresentationPage(), "演示遥控");
    setCentralWidget(tabs);
}

QWidget *MainWindow::buildPresentationPage()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    auto *heading = new QLabel("WPS 演示遥控");
    heading->setStyleSheet("font-size:24px;font-weight:600;");
    m_presentationConnection = new QLabel("手表连接：离线");
    m_presentationWatchPage = new QLabel("手表页面：未进入演示遥控");
    m_presentationEnabled = new QCheckBox("启用演示控制");
    m_presentationEnabled->setEnabled(false);
    m_presentationRecent = new QLabel("最近指令：无");
    m_presentationRecent->setWordWrap(true);
    auto *help = new QLabel(
        "开启此开关后，请切回 WPS 全屏放映窗口并让它保持在前台。手表上拨上一页、下拨下一页。"
        "电脑只确认模拟按键已执行，不读取或推算当前页码；断线指令不会补发。");
    help->setWordWrap(true);
    help->setStyleSheet("color:#666666;");

    layout->addWidget(heading);
    layout->addSpacing(16);
    layout->addWidget(m_presentationConnection);
    layout->addWidget(m_presentationWatchPage);
    layout->addWidget(m_presentationEnabled);
    layout->addSpacing(12);
    layout->addWidget(m_presentationRecent);
    layout->addWidget(help);
    layout->addStretch();

    connect(m_presentationEnabled, &QCheckBox::toggled, this,
            [this](bool enabled) { setPresentationEnabled(enabled); });
    return page;
}

QWidget *MainWindow::buildFocusPage()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);

    auto *createGroup = new QGroupBox("新建专注任务");
    auto *createLayout = new QHBoxLayout(createGroup);
    m_focusProject = new QLineEdit;
    m_focusProject->setPlaceholderText("项目名称，例如：复变函数习题");
    m_focusMinutes = new QSpinBox;
    m_focusMinutes->setRange(1, 720);
    m_focusMinutes->setValue(25);
    m_focusMinutes->setSuffix(" 分钟");
    m_focusCreate = new QPushButton("创建任务");
    createLayout->addWidget(m_focusProject, 1);
    createLayout->addWidget(m_focusMinutes);
    createLayout->addWidget(m_focusCreate);

    auto *currentGroup = new QGroupBox("当前任务");
    auto *currentLayout = new QVBoxLayout(currentGroup);
    m_focusConnection = new QLabel("手表连接：离线");
    m_focusCurrentProject = new QLabel("尚未创建任务");
    m_focusCurrentProject->setWordWrap(true);
    m_focusCurrentProject->setStyleSheet("font-size:22px;font-weight:600;");
    m_focusRemaining = new QLabel("00:00");
    m_focusRemaining->setAlignment(Qt::AlignCenter);
    m_focusRemaining->setStyleSheet("font-size:52px;font-weight:600;color:#3b82f6;");
    m_focusStateLabel = new QLabel("状态：无任务");
    m_focusStateLabel->setAlignment(Qt::AlignCenter);
    auto *buttons = new QHBoxLayout;
    m_focusToggle = new QPushButton("开始");
    m_focusAbort = new QPushButton("结束任务");
    buttons->addStretch();
    buttons->addWidget(m_focusToggle);
    buttons->addWidget(m_focusAbort);
    buttons->addStretch();
    currentLayout->addWidget(m_focusConnection);
    currentLayout->addWidget(m_focusCurrentProject);
    currentLayout->addWidget(m_focusRemaining);
    currentLayout->addWidget(m_focusStateLabel);
    currentLayout->addLayout(buttons);

    auto *historyGroup = new QGroupBox("专注历史");
    auto *historyLayout = new QVBoxLayout(historyGroup);
    m_focusHistoryTable = new QTableWidget(0, 4);
    m_focusHistoryTable->setHorizontalHeaderLabels({"项目", "计划", "实际", "结果"});
    m_focusHistoryTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_focusHistoryTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_focusHistoryTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_focusHistoryTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_focusHistoryTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_focusHistoryTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    historyLayout->addWidget(m_focusHistoryTable);

    layout->addWidget(createGroup);
    layout->addWidget(currentGroup);
    layout->addWidget(historyGroup, 1);

    connect(m_focusCreate, &QPushButton::clicked, this, &MainWindow::createFocusTask);
    connect(m_focusToggle, &QPushButton::clicked, this, &MainWindow::toggleFocusTask);
    connect(m_focusAbort, &QPushButton::clicked, this, &MainWindow::abortFocusTask);
    return page;
}

QWidget *MainWindow::buildConnectionPage()
{
    auto *page = new QWidget;
    auto *pageLayout = new QVBoxLayout(page);

    auto *topRow = new QHBoxLayout;
    auto *usbGroup = new QGroupBox("USB 配置");
    auto *usbLayout = new QVBoxLayout(usbGroup);
    auto *serialRow = new QHBoxLayout;
    m_portCombo = new QComboBox;
    m_baudCombo = new QComboBox;
    m_baudCombo->addItems({"115200", "460800", "921600"});
    m_serialButton = new QPushButton("连接串口");
    auto *refreshButton = new QPushButton("刷新");
    serialRow->addWidget(m_portCombo, 1);
    serialRow->addWidget(m_baudCombo);
    serialRow->addWidget(refreshButton);
    serialRow->addWidget(m_serialButton);
    usbLayout->addLayout(serialRow);

    auto *form = new QFormLayout;
    m_wifiSsid = new QLineEdit;
    m_wifiPassword = new QLineEdit;
    m_wifiPassword->setEchoMode(QLineEdit::Password);
    m_owner = new QLineEdit;
    m_deviceName = new QLineEdit;
    m_deviceId = new QLineEdit;
    m_deviceId->setReadOnly(true);
    m_biliUid = new QLineEdit;
    m_sessdata = new QLineEdit;
    m_sessdata->setEchoMode(QLineEdit::Password);
    m_latitude = new QLineEdit;
    m_longitude = new QLineEdit;
    m_hostAddress = new QLineEdit;
    m_hostAddress->setPlaceholderText("例如 192.168.137.1");
    m_hostPort = new QSpinBox;
    m_hostPort->setRange(1, 65535);
    m_pairToken = new QLineEdit;
    form->addRow("热点名称（SSID）", m_wifiSsid);
    form->addRow("热点密码", m_wifiPassword);
    form->addRow("拥有者（ASCII）", m_owner);
    form->addRow("设备名（ASCII）", m_deviceName);
    form->addRow("设备 ID", m_deviceId);
    form->addRow("B站 UID", m_biliUid);
    form->addRow("SESSDATA", m_sessdata);
    form->addRow("纬度", m_latitude);
    form->addRow("经度", m_longitude);
    form->addRow("电脑 IPv4 地址", m_hostAddress);
    form->addRow("服务端口", m_hostPort);
    form->addRow("配对令牌", m_pairToken);
    usbLayout->addLayout(form);

    auto *configButtons = new QHBoxLayout;
    m_readButton = new QPushButton("读取配置");
    m_writeButton = new QPushButton("写入配置");
    m_rebootButton = new QPushButton("重启手表");
    configButtons->addWidget(m_readButton);
    configButtons->addWidget(m_writeButton);
    configButtons->addWidget(m_rebootButton);
    usbLayout->addLayout(configButtons);

    auto *wirelessGroup = new QGroupBox("无线服务");
    auto *wirelessLayout = new QVBoxLayout(wirelessGroup);
    m_serverStatus = new QLabel;
    m_serverStatus->setStyleSheet("font-size: 20px; font-weight: 600;");
    m_addressHint = new QLabel;
    m_addressHint->setWordWrap(true);
    m_connectedDevice = new QLabel("当前设备：无");
    m_connectedDevice->setWordWrap(true);
    m_serverButton = new QPushButton("启动无线服务");
    auto *help = new QLabel("端口和配对令牌必须与写入手表的值一致。首次弹出 Windows 防火墙提示时，请允许专用网络访问。");
    help->setWordWrap(true);
    wirelessLayout->addWidget(m_serverStatus);
    wirelessLayout->addWidget(m_addressHint);
    wirelessLayout->addWidget(m_connectedDevice);
    wirelessLayout->addWidget(help);
    wirelessLayout->addWidget(m_serverButton);
    wirelessLayout->addStretch();

    topRow->addWidget(usbGroup, 3);
    topRow->addWidget(wirelessGroup, 2);
    pageLayout->addLayout(topRow, 3);

    m_log = new QPlainTextEdit;
    m_log->setReadOnly(true);
    m_log->setMaximumBlockCount(500);
    pageLayout->addWidget(new QLabel("运行日志"));
    pageLayout->addWidget(m_log, 2);

    connect(refreshButton, &QPushButton::clicked, this, &MainWindow::refreshSerialPorts);
    connect(m_serialButton, &QPushButton::clicked, this, &MainWindow::toggleSerial);
    connect(m_readButton, &QPushButton::clicked, this, &MainWindow::readDeviceConfig);
    connect(m_writeButton, &QPushButton::clicked, this, &MainWindow::writeDeviceConfig);
    connect(m_rebootButton, &QPushButton::clicked, this, &MainWindow::rebootDevice);
    connect(m_serverButton, &QPushButton::clicked, this, &MainWindow::toggleServer);
    return page;
}

QWidget *MainWindow::buildPlaceholderPage(const QString &title, const QString &description)
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    auto *heading = new QLabel(title);
    heading->setStyleSheet("font-size: 24px; font-weight: 600;");
    auto *body = new QLabel(description);
    body->setWordWrap(true);
    layout->addStretch();
    layout->addWidget(heading, 0, Qt::AlignHCenter);
    layout->addWidget(body, 0, Qt::AlignHCenter);
    layout->addStretch();
    return page;
}

void MainWindow::refreshSerialPorts()
{
    const QString oldPort = m_portCombo->currentData().toString();
    m_portCombo->clear();
    for(const QSerialPortInfo &info : QSerialPortInfo::availablePorts()) {
        QString label = info.portName();
        if(!info.description().isEmpty()) {
            label += " - " + info.description();
        }
        m_portCombo->addItem(label, info.portName());
    }
    const int oldIndex = m_portCombo->findData(oldPort);
    if(oldIndex >= 0) {
        m_portCombo->setCurrentIndex(oldIndex);
    }
    appendLog(QString("发现 %1 个串口。").arg(m_portCombo->count()));
}

void MainWindow::toggleSerial()
{
    if(m_serial->isOpen()) {
        m_serial->close();
        m_serialButton->setText("连接串口");
        setSerialControlsEnabled(false);
        appendLog("USB 串口已断开。");
        return;
    }
    if(m_portCombo->currentIndex() < 0) {
        QMessageBox::warning(this, "没有串口", "请连接手表并点击刷新。");
        return;
    }
    m_serial->setPortName(m_portCombo->currentData().toString());
    m_serial->setBaudRate(m_baudCombo->currentText().toInt());
    m_serial->setDataBits(QSerialPort::Data8);
    m_serial->setParity(QSerialPort::NoParity);
    m_serial->setStopBits(QSerialPort::OneStop);
    m_serial->setFlowControl(QSerialPort::NoFlowControl);
    if(!m_serial->open(QIODevice::ReadWrite)) {
        QMessageBox::critical(this, "串口打开失败", m_serial->errorString());
        return;
    }
    m_serialBuffer.clear();
    m_serialButton->setText("断开串口");
    setSerialControlsEnabled(true);
    appendLog("USB 串口已连接：" + m_serial->portName());
}

void MainWindow::setSerialControlsEnabled(bool connected)
{
    m_portCombo->setEnabled(!connected);
    m_baudCombo->setEnabled(!connected);
    m_readButton->setEnabled(connected);
    m_writeButton->setEnabled(connected);
    m_rebootButton->setEnabled(connected);
}

void MainWindow::readDeviceConfig()
{
    sendSerialJson({{"cmd", "get_config"}});
}

void MainWindow::writeDeviceConfig()
{
    QJsonObject object{{"cmd", "set_config"}};
    auto addText = [&object](const char *key, QLineEdit *edit) {
        if(!edit->text().isEmpty()) object.insert(key, edit->text());
    };
    addText("wifi_ssid", m_wifiSsid);
    addText("wifi_pass", m_wifiPassword);
    addText("owner", m_owner);
    addText("device_name", m_deviceName);
    addText("bili_uid", m_biliUid);
    addText("sessdata", m_sessdata);
    addText("latitude", m_latitude);
    addText("longitude", m_longitude);
    addText("host_addr", m_hostAddress);
    addText("pair_token", m_pairToken);
    object.insert("host_port", m_hostPort->value());

    if(object.size() <= 2) {
        QMessageBox::warning(this, "没有配置", "请至少填写一个配置项。");
        return;
    }
    sendSerialJson(object);
    appendLog("配置已发送；未填写的字符串字段会保留手表中的原值。修改 Wi-Fi 后请重启手表。");
}

void MainWindow::rebootDevice()
{
    sendSerialJson({{"cmd", "reboot"}});
}

void MainWindow::sendSerialJson(const QJsonObject &object)
{
    if(!m_serial->isOpen()) return;
    QByteArray bytes = QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
    m_serial->write(bytes);
    QJsonObject safe = object;
    for(const char *key : {"wifi_pass", "sessdata", "pair_token"}) {
        if(safe.contains(key)) safe.insert(key, "***");
    }
    appendLog("USB TX: " + QString::fromUtf8(QJsonDocument(safe).toJson(QJsonDocument::Compact)));
}

void MainWindow::consumeSerialData()
{
    m_serialBuffer += m_serial->readAll();
    while(true) {
        const qsizetype newline = m_serialBuffer.indexOf('\n');
        if(newline < 0) break;
        QByteArray line = m_serialBuffer.left(newline).trimmed();
        m_serialBuffer.remove(0, newline + 1);
        if(line.isEmpty()) continue;
        QJsonParseError error;
        const QJsonDocument document = QJsonDocument::fromJson(line, &error);
        if(error.error == QJsonParseError::NoError && document.isObject()) {
            handleSerialMessage(document.object());
        }
    }
    if(m_serialBuffer.size() > kMaximumLine) m_serialBuffer.clear();
}

void MainWindow::handleSerialMessage(const QJsonObject &object)
{
    appendLog("USB RX: " + QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact)));
    if(object.value("msg").toString() != "config") return;
    m_wifiSsid->setText(object.value("wifi_ssid").toString());
    m_owner->setText(object.value("owner").toString());
    m_deviceName->setText(object.value("device_name").toString());
    m_deviceId->setText(object.value("device_id").toString());
    m_biliUid->setText(object.value("bili_uid").toString());
    m_latitude->setText(object.value("latitude").toString());
    m_longitude->setText(object.value("longitude").toString());
    m_hostAddress->setText(object.value("host_addr").toString());
    m_hostPort->setValue(object.value("host_port").toInt(8765));
    m_wifiPassword->clear();
    m_sessdata->clear();
    if(object.value("has_wifi_pass").toBool()) m_wifiPassword->setPlaceholderText("已保存；留空则保持原值");
    if(object.value("has_sessdata").toBool()) m_sessdata->setPlaceholderText("已保存；留空则保持原值");
    if(object.value("has_pair_token").toBool()) m_pairToken->setPlaceholderText("已保存；需与电脑当前令牌一致");
}

void MainWindow::toggleServer()
{
    if(m_server->isListening()) {
        disconnectClient();
        m_server->close();
        m_heartbeatTimer->stop();
        m_serverButton->setText("启动无线服务");
        setWirelessStatus("服务未启动", "#777777");
        appendLog("无线服务已停止。");
        return;
    }
    if(m_pairToken->text().size() < 8) {
        QMessageBox::warning(this, "配对令牌过短", "配对令牌至少需要 8 个字符。");
        return;
    }
    if(!m_server->listen(QHostAddress::AnyIPv4, quint16(m_hostPort->value()))) {
        QMessageBox::critical(this, "启动失败", m_server->errorString());
        return;
    }
    m_addressHint->setText(localIpv4Text());
    QSettings settings;
    settings.setValue("network/port", m_hostPort->value());
    settings.setValue("network/token", m_pairToken->text());
    m_serverButton->setText("停止无线服务");
    setWirelessStatus(QString("正在监听端口 %1").arg(m_hostPort->value()), "#d28b00");
    m_heartbeatTimer->start();
    appendLog(QString("无线服务已启动，监听 0.0.0.0:%1。").arg(m_hostPort->value()));
}

void MainWindow::acceptClient()
{
    while(m_server->hasPendingConnections()) {
        QTcpSocket *socket = m_server->nextPendingConnection();
        if(m_client) {
            appendLog("已有手表连接，拒绝额外客户端。");
            socket->disconnectFromHost();
            socket->deleteLater();
            continue;
        }
        m_client = socket;
        m_clientBuffer.clear();
        m_authenticated = false;
        m_lastClientRx.start();
        connect(socket, &QTcpSocket::readyRead, this, &MainWindow::consumeClientData);
        connect(socket, &QTcpSocket::disconnected, this, &MainWindow::disconnectClient);
        setWirelessStatus("客户端正在认证", "#d28b00");
        appendLog("收到连接：" + socket->peerAddress().toString());
    }
}

void MainWindow::consumeClientData()
{
    if(!m_client) return;
    m_clientBuffer += m_client->readAll();
    m_lastClientRx.restart();
    while(true) {
        const qsizetype newline = m_clientBuffer.indexOf('\n');
        if(newline < 0) break;
        QByteArray line = m_clientBuffer.left(newline).trimmed();
        m_clientBuffer.remove(0, newline + 1);
        if(line.isEmpty()) continue;
        QJsonParseError error;
        const QJsonDocument document = QJsonDocument::fromJson(line, &error);
        if(error.error != QJsonParseError::NoError || !document.isObject()) {
            appendLog("无线 RX：JSON 格式错误。");
            continue;
        }
        handleClientMessage(document.object());
    }
    if(m_clientBuffer.size() > kMaximumLine) {
        appendLog("无线消息过长，断开客户端。");
        m_client->disconnectFromHost();
    }
}

void MainWindow::handleClientMessage(const QJsonObject &object)
{
    const int version = object.value("v").toInt(-1);
    const QString type = object.value("type").toString();
    if(version != kProtocolVersion || type.isEmpty()) {
        appendLog("无线消息版本或类型无效。");
        if(m_client) m_client->disconnectFromHost();
        return;
    }

    if(type == "hello") {
        const bool tokenOk = object.value("token").toString() == m_pairToken->text();
        const QString deviceId = object.value("device_id").toString();
        const bool accepted = tokenOk && !deviceId.isEmpty();
        m_sessionId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        sendClientJson({{"v", kProtocolVersion}, {"type", "hello_ack"},
                        {"ok", accepted}, {"session", m_sessionId},
                        {"reason", accepted ? "accepted" : "token or device invalid"}});
        if(!accepted) {
            appendLog("手表认证失败：令牌或设备 ID 不正确。");
            if(m_client) m_client->disconnectFromHost();
            return;
        }
        m_authenticated = true;
        m_watchPresentationActive = false;
        m_presentationResults.clear();
        m_presentationResultOrder.clear();
        m_focusResults.clear();
        m_focusResultOrder.clear();
        setPresentationEnabled(false, false);
        updatePresentationAvailability();
        m_connectedDevice->setText(QString("当前设备：%1（%2）")
                                   .arg(object.value("device_name").toString(), deviceId));
        setWirelessStatus("手表已连接", "#16833a");
        appendLog("手表认证成功，设备 ID：" + deviceId);
        refreshFocusUi();
        sendFocusSnapshot();
        return;
    }

    if(!m_authenticated) {
        if(m_client) m_client->disconnectFromHost();
        return;
    }
    if(type == "heartbeat") {
        sendClientJson({{"v", kProtocolVersion}, {"type", "heartbeat_ack"},
                        {"seq", object.value("seq")}, {"session", m_sessionId}});
    } else if(type == "pong") {
        // 收到即由 m_lastClientRx 更新存活时间。
    } else if(type == "presentation_state") {
        if(object.value("session").toString() != m_sessionId ||
           !object.value("active").isBool()) {
            appendLog("演示页面状态的会话或字段无效。");
            return;
        }
        m_watchPresentationActive = object.value("active").toBool();
        updatePresentationAvailability();
        appendLog(m_watchPresentationActive ? "手表已进入演示遥控页面。" :
                                              "手表已退出演示遥控页面。");
    } else if(type == "presentation_control") {
        handlePresentationControl(object);
    } else if(type == "focus_page_state") {
        if(object.value("session").toString() == m_sessionId &&
           object.value("active").isBool()) {
            m_watchFocusActive = object.value("active").toBool();
            if(m_watchFocusActive) sendFocusSnapshot();
        }
    } else if(type == "focus_sync") {
        if(object.value("session").toString() == m_sessionId) sendFocusSnapshot();
    } else if(type == "focus_control") {
        handleFocusControl(object);
    } else {
        appendLog("收到尚未在阶段 6 启用的消息：" + type);
    }
}

void MainWindow::sendClientJson(const QJsonObject &object)
{
    if(!m_client) return;
    m_client->write(QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n');
}

void MainWindow::heartbeatTick()
{
    if(!m_client) return;
    if(m_lastClientRx.isValid() && m_lastClientRx.elapsed() > 15000) {
        appendLog("手表心跳超时，关闭连接并等待重连。");
        m_client->disconnectFromHost();
        return;
    }
    if(m_authenticated) {
        sendClientJson({{"v", kProtocolVersion}, {"type", "ping"},
                        {"seq", int(m_pingSequence++)}, {"session", m_sessionId}});
    }
}

void MainWindow::disconnectClient()
{
    if(m_client) {
        disconnect(m_client, nullptr, this, nullptr);
        m_client->close();
        m_client->deleteLater();
        m_client = nullptr;
    }
    m_authenticated = false;
    m_watchPresentationActive = false;
    m_watchFocusActive = false;
    setPresentationEnabled(false, false);
    m_presentationResults.clear();
    m_presentationResultOrder.clear();
    m_focusResults.clear();
    m_focusResultOrder.clear();
    updatePresentationAvailability();
    m_clientBuffer.clear();
    m_connectedDevice->setText("当前设备：无");
    if(m_server->isListening()) setWirelessStatus("服务已启动，等待手表", "#d28b00");
    refreshFocusUi();
}

void MainWindow::setPresentationEnabled(bool enabled, bool notifyWatch)
{
    if(enabled && !m_authenticated) enabled = false;
    if(m_presentationEnabled && m_presentationEnabled->isChecked() != enabled) {
        const QSignalBlocker blocker(m_presentationEnabled);
        m_presentationEnabled->setChecked(enabled);
    }
    if(notifyWatch && m_authenticated) {
        sendClientJson({{"v", kProtocolVersion}, {"type", "presentation_status"},
                        {"session", m_sessionId}, {"enabled", enabled}});
    }
    appendLog(enabled ? "演示控制已开启。" : "演示控制已暂停。");
    if(m_presentationRecent && !enabled) {
        m_presentationRecent->setText("最近指令：演示控制已暂停");
    }
}

void MainWindow::updatePresentationAvailability()
{
    if(!m_presentationEnabled) return;
    m_presentationEnabled->setEnabled(m_authenticated);
    m_presentationConnection->setText(m_authenticated ? "手表连接：在线" : "手表连接：离线");
    m_presentationConnection->setStyleSheet(m_authenticated ? "color:#16833a;" : "color:#777777;");
    m_presentationWatchPage->setText(m_watchPresentationActive ?
        "手表页面：演示遥控已打开" : "手表页面：未进入演示遥控");
}

void MainWindow::handlePresentationControl(const QJsonObject &object)
{
    const QString requestSession = object.value("session").toString();
    const double rawOpId = object.value("op_id").toDouble(-1);
    const QString action = object.value("action").toString();
    if(requestSession != m_sessionId || rawOpId < 1 || rawOpId > 4294967295.0 ||
       (action != "next" && action != "previous")) {
        appendLog("忽略字段无效的演示控制请求。");
        return;
    }

    const quint32 opId = quint32(rawOpId);
    if(m_presentationResults.contains(opId)) {
        sendClientJson(m_presentationResults.value(opId));
        appendLog(QString("演示指令 #%1 重复，返回首次结果但不再次翻页。").arg(opId));
        return;
    }

    QString reason;
    bool processed = false;
    if(!m_presentationEnabled->isChecked()) {
        reason = "disabled";
    } else if(!m_watchPresentationActive) {
        reason = "watch_inactive";
    } else {
        processed = sendPresentationKey(action == "next", &reason);
    }

    QJsonObject result{{"v", kProtocolVersion}, {"type", "presentation_result"},
                       {"session", m_sessionId}, {"op_id", int(opId)},
                       {"outcome", processed ? "processed" : "rejected"},
                       {"reason", reason}};
    m_presentationResults.insert(opId, result);
    m_presentationResultOrder.append(opId);
    if(m_presentationResultOrder.size() > kPresentationResultCache) {
        m_presentationResults.remove(m_presentationResultOrder.takeFirst());
    }
    sendClientJson(result);

    if(processed) {
        const QString direction = action == "next" ? "下一页" : "上一页";
        m_presentationRecent->setText(QString("最近指令：%1（电脑已发送按键）").arg(direction));
        appendLog(QString("演示指令 #%1：已向 WPS/PowerPoint 发送%2按键。")
                  .arg(opId).arg(direction));
    } else {
        const QString displayReason = reason == "disabled" ? "电脑端开关未开启" :
            reason == "watch_inactive" ? "手表未处于演示遥控页面" :
            reason == "foreground_not_slideshow" ? "前台不是 WPS/PowerPoint 全屏放映" :
            "Windows 模拟按键失败";
        m_presentationRecent->setText("最近指令：已拒绝（" + displayReason + "）");
        appendLog(QString("演示指令 #%1 已拒绝：%2。").arg(opId).arg(displayReason));
    }
}

void MainWindow::createFocusTask()
{
    const QString project = m_focusProject->text().trimmed();
    if(project.isEmpty()) {
        QMessageBox::warning(this, "缺少项目名称", "请输入本次专注的项目名称。");
        return;
    }
    if(m_focusState == FocusState::Ready || m_focusState == FocusState::Running ||
       m_focusState == FocusState::Paused) {
        QMessageBox::information(this, "已有任务", "请先完成或结束当前任务。");
        return;
    }

    m_focusTaskId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_focusTaskProject = project.left(80);
    m_focusPlannedMs = qint64(m_focusMinutes->value()) * 60 * 1000;
    m_focusAccumulatedMs = 0;
    m_focusState = FocusState::Ready;
    m_focusRunClock.invalidate();
    m_focusLastBroadcastSecond = -1;
    ++m_focusVersion;
    m_focusProject->clear();
    refreshFocusUi();
    sendFocusSnapshot();
    appendLog("已创建专注任务：" + m_focusTaskProject);
}

void MainWindow::toggleFocusTask()
{
    if(m_focusState == FocusState::Ready || m_focusState == FocusState::Paused) {
        m_focusState = FocusState::Running;
        m_focusRunClock.start();
    } else if(m_focusState == FocusState::Running) {
        m_focusAccumulatedMs += m_focusRunClock.elapsed();
        m_focusRunClock.invalidate();
        m_focusState = FocusState::Paused;
    } else {
        return;
    }
    m_focusLastBroadcastSecond = -1;
    ++m_focusVersion;
    refreshFocusUi();
    sendFocusSnapshot();
}

void MainWindow::abortFocusTask()
{
    if(m_focusState != FocusState::Ready && m_focusState != FocusState::Running &&
       m_focusState != FocusState::Paused) return;
    if(QMessageBox::question(this, "结束专注", "确定中途结束本次专注吗？",
                             QMessageBox::Yes | QMessageBox::No,
                             QMessageBox::No) != QMessageBox::Yes) return;
    finishFocusTask(false);
}

void MainWindow::focusTick()
{
    qint64 focused = m_focusAccumulatedMs;
    if(m_focusState == FocusState::Running && m_focusRunClock.isValid()) {
        focused += m_focusRunClock.elapsed();
        if(focused >= m_focusPlannedMs) {
            finishFocusTask(true);
            return;
        }
    }
    refreshFocusUi();
    const qint64 second = focused / 1000;
    if(m_focusState == FocusState::Running && second != m_focusLastBroadcastSecond) {
        m_focusLastBroadcastSecond = second;
        ++m_focusVersion;
        sendFocusSnapshot();
    }
}

void MainWindow::refreshFocusUi()
{
    if(!m_focusRemaining) return;
    qint64 focused = m_focusAccumulatedMs;
    if(m_focusState == FocusState::Running && m_focusRunClock.isValid()) {
        focused += m_focusRunClock.elapsed();
    }
    const qint64 remaining = qMax<qint64>(0, m_focusPlannedMs - focused);
    const qint64 seconds = (remaining + 999) / 1000;
    m_focusRemaining->setText(QString("%1:%2")
        .arg(seconds / 60, 2, 10, QLatin1Char('0'))
        .arg(seconds % 60, 2, 10, QLatin1Char('0')));
    m_focusCurrentProject->setText(m_focusTaskProject.isEmpty() ?
                                   "尚未创建任务" : m_focusTaskProject);
    const QString state = m_focusState == FocusState::Ready ? "待开始" :
        m_focusState == FocusState::Running ? "专注中" :
        m_focusState == FocusState::Paused ? "已暂停" :
        m_focusState == FocusState::Completed ? "已完成" :
        m_focusState == FocusState::Aborted ? "已中止" : "无任务";
    m_focusStateLabel->setText(QString("状态：%1　计划：%2 分钟　有效专注：%3 分 %4 秒")
        .arg(state).arg(m_focusPlannedMs / 60000).arg(focused / 60000)
        .arg((focused / 1000) % 60));
    m_focusConnection->setText(m_authenticated ? "手表连接：在线" : "手表连接：离线（电脑仍继续计时）");
    m_focusConnection->setStyleSheet(m_authenticated ? "color:#16833a;" : "color:#777777;");
    const bool active = m_focusState == FocusState::Ready ||
                        m_focusState == FocusState::Running ||
                        m_focusState == FocusState::Paused;
    m_focusCreate->setEnabled(!active);
    m_focusProject->setEnabled(!active);
    m_focusMinutes->setEnabled(!active);
    m_focusToggle->setEnabled(active);
    m_focusAbort->setEnabled(active);
    m_focusToggle->setText(m_focusState == FocusState::Running ? "暂停" :
                           m_focusState == FocusState::Paused ? "继续" : "开始");
}

void MainWindow::sendFocusSnapshot()
{
    if(!m_authenticated) return;
    qint64 focused = m_focusAccumulatedMs;
    if(m_focusState == FocusState::Running && m_focusRunClock.isValid()) {
        focused += m_focusRunClock.elapsed();
    }
    focused = qMin(focused, m_focusPlannedMs);
    const qint64 remaining = qMax<qint64>(0, m_focusPlannedMs - focused);
    const QString state = m_focusState == FocusState::Ready ? "ready" :
        m_focusState == FocusState::Running ? "running" :
        m_focusState == FocusState::Paused ? "paused" :
        m_focusState == FocusState::Completed ? "completed" :
        m_focusState == FocusState::Aborted ? "aborted" : "none";
    sendClientJson({{"v", kProtocolVersion}, {"type", "focus_snapshot"},
                    {"session", m_sessionId}, {"task_id", m_focusTaskId},
                    {"version", int(m_focusVersion)}, {"project", m_focusTaskProject},
                    {"state", state}, {"planned_sec", m_focusPlannedMs / 1000},
                    {"focused_sec", focused / 1000},
                    {"remaining_sec", (remaining + 999) / 1000}});
}

void MainWindow::handleFocusControl(const QJsonObject &object)
{
    const QString session = object.value("session").toString();
    const double rawId = object.value("op_id").toDouble(-1);
    const QString action = object.value("action").toString();
    if(session != m_sessionId || rawId < 1 || rawId > 4294967295.0) return;
    const quint32 opId = quint32(rawId);
    if(m_focusResults.contains(opId)) {
        sendClientJson(m_focusResults.value(opId));
        sendFocusSnapshot();
        return;
    }

    bool accepted = false;
    if(action == "start" && m_focusState == FocusState::Ready) {
        toggleFocusTask();
        accepted = true;
    } else if(action == "pause" && m_focusState == FocusState::Running) {
        toggleFocusTask();
        accepted = true;
    } else if(action == "resume" && m_focusState == FocusState::Paused) {
        toggleFocusTask();
        accepted = true;
    } else if(action == "abort" && (m_focusState == FocusState::Ready ||
              m_focusState == FocusState::Running || m_focusState == FocusState::Paused)) {
        finishFocusTask(false);
        accepted = true;
    }
    QJsonObject result{{"v", kProtocolVersion}, {"type", "focus_result"},
                       {"session", m_sessionId}, {"op_id", int(opId)},
                       {"outcome", accepted ? "processed" : "rejected"},
                       {"reason", accepted ? "ok" : "invalid_state"}};
    m_focusResults.insert(opId, result);
    m_focusResultOrder.append(opId);
    if(m_focusResultOrder.size() > kPresentationResultCache) {
        m_focusResults.remove(m_focusResultOrder.takeFirst());
    }
    sendClientJson(result);
    sendFocusSnapshot();
}

void MainWindow::finishFocusTask(bool completed)
{
    if(m_focusState == FocusState::Running && m_focusRunClock.isValid()) {
        m_focusAccumulatedMs += m_focusRunClock.elapsed();
    }
    m_focusRunClock.invalidate();
    if(completed) m_focusAccumulatedMs = m_focusPlannedMs;
    m_focusAccumulatedMs = qMin(m_focusAccumulatedMs, m_focusPlannedMs);
    m_focusState = completed ? FocusState::Completed : FocusState::Aborted;
    ++m_focusVersion;

    bool exists = false;
    for(const QJsonObject &entry : m_focusHistory) {
        if(entry.value("task_id").toString() == m_focusTaskId) exists = true;
    }
    if(!exists && !m_focusTaskId.isEmpty()) {
        m_focusHistory.prepend({{"task_id", m_focusTaskId},
            {"project", m_focusTaskProject}, {"planned_sec", m_focusPlannedMs / 1000},
            {"focused_sec", m_focusAccumulatedMs / 1000},
            {"result", completed ? "completed" : "aborted"},
            {"finished_at", QDateTime::currentDateTime().toString(Qt::ISODate)}});
        saveFocusHistory();
        refreshFocusHistory();
    }
    refreshFocusUi();
    sendFocusSnapshot();
}

void MainWindow::loadFocusHistory()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QFile file(dir + "/focus_history.json");
    if(file.open(QIODevice::ReadOnly)) {
        const QJsonArray array = QJsonDocument::fromJson(file.readAll()).array();
        QSet<QString> ids;
        for(const QJsonValue &value : array) {
            const QJsonObject item = value.toObject();
            const QString id = item.value("task_id").toString();
            if(!id.isEmpty() && !ids.contains(id)) {
                ids.insert(id);
                m_focusHistory.append(item);
            }
        }
    }
    refreshFocusHistory();
}

void MainWindow::saveFocusHistory() const
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dir);
    QJsonArray array;
    for(const QJsonObject &item : m_focusHistory) array.append(item);
    QSaveFile file(dir + "/focus_history.json");
    if(file.open(QIODevice::WriteOnly)) {
        file.write(QJsonDocument(array).toJson(QJsonDocument::Indented));
        file.commit();
    }
}

void MainWindow::refreshFocusHistory()
{
    if(!m_focusHistoryTable) return;
    m_focusHistoryTable->setRowCount(m_focusHistory.size());
    for(int row = 0; row < m_focusHistory.size(); ++row) {
        const QJsonObject item = m_focusHistory.at(row);
        const qint64 planned = item.value("planned_sec").toInteger();
        const qint64 focused = item.value("focused_sec").toInteger();
        const QString result = item.value("result").toString() == "completed" ? "完成" : "中止";
        m_focusHistoryTable->setItem(row, 0, new QTableWidgetItem(item.value("project").toString()));
        m_focusHistoryTable->setItem(row, 1, new QTableWidgetItem(QString("%1 分").arg(planned / 60)));
        m_focusHistoryTable->setItem(row, 2, new QTableWidgetItem(QString("%1:%2")
            .arg(focused / 60).arg(focused % 60, 2, 10, QLatin1Char('0'))));
        m_focusHistoryTable->setItem(row, 3, new QTableWidgetItem(result));
    }
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if(m_focusState == FocusState::Running || m_focusState == FocusState::Paused) {
        if(QMessageBox::question(this, "退出上位机", "当前专注尚未结束。退出将中止并记录任务，是否继续？",
                                 QMessageBox::Yes | QMessageBox::No,
                                 QMessageBox::No) != QMessageBox::Yes) {
            event->ignore();
            return;
        }
        finishFocusTask(false);
    }
    event->accept();
}

void MainWindow::setWirelessStatus(const QString &text, const QString &color)
{
    m_serverStatus->setText(text);
    m_serverStatus->setStyleSheet(QString("font-size:20px;font-weight:600;color:%1").arg(color));
}

void MainWindow::appendLog(const QString &text)
{
    if(!m_log) return;
    m_log->appendPlainText("[" + QDateTime::currentDateTime().toString("HH:mm:ss") + "] " + text);
}

QString MainWindow::localIpv4Text() const
{
    QStringList addresses;
    for(const QHostAddress &address : QNetworkInterface::allAddresses()) {
        if(address.protocol() == QAbstractSocket::IPv4Protocol && !address.isLoopback()) {
            addresses << address.toString();
        }
    }
    return addresses.isEmpty() ? "未发现可用 IPv4 地址；请先开启电脑热点。"
                               : "电脑当前 IPv4：" + addresses.join("、");
}

QString MainWindow::createPairToken() const
{
    return QString::number(QRandomGenerator::global()->generate64(), 16).rightJustified(16, '0');
}
