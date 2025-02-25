/*
 * Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
 */

#ifndef DB_HANDLER_H_
#define DB_HANDLER_H_

#include <boost/scoped_ptr.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/ptr_container/ptr_map.hpp>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/name_generator.hpp>

#if __GNUC_PREREQ(4, 6)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-result"
#endif
#include <boost/uuid/uuid_generators.hpp>
#if __GNUC_PREREQ(4, 6)
#pragma GCC diagnostic pop
#endif

#include <boost/tuple/tuple.hpp>

#include "Thrift.h"
#include "base/parse_object.h"
#include "io/event_manager.h"
#include "base/random_generator.h"
#include "gendb_if.h"
#include "gendb_statistics.h"
#include "sandesh/sandesh.h"
#include "viz_message.h"
#include "uflow_types.h"

class DbHandler {
public:
    static const int DefaultDbTTL = 0;
    static boost::uuids::uuid seed_uuid;

    typedef enum {
        INVALID = 0,
        UINT64 = 1,
        STRING = 2,
        DOUBLE = 3,
        MAXVAL 
    } VarType;

    struct Var {
        Var() : type(INVALID), str(""), num(0), dbl(0) {}
        Var(const std::string &s) : type(STRING), str(s), num(0), dbl(0) {}
        Var(uint64_t v) : type(UINT64), str(""), num(v), dbl(0) {}
        Var(double d) : type(DOUBLE), str(""), num(0), dbl(d) {}
        VarType type;
        std::string str;
        uint64_t num;
        double dbl;
        bool operator==(const Var &other) const {
            if (type!=other.type) return false;
            switch (type) {
                case STRING:
                    if (str!=other.str) return false;
                    break;
                case UINT64:
                    if (num!=other.num) return false;
                    break;
                case DOUBLE:
                    if (dbl!=other.dbl) return false;
                    break;
                default:
                    break; 
            }
            return true;
        }
        friend inline std::ostream& operator<<(std::ostream& out,
            const Var& value);
    };

    typedef enum {
        INVALID_TTL = 0,
        FLOWDATA_TTL = 1,
        STATSDATA_TTL = 2,
        CONFIGAUDIT_TTL = 3,
        GLOBAL_TTL = 4,
    } TtlType;
    typedef std::map<TtlType, int> TtlMap;

    typedef std::map<std::string, std::string> RuleMap;

    typedef std::map<std::string, Var > AttribMap;
    typedef std::multimap<std::string, std::pair<Var, AttribMap> > TagMap;

    DbHandler(EventManager *evm, GenDb::GenDbIf::DbErrorHandler err_handler,
        const std::vector<std::string> &cassandra_ips,
        const std::vector<int> &cassandra_ports,
        std::string name, const TtlMap& ttl_map,
        const std::string& cassandra_user,
        const std::string& cassandra_password);
    DbHandler(GenDb::GenDbIf *dbif, const TtlMap& ttl_map);
    virtual ~DbHandler();

    static int GetTtlFromMap(const TtlMap& ttl_map,
            TtlType type);
    bool DropMessage(const SandeshHeader &header, const VizMsg *vmsg);
    bool Init(bool initial, int instance);
    void UnInit(int instance);
    void UnInitUnlocked(int instance);

    bool AllowMessageTableInsert(const SandeshHeader &header);
    bool MessageIndexTableInsert(const std::string& cfname,
        const SandeshHeader& header, const std::string& message_type,
        const boost::uuids::uuid& unm, const std::string keyword);
    virtual void MessageTableInsert(const VizMsg *vmsgp);
    void MessageTableOnlyInsert(const VizMsg *vmsgp);
    void FieldNamesTableInsert(const std::string& table_name,
        const std::string& field_name, const std::string& field_val,
        uint64_t timestamp, int ttl);
    void GetRuleMap(RuleMap& rulemap);

    void ObjectTableInsert(const std::string &table, const std::string &rowkey,
        uint64_t &timestamp, const boost::uuids::uuid& unm,
        const VizMsg *vmsgp);

    static std::vector<std::string> StatTableSelectStr(
            const std::string& statName, const std::string& statAttr,
            const AttribMap & attribs);

    void StatTableInsert(uint64_t ts, 
            const std::string& statName,
            const std::string& statAttr,
            const TagMap & attribs_tag,
            const AttribMap & attribs_all);

    void StatTableInsertTtl(uint64_t ts, 
            const std::string& statName,
            const std::string& statAttr,
            const TagMap & attribs_tag,
            const AttribMap & attribs_all, int ttl);

