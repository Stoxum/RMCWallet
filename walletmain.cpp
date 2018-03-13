#include <QApplication>
#include <QMessageBox>
#include <QStandardPaths>
#include <QFile>
#include <QDir>
#include <QTimer>
#include <QClipboard>
#include <QtGlobal>
#include <QSslCertificate>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QtNetwork>

#include "walletmain.h"
#include "ui_walletmain.h"
#include "transactionpreview.h"
#include "transactionview.h"
#include "doublevalidator.h"
#include "intvalidator.h"
#include "importdialog.h"
#include "enterpassword.h"
#include "aboutdialog.h"

#include <ripple/protocol/AccountID.h>
#include <ripple/protocol/digest.h>
#include <ripple/protocol/HashPrefix.h>
#include <ripple/protocol/JsonFields.h>
#include <ripple/protocol/Sign.h>
#include <ripple/protocol/st.h>
#include <ripple/protocol/TxFlags.h>
#include <ripple/basics/StringUtilities.h>

#include <algorithm>
#include <random>
#include <tuple>

static void showMessage(const QString& strCaption, const QString& strMessage, int nType)
{
    QMessageBox messageBox;
    if (nType == 0)
        messageBox.information(nullptr, strCaption, strMessage);
    if (nType == 1)
        messageBox.warning(nullptr, strCaption, strMessage);
    if (nType == 2)
        messageBox.critical(nullptr, strCaption, strMessage);
    messageBox.setFixedSize(500, 200);
}

//------------------------------------------------------------------------------

bool WalletMain::createPaymentTx(const QString& receiverAccount, std::int64_t nAmount, std::int64_t nTransactionFee, std::int64_t nDestinationID, QString& dataJson, QString& dataHex, QString& errorMsg)
{
    using namespace ripple;

    auto const destination = parseBase58<AccountID>(receiverAccount.toStdString());
    if (! destination) {
        errorMsg = "Invalid receiver address";
        return false;
    }

    if (nAmount <= 0) {
        errorMsg = "You can send only positive amounts.";
        return false;
    }

    STTx noopTx(ttPAYMENT,
                [&](auto& obj)
    {
        // General transaction fields
        obj[sfAccount] = keyData.accountID;
        obj[sfFee] = STAmount{ static_cast<uint64_t>(nTransactionFee) };
        obj[sfFlags] = tfFullyCanonicalSig;
        obj[sfSequence] = nSequence;
        obj[sfSigningPubKey] = keyData.publicKey.slice();
        // Payment-specific fields
        obj[sfAmount] = STAmount { static_cast<uint64_t>(nAmount) };
        obj[sfDestination] = *destination;
        if (nDestinationID != 0)
            obj[sfDestinationTag] = nDestinationID;

    });

    try
    {
        QString errorStr("");
        if (! askPassword(errorStr)) {
            if (errorStr == "Password")
                errorMsg = "Password";
            else
                errorMsg = QString("Error while decrypting your key: ") + errorStr;
            return false;
        }

        noopTx.sign(keyData.publicKey, keyData.secretKey);
        if (keyData.nDeriveIterations != 0)
            keyData.secretKey.~SecretKey();
        dataJson = noopTx.getJson(0).toStyledString().c_str();
        dataHex = strHex(noopTx.getSerializer().peekData()).c_str();
    }
    catch(const std::exception e)
    {
        errorMsg = e.what();
        return false;
    }

    return true;
}

