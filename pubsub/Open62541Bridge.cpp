#include "pubsub/Open62541Bridge.hpp"

#include <open62541/server.h>
#include <open62541/server_config_default.h>
#include <open62541/server_pubsub.h>

#include <cstring>
#include <stdexcept>
#include <thread>

#ifndef UA_ENABLE_PUBSUB
#error "Open62541Bridge requires open62541 built with UA_ENABLE_PUBSUB"
#endif

namespace tsnhub {
namespace {
constexpr UA_UInt16 namespaceIndex = 1;
constexpr UA_UInt32 inputNodeNumericId = 5001;
constexpr UA_UInt32 outputNodeNumericId = 5002;

void requireGood(UA_StatusCode status, const char *operation) {
    if(status != UA_STATUSCODE_GOOD)
        throw std::runtime_error(std::string(operation) + ": " + UA_StatusCode_name(status));
}

UA_NodeId addInt32Variable(UA_Server *server, UA_UInt32 numericId, const char *name) {
    UA_VariableAttributes attributes = UA_VariableAttributes_default;
    attributes.displayName = UA_LOCALIZEDTEXT(const_cast<char *>("en-US"), const_cast<char *>(name));
    attributes.dataType = UA_TYPES[UA_TYPES_INT32].typeId;
    UA_Int32 initial = 0;
    UA_Variant_setScalar(&attributes.value, &initial, &UA_TYPES[UA_TYPES_INT32]);
    const UA_NodeId requested = UA_NODEID_NUMERIC(namespaceIndex, numericId);
    UA_NodeId created;
    UA_NodeId_init(&created);
    requireGood(UA_Server_addVariableNode(server, requested,
        UA_NODEID_NUMERIC(0, UA_NS0ID_OBJECTSFOLDER),
        UA_NODEID_NUMERIC(0, UA_NS0ID_ORGANIZES),
        UA_QUALIFIEDNAME(namespaceIndex, const_cast<char *>(name)),
        UA_NODEID_NUMERIC(0, UA_NS0ID_BASEDATAVARIABLETYPE), attributes, nullptr, &created),
        "add variable");
    return created;
}

UA_NodeId addConnection(UA_Server *server, const std::string &name, const std::string &url,
                        UA_UInt16 publisherId) {
    UA_PubSubConnectionConfig config;
    std::memset(&config, 0, sizeof(config));
    config.name = UA_STRING(const_cast<char *>(name.c_str()));
    UA_NetworkAddressUrlDataType address{UA_STRING_NULL, UA_STRING(const_cast<char *>(url.c_str()))};
    UA_Variant_setScalar(&config.address, &address, &UA_TYPES[UA_TYPES_NETWORKADDRESSURLDATATYPE]);
    config.transportProfileUri = UA_STRING(const_cast<char *>(
        "http://opcfoundation.org/UA-Profile/Transport/pubsub-udp-uadp"));
    config.publisherId.idType = UA_PUBLISHERIDTYPE_UINT16;
    config.publisherId.id.uint16 = publisherId;
    UA_NodeId id;
    UA_NodeId_init(&id);
    requireGood(UA_Server_addPubSubConnection(server, &config, &id), "add PubSub connection");
    return id;
}
} // namespace

struct Open62541Bridge::Impl {
    NativePubSubConfig config;
    Scheduler scheduler;
    UA_Server *server{};
    UA_NodeId inputNode{};
    UA_NodeId outputNode{};
    UA_Int32 lastInput{};
    bool hasInput{};
    std::uint64_t sequence{};

    Impl(NativePubSubConfig cfg, SchedulerConfig schedulerConfig)
        : config(std::move(cfg)), scheduler(std::move(schedulerConfig)) {
        UA_ServerConfig serverConfig;
        std::memset(&serverConfig, 0, sizeof(serverConfig));
        requireGood(UA_ServerConfig_setMinimal(&serverConfig, 0, nullptr), "configure server");
        server = UA_Server_newWithConfig(&serverConfig);
        if(!server) {
            UA_ServerConfig_clear(&serverConfig);
            throw std::runtime_error("UA_Server_newWithConfig failed");
        }
        inputNode = addInt32Variable(server, inputNodeNumericId, "TsnHubInput");
        outputNode = addInt32Variable(server, outputNodeNumericId, "TsnHubOutput");
        configureSubscriber();
        configurePublisher();
        requireGood(UA_Server_enableAllPubSubComponents(server), "enable PubSub components");
    }

    ~Impl() {
        if(server) UA_Server_delete(server);
    }

