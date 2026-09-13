#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QByteArray>
#include <QElapsedTimer>
#include <QJsonObject>
#include <QMainWindow>
#include <QPointer>

class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSerialPort;
class QSpinBox;
class QTcpServer;
class QTcpSocket;
class QTimer;

class MainWindow : public QMainWindow
{
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

private:
    void buildUi();
    QWidget *buildConnectionPage();
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
};

#endif