bool WalletMain::loadWallet(QString& errorMsg)
{
    using namespace ripple;

    QFile keyFile;
    keyFile.setFileName(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + QDir::separator() + "keyStore.json");
    bool fOpened = keyFile.open(QIODevice::ReadOnly | QIODevice::Text);

    if (!fOpened)
    {
        // Brand new wallet
        ImportDialog importReq;
        while (true)
        {
            // Ask user to import his WIF formatted key
            if (importReq.exec() == QDialog::Accepted)
            {
                if(!importKey(importReq.getKeyData()))
                    continue; // Incorrect WIF string entered, ask user again
            }
            else newKey(); // User refused, generating new private key
            break;
        }

        return true;
    }
    else
    {
        QJsonObject keyObj = QJsonDocument::fromJson(keyFile.readAll()).object();
        keyFile.close();

        auto decodeResult1 = parseBase58<AccountID>(keyObj["account_id"].toString().toStdString());
        if (! decodeResult1)
        {
            errorMsg = "Unable to load your account ID, it looks like your wallet data was corrupted.";
            return false;
        }

        keyData.accountID = *decodeResult1;

        if ( !keyObj["private_key"].isUndefined() && !keyObj["private_key"].isNull())
        {
            // Plain wallet
            auto decodeResult = parseHex<SecretKey>(keyObj["private_key"].toString().toStdString());
            if (! decodeResult)
            {
                errorMsg = "Unable to read private key, it looks like your wallet data was corrupted";
                return false;
            }
            keyData.secretKey = *decodeResult;
            keyData.publicKey = derivePublicKey(keyData.secretKey);
            if (keyData.accountID != calcAccountID(keyData.publicKey))
            {
                errorMsg = "Private key doesn't match your account ID, it looks like your wallet data was corrupted";
                return false;
            }

            return true;
        }
        else
        {
            // Encrypted wallet
            auto decodeResult2 = parseHex<std::vector<unsigned char> >(keyObj["encrypted_private_key"].toString().toStdString());
            auto decodeResult3 = parseHex<std::vector<unsigned char> >(keyObj["salt"].toString().toStdString());

            if (! decodeResult2 || ! decodeResult3)
            {
                errorMsg = "Unable to decode encrypted keys, it looks like your wallet data was corrupted.";
                return false;
            }

            keyData.encryptedKey = *decodeResult2;
            keyData.salt = *decodeResult3;
            keyData.nDeriveIterations = keyObj["iterations"].toInt();
            if (keyData.nDeriveIterations == 0) keyData.nDeriveIterations = 500000;

            while (true)
            {
                EnterPassword pwDialog(this);
                if (pwDialog.exec() == QDialog::Accepted) {

                    bool fOk = true;
                    secure::secret secretData;
                    if (fOk) fOk = decryptKey(keyData.encryptedKey, pwDialog.getPassword(), keyData.salt, keyData.nDeriveIterations, secretData);
                    if (fOk)
                    {
                        keyData.secretKey = SecretKey(Slice(&secretData[0], secretData.size()));
                        keyData.publicKey = derivePublicKey(keyData.secretKey);
                        fOk = (keyData.accountID == calcAccountID(keyData.publicKey));
                    }

                    if (fOk) break;

                    // Wrong password, try again
                    continue;
                }
                else
                {
                    // User refused to enter the password
                    errorMsg = "Password";
                    return false;
                }
            }

            // Destroy key object and return
            keyData.secretKey.~SecretKey();
            return true;
        }
    }

    errorMsg = "Shouldn't happen in real life";
    return false;
}

void WalletMain::saveKeys()
{
    using namespace ripple;

    auto keyJSONData = QJsonDocument (QJsonObject
    {
        { "private_key", strHex(keyData.secretKey.data(), 32).c_str() },
        { "account_id", toBase58(keyData.accountID).c_str() },
    }).toJson();

    QFile keyFile;
    keyFile.setFileName(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + QDir::separator() + "keyStore.json");
    keyFile.open(QIODevice::WriteOnly | QIODevice::Text);
    keyFile.write(keyJSONData, keyJSONData.size());
    keyFile.close();
}

void WalletMain::newKey()
{
    using namespace ripple;
    std::tie(keyData.publicKey, keyData.secretKey) = randomKeyPair();
    keyData.accountID = calcAccountID(keyData.publicKey);
    saveKeys();
}

bool WalletMain::importKey(const secure::string& keyString)
{
    using namespace ripple;
    auto decodeResult = parseBase58<SecretKey>(TOKEN_ACCOUNT_WIF, keyString.c_str());
    if (! decodeResult)
        return false; // Incorrect WIF string
    keyData.secretKey = *decodeResult;
    keyData.publicKey = derivePublicKey(keyData.secretKey);
    keyData.accountID = calcAccountID(keyData.publicKey);
    saveKeys();
    return true;
}

QString WalletMain::exportKey(QString& errorMsg)
{
    using namespace ripple;
    if (askPassword(errorMsg))
        return toWIF(keyData.secretKey).c_str();
    return "";
}


