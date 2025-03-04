/*
 * Copyright (c) 2014 Juniper Networks, Inc. All rights reserved.
 */

#ifndef SRC_VNSW_AGENT_OVS_TOR_AGENT_OVSDB_CLIENT_UNICAST_MAC_REMOTE_OVSDB_H_
#define SRC_VNSW_AGENT_OVS_TOR_AGENT_OVSDB_CLIENT_UNICAST_MAC_REMOTE_OVSDB_H_

#include <base/lifetime.h>
#include <ovsdb_entry.h>
#include <ovsdb_object.h>
#include <ovsdb_client_idl.h>

class BridgeRouteEntry;

namespace OVSDB {
class VrfOvsdbObject;

class UnicastMacRemoteTable : public OvsdbDBObject {
public:
    UnicastMacRemoteTable(OvsdbClientIdl *idl,
                          const std::string &logical_switch_name);
    UnicastMacRemoteTable(OvsdbClientIdl *idl, AgentRouteTable *table,
                          const std::string &logical_switch_name);
    virtual ~UnicastMacRemoteTable();

    virtual void OvsdbRegisterDBTable(DBTable *tbl);

    void OvsdbNotify(OvsdbClientIdl::Op, struct ovsdb_idl_row *);

    KSyncEntry *Alloc(const KSyncEntry *key, uint32_t index);
    KSyncEntry *DBToKSyncEntry(const DBEntry*);
    OvsdbDBEntry *AllocOvsEntry(struct ovsdb_idl_row *row);

    DBFilterResp OvsdbDBEntryFilter(const DBEntry *entry,
                                    const OvsdbDBEntry *ovsdb_entry);

    void ManagedDelete();
    virtual void EmptyTable();

    const std::string &logical_switch_name() const;

private:
    std::string logical_switch_name_;
    LifetimeRef<UnicastMacRemoteTable> table_delete_ref_;
    DISALLOW_COPY_AND_ASSIGN(UnicastMacRemoteTable);
};

class UnicastMacRemoteEntry : public OvsdbDBEntry {
public:
    typedef std::set<struct ovsdb_idl_row *> OvsdbDupIdlList;
    enum Trace {
        ADD_REQ,
        DEL_REQ,
        ADD_ACK,
        DEL_ACK,
    };
    UnicastMacRemoteEntry(UnicastMacRemoteTable *table, const std::string mac);
    UnicastMacRemoteEntry(UnicastMacRemoteTable *table,
                          const BridgeRouteEntry *entry);
    UnicastMacRemoteEntry(UnicastMacRemoteTable *table,
                          const UnicastMacRemoteEntry *key);
    UnicastMacRemoteEntry(UnicastMacRemoteTable *table,
                          struct ovsdb_idl_row *entry);

    // OVSDB schema does not consider a key for unicast mac remote entry
    // so over-ride the default behaviour of NotifyAdd and NotifyDelete
    // to handle multiple possible idl row associated with single unicast
    // mac remote entry.
    virtual void NotifyAdd(struct ovsdb_idl_row *);
    virtual void NotifyDelete(struct ovsdb_idl_row *);

    void PreAddChange();
    void PostDelete();
    void AddMsg(struct ovsdb_idl_txn *);
    void ChangeMsg(struct ovsdb_idl_txn *);
    void DeleteMsg(struct ovsdb_idl_txn *);

    void OvsdbChange();

    bool Sync(DBEntry*);
    bool IsLess(const KSyncEntry&) const;
    std::string ToString() const {return "Unicast Mac Remote";}
    KSyncEntry* UnresolvedReference();

    const std::string &mac() const;
    const std::string &logical_switch_name() const;
    const std::string &dest_ip() const;
    bool self_exported_route() const;

private:
    friend class UnicastMacRemoteTable;
    friend class VrfOvsdbObject;
    void SendTrace(Trace event) const;
    void DeleteDupEntries(struct ovsdb_idl_txn *);

    std::string mac_;
    std::string logical_switch_name_;
    std::string dest_ip_;
    bool self_exported_route_;
    KSyncEntryPtr logical_switch_;
    OvsdbDupIdlList dup_list_;
    DISALLOW_COPY_AND_ASSIGN(UnicastMacRemoteEntry);
};
};

#endif //SRC_VNSW_AGENT_OVS_TOR_AGENT_OVSDB_CLIENT_UNICAST_MAC_REMOTE_OVSDB_H_