    void configureSubscriber() {
        const UA_NodeId connection = addConnection(server, "TsnHub input", config.listenUrl,
                                                    config.inputPublisherId);
        UA_ReaderGroupConfig groupConfig;
        std::memset(&groupConfig, 0, sizeof(groupConfig));
        groupConfig.name = UA_STRING(const_cast<char *>("TsnHub ReaderGroup"));
        UA_NodeId readerGroup;
        UA_NodeId_init(&readerGroup);
        requireGood(UA_Server_addReaderGroup(server, connection, &groupConfig, &readerGroup),
                    "add ReaderGroup");

        UA_DataSetReaderConfig readerConfig;
        std::memset(&readerConfig, 0, sizeof(readerConfig));
        readerConfig.name = UA_STRING(const_cast<char *>("TsnHub DataSetReader"));
        readerConfig.publisherId.idType = UA_PUBLISHERIDTYPE_UINT16;
        readerConfig.publisherId.id.uint16 = config.inputPublisherId;
        readerConfig.writerGroupId = config.inputWriterGroupId;
        readerConfig.dataSetWriterId = config.inputDataSetWriterId;
        UA_DataSetMetaDataType_init(&readerConfig.dataSetMetaData);
        readerConfig.dataSetMetaData.name = UA_STRING(const_cast<char *>("TsnHub Int32 Input"));
        readerConfig.dataSetMetaData.fieldsSize = 1;
        readerConfig.dataSetMetaData.fields = static_cast<UA_FieldMetaData *>(
            UA_Array_new(1, &UA_TYPES[UA_TYPES_FIELDMETADATA]));
        if(!readerConfig.dataSetMetaData.fields) throw std::bad_alloc();
        UA_FieldMetaData_init(readerConfig.dataSetMetaData.fields);
        requireGood(UA_NodeId_copy(&UA_TYPES[UA_TYPES_INT32].typeId,
                                   &readerConfig.dataSetMetaData.fields[0].dataType),
                    "copy reader field type");
        readerConfig.dataSetMetaData.fields[0].builtInType = UA_NS0ID_INT32;
        readerConfig.dataSetMetaData.fields[0].valueRank = -1;

        UA_NodeId reader;
        UA_NodeId_init(&reader);
        requireGood(UA_Server_addDataSetReader(server, readerGroup, &readerConfig, &reader),
                    "add DataSetReader");
        UA_FieldTargetDataType target;
        UA_FieldTargetDataType_init(&target);
        target.attributeId = UA_ATTRIBUTEID_VALUE;
        target.targetNodeId = inputNode;
        requireGood(UA_Server_DataSetReader_createTargetVariables(server, reader, 1, &target),
                    "create target variable");
        // String literals use UA_STRING and are not heap-owned. Release only
        // the metadata allocations created by UA_Array_new / UA_NodeId_copy.
        UA_NodeId_clear(&readerConfig.dataSetMetaData.fields[0].dataType);
        UA_Array_delete(readerConfig.dataSetMetaData.fields, 1,
                        &UA_TYPES[UA_TYPES_FIELDMETADATA]);
        readerConfig.dataSetMetaData.fields = nullptr;
        readerConfig.dataSetMetaData.fieldsSize = 0;
    }