bool WalletMain::askPassword(QString& errorMsg)
{
    using namespace ripple;

    if (keyData.nDeriveIterations == 0)
        return true;

    try
    {
        while (true)
        {
            EnterPassword pwDialog(this);
            if (pwDialog.exec() == QDialog::Accepted) {

                bool fOk = true;
                secure::secret secretData;
                if (fOk) fOk = decryptKey(keyData.encryptedKey, pwDialog.getPassword(), keyData.salt, keyData.nDeriveIterations, secretData);
                if (fOk)
                {
                    keyData.secretKey = SecretKey(Slice(&secretData[0], secretData.size()));
                    keyData.publicKey = derivePublicKey(keyData.secretKey);
                    fOk = (keyData.accountID == calcAccountID(keyData.publicKey));
                }

                if (fOk) return true;

                // Wrong password, try again
                continue;
            }
            else
            {
                // User refused to enter the password
                errorMsg = "Password";
                return false;
            }
        }
    }
    catch(const std::exception e)
    {
        errorMsg = e.what();
    }

    return false;
}

WalletMain::WalletMain(QWidget *parent) :
    QMainWindow(parent),
    ui(new Ui::WalletMain)
{

#ifdef QT_NO_SSL
#error "SSL support is required"
#endif

    QString errStr("");
    if (! loadWallet(errStr) )
    {
        if (errStr != "Password")
            showMessage(QString("Error"), QString("Error happened while loading private key: ") + errStr, 2);
        QTimer::singleShot(250, qApp, SLOT(quit()));
        return;
    }

    ui->setupUi(this);
    // Init in offline state
    setOnline(false, "Initialization is in progress");
    setupControls(parent);

    // Set message handlers
    connect(&m_webSocket, &QWebSocket::connected, this, &WalletMain::onConnected);
    connect(&m_webSocket, SIGNAL(error(QAbstractSocket::SocketError)), this, SLOT(onConnectionError(QAbstractSocket::SocketError)));

    //socketConnect();

    QTimer::singleShot(500, this, SLOT(doReconnect()));
}

void WalletMain::onConnectionError(QAbstractSocket::SocketError error)
{
    switch (error) {
    case QAbstractSocket::RemoteHostClosedError:
        setOnline(false, "Connection was closed by remote host.");
        break;
    case QAbstractSocket::HostNotFoundError:
        setOnline(false, "The host was not found. Please check your internet connection settings.");
        break;
    case QAbstractSocket::ConnectionRefusedError:
        setOnline(false, "The connection was refused by the peer. Please try again later. ");
        break;
    default:
        setOnline(false, QString("The following error occurred: %1.").arg(m_webSocket.errorString()));
    }

    if (nConnectAttempt <= 3)
        doReconnect();

    nConnectAttempt++;
}

void WalletMain::setOnline(bool flag, const QString& reason)
{
    QString strAccountID = ripple::toBase58(keyData.accountID).c_str();
    this->setWindowTitle(QString("RMC Wallet [%1%2]").arg(strAccountID).arg(flag ? "" : ", offline"));
    networkStatusLabel.setText(QString("Network status: %1").arg(reason));

    // Select default tab
    if (! flag) ui->tabWidget->setCurrentIndex(0);
    if (! flag) nBalance = 0;
    if (! flag) nLedger = -1;
    if (flag) nConnectAttempt = 0;

    ui->sendButton->setEnabled(flag);
    ui->previewButton->setEnabled(flag);
    ui->tabWidget->setTabEnabled(1, flag);
    ui->tabWidget->setTabEnabled(2, flag);

    balanceLabel.setText(QString("Balance: %1 RMC").arg(QString::number(nBalance / 1000000.0, 'f', 6)));
    ledgerLabel.setText(QString("Current ledger: %1").arg(nLedger));

    ui->latestLedgerHash->setText(ledgerHash);
    ui->latestLedgerNum->setText(QString("%1").arg(nLedger));
    ui->transactionsCount->setText(QString("%1").arg(ledgerTransactions));
    ui->closeTime->setText(QDateTime::fromTime_t(946684800 + ledgerCloseTime).toString("dd/MM/yyyy hh:mm:ss"));
    ui->baseFeeValue->setText(QString("%1").arg(QString::number(nFee / 1000000.0, 'f', 6) + " RMC"));
    ui->feeRefValue->setText(QString("%1").arg(QString::number(nFeeRef / 1000000.0, 'f', 6) + " RMC"));
    ui->baseReserveValue->setText(QString("%1").arg(QString::number(nReserve / 1000000.0, 'f', 6) + " RMC"));
    ui->sendTransactionFeeValue->setPlaceholderText(QString("%1").arg(QString::number(nFeeRef / 1000000.0, 'f', 6)));
}

