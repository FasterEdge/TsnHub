#include "tsn/Open62541Adapter.h"

#include <chrono>
#include <iostream>

namespace {

bool isPublisher(const PubSubConfig &cfg) {
    return cfg.role == PubSubRole::Publisher || cfg.role == PubSubRole::Both;
}

bool isSubscriber(const PubSubConfig &cfg) {
    return cfg.role == PubSubRole::Subscriber || cfg.role == PubSubRole::Both;
}

UA_String makeUAString(const std::string &s) {
    return UA_STRING_ALLOC(s.c_str());
}

} // namespace

Open62541Adapter::Open62541Adapter() = default;

Open62541Adapter::~Open62541Adapter() {
    stop();
}

bool Open62541Adapter::start() {
    if (running_.load()) {
        return true;
    }
    running_.store(true);
    serverThread_ = std::thread(&Open62541Adapter::loop, this);
    std::cout << "[Open62541Adapter] PubSub 线程已启动" << std::endl;
    return true;
}

void Open62541Adapter::stop() {
    if (!running_.exchange(false)) {
        return;
    }
    if (serverThread_.joinable()) {
        serverThread_.join();
    }
    std::lock_guard<std::mutex> lk(mu_);
    teardownLocked();
    std::cout << "[Open62541Adapter] 已停止" << std::endl;
}

bool Open62541Adapter::configure(const PubSubConfig &cfg) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        cfg_ = cfg;
    }
    connectEnabled_.store(true);
    reconfigureRequested_.store(true);
    return true;
}

void Open62541Adapter::setConnectEnabled(bool enabled) {
    if (!enabled) {
        std::lock_guard<std::mutex> lk(mu_);
        teardownLocked();
    }
    connectEnabled_.store(enabled);
}

void Open62541Adapter::subscribe(std::function<void(const std::string &)> onMsg) {
    std::lock_guard<std::mutex> lk(mu_);
    onMsg_ = std::move(onMsg);
}

bool Open62541Adapter::send(const std::string &payload) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!connectEnabled_.load()) {
        std::cerr << "[Open62541Adapter] send 失败：未开启连接" << std::endl;
        return false;
    }
    if (!server_) {
        std::cerr << "[Open62541Adapter] send 失败：服务器未准备好" << std::endl;
        return false;
    }
    if (!isPublisher(cfg_)) {
        std::cerr << "[Open62541Adapter] send 失败：当前角色非 Publisher" << std::endl;
        return false;
    }

    UA_Variant value;
    UA_Variant_init(&value);
    UA_String uaStr = makeUAString(payload);
    UA_Variant_setScalar(&value, &uaStr, &UA_TYPES[UA_TYPES_STRING]);
    UA_StatusCode rc = UA_Server_writeValue(server_, payloadVar_, value);
    UA_String_clear(&uaStr);
    if (rc != UA_STATUSCODE_GOOD) {
        std::cerr << "[Open62541Adapter] 写变量失败 rc=0x" << std::hex << rc << std::dec << std::endl;
        return false;
    }
    return true;
}

void Open62541Adapter::readerDataSetListener(UA_Server *server, UA_UInt32 readerId,
                                             void *readerContext, const UA_ByteString *msg,
                                             const UA_NetworkMessage *nm) {
    (void)server;
    (void)readerId;
    (void)msg;
    auto *self = static_cast<Open62541Adapter *>(readerContext);
    if (!self || !nm) {
        return;
    }
    if (nm->payloadHeaderEnabled && nm->payloadHeader.dataSetPayloadHeader.count > 0) {
        for (size_t i = 0; i < nm->payloadHeader.dataSetPayloadHeader.count; ++i) {
            const UA_DataSetMessage *dsm = &nm->payload.dataSetPayload.dataSetMessages[i];
            if (dsm->header.dataSetMessageType != UA_DATASETMESSAGETYPE_KEYFRAME) {
                continue;
            }
            if (dsm->data.keyFrameData.fieldCount == 0) {
                continue;
            }
            const UA_Variant *v = &dsm->data.keyFrameData.dataSetFields[0].value;
            if (UA_Variant_hasScalarType(v, &UA_TYPES[UA_TYPES_STRING])) {
                UA_String *s = (UA_String *)v->data;
                std::string payload(reinterpret_cast<const char *>(s->data), s->length);
                std::function<void(const std::string &)> cb;
                {
                    std::lock_guard<std::mutex> lk(self->mu_);
                    cb = self->onMsg_;
                }
                if (cb) {
                    cb(payload);
                }
            }
        }
    }
}