    void configurePublisher() {
        const UA_NodeId connection = addConnection(server, "TsnHub output", config.publishUrl,
                                                    config.outputPublisherId);
        UA_PublishedDataSetConfig dataSetConfig;
        std::memset(&dataSetConfig, 0, sizeof(dataSetConfig));
        dataSetConfig.publishedDataSetType = UA_PUBSUB_DATASET_PUBLISHEDITEMS;
        dataSetConfig.name = UA_STRING(const_cast<char *>("TsnHub PublishedDataSet"));
        UA_NodeId publishedDataSet;
        UA_NodeId_init(&publishedDataSet);
        const auto addResult = UA_Server_addPublishedDataSet(server, &dataSetConfig, &publishedDataSet);
        requireGood(addResult.addResult, "add PublishedDataSet");

        UA_DataSetFieldConfig fieldConfig;
        std::memset(&fieldConfig, 0, sizeof(fieldConfig));
        fieldConfig.dataSetFieldType = UA_PUBSUB_DATASETFIELD_VARIABLE;
        fieldConfig.field.variable.fieldNameAlias = UA_STRING(const_cast<char *>("TsnHubValue"));
        fieldConfig.field.variable.publishParameters.publishedVariable = outputNode;
        fieldConfig.field.variable.publishParameters.attributeId = UA_ATTRIBUTEID_VALUE;
        const auto fieldResult = UA_Server_addDataSetField(server, publishedDataSet, &fieldConfig, nullptr);
        requireGood(fieldResult.result, "add DataSetField");

        UA_WriterGroupConfig writerGroupConfig;
        std::memset(&writerGroupConfig, 0, sizeof(writerGroupConfig));
        writerGroupConfig.name = UA_STRING(const_cast<char *>("TsnHub WriterGroup"));
        writerGroupConfig.publishingInterval = config.publishingIntervalMs;
        writerGroupConfig.writerGroupId = config.outputWriterGroupId;
        writerGroupConfig.encodingMimeType = UA_PUBSUB_ENCODING_UADP;
        UA_UadpWriterGroupMessageDataType message;
        UA_UadpWriterGroupMessageDataType_init(&message);
        message.networkMessageContentMask = static_cast<UA_UadpNetworkMessageContentMask>(
            UA_UADPNETWORKMESSAGECONTENTMASK_PUBLISHERID |
            UA_UADPNETWORKMESSAGECONTENTMASK_GROUPHEADER |
            UA_UADPNETWORKMESSAGECONTENTMASK_WRITERGROUPID |
            UA_UADPNETWORKMESSAGECONTENTMASK_PAYLOADHEADER);
        UA_ExtensionObject_setValueNoDelete(&writerGroupConfig.messageSettings, &message,
            &UA_TYPES[UA_TYPES_UADPWRITERGROUPMESSAGEDATATYPE]);
        UA_NodeId writerGroup;
        UA_NodeId_init(&writerGroup);
        requireGood(UA_Server_addWriterGroup(server, connection, &writerGroupConfig, &writerGroup),
                    "add WriterGroup");

        UA_DataSetWriterConfig writerConfig;
        std::memset(&writerConfig, 0, sizeof(writerConfig));
        writerConfig.name = UA_STRING(const_cast<char *>("TsnHub DataSetWriter"));
        writerConfig.dataSetWriterId = config.outputDataSetWriterId;
        writerConfig.keyFrameCount = 10;
        requireGood(UA_Server_addDataSetWriter(server, writerGroup, publishedDataSet,
                                               &writerConfig, nullptr),
                    "add DataSetWriter");
    }

    void pollInput() {
        UA_Variant value;
        UA_Variant_init(&value);
        const UA_StatusCode status = UA_Server_readValue(server, inputNode, &value);
        if(status == UA_STATUSCODE_GOOD && UA_Variant_hasScalarType(&value, &UA_TYPES[UA_TYPES_INT32])) {
            const auto current = *static_cast<UA_Int32 *>(value.data);
            if(!hasInput || current != lastInput) {
                Frame frame;
                frame.sequence = ++sequence;
                frame.priority = 0;
                frame.stream = "opcua.int32";
                frame.payload.resize(sizeof(current));
                std::memcpy(frame.payload.data(), &current, sizeof(current));
                scheduler.enqueue(std::move(frame));
                lastInput = current;
                hasInput = true;
            }
        }
        UA_Variant_clear(&value);
    }

    void release() {
        for(auto &frame : scheduler.releaseReady()) {
            if(frame.payload.size() != sizeof(UA_Int32)) continue;
            UA_Int32 value;
            std::memcpy(&value, frame.payload.data(), sizeof(value));
            UA_Variant variant;
            UA_Variant_init(&variant);
            UA_Variant_setScalar(&variant, &value, &UA_TYPES[UA_TYPES_INT32]);
            requireGood(UA_Server_writeValue(server, outputNode, variant), "write output value");
        }
    }
};

Open62541Bridge::Open62541Bridge(NativePubSubConfig config, SchedulerConfig scheduler)
    : impl_(std::make_unique<Impl>(std::move(config), std::move(scheduler))) {}
Open62541Bridge::~Open62541Bridge() = default;
void Open62541Bridge::run(std::atomic_bool &running) {
    requireGood(UA_Server_run_startup(impl_->server), "server startup");
    while(running.load(std::memory_order_relaxed)) {
        UA_Server_run_iterate(impl_->server, false);
        impl_->pollInput();
        impl_->release();
        std::this_thread::sleep_for(impl_->config.pollInterval);
    }
    UA_Server_run_shutdown(impl_->server);
}
SchedulerStats Open62541Bridge::stats() const { return impl_->scheduler.stats(); }

} // namespace tsnhub