void WalletMain::setupControls(QWidget *parent)
{
    // Setup amount validator
    std::unique_ptr<DoubleValidator> amountValidator(new DoubleValidator(parent));
    amountValidator->setDecimals(6);
    amountValidator->setBottom(0.00);
    amountValidator->setTop(10757.0);
    amountValidator->setNotation(QDoubleValidator::StandardNotation);
    ui->amountToSend->setValidator(amountValidator.get());
    ui->sendTransactionFeeValue->setValidator(amountValidator.get());

    // Setup tag validator
    std::unique_ptr<IntValidator> tagValidator(new IntValidator(parent));
    amountValidator->setBottom(0);
    ui->destinationTag->setValidator(tagValidator.get());

    // Hide columns
    ui->txView->setColumnHidden(4, true);
    // Set column sizes
    for(auto nCol : {0, 1, 3})
        ui->txView->setColumnWidth(nCol, 150);

    ui->txView->verticalHeader()->setVisible(false);
    ui->txView->setSelectionBehavior(QAbstractItemView::SelectRows);
    ui->txView->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);

    connect(ui->txView, SIGNAL(cellDoubleClicked(int,int)), this, SLOT(txItemClicked(int,int)));

    // Add statusBar labels
    ui->statusBar->addWidget(&balanceLabel);
    ui->statusBar->addWidget(&ledgerLabel);
    ui->statusBar->addWidget(&networkStatusLabel);
    ui->actionEncrypt_wallet->setDisabled(keyData.nDeriveIterations != 0);
}

WalletMain::~WalletMain()
{
    delete ui;
}

bool WalletMain::isNetworkAvailable()
{
    // Trying connect to google.com
    QNetworkAccessManager nam;
    QNetworkRequest req(QUrl("http://google.com"));
    QNetworkReply *reply = nam.get(req);
    QEventLoop loop;
    connect(reply, SIGNAL(finished()), &loop, SLOT(quit()));
    loop.exec();
    return reply->bytesAvailable();
}

void WalletMain::socketConnect()
{
    if (! isNetworkAvailable()) {
        setOnline(false, "No internet connection");
        nConnectAttempt = 4;
        return;
    }

    // Connect to random RPC server
    std::vector<QString> servers = {"wss://connor.rmc.one:443/", "wss://kirk.rmc.one:443/", "wss://forrest.rmc.one:443/", "wss://archer.rmc.one:443/", "wss://lorca.rmc.one:443/"};
    std::random_device random_device;
    std::mt19937 engine{random_device()};
    std::uniform_int_distribution<int> dist(0, servers.size() - 1);
    m_webSocket.open(QUrl(servers[dist(engine)]));
}

void WalletMain::onConnected()
{
    connect(&m_webSocket, &QWebSocket::textMessageReceived,
            this, &WalletMain::onTextMessageReceived);
    accInfoRequest();
    accTxRequest();
    subsLedgerAndAccountRequest();
}

void WalletMain::doReconnect()
{
    setOnline(false, "Reconnecting");
    //reqMap.clear();
    if (m_webSocket.state() == QAbstractSocket::SocketState::ConnectedState)
        m_webSocket.close();
    socketConnect();
}

void WalletMain::onTextMessageReceived(QString message)
{
    auto msgDoc = QJsonDocument::fromJson(message.toUtf8());
    auto msgObj = msgDoc.object();

    // Check validity of received message
    if (!msgObj.contains("id") && !msgObj.contains("type"))
    {
        qDebug() << "Malformed JSON message received:" << message;
        return;
    }

    // Check whether this message is a response or notification
    if (msgObj.contains("id"))
    {
        // Exctract and check ID against the map of previously made requests
        int nMsgId = msgObj["id"].toInt();

        if (msgObj["result"].isNull() || msgObj["result"].isUndefined())
        {
            qDebug() << "Something went wrong, NULL data received insted of proper response";

            // Remove message ID from the map and return
            reqMap.erase(nMsgId);
            return;
        }

        try
        {
            auto msgKind = reqMap.at(nMsgId);

            switch(msgKind)
            {
            case MSG_ACCOUNT_INFO:
                accInfoResponse(msgObj);
                break;
            case MSG_ACCOUNT_TX:
                accTxResponse(msgObj);
                break;
            case MSG_SUBMIT_TX:
                submitResponse(msgObj);
                break;
            case MSG_SUBSCRIBE_LEDGER_AND_ACCOUNT:
                subsLedgerAndAccountResponse(msgObj);
                break;
            }

            // Remove message ID from the map
            reqMap.erase(nMsgId);
        }
        catch(std::out_of_range e)
        {
            qDebug() << "Unrequested message received: " << message;
        }
    }

    if (msgObj.contains("type"))
    {
        if (msgObj["type"].toString() == "transaction")
        {
            // New transaction accepted
            processTxMessage(msgObj);
        }

        if (msgObj["type"].toString() == "ledgerClosed")
        {
            // New ledger closed
            processLedgerMessage(msgObj);
        }
    }
}

