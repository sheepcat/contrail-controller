/*
 * Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
 */

#ifndef vnsw_test_ovs_agent_init_hpp
#define vnsw_test_ovs_agent_init_hpp

#include <boost/program_options.hpp>
#include <init/contrail_init_common.h>
#include <test/test_pkt0_interface.h>
#include <test/test_agent_init.h>
#include <ovs_tor_agent/ovsdb_client/ovsdb_client_tcp.h>

class Agent;
class AgentParam;
class TestClient;
class OvsPeerManager;

void LoadAndRun(const std::string &file_name);

TestClient *OvsTestInit(const char *init_file, bool ovs_init);

namespace OVSDB {
// OVSDB::OvsdbClientTcp objects override for test code to
// provide test functionality
class OvsdbClientTcpSessionTest : public OvsdbClientTcpSession {
public:
    OvsdbClientTcpSessionTest(Agent *agent, OvsPeerManager *manager,
                              TcpServer *server, Socket *sock,
                              bool async_ready = true);
    virtual ~OvsdbClientTcpSessionTest();

    // maximum number of inflight txn messages allowed
    virtual bool ThrottleInFlightTxnMessages() { return true; }
};

class OvsdbClientTcpTest : public OvsdbClientTcp {
public:
    OvsdbClientTcpTest(Agent *agent, IpAddress tor_ip, int tor_port,
                       IpAddress tsn_ip, int keepalive_interval,
                       OvsPeerManager *manager);
    virtual ~OvsdbClientTcpTest();

    virtual TcpSession *AllocSession(Socket *socket);
};
};

// The class to drive agent initialization.
// Defines control parameters used to enable/disable agent features
class TestOvsAgentInit : public TestAgentInit {
public:
    TestOvsAgentInit();
    virtual ~TestOvsAgentInit();

    void CreatePeers();
    void CreateModules();
    void CreateDBTables();
    void RegisterDBClients();

    OvsPeerManager *ovs_peer_manager() const;
    OVSDB::OvsdbClientTcp *ovsdb_client() const;

    void set_ovs_init(bool ovs_init);

    void KSyncShutdown();

private:
    std::auto_ptr<OvsPeerManager> ovs_peer_manager_;
    std::auto_ptr<OVSDB::OvsdbClientTcp> ovsdb_client_;

    bool ovs_init_;
    DISALLOW_COPY_AND_ASSIGN(TestOvsAgentInit);
};

#endif // vnsw_test_ovs_agent_init_hpp
