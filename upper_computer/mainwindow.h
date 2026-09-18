#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QByteArray>
#include <QElapsedTimer>
#include <QJsonObject>
#include <QHash>
#include <QList>
#include <QMainWindow>
#include <QPointer>

class QComboBox;
class QCheckBox;
class QCloseEvent;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSerialPort;
class QSpinBox;
class QTcpServer;
class QTcpSocket;
class QTableWidget;
class QTimer;

class MainWindow : public QMainWindow
{
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    void buildUi();
    QWidget *buildConnectionPage();
    QWidget *buildPresentationPage();
    QWidget *buildFocusPage();
    QWidget *buildPlaceholderPage(const QString &title, const QString &description);

    void refreshSerialPorts();
    void toggleSerial();
    void readDeviceConfig();
    void writeDeviceConfig();
    void rebootDevice();
    void sendSerialJson(const QJsonObject &object);
    void consumeSerialData();
    void handleSerialMessage(const QJsonObject &object);
    void setSerialControlsEnabled(bool connected);

    void toggleServer();
    void acceptClient();
    void consumeClientData();
    void handleClientMessage(const QJsonObject &object);
    void disconnectClient();
    void sendClientJson(const QJsonObject &object);
    void heartbeatTick();
    void setWirelessStatus(const QString &text, const QString &color);
    void setPresentationEnabled(bool enabled, bool notifyWatch = true);
    void updatePresentationAvailability();
    void handlePresentationControl(const QJsonObject &object);
    void createFocusTask();
    void toggleFocusTask();
    void abortFocusTask();
    void handleFocusControl(const QJsonObject &object);
    void focusTick();
    void sendFocusSnapshot();
    void refreshFocusUi();
    void finishFocusTask(bool completed);
    void loadFocusHistory();
    void saveFocusHistory() const;
    void refreshFocusHistory();
    void appendLog(const QString &text);
    QString localIpv4Text() const;
    QString createPairToken() const;

    QSerialPort *m_serial;
    QByteArray m_serialBuffer;
    QComboBox *m_portCombo;
    QComboBox *m_baudCombo;
    QPushButton *m_serialButton;
    QPushButton *m_readButton;
    QPushButton *m_writeButton;
    QPushButton *m_rebootButton;

    QLineEdit *m_wifiSsid;
    QLineEdit *m_wifiPassword;
    QLineEdit *m_owner;
    QLineEdit *m_deviceName;
    QLineEdit *m_deviceId;
    QLineEdit *m_biliUid;
    QLineEdit *m_sessdata;
    QLineEdit *m_latitude;
    QLineEdit *m_longitude;
    QLineEdit *m_hostAddress;
    QSpinBox *m_hostPort;
    QLineEdit *m_pairToken;

    QTcpServer *m_server;
    QPointer<QTcpSocket> m_client;
    QByteArray m_clientBuffer;
    QTimer *m_heartbeatTimer;
    QElapsedTimer m_lastClientRx;
    bool m_authenticated = false;
    quint32 m_pingSequence = 1;
    QString m_sessionId;
    QPushButton *m_serverButton;
    QLabel *m_serverStatus;
    QLabel *m_addressHint;
    QLabel *m_connectedDevice;
    QPlainTextEdit *m_log;

    QCheckBox *m_presentationEnabled;
    QLabel *m_presentationConnection;
    QLabel *m_presentationWatchPage;
    QLabel *m_presentationRecent;
    bool m_watchPresentationActive = false;
    QHash<quint32, QJsonObject> m_presentationResults;
    QList<quint32> m_presentationResultOrder;

    enum class FocusState { None, Ready, Running, Paused, Completed, Aborted };
    QLineEdit *m_focusProject;
    QSpinBox *m_focusMinutes;
    QPushButton *m_focusCreate;
    QLabel *m_focusConnection;
    QLabel *m_focusCurrentProject;
    QLabel *m_focusRemaining;
    QLabel *m_focusStateLabel;
    QPushButton *m_focusToggle;
    QPushButton *m_focusAbort;
    QTableWidget *m_focusHistoryTable;
    QTimer *m_focusTimer;
    QElapsedTimer m_focusRunClock;
    FocusState m_focusState = FocusState::None;
    QString m_focusTaskId;
    QString m_focusTaskProject;
    qint64 m_focusPlannedMs = 0;
    qint64 m_focusAccumulatedMs = 0;
    quint32 m_focusVersion = 0;
    qint64 m_focusLastBroadcastSecond = -1;
    bool m_watchFocusActive = false;
    QList<QJsonObject> m_focusHistory;
    QHash<quint32, QJsonObject> m_focusResults;
    QList<quint32> m_focusResultOrder;
};

#endif