void WalletMain::processTxMessage(QJsonObject txMsg)
{
    auto txMetaObj = txMsg["meta"].toObject();
    auto txObj = txMsg["transaction"].toObject();

    // Ignore unsuccessful transactions
    if (txMsg["engine_result"].toString() != "tesSUCCESS")
    {
        accInfoRequest();
        return;
    }

    // Get account ID as string.
    QString strAccountID = ripple::toBase58(keyData.accountID).c_str();

    // Parse transaction metadata
    if (txObj["TransactionType"].toString() == "Payment" && !txObj["Amount"].isObject())
    {
        // Parse affected nodes list
        for (const auto& affRecord : txMetaObj["AffectedNodes"].toArray())
        {
            QJsonObject fieldsObj;

            // Check if our account was created just now
            if (affRecord.toObject()["CreatedNode"].isObject())
            {
                auto nodeObj = affRecord.toObject()["CreatedNode"].toObject();
                if (nodeObj["LedgerEntryType"].toString() == "AccountRoot") {
                    fieldsObj = nodeObj["NewFields"].toObject();
                } else continue;
            }
            else
            {
                auto nodeObj = affRecord.toObject()["ModifiedNode"].toObject();
                if (nodeObj["LedgerEntryType"].toString() == "AccountRoot") {
                    fieldsObj = nodeObj["FinalFields"].toObject();
                } else continue;
            }

            if (fieldsObj["Account"].toString() == strAccountID)
            {
                nBalance = fieldsObj["Balance"].toString().toDouble();
                nSequence = fieldsObj["Sequence"].toDouble();
                break;
            }
        }

        // Add transaction record to history grid
        bool isDebit = (txObj["Destination"].toString() != strAccountID);
        std::vector<QString> newRow {
            QDateTime::fromTime_t(946684800 + txObj["date"].toDouble()).toString("dd/MM/yyyy hh:mm:ss"),
            txObj["TransactionType"].toString(),
            txObj["hash"].toString(),
            QString("%1%2 RMC").arg(isDebit ? "-" : "").arg(QString::number ( txObj["Amount"].toString().toDouble() / 1000000, 'f', 6 )),
            QJsonDocument(txObj).toJson()
        };

        ui->txView->insertRow(0);
        for (auto nCol = 0u; nCol < newRow.size(); ++nCol)
        {
            QTableWidgetItem *newItem = new QTableWidgetItem();
            newItem->setText(newRow[nCol]);
            if (nCol == 5) newItem->setTextAlignment(Qt::AlignRight);
            ui->txView->setItem( 0, nCol, newItem);
        }
    }
    else
    {
        // No support for "complex" transactions yet, just ask to send us fresh transaction list
        accTxRequest();
    }

    // Update current ledger index, account balance and sequence
    ledgerLabel.setText("Current ledger: " + QString("%1").arg(txMsg["ledger_index"].toDouble()));
}

void WalletMain::processLedgerMessage(QJsonObject ledgerObj)
{
    nLedger = ledgerObj["ledger_index"].toDouble();
    nFee = ledgerObj["fee_base"].toDouble();
    nFeeRef = ledgerObj["fee_ref"].toDouble();
    nReserve = ledgerObj["reserve_base"].toDouble();
    ledgerHash = ledgerObj["ledger_hash"].toString();
    ledgerCloseTime = ledgerObj["ledger_time"].toDouble();
    ledgerTransactions = ledgerObj["txn_count"].toDouble();
    setOnline(true, QString("ledger %1 closed").arg(ledgerHash.left(6)));
}


void WalletMain::txItemClicked(int nRow, int nCol)
{
    QTableWidgetItem *item = new QTableWidgetItem;
    item = ui->txView->item(nRow, 4);
    TransactionView txDlg(nullptr, item->text());
    txDlg.exec();
}

