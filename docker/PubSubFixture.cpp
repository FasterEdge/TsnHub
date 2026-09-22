// FasterEdge 开源项目 - Github: https://github.com/FasterEdge - Gitee: https://gitee.com/FasterEdge
#include <open62541/server.h>
#include <open62541/server_config_default.h>
#include <open62541/server_pubsub.h>

#include <CLI/CLI.hpp>

#include <chrono>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

#ifndef UA_ENABLE_PUBSUB
#error "PubSubFixture requires UA_ENABLE_PUBSUB"
#endif

namespace {
void requireGood(UA_StatusCode status, const char *operation) {
    if(status != UA_STATUSCODE_GOOD)
        throw std::runtime_error(std::string(operation) + ": " + UA_StatusCode_name(status));
}
UA_NodeId addVariable(UA_Server *server, UA_UInt32 id, const char *name) {
    UA_VariableAttributes attr = UA_VariableAttributes_default;
    attr.displayName = UA_LOCALIZEDTEXT(const_cast<char *>("en-US"), const_cast<char *>(name));
    attr.dataType = UA_TYPES[UA_TYPES_INT32].typeId;
    UA_Int32 initial = 0;
    UA_Variant_setScalar(&attr.value, &initial, &UA_TYPES[UA_TYPES_INT32]);
    UA_NodeId node;
    requireGood(UA_Server_addVariableNode(server, UA_NODEID_NUMERIC(1, id),
        UA_NODEID_NUMERIC(0, UA_NS0ID_OBJECTSFOLDER), UA_NODEID_NUMERIC(0, UA_NS0ID_ORGANIZES),
        UA_QUALIFIEDNAME(1, const_cast<char *>(name)),
        UA_NODEID_NUMERIC(0, UA_NS0ID_BASEDATAVARIABLETYPE), attr, nullptr, &node), "add variable");
    return node;
}
UA_NodeId addConnection(UA_Server *server, const std::string &url, UA_UInt16 publisherId) {
    UA_PubSubConnectionConfig config{};
    config.name = UA_STRING(const_cast<char *>("fixture connection"));
    UA_NetworkAddressUrlDataType address{UA_STRING_NULL, UA_STRING(const_cast<char *>(url.c_str()))};
    UA_Variant_setScalar(&config.address, &address, &UA_TYPES[UA_TYPES_NETWORKADDRESSURLDATATYPE]);
    config.transportProfileUri = UA_STRING(const_cast<char *>("http://opcfoundation.org/UA-Profile/Transport/pubsub-udp-uadp"));
    config.publisherId.idType = UA_PUBLISHERIDTYPE_UINT16;
    config.publisherId.id.uint16 = publisherId;
    UA_NodeId id;
    requireGood(UA_Server_addPubSubConnection(server, &config, &id), "add connection");
    return id;
}
UA_NodeId configurePublisher(UA_Server *server, const std::string &bindUrl,
                             const std::string &sendUrl, UA_NodeId variable,
                             UA_UInt16 publisherId, UA_UInt16 groupId,
                             UA_UInt16 writerId) {
    const auto connection = addConnection(server, bindUrl, publisherId);
    UA_PublishedDataSetConfig pds{};
    pds.publishedDataSetType = UA_PUBSUB_DATASET_PUBLISHEDITEMS;
    pds.name = UA_STRING(const_cast<char *>("fixture data"));
    UA_NodeId pdsId;
    requireGood(UA_Server_addPublishedDataSet(server, &pds, &pdsId).addResult, "add data set");
    UA_DataSetFieldConfig field{};
    field.dataSetFieldType = UA_PUBSUB_DATASETFIELD_VARIABLE;
    field.field.variable.fieldNameAlias = UA_STRING(const_cast<char *>("value"));
    field.field.variable.publishParameters.publishedVariable = variable;
    field.field.variable.publishParameters.attributeId = UA_ATTRIBUTEID_VALUE;
    requireGood(UA_Server_addDataSetField(server, pdsId, &field, nullptr).result, "add field");
    UA_WriterGroupConfig group{};
    group.name = UA_STRING(const_cast<char *>("fixture writer group"));
    group.publishingInterval = 10.0;
    group.writerGroupId = groupId;
    group.encodingMimeType = UA_PUBSUB_ENCODING_UADP;
    UA_UadpWriterGroupMessageDataType message;
    UA_UadpWriterGroupMessageDataType_init(&message);
    message.networkMessageContentMask = static_cast<UA_UadpNetworkMessageContentMask>(
        UA_UADPNETWORKMESSAGECONTENTMASK_PUBLISHERID | UA_UADPNETWORKMESSAGECONTENTMASK_GROUPHEADER |
        UA_UADPNETWORKMESSAGECONTENTMASK_WRITERGROUPID | UA_UADPNETWORKMESSAGECONTENTMASK_PAYLOADHEADER);
    UA_ExtensionObject_setValueNoDelete(&group.messageSettings, &message,
        &UA_TYPES[UA_TYPES_UADPWRITERGROUPMESSAGEDATATYPE]);
    UA_NetworkAddressUrlDataType sendAddress{UA_STRING_NULL,
        UA_STRING(const_cast<char *>(sendUrl.c_str()))};
    UA_DatagramWriterGroupTransport2DataType udpTransport;
    UA_DatagramWriterGroupTransport2DataType_init(&udpTransport);
    udpTransport.address.encoding = UA_EXTENSIONOBJECT_DECODED;
    udpTransport.address.content.decoded.type = &UA_TYPES[UA_TYPES_NETWORKADDRESSURLDATATYPE];
    udpTransport.address.content.decoded.data = &sendAddress;
    UA_ExtensionObject transportSettings;
    std::memset(&transportSettings, 0, sizeof(transportSettings));
    transportSettings.encoding = UA_EXTENSIONOBJECT_DECODED;
    transportSettings.content.decoded.type = &UA_TYPES[UA_TYPES_DATAGRAMWRITERGROUPTRANSPORT2DATATYPE];
    transportSettings.content.decoded.data = &udpTransport;
    group.transportSettings = transportSettings;
    UA_NodeId groupNode;
    requireGood(UA_Server_addWriterGroup(server, connection, &group, &groupNode), "add writer group");
    UA_DataSetWriterConfig writer{};
    writer.name = UA_STRING(const_cast<char *>("fixture writer"));
    writer.dataSetWriterId = writerId;
    writer.keyFrameCount = 1;
    requireGood(UA_Server_addDataSetWriter(server, groupNode, pdsId, &writer, nullptr), "add writer");
    return groupNode;
}
void configureSubscriber(UA_Server *server, const std::string &url, UA_NodeId variable,
                         UA_UInt16 publisherId, UA_UInt16 groupId, UA_UInt16 writerId) {
    const auto connection = addConnection(server, url, publisherId);
    UA_ReaderGroupConfig group{};
    group.name = UA_STRING(const_cast<char *>("fixture reader group"));
    UA_NodeId groupNode;
    requireGood(UA_Server_addReaderGroup(server, connection, &group, &groupNode), "add reader group");
    UA_DataSetReaderConfig reader{};
    reader.name = UA_STRING(const_cast<char *>("fixture reader"));
    reader.publisherId.idType = UA_PUBLISHERIDTYPE_UINT16;
    reader.publisherId.id.uint16 = publisherId;
    reader.writerGroupId = groupId;
    reader.dataSetWriterId = writerId;
    reader.dataSetMetaData.name = UA_STRING(const_cast<char *>("fixture int32"));
    reader.dataSetMetaData.fieldsSize = 1;
    reader.dataSetMetaData.fields = static_cast<UA_FieldMetaData *>(UA_Array_new(1, &UA_TYPES[UA_TYPES_FIELDMETADATA]));
    UA_FieldMetaData_init(reader.dataSetMetaData.fields);
    UA_NodeId_copy(&UA_TYPES[UA_TYPES_INT32].typeId, &reader.dataSetMetaData.fields[0].dataType);
    reader.dataSetMetaData.fields[0].builtInType = UA_NS0ID_INT32;
    reader.dataSetMetaData.fields[0].valueRank = -1;
    UA_NodeId readerNode;
    requireGood(UA_Server_addDataSetReader(server, groupNode, &reader, &readerNode), "add reader");
    UA_FieldTargetDataType target;
    UA_FieldTargetDataType_init(&target);
    target.attributeId = UA_ATTRIBUTEID_VALUE;
    target.targetNodeId = variable;
    requireGood(UA_Server_DataSetReader_createTargetVariables(server, readerNode, 1, &target), "target variable");
    UA_NodeId_clear(&reader.dataSetMetaData.fields[0].dataType);
    UA_Array_delete(reader.dataSetMetaData.fields, 1, &UA_TYPES[UA_TYPES_FIELDMETADATA]);
    reader.dataSetMetaData.fields = nullptr;
    reader.dataSetMetaData.fieldsSize = 0;
}
}