bool Open62541Adapter::setupPubSubLocked() {
    teardownLocked();

    server_ = UA_Server_new();
    UA_ServerConfig_setDefault(UA_Server_getConfig(server_));

    // 创建数据变量，作为发布数据源。
    UA_VariableAttributes attr = UA_VariableAttributes_default;
    UA_String init = UA_STRING("init");
    UA_Variant_setScalar(&attr.value, &init, &UA_TYPES[UA_TYPES_STRING]);
    attr.displayName = UA_LOCALIZEDTEXT(const_cast<char *>(""), const_cast<char *>(cfg_.fieldName.c_str()));
    payloadVar_ = UA_NODEID_STRING_ALLOC(1, const_cast<char *>(cfg_.fieldName.c_str()));
    UA_NodeId parent = UA_NODEID_NUMERIC(0, UA_NS0ID_OBJECTSFOLDER);
    UA_NodeId varType = UA_NODEID_NUMERIC(0, UA_NS0ID_BASEDATAVARIABLETYPE);
    UA_StatusCode rc = UA_Server_addVariableNode(server_, payloadVar_, parent,
                                                 UA_NODEID_NUMERIC(0, UA_NS0ID_ORGANIZES),
                                                 UA_QUALIFIEDNAME(1, const_cast<char *>(cfg_.fieldName.c_str())),
                                                 varType, attr, nullptr, nullptr);
    if (rc != UA_STATUSCODE_GOOD) {
        std::cerr << "[Open62541Adapter] 创建变量失败 rc=0x" << std::hex << rc << std::dec << std::endl;
        return false;
    }

    // 创建 PubSub 连接（UDP）。
    UA_PubSubConnectionConfig connCfg;
    UA_PubSubConnectionConfig_init(&connCfg);
    connCfg.name = UA_STRING("UDP-UADP-Connection");
    connCfg.transportProfileUri = UA_STRING("http://opcfoundation.org/UA-Profile/Transport/pubsub-udp-uadp");
    UA_NetworkAddressUrlDataType address;
    UA_NetworkAddressUrlDataType_init(&address);
    address.networkInterface = UA_STRING_NULL;
    address.url = UA_STRING_ALLOC(cfg_.address.c_str());
    UA_Variant_setScalar(&connCfg.address, &address, &UA_TYPES[UA_TYPES_NETWORKADDRESSURLDATATYPE]);
    rc = UA_Server_addPubSubConnection(server_, &connCfg, &connection_);
    UA_String_clear(&address.url);
    if (rc != UA_STATUSCODE_GOOD) {
        std::cerr << "[Open62541Adapter] 创建连接失败 rc=0x" << std::hex << rc << std::dec << std::endl;
        return false;
    }

    // 发布者配置。
    if (isPublisher(cfg_)) {
        UA_PublishedDataSetConfig pdsCfg;
        UA_PublishedDataSetConfig_init(&pdsCfg);
        pdsCfg.publishedDataSetType = UA_PUBSUB_DATASET_PUBLISHEDITEMS;
        pdsCfg.name = UA_STRING("PDS");
        rc = UA_Server_addPublishedDataSet(server_, &pdsCfg, &publishedDataSet_);
        if (rc != UA_STATUSCODE_GOOD) {
            std::cerr << "[Open62541Adapter] 创建 PDS 失败 rc=0x" << std::hex << rc << std::dec << std::endl;
            return false;
        }

        UA_DataSetFieldConfig fieldCfg;
        UA_DataSetFieldConfig_init(&fieldCfg);
        fieldCfg.dataSetFieldType = UA_PUBSUB_DATASETFIELD_VARIABLE;
        fieldCfg.field.variable.fieldNameAlias = UA_STRING_ALLOC(cfg_.fieldName.c_str());
        fieldCfg.field.variable.promotedField = UA_FALSE;
        fieldCfg.field.variable.publishParameters.publishedVariable = payloadVar_;
        fieldCfg.field.variable.publishParameters.attributeId = UA_ATTRIBUTEID_VALUE;
        rc = UA_Server_addDataSetField(server_, publishedDataSet_, &fieldCfg, nullptr);
        UA_String_clear(&fieldCfg.field.variable.fieldNameAlias);
        if (rc != UA_STATUSCODE_GOOD) {
            std::cerr << "[Open62541Adapter] 创建 DataSetField 失败 rc=0x" << std::hex << rc << std::dec << std::endl;
            return false;
        }

        UA_WriterGroupConfig wgCfg;
        UA_WriterGroupConfig_init(&wgCfg);
        wgCfg.name = UA_STRING("WriterGroup");
        wgCfg.publishingInterval = static_cast<UA_Double>(cfg_.publishIntervalMs);
        wgCfg.enabled = UA_TRUE;
        wgCfg.writerGroupId = cfg_.publisherId;
        rc = UA_Server_addWriterGroup(server_, connection_, &wgCfg, &writerGroup_);
        if (rc != UA_STATUSCODE_GOOD) {
            std::cerr << "[Open62541Adapter] 创建 WriterGroup 失败 rc=0x" << std::hex << rc << std::dec << std::endl;
            return false;
        }

        UA_DataSetWriterConfig dswCfg;
        UA_DataSetWriterConfig_init(&dswCfg);
        dswCfg.name = UA_STRING("DataSetWriter");
        dswCfg.dataSetWriterId = cfg_.writerId;
        rc = UA_Server_addDataSetWriter(server_, writerGroup_, publishedDataSet_, &dswCfg, &dataSetWriter_);
        if (rc != UA_STATUSCODE_GOOD) {
            std::cerr << "[Open62541Adapter] 创建 DataSetWriter 失败 rc=0x" << std::hex << rc << std::dec << std::endl;
            return false;
        }
    }

    // 订阅者配置。
    if (isSubscriber(cfg_)) {
        UA_ReaderGroupConfig rgCfg;
        UA_ReaderGroupConfig_init(&rgCfg);
        rgCfg.name = UA_STRING("ReaderGroup");
        rc = UA_Server_addReaderGroup(server_, connection_, &rgCfg, &readerGroup_);
        if (rc != UA_STATUSCODE_GOOD) {
            std::cerr << "[Open62541Adapter] 创建 ReaderGroup 失败 rc=0x" << std::hex << rc << std::dec << std::endl;
            return false;
        }

        UA_DataSetReaderConfig dsrCfg;
        UA_DataSetReaderConfig_init(&dsrCfg);
        dsrCfg.name = UA_STRING("DataSetReader");
        dsrCfg.dataSetReaderId = cfg_.readerId;
        dsrCfg.publisherId.idType = UA_PUBLISHERIDTYPE_UINT16;
        dsrCfg.publisherId.id.uint16 = cfg_.publisherId;
        dsrCfg.writerGroupId = cfg_.publisherId;
        dsrCfg.dataSetWriterId = cfg_.writerId;

        // 设置元数据（单字符串字段）。
        UA_DataSetMetaDataType metaData;
        UA_DataSetMetaDataType_init(&metaData);
        metaData.name = UA_STRING("MetaData");
        metaData.fieldsSize = 1;
        metaData.fields = (UA_FieldMetaData *)UA_Array_new(metaData.fieldsSize, &UA_TYPES[UA_TYPES_FIELDMETADATA]);
        UA_FieldMetaData_init(&metaData.fields[0]);
        metaData.fields[0].name = UA_STRING_ALLOC(cfg_.fieldName.c_str());
        metaData.fields[0].builtInType = UA_NS0ID_STRING;
        metaData.fields[0].dataType = UA_TYPES[UA_TYPES_STRING].typeId;
        metaData.fields[0].valueRank = -1;
        dsrCfg.metaData = metaData;

        // UADP reader message设置
        UA_UadpDataSetReaderMessageDataType uadpMsgCfg;
        UA_UadpDataSetReaderMessageDataType_init(&uadpMsgCfg);
        uadpMsgCfg.networkMessageContentMask = UA_UADPNETWORKMESSAGECONTENTMASK_PUBLISHERID | UA_UADPNETWORKMESSAGECONTENTMASK_GROUPHEADER | UA_UADPNETWORKMESSAGECONTENTMASK_WRITERGROUPID | UA_UADPNETWORKMESSAGECONTENTMASK_PAYLOADHEADER;
        uadpMsgCfg.dataSetMessageContentMask = UA_UADPDATASETMESSAGECONTENTMASK_SEQUENCENUMBER | UA_UADPDATASETMESSAGECONTENTMASK_TIMESTAMP;
        UA_Variant_setScalar(&dsrCfg.messageSettings, &uadpMsgCfg, &UA_TYPES[UA_TYPES_UADPDATASETREADERMESSAGEDATATYPE]);

        rc = UA_Server_addDataSetReader(server_, readerGroup_, &dsrCfg, &dataSetReader_);

        UA_String_clear(&metaData.fields[0].name);
        UA_Array_delete(metaData.fields, metaData.fieldsSize, &UA_TYPES[UA_TYPES_FIELDMETADATA]);
        UA_Variant_clear(&dsrCfg.messageSettings);

        if (rc != UA_STATUSCODE_GOOD) {
            std::cerr << "[Open62541Adapter] 创建 DataSetReader 失败 rc=0x" << std::hex << rc << std::dec << std::endl;
            return false;
        }

        // 设置回调。
        UA_Server_DataSetReader_setSubscriberCallback(server_, dataSetReader_, readerDataSetListener, this);
    }

    return true;
}