void WalletMain::accInfoResponse(QJsonObject obj)
{
    auto result = obj["result"].toObject();
    auto accountData = result["account_data"].toObject();

    nSequence = accountData["Sequence"].toDouble();
    nBalance = accountData["Balance"].toString().toDouble();

    setOnline(true, "Account info retrieved");
}

void WalletMain::accTxResponse(QJsonObject obj)
{
    QJsonObject result = obj["result"].toObject();
    QJsonArray txes = result["transactions"].toArray();

    // Get account ID as string.
    QString strAccountID = ripple::toBase58(keyData.accountID).c_str();

    std::vector<std::vector<QString> > rowData;
    for (int i = 0; i < txes.size(); i++)
    {
        QJsonObject txObj = txes[i].toObject();
        if (!txObj["validated"].toBool())
            continue;
        if (txObj["meta"].toObject()["TransactionResult"].toString() != "tesSUCCESS")
            continue;

        txObj = txObj["tx"].toObject();

        bool isDebit = (txObj["Destination"].toString() != strAccountID);

        rowData.insert(rowData.end(), std::vector<QString> {
            QDateTime::fromTime_t(946684800 + txObj["date"].toDouble()).toString("dd/MM/yyyy hh:mm:ss"),
            txObj["TransactionType"].toString(),
            txObj["hash"].toString(),
            QString("%1%2 RMC").arg(isDebit ? "-" : "").arg(QString::number ( txObj["Amount"].toString().toDouble() / 1000000, 'f', 6 )),
            QJsonDocument(txObj).toJson()
        });
    }

    ui->txView->clearContents();
    ui->txView->setRowCount(rowData.size());

    for (auto nRow = 0u; nRow < rowData.size(); ++nRow)
    {
        for (auto nCol = 0u; nCol < rowData[nRow].size(); ++nCol)
        {
            QTableWidgetItem *newItem = new QTableWidgetItem();
            newItem->setText(rowData[nRow][nCol]);
            if (nCol == 5) newItem->setTextAlignment(Qt::AlignRight);
            ui->txView->setItem( nRow, nCol, newItem);
        }
    }

    setOnline(true, "New transaction entry received");
}

void WalletMain::submitResponse(QJsonObject obj)
{
    QJsonObject result = obj["result"].toObject();

    if (result["status"].toString() == "error")
        showMessage(QString("Transaction error"), QString("Failure while committing transaction to the RMC network: ") + obj["error_message"].toString(), 1);
    else if (result["engine_result"].toString() != "tesSUCCESS")
        showMessage(QString("Transaction error"), QString("Error while processing transaction by the RMC network: ") + result["engine_result_message"].toString(), 1);
    else
        showMessage(QString("Transaction applied"), result["engine_result_message"].toString(), 0);
}

void WalletMain::subsLedgerAndAccountResponse(QJsonObject obj)
{
    QJsonObject result = obj["result"].toObject();

    nFee = result["fee_base"].toDouble();
    nFeeRef = result["fee_ref"].toDouble();
    nLedger = result["ledger_index"].toDouble();
    nReserve = result["reserve_base"].toDouble();

    setOnline(true, "Subscribed to ledger and account notifications");
}

void WalletMain::accInfoRequest()
{
    // Request account info
    reqMap[nRequestID] = MSG_ACCOUNT_INFO;
    m_webSocket.sendTextMessage(
       QJsonDocument(
       QJsonObject {
        {"id", nRequestID++},
        {"command", "account_info"},
        {"account",  ripple::toBase58(keyData.accountID).c_str() },
    }).toJson());
}

void WalletMain::accTxRequest()
{
    // Request account transactions
    reqMap[nRequestID] = MSG_ACCOUNT_TX;
    m_webSocket.sendTextMessage(
        QJsonDocument(
            QJsonObject {
                {"id", nRequestID++},
                {"command", "account_tx"},
                {"account", ripple::toBase58(keyData.accountID).c_str() },
                {"ledger_index_min", -1 },
                {"ledger_index_max", -1 },
                //{"limit", -1 },
                {"forward", false },
            }).toJson());
}

void WalletMain::submitRequest(QString hexBlobData)
{
    reqMap[nRequestID] = MSG_SUBMIT_TX;
    m_webSocket.sendTextMessage(
        QJsonDocument(
            QJsonObject {
                {"id", nRequestID++},
                {"command", "submit"},
                {"tx_blob", hexBlobData },
                {"fail_hard", true}
            }).toJson());
}