int main(int argc, char **argv) {
    CLI::App app{"TsnHub native PubSub integration fixture"};
    std::string role;
    std::string url;
    std::string bindUrl;
    std::uint16_t publisherId = 1, groupId = 1, writerId = 1;
    int count = 20, expect = 20;
    app.add_option("role", role)->required()->check(CLI::IsMember({"publisher", "subscriber"}));
    app.add_option("--url", url)->required();
    app.add_option("--bind-url", bindUrl, "Local UDP bind URL for publisher (default: ephemeral wildcard)");
    app.add_option("--publisher-id", publisherId);
    app.add_option("--writer-group-id", groupId);
    app.add_option("--writer-id", writerId);
    app.add_option("--count", count);
    app.add_option("--expect", expect);
    CLI11_PARSE(app, argc, argv);
    try {
        UA_ServerConfig config;
        std::memset(&config, 0, sizeof(config));
        requireGood(UA_ServerConfig_setMinimal(&config, 0, nullptr), "server config");
        UA_Server *server = UA_Server_newWithConfig(&config);
        if(!server) {
            UA_ServerConfig_clear(&config);
            throw std::runtime_error("server allocation failed");
        }
        const auto variable = addVariable(server, 6001, "FixtureValue");
        UA_NodeId writerGroup;
        UA_NodeId_init(&writerGroup);
        if(role == "publisher") {
            const auto bind = bindUrl.empty() ? std::string("opc.udp://0.0.0.0:0") : bindUrl;
            writerGroup = configurePublisher(server, bind, url, variable,
                                             publisherId, groupId, writerId);
        }
        else configureSubscriber(server, url, variable, publisherId, groupId, writerId);
        requireGood(UA_Server_enableAllPubSubComponents(server), "enable PubSub");
        requireGood(UA_Server_run_startup(server), "startup");
        if(role == "publisher") {
            // open62541's publish callback samples the current variable value,
            // so a write issued before the WriterGroup becomes operational can
            // be superseded before the first network message is built. Iterate
            // until the group is ready so the first fixture value is actually
            // sent instead of dropped by the startup window.
            UA_PubSubState state = UA_PUBSUBSTATE_PAUSED;
            const auto ready = std::chrono::steady_clock::now() + std::chrono::seconds{5};
            while(state != UA_PUBSUBSTATE_OPERATIONAL &&
                  std::chrono::steady_clock::now() < ready) {
                UA_Server_run_iterate(server, false);
                requireGood(UA_Server_getWriterGroupState(server, writerGroup, &state),
                            "writer group state");
                if(state != UA_PUBSUBSTATE_OPERATIONAL)
                    std::this_thread::sleep_for(std::chrono::milliseconds{10});
            }
            if(state != UA_PUBSUBSTATE_OPERATIONAL)
                throw std::runtime_error("writer group did not become operational");
        }
        int last = 0;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{20};
        while(std::chrono::steady_clock::now() < deadline) {
            if(role == "subscriber") {
                UA_Server_run_iterate(server, false);
                UA_Variant value;
                UA_Variant_init(&value);
                if(UA_Server_readValue(server, variable, &value) == UA_STATUSCODE_GOOD &&
                   UA_Variant_hasScalarType(&value, &UA_TYPES[UA_TYPES_INT32])) {
                    last = *static_cast<UA_Int32 *>(value.data);
                }
                UA_Variant_clear(&value);
                if(last >= expect) break;
                std::this_thread::sleep_for(std::chrono::milliseconds{10});
                continue;
            }

            if(last < count) {
                ++last;
                UA_Int32 value = last;
                UA_Variant variant;
                UA_Variant_init(&variant);
                UA_Variant_setScalar(&variant, &value, &UA_TYPES[UA_TYPES_INT32]);
                // Wait for one publish cycle so the just-written value is
                // actually sent before the next value replaces it. open62541's
                // publish callback samples the current variable value, so a
                // new write can otherwise erase a frame that was not yet sent.
                UA_DateTime previousPublish = 0;
                requireGood(UA_Server_getWriterGroupLastPublishTimestamp(
                                server, writerGroup, &previousPublish),
                            "writer group last publish");
                requireGood(UA_Server_writeValue(server, variable, variant), "write fixture value");
                const auto publishDeadline = std::chrono::steady_clock::now() +
                                             std::chrono::seconds{1};
                while(std::chrono::steady_clock::now() < publishDeadline) {
                    UA_Server_run_iterate(server, false);
                    UA_DateTime currentPublish = 0;
                    const UA_StatusCode status =
                        UA_Server_getWriterGroupLastPublishTimestamp(
                            server, writerGroup, &currentPublish);
                    if(status == UA_STATUSCODE_GOOD &&
                       currentPublish != previousPublish)
                        break;
                    std::this_thread::sleep_for(std::chrono::milliseconds{1});
                }
                if(std::chrono::steady_clock::now() >= publishDeadline)
                    throw std::runtime_error("fixture publish timed out");
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds{250});
                break;
            }
        }
        UA_Server_run_shutdown(server);
        UA_Server_delete(server);
        std::cout << role << " final_value=" << last << '\n';
        return last >= (role == "subscriber" ? expect : count) ? 0 : 1;
    } catch(const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