    bool FlowTableInsert(const pugi::xml_node& parent,
        const SandeshHeader &header);
    bool UnderlayFlowSampleInsert(const UFlowData& flow_data,
        uint64_t timestamp);
    bool GetStats(uint64_t *queue_count, uint64_t *enqueues) const;
    bool GetStats(std::vector<GenDb::DbTableInfo> *vdbti,
        GenDb::DbErrors *dbe, std::vector<GenDb::DbTableInfo> *vstats_dbti);
    void GetSandeshStats(std::string *drop_level,
        std::vector<SandeshStats> *vdropmstats) const;

    void SetDbQueueWaterMarkInfo(Sandesh::QueueWaterMarkInfo &wm,
        boost::function<void (void)> defer_undefer_cb);
    void ResetDbQueueWaterMarkInfo();
    std::string GetHost() const;
    int GetPort() const;
    std::string GetName() const;

private:
    bool CreateTables();
    void SetDropLevel(size_t queue_count, SandeshLevel::type level,
        boost::function<void (void)> cb);
    bool Setup(int instance);
    bool Initialize(int instance);
    bool StatTableWrite(uint32_t t2,
        const std::string& statName, const std::string& statAttr,
        const std::pair<std::string,DbHandler::Var>& ptag,
        const std::pair<std::string,DbHandler::Var>& stag,
        uint32_t t1, const boost::uuids::uuid& unm,
        const std::string& jsonline, int ttl);
    int GetTtl(TtlType type) {
        return GetTtlFromMap(ttl_map_, type);
    }

    boost::scoped_ptr<GenDb::GenDbIf> dbif_;

    // Random generator for UUIDs
    ThreadSafeUuidGenerator umn_gen_;
    std::string name_;
    std::string col_name_;
    SandeshLevel::type drop_level_;
    VizMsgStatistics dropped_msg_stats_;
    GenDb::DbTableStatistics stable_stats_;
    mutable tbb::mutex smutex_;
    TtlMap ttl_map_;

    DISALLOW_COPY_AND_ASSIGN(DbHandler);
};

inline std::ostream& operator<<(std::ostream& out, const DbHandler::Var& value) {
    switch (value.type) {
      case DbHandler::STRING:
	out << value.str;
	break;
      case DbHandler::UINT64:
	out << value.num;
	break;
      case DbHandler::DOUBLE:
	out << value.dbl;
	break;
      default:
	out << "Invalid type: " << value.type;
	break;
    }
    return out;
}

//
// DbHandlerInitializer - Wrapper to perform DbHandler initialization
//
class DbHandlerInitializer {
 public:
    typedef boost::function<void(void)> InitializeDoneCb;
    DbHandlerInitializer(EventManager *evm,
        const std::string &db_name, int db_task_instance,
        const std::string &timer_task_name, InitializeDoneCb callback,
        const std::vector<std::string> &cassandra_ips,
        const std::vector<int> &cassandra_ports,
        const DbHandler::TtlMap& ttl_map,
        const std::string& cassandra_user,
        const std::string& cassandra_password);
    DbHandlerInitializer(EventManager *evm,
        const std::string &db_name, int db_task_instance,
        const std::string &timer_task_name, InitializeDoneCb callback,
        DbHandler *db_handler);
    virtual ~DbHandlerInitializer();
    bool Initialize();
    void Shutdown();
    DbHandler* GetDbHandler() const;

 private:
    bool InitTimerExpired();
    void InitTimerErrorHandler(std::string error_name,
        std::string error_message);
    void StartInitTimer();
    void ScheduleInit();

    static const int kInitRetryInterval = 10 * 1000; // in ms
    const std::string db_name_;
    const int db_task_instance_;
    boost::scoped_ptr<DbHandler> db_handler_;
    InitializeDoneCb callback_;
    Timer *db_init_timer_;
};

/*
 * pugi walker to process flow message
 */
template <typename T>
class FlowDataIpv4ObjectWalker : public pugi::xml_tree_walker {
public:
    FlowDataIpv4ObjectWalker(T &values) :
        values_(values) {
    }
    ~FlowDataIpv4ObjectWalker() {}

    // Callback that is called when traversal begins
    virtual bool begin(pugi::xml_node& node) {
        return true;
    }

    // Callback that is called for each node traversed
    virtual bool for_each(pugi::xml_node& node);

    // Callback that is called when traversal ends
    virtual bool end(pugi::xml_node& node) {
        return true;
    }

private:
    T &values_;
};

#endif /* DB_HANDLER_H_ */