void WalletMain::subsLedgerAndAccountRequest()
{
    // Subscribe to ledger and account streams
    reqMap[nRequestID] = MSG_SUBSCRIBE_LEDGER_AND_ACCOUNT;
    m_webSocket.sendTextMessage(
        QJsonDocument(
            QJsonObject {
                {"id", nRequestID++},
                {"command", "subscribe"},
                {"accounts", QJsonArray { ripple::toBase58(keyData.accountID).c_str() } },
                {"streams", QJsonArray { "ledger" } },
        }).toJson());
}

void WalletMain::on_actionExit_triggered()
{
    qApp->quit();
}

void WalletMain::on_clearButton_clicked()
{
    ui->receiverAddressEdit->setText("");
    ui->amountToSend->setText("0");
    ui->destinationTag->setText("0");
}

void WalletMain::on_previewButton_clicked()
{
    if (ui->receiverAddressEdit->text() == ripple::toBase58(keyData.accountID).c_str() )
    {
        showMessage(QString("Error"), QString("Sending to self basically has no sense and is not supported."), 1);
        return;
    }

    QString transactionHex, transactionJSON, strError;
    int64_t nAmount = QLocale::system().toDouble(ui->amountToSend->text()) * 1000000;
    int64_t nDestinationID = QLocale::system().toInt(ui->destinationTag->text());
    int64_t nTransactionFee = (QLocale::system().toDouble(ui->sendTransactionFeeValue->text()) * 1000000);
    if (nTransactionFee == 0) nTransactionFee = nFeeRef;

    if (nAmount > (nBalance - nTransactionFee - nReserve))
    {
        showMessage(QString("Warning"), QString("Transaction amount is greater than amount of available funds. This could happen if your available balance doesn't comply with either fee or reserve requirements."), 1);
        return;
    }

    bool result = createPaymentTx(
                ui->receiverAddressEdit->text(), nAmount, nTransactionFee, nDestinationID,
                    transactionJSON, transactionHex, strError);
    if (! result)
    {
        if (strError != "Password") showMessage(QString("Error"), strError, 1);
    }
    else
    {
        TransactionPreview preview(nullptr, transactionJSON, transactionHex);
        if (preview.exec() == QDialog::Accepted)
        {
            // Submit transaction and disable send button till confirmation from server
            submitRequest(transactionHex);
            ui->sendButton->setEnabled(false);
            ui->previewButton->setEnabled(false);
            ui->receiverAddressEdit->setText("");
            ui->amountToSend->setText("0");
            ui->destinationTag->setText("0");
        }
    }
}

void WalletMain::on_sendButton_clicked()
{
    if (ui->receiverAddressEdit->text() == ripple::toBase58(keyData.accountID).c_str())
    {
        showMessage(QString("Error"), QString("Sending to self basically has no sense and is not supported."), 1);
        return;
    }

    QString transactionHex, transactionJSON, strError;
    int64_t nAmount = QLocale::system().toDouble(ui->amountToSend->text()) * 1000000;
    int64_t nDestinationID = QLocale::system().toInt(ui->destinationTag->text());
    int64_t nTransactionFee = (QLocale::system().toDouble(ui->sendTransactionFeeValue->text()) * 1000000);
    if (nTransactionFee == 0) nTransactionFee = nFeeRef;

    if (nAmount > (nBalance - nTransactionFee - nReserve))
    {
        showMessage(QString("Warning"), QString("Transaction amount is greater than amount of available funds. This coud happen if your available balance doesn't comply with either fee or reserve requirements."), 1);
        return;
    }

    QString sendMessage = "I'm about to send " + ui->amountToSend->text() + " RMC to " + ui->receiverAddressEdit->text() + ". Do you agree?";

    bool result = createPaymentTx(
                ui->receiverAddressEdit->text(), nAmount, nTransactionFee, nDestinationID, transactionJSON, transactionHex, strError);
    if (! result ) {
        if (strError != "Password") showMessage(QString("Error"), strError, 1);
    }
    else if (QMessageBox::Yes == QMessageBox(QMessageBox::Information, "Confirmation", sendMessage, QMessageBox::Yes|QMessageBox::No).exec())
    {
        // Submit transaction and disable send button till confirmation from server
        submitRequest(transactionHex);
        ui->sendButton->setEnabled(false);
        ui->previewButton->setEnabled(false);
        ui->receiverAddressEdit->setText("");
        ui->amountToSend->setText("0");
        ui->destinationTag->setText("0");
    }
}