void Open62541Adapter::teardownLocked() {
    if (server_) {
        UA_Server_run_shutdown(server_);
        UA_Server_delete(server_);
    }
    server_ = nullptr;
    publishedDataSet_ = UA_NODEID_NULL;
    writerGroup_ = UA_NODEID_NULL;
    dataSetWriter_ = UA_NODEID_NULL;
    connection_ = UA_NODEID_NULL;
    readerGroup_ = UA_NODEID_NULL;
    dataSetReader_ = UA_NODEID_NULL;
    if (!UA_NodeId_isNull(&payloadVar_)) {
        UA_NodeId_clear(&payloadVar_);
        payloadVar_ = UA_NODEID_NULL;
    }
}

void Open62541Adapter::loop() {
    while (running_.load()) {
        if (!connectEnabled_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            continue;
        }

        bool ok = false;
        {
            std::lock_guard<std::mutex> lk(mu_);
            ok = setupPubSubLocked();
        }
        if (!ok) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }

        // 主循环：驱动 UA_Server_run_iterate。
        while (running_.load()) {
            if (reconfigureRequested_.exchange(false) || !connectEnabled_.load()) {
                std::lock_guard<std::mutex> lk(mu_);
                teardownLocked();
                break;
            }
            UA_UInt16 timeoutMs = static_cast<UA_UInt16>(cfg_.publishIntervalMs);
            if (timeoutMs == 0) timeoutMs = 10;
            UA_Server_run_iterate(server_, true);
            std::this_thread::sleep_for(std::chrono::milliseconds(timeoutMs));
        }
    }
}
