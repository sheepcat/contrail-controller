/*
 * Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
 */

#include "viz_collector.h"
#include <boost/asio/ip/host_name.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/bind.hpp>

#include "base/logging.h"
#include "base/task.h"
#include "base/parse_object.h"
#include "io/event_manager.h"

#include "sandesh/sandesh_types.h"
#include "sandesh/sandesh.h"
#include "sandesh/sandesh_session.h"

#include "ruleeng.h"
#include "protobuf_collector.h"
#include "sflow_collector.h"
#include "ipfix_collector.h"

using std::string;
using boost::system::error_code;

VizCollector::VizCollector(EventManager *evm, unsigned short listen_port,
            bool protobuf_collector_enabled,
            unsigned short protobuf_listen_port,
            const std::vector<std::string> &cassandra_ips,
            const std::vector<int> &cassandra_ports,
            const std::string &redis_uve_ip, unsigned short redis_uve_port,
            const std::string &redis_password,
            const std::string &brokers,
            int syslog_port, int sflow_port, int ipfix_port,
            uint16_t partitions,
            bool dup, const DbHandler::TtlMap& ttl_map,
            const std::string &cassandra_user,
            const std::string &cassandra_password) :
    db_initializer_(new DbHandlerInitializer(evm, DbGlobalName(dup), -1,
        std::string("collector:DbIf"),
        boost::bind(&VizCollector::DbInitializeCb, this),
        cassandra_ips, cassandra_ports, ttl_map, cassandra_user, cassandra_password)),
    osp_(new OpServerProxy(evm, this, redis_uve_ip, redis_uve_port,
         redis_password, brokers, partitions)),
    ruleeng_(new Ruleeng(db_initializer_->GetDbHandler(), osp_.get())),
    collector_(new Collector(evm, listen_port, db_initializer_->GetDbHandler(),
        osp_.get(),
        boost::bind(&Ruleeng::rule_execute, ruleeng_.get(), _1, _2, _3),
        cassandra_ips, cassandra_ports, ttl_map, cassandra_user, cassandra_password)),
    syslog_listener_(new SyslogListeners(evm,
            boost::bind(&Ruleeng::rule_execute, ruleeng_.get(), _1, _2, _3),
            db_initializer_->GetDbHandler(), syslog_port)),
    sflow_collector_(new SFlowCollector(evm, db_initializer_->GetDbHandler(),
        sflow_port, -1)),
    ipfix_collector_(new IpfixCollector(evm, db_initializer_->GetDbHandler(),
        string(), ipfix_port)) {
    error_code error;
    if (dup)
        name_ = boost::asio::ip::host_name(error) + "dup";
    else
        name_ = boost::asio::ip::host_name(error);
    if (protobuf_collector_enabled) {
        protobuf_collector_.reset(new ProtobufCollector(evm,
            protobuf_listen_port, cassandra_ips, cassandra_ports,
            ttl_map, cassandra_user, cassandra_password));
    }
}

VizCollector::VizCollector(EventManager *evm, DbHandler *db_handler,
        Ruleeng *ruleeng, Collector *collector, OpServerProxy *osp) :
    db_initializer_(new DbHandlerInitializer(evm, DbGlobalName(false), -1,
        std::string("collector::DbIf"),
        boost::bind(&VizCollector::DbInitializeCb, this),
        db_handler)),
    osp_(osp),
    ruleeng_(ruleeng),
    collector_(collector),
    syslog_listener_(new SyslogListeners (evm,
            boost::bind(&Ruleeng::rule_execute, ruleeng, _1, _2, _3),
            db_handler)),
    sflow_collector_(NULL), ipfix_collector_(NULL) {
    error_code error;
    name_ = boost::asio::ip::host_name(error);
}

VizCollector::~VizCollector() {
}

std::string VizCollector::DbGlobalName(bool dup) {
    return collector_->DbGlobalName(dup);
}

bool VizCollector::SendRemote(const string& destination,
        const string& dec_sandesh) {
    if (collector_){
        return collector_->SendRemote(destination, dec_sandesh);
    } else {
        return false;
    }
}

void VizCollector::WaitForIdle() {
    static const int kTimeout = 15;
    TaskScheduler *scheduler = TaskScheduler::GetInstance();

    for (int i = 0; i < (kTimeout * 1000); i++) {
        if (scheduler->IsEmpty()) {
            break;
        }
        usleep(1000);
    }
}

void VizCollector::Shutdown() {
    // First shutdown collector
    collector_->Shutdown();
    WaitForIdle();

    // Wait until all connections are cleaned up.
    for (int cnt = 0; collector_->ConnectionsCount() != 0 && cnt < 15; cnt++) {
        sleep(1);
    }
    TcpServerManager::DeleteServer(collector_);

    syslog_listener_->Shutdown();
    WaitForIdle();

    if (protobuf_collector_) {
        protobuf_collector_->Shutdown();
        WaitForIdle();
    }
    if (sflow_collector_) {
        sflow_collector_->Shutdown();
        WaitForIdle();
        UdpServerManager::DeleteServer(sflow_collector_);
    }
    if (ipfix_collector_) {
        ipfix_collector_->Shutdown();
        WaitForIdle();
        UdpServerManager::DeleteServer(ipfix_collector_);
    }

    db_initializer_->Shutdown();
    LOG(DEBUG, __func__ << " viz_collector done");
}

void VizCollector::DbInitializeCb() {
    ruleeng_->Init();
    if (!syslog_listener_->IsRunning()) {
        syslog_listener_->Start();
        LOG(DEBUG, __func__ << " Initialization of syslog listener done!");
    }
    if (protobuf_collector_) {
        protobuf_collector_->Initialize();
    }
    if (sflow_collector_) {
        sflow_collector_->Start();
    }
    if (ipfix_collector_) {
        ipfix_collector_->Start();
    }
}

bool VizCollector::Init() {
    return db_initializer_->Initialize();
}

void VizCollector::SendProtobufCollectorStatistics() {
    if (protobuf_collector_) {
        protobuf_collector_->SendStatistics(name_);
    }
}

void VizCollector::SendGeneratorStatistics() {
    if (collector_) {
        collector_->SendGeneratorStatistics();
    }
}

void VizCollector::TestDatabaseConnection() {
    if (collector_) {
        collector_->TestDatabaseConnection();
    }
}