void WalletMain::on_actionReinitializeWallet_triggered()
{
    if (QMessageBox::Yes == QMessageBox(QMessageBox::Warning, "Confirmation", "Are you SURE you want to OVERWRITE your private key with newly generated one? This could cause LOSS of your funds if you have no backup of your current key.", QMessageBox::Yes|QMessageBox::No).exec())
    {
        newKey();
        QTimer::singleShot(250, qApp, SLOT(quit()));
    }
}

void WalletMain::on_actionImport_key_triggered()
{
    ImportDialog importReq;
    importReq.hideNewKeyLabel();

    while (true)
    {
        // Ask user to import his WIF formatted key
        auto result = importReq.exec();
        if (result == QDialog::Accepted)
        {
            if(!importKey(importReq.getKeyData()))
                continue; // Incorrect WIF string, ask user again
            QTimer::singleShot(250, qApp, SLOT(quit()));
            break;
        }
        else
        {
            // User refused
            break;
        }
    }
}

void WalletMain::on_actionExport_key_triggered()
{
    QClipboard *clipboard = QApplication::clipboard();
    clipboard->clear();
    QString errorStr;
    QString keyWIF = exportKey(errorStr);

    if (keyWIF == "") {
        if (errorStr != "Password")
            showMessage("Error", QString("Unable to decrypt your private key:") + errorStr, 1);
        return;
    }

    keyData.secretKey.~SecretKey();
    clipboard->setText(keyWIF);
    showMessage("Export", QString("Your key is in clipboard now."), 0);
}

void WalletMain::on_actionCopy_account_address_triggered()
{
    QClipboard *clipboard = QApplication::clipboard();
    clipboard->clear();
    clipboard->setText(ripple::toBase58(keyData.accountID).c_str());
    showMessage("Copy account ID", QString("Your address is in clipboard now."), 0);
}


void WalletMain::on_actionEncrypt_wallet_triggered()
{
    using namespace ripple;

    if (keyData.nDeriveIterations != 0) {
        showMessage("Error", "Changing passphrase is not yet implemented.", 2);
        return;
    }

    EnterPassword pwDialog(this), pwDialogConf(this, true);

    if (pwDialog.exec() == QDialog::Accepted && pwDialogConf.exec() == QDialog::Accepted)
    {
        const auto& strPassword1 = pwDialog.getPassword();
        const auto& strPassword2 = pwDialogConf.getPassword();

        if (strPassword1 != strPassword2)
        {
            showMessage("Error", "Entered passwords do not match.", 2);
            return;
        }

        secure::secret secretData;
        secretData.assign(keyData.secretKey.data(), keyData.secretKey.data() + keyData.secretKey.size());
        if (! encryptKey(secretData, strPassword1, keyData.salt, keyData.nDeriveIterations, keyData.encryptedKey))
        {
            showMessage("Error", "Error while encrypting your wallet", 2);
            return;
        }

        auto keyJSONData = QJsonDocument (QJsonObject
        {
            { "encrypted_private_key", strHex(keyData.encryptedKey.begin(), keyData.encryptedKey.size()).c_str() },
            { "salt", strHex(keyData.salt.begin(), keyData.salt.size()).c_str() },
            { "iterations", keyData.nDeriveIterations },
            { "account_id", toBase58(keyData.accountID).c_str() },
        }).toJson();

        keyData.secretKey.~SecretKey(); // Destroy secret key object

        QFile keyFile;
        keyFile.setFileName(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + QDir::separator() + "keyStore.json");
        keyFile.open(QIODevice::ReadWrite | QIODevice::Text);

        // Overwrite old file contents with zeros
        QByteArray arrZeros(keyFile.size(), '0');
        keyFile.write(arrZeros, arrZeros.size());
        keyFile.flush();
        keyFile.close();

        // Write encrypted data
        keyFile.open(QIODevice::WriteOnly | QIODevice::Text);
        keyFile.write(keyJSONData, keyJSONData.size());
        keyFile.close();

        showMessage("Information", "Your wallet file was successfully encrypted.", 0);
    }
}

void WalletMain::on_actionReconnect_triggered()
{
    nConnectAttempt = 0;
    doReconnect();
}


void WalletMain::on_actionAbout_triggered()
{
    AboutDialog aboutDlg;
    aboutDlg.exec();
}

// Entry point

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);
    WalletMain w;
    w.show();

    return a.exec();
}


