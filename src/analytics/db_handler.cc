/*
 * Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
 */

#include <exception>
#include <boost/bind.hpp>
#include <boost/assign/list_of.hpp>
#include <boost/asio/ip/host_name.hpp>
#include <boost/array.hpp>
#include <boost/uuid/name_generator.hpp>

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <base/logging.h>
#include <io/event_manager.h>
#include <base/connection_info.h>
#include <sandesh/sandesh_types.h>
#include <sandesh/sandesh.h>
#include <sandesh/sandesh_message_builder.h>
#include <sandesh/protocol/TXMLProtocol.h>

#include "viz_constants.h"
#include "vizd_table_desc.h"
#include "collector.h"
#include "db_handler.h"
#include "parser_util.h"

#define DB_LOG(_Level, _Msg)                                                   \
    do {                                                                       \
        if (LoggingDisabled()) break;                                          \
        log4cplus::Logger _Xlogger = log4cplus::Logger::getRoot();             \
        if (_Xlogger.isEnabledFor(log4cplus::_Level##_LOG_LEVEL)) {            \
            log4cplus::tostringstream _Xbuf;                                   \
            _Xbuf << name_ << ": " << __func__ << ": " << _Msg;                \
            _Xlogger.forcedLog(log4cplus::_Level##_LOG_LEVEL,                  \
                               _Xbuf.str());                                   \
        }                                                                      \
    } while (false)

using std::pair;
using std::string;
using boost::system::error_code;
using namespace pugi;
using namespace contrail::sandesh::protocol;
using process::ConnectionState;
using process::ConnectionType;
using process::ConnectionStatus;

DbHandler::DbHandler(EventManager *evm,
        GenDb::GenDbIf::DbErrorHandler err_handler,
        const std::vector<std::string> &cassandra_ips,
        const std::vector<int> &cassandra_ports,
        std::string name, const TtlMap& ttl_map,
        const std::string& cassandra_user,
        const std::string& cassandra_password) :
    name_(name),
    drop_level_(SandeshLevel::INVALID), ttl_map_(ttl_map) {
        int analytics_ttl = DbHandler::GetTtlFromMap(ttl_map, DbHandler::GLOBAL_TTL);
        if (analytics_ttl == -1) {
            DB_LOG(ERROR, "Unexpected analytics_ttl value: " << analytics_ttl);
            analytics_ttl = 0;
        }
        dbif_.reset(GenDb::GenDbIf::GenDbIfImpl(err_handler,
          cassandra_ips, cassandra_ports, analytics_ttl, name, false,
          cassandra_user, cassandra_password));

        error_code error;
        col_name_ = boost::asio::ip::host_name(error);
}

DbHandler::DbHandler(GenDb::GenDbIf *dbif, const TtlMap& ttl_map) :
    dbif_(dbif),
    ttl_map_(ttl_map) {
}

DbHandler::~DbHandler() {
}

int DbHandler::GetTtlFromMap(const DbHandler::TtlMap& ttl_map,
        DbHandler::TtlType type) {
    DbHandler::TtlMap::const_iterator it = ttl_map.find(type);
    if (it != ttl_map.end()) {
        return it->second*3600;
    } else {
        return -1;
    }
}

std::string DbHandler::GetName() const {
    return name_;
}
std::string DbHandler::GetHost() const {
    return dbif_->Db_GetHost();
}

int DbHandler::GetPort() const {
    return dbif_->Db_GetPort();
}

bool DbHandler::DropMessage(const SandeshHeader &header,
    const VizMsg *vmsg) {
    bool drop(DoDropSandeshMessage(header, drop_level_));
    if (drop) {
        tbb::mutex::scoped_lock lock(smutex_);
        dropped_msg_stats_.Update(vmsg);
    }
    return drop;
}
 
void DbHandler::SetDropLevel(size_t queue_count, SandeshLevel::type level,
    boost::function<void (void)> cb) {
    if (drop_level_ != level) {
        DB_LOG(INFO, "DB DROP LEVEL: [" << 
            Sandesh::LevelToString(drop_level_) << "] -> [" <<
            Sandesh::LevelToString(level) << "], DB QUEUE COUNT: " << 
            queue_count);
        drop_level_ = level;
        if (!cb.empty()) {
            cb();
        }
    }
}

bool DbHandler::CreateTables() {
    for (std::vector<GenDb::NewCf>::const_iterator it = vizd_tables.begin();
            it != vizd_tables.end(); it++) {
        if (!dbif_->Db_AddColumnfamily(*it)) {
            DB_LOG(ERROR, it->cfname_ << " FAILED");
            return false;
        }
    }

    for (std::vector<GenDb::NewCf>::const_iterator it = vizd_flow_tables.begin();
            it != vizd_flow_tables.end(); it++) {
        if (!dbif_->Db_AddColumnfamily(*it)) {
            DB_LOG(ERROR, it->cfname_ << " FAILED");
            return false;
        }
    }

    for (std::vector<GenDb::NewCf>::const_iterator it = vizd_stat_tables.begin();
            it != vizd_stat_tables.end(); it++) {
        if (!dbif_->Db_AddColumnfamily(*it)) {
            DB_LOG(ERROR, it->cfname_ << " FAILED");
            return false;
        }
    }

    GenDb::ColList col_list;
    std::string cfname = g_viz_constants.SYSTEM_OBJECT_TABLE;
    GenDb::DbDataValueVec key;
    key.push_back(g_viz_constants.SYSTEM_OBJECT_ANALYTICS);

    bool init_done = false;
    if (dbif_->Db_GetRow(col_list, cfname, key)) {
        for (GenDb::NewColVec::iterator it = col_list.columns_.begin();
                it != col_list.columns_.end(); it++) {
            std::string col_name;
            try {
                col_name = boost::get<std::string>(it->name->at(0));
            } catch (boost::bad_get& ex) {
                DB_LOG(ERROR, cfname << ": Column Name Get FAILED");
            }

            if (col_name == g_viz_constants.SYSTEM_OBJECT_START_TIME) {
                init_done = true;
            }
        }
    }

    if (!init_done) {
        std::auto_ptr<GenDb::ColList> col_list(new GenDb::ColList);
        col_list->cfname_ = g_viz_constants.SYSTEM_OBJECT_TABLE;
        // Rowkey
        GenDb::DbDataValueVec& rowkey = col_list->rowkey_;
        rowkey.reserve(1);
        rowkey.push_back(g_viz_constants.SYSTEM_OBJECT_ANALYTICS);
        // Columns
        GenDb::NewColVec& columns = col_list->columns_;
        columns.reserve(4);

        uint64_t current_tm = UTCTimestampUsec();

        GenDb::NewCol *col(new GenDb::NewCol(
            g_viz_constants.SYSTEM_OBJECT_START_TIME, current_tm, 0));
        columns.push_back(col);

        GenDb::NewCol *flow_col(new GenDb::NewCol(
            g_viz_constants.SYSTEM_OBJECT_FLOW_START_TIME, current_tm, 0));
        columns.push_back(flow_col);

        GenDb::NewCol *msg_col(new GenDb::NewCol(
            g_viz_constants.SYSTEM_OBJECT_MSG_START_TIME, current_tm, 0));
        columns.push_back(msg_col);

        GenDb::NewCol *stat_col(new GenDb::NewCol(
            g_viz_constants.SYSTEM_OBJECT_STAT_START_TIME, current_tm, 0));
        columns.push_back(stat_col);

        if (!dbif_->Db_AddColumnSync(col_list)) {
            VIZD_ASSERT(0);
        }
    }

    return true;
}

void DbHandler::UnInit(int instance) {
    dbif_->Db_Uninit("analytics::DbHandler", instance);
    dbif_->Db_SetInitDone(false);
}

// The caller *SHOULD* ensure that UnInit() is not called from another
// task that can be executed in parallel.
void DbHandler::UnInitUnlocked(int instance) {
    dbif_->Db_UninitUnlocked("analytics::DbHandler", instance);
    dbif_->Db_SetInitDone(false);
}

bool DbHandler::Init(bool initial, int instance) {
    SetDropLevel(0, SandeshLevel::INVALID, NULL);
    if (initial) {
        return Initialize(instance);
    } else {
        return Setup(instance);
    }
}

bool DbHandler::Initialize(int instance) {
    DB_LOG(DEBUG, "Initializing..");

    /* init of vizd table structures */
    init_vizd_tables();

    if (!dbif_->Db_Init("analytics::DbHandler", instance)) {
        DB_LOG(ERROR, "Connection to DB FAILED");
        return false;
    }

    if (!dbif_->Db_AddSetTablespace(g_viz_constants.COLLECTOR_KEYSPACE,"2")) {
        DB_LOG(ERROR, "Create/Set KEYSPACE: " <<
            g_viz_constants.COLLECTOR_KEYSPACE << " FAILED");
        return false;
    }

    if (!CreateTables()) {
        DB_LOG(ERROR, "CreateTables FAILED");
        return false;
    }

    dbif_->Db_SetInitDone(true);
    DB_LOG(DEBUG, "Initializing Done");

    return true;
}

bool DbHandler::Setup(int instance) {
    DB_LOG(DEBUG, "Setup..");
    if (!dbif_->Db_Init("analytics::DbHandler", 
                       instance)) {
        DB_LOG(ERROR, "Connection to DB FAILED");
        return false;
    }
    if (!dbif_->Db_SetTablespace(g_viz_constants.COLLECTOR_KEYSPACE)) {
        DB_LOG(ERROR, "Set KEYSPACE: " <<
                g_viz_constants.COLLECTOR_KEYSPACE << " FAILED");
        return false;
    }   
    for (std::vector<GenDb::NewCf>::const_iterator it = vizd_tables.begin();
            it != vizd_tables.end(); it++) {
        if (!dbif_->Db_UseColumnfamily(*it)) {
            DB_LOG(ERROR, it->cfname_ << 
                   ": Db_UseColumnfamily FAILED");
            return false;
        }
    }
    for (std::vector<GenDb::NewCf>::const_iterator it = vizd_flow_tables.begin();
            it != vizd_flow_tables.end(); it++) {
        if (!dbif_->Db_UseColumnfamily(*it)) {
            DB_LOG(ERROR, it->cfname_ << ": Db_UseColumnfamily FAILED");
            return false;
        }
    }
    for (std::vector<GenDb::NewCf>::const_iterator it = vizd_stat_tables.begin();
            it != vizd_stat_tables.end(); it++) {
        if (!dbif_->Db_UseColumnfamily(*it)) {
            DB_LOG(ERROR, it->cfname_ << ": Db_UseColumnfamily FAILED");
            return false;
        }
    }
    dbif_->Db_SetInitDone(true);
    DB_LOG(DEBUG, "Setup Done");
    return true;
}

void DbHandler::SetDbQueueWaterMarkInfo(Sandesh::QueueWaterMarkInfo &wm,
    boost::function<void (void)> defer_undefer_cb) {
    dbif_->Db_SetQueueWaterMark(boost::get<2>(wm),
        boost::get<0>(wm),
        boost::bind(&DbHandler::SetDropLevel, this, _1, boost::get<1>(wm),
        defer_undefer_cb));
}

void DbHandler::ResetDbQueueWaterMarkInfo() {
    dbif_->Db_ResetQueueWaterMarks();
}

void DbHandler::GetSandeshStats(std::string *drop_level,
    std::vector<SandeshStats> *vdropmstats) const {
    *drop_level = Sandesh::LevelToString(drop_level_);
    if (vdropmstats) {
        tbb::mutex::scoped_lock lock(smutex_);
        dropped_msg_stats_.Get(*vdropmstats);
    }
}

bool DbHandler::GetStats(uint64_t *queue_count, uint64_t *enqueues) const {
    return dbif_->Db_GetQueueStats(queue_count, enqueues);
}

bool DbHandler::GetStats(std::vector<GenDb::DbTableInfo> *vdbti,
    GenDb::DbErrors *dbe, std::vector<GenDb::DbTableInfo> *vstats_dbti) {
    {
        tbb::mutex::scoped_lock lock(smutex_);
        stable_stats_.Get(vstats_dbti);
    }
    return dbif_->Db_GetStats(vdbti, dbe);
}

bool DbHandler::AllowMessageTableInsert(const SandeshHeader &header) {
    return header.get_Type() != SandeshType::FLOW;
}

bool DbHandler::MessageIndexTableInsert(const std::string& cfname,
        const SandeshHeader& header,
        const std::string& message_type,
        const boost::uuids::uuid& unm,
        const std::string keyword) {
    std::auto_ptr<GenDb::ColList> col_list(new GenDb::ColList);
    col_list->cfname_ = cfname;
    // Rowkey
    GenDb::DbDataValueVec& rowkey = col_list->rowkey_;
    rowkey.reserve(8);
    uint32_t T2(header.get_Timestamp() >> g_viz_constants.RowTimeInBits);
    rowkey.push_back(T2);
    if (cfname == g_viz_constants.MESSAGE_TABLE_SOURCE) {
        rowkey.push_back(header.get_Source());
    } else if (cfname == g_viz_constants.MESSAGE_TABLE_MODULE_ID) {
        rowkey.push_back(header.get_Module());
    } else if (cfname == g_viz_constants.MESSAGE_TABLE_CATEGORY) {
        rowkey.push_back(header.get_Category());
    } else if (cfname == g_viz_constants.MESSAGE_TABLE_MESSAGE_TYPE) {
        rowkey.push_back(message_type);
    } else if (cfname == g_viz_constants.MESSAGE_TABLE_TIMESTAMP) {
    } else if (cfname == g_viz_constants.MESSAGE_TABLE_KEYWORD) {
        if (keyword.length())
            rowkey.push_back(keyword);
        else
            return false;
    } else {
        DB_LOG(ERROR, "Unknown table: " << cfname << ", message: "
                << message_type << ", message UUID: " << unm);
        return false;
    }
    // Columns
    GenDb::NewColVec& columns = col_list->columns_;
    columns.reserve(1);
    uint32_t T1(header.get_Timestamp() & g_viz_constants.RowTimeInMask);
    GenDb::DbDataValueVec *col_name(new GenDb::DbDataValueVec(1, T1));
    GenDb::DbDataValueVec *col_value(new GenDb::DbDataValueVec(1, unm));
    int ttl;
    if (message_type == "VncApiConfigLog") {
        ttl = GetTtl(CONFIGAUDIT_TTL);
    } else {
        ttl = GetTtl(GLOBAL_TTL);
    }
    GenDb::NewCol *col(new GenDb::NewCol(col_name, col_value, ttl));
    columns.push_back(col);
    if (!dbif_->Db_AddColumn(col_list)) {
        DB_LOG(ERROR, "Addition of message: " << message_type <<
                ", message UUID: " << unm << " to table: " << cfname <<
                " FAILED");
        return false;
    }
    return true;
}

void DbHandler::MessageTableOnlyInsert(const VizMsg *vmsgp) {
    const SandeshHeader &header(vmsgp->msg->GetHeader());
    const std::string &message_type(vmsgp->msg->GetMessageType());
    uint64_t temp_u64;
    uint32_t temp_u32;
    std::string temp_str;

    int ttl = GetTtl(GLOBAL_TTL);
    std::auto_ptr<GenDb::ColList> col_list(new GenDb::ColList);
    col_list->cfname_ = g_viz_constants.COLLECTOR_GLOBAL_TABLE;
    // Rowkey
    GenDb::DbDataValueVec& rowkey = col_list->rowkey_;
    rowkey.reserve(1);
    rowkey.push_back(vmsgp->unm);
    // Columns
    GenDb::NewColVec& columns = col_list->columns_;
    columns.reserve(16);
    columns.push_back(new GenDb::NewCol(g_viz_constants.SOURCE,
        header.get_Source(), ttl));
    columns.push_back(new GenDb::NewCol(g_viz_constants.NAMESPACE,
        header.get_Namespace(), ttl));
    columns.push_back(new GenDb::NewCol(g_viz_constants.MODULE,
        header.get_Module(), ttl));
    if (!header.get_Context().empty()) {
        columns.push_back(new GenDb::NewCol(g_viz_constants.CONTEXT,
            header.get_Context(), ttl));
    }
    if (!header.get_InstanceId().empty()) {
        columns.push_back(new GenDb::NewCol(g_viz_constants.INSTANCE_ID, 
                                            header.get_InstanceId(), ttl));
    }
    if (!header.get_NodeType().empty()) {
        columns.push_back(new GenDb::NewCol(g_viz_constants.NODE_TYPE,
                                            header.get_NodeType(), ttl));
    }
    if (header.__isset.IPAddress) {
        columns.push_back(new GenDb::NewCol(g_viz_constants.IPADDRESS,
                                            header.get_IPAddress(), ttl));
    }
    // Convert to network byte order
    temp_u64 = header.get_Timestamp();
    columns.push_back(new GenDb::NewCol(g_viz_constants.TIMESTAMP, temp_u64, ttl));

    columns.push_back(new GenDb::NewCol(g_viz_constants.CATEGORY,
        header.get_Category(), ttl));

    temp_u32 = header.get_Level();
    columns.push_back(new GenDb::NewCol(g_viz_constants.LEVEL, temp_u32, ttl));

    columns.push_back(new GenDb::NewCol(g_viz_constants.MESSAGE_TYPE,
        message_type, ttl));

    temp_u32 = header.get_SequenceNum();
    columns.push_back(new GenDb::NewCol(g_viz_constants.SEQUENCE_NUM,
        temp_u32, ttl));

    temp_u32 = header.get_VersionSig();
    columns.push_back(new GenDb::NewCol(g_viz_constants.VERSION, temp_u32, ttl));

    uint8_t temp_u8 = header.get_Type();
    columns.push_back(new GenDb::NewCol(g_viz_constants.SANDESH_TYPE,
        temp_u8, ttl));
    if (header.__isset.Pid) {
        temp_u32 = header.get_Pid();
        columns.push_back(new GenDb::NewCol(g_viz_constants.PID,
                                        temp_u32, ttl));
    }

    columns.push_back(new GenDb::NewCol(g_viz_constants.DATA,
        vmsgp->msg->ExtractMessage(), ttl));

    if (!dbif_->Db_AddColumn(col_list)) {
        DB_LOG(ERROR, "Addition of message: " << message_type <<
                ", message UUID: " << vmsgp->unm << " COLUMN FAILED");
        return;
    }
}

void DbHandler::MessageTableInsert(const VizMsg *vmsgp) {
    const SandeshHeader &header(vmsgp->msg->GetHeader());
    const std::string &message_type(vmsgp->msg->GetMessageType());

    if (!AllowMessageTableInsert(header))
        return;

    MessageTableOnlyInsert(vmsgp);

    MessageIndexTableInsert(g_viz_constants.MESSAGE_TABLE_SOURCE, header,
            message_type, vmsgp->unm, "");
    MessageIndexTableInsert(g_viz_constants.MESSAGE_TABLE_MODULE_ID, header,
            message_type, vmsgp->unm, "");
    MessageIndexTableInsert(g_viz_constants.MESSAGE_TABLE_CATEGORY, header,
            message_type, vmsgp->unm, "");
    MessageIndexTableInsert(g_viz_constants.MESSAGE_TABLE_MESSAGE_TYPE, header,
            message_type, vmsgp->unm, "");
    MessageIndexTableInsert(g_viz_constants.MESSAGE_TABLE_TIMESTAMP, header,
            message_type, vmsgp->unm, "");

    const SandeshType::type &stype(header.get_Type());
    std::string s;

    if (stype == SandeshType::SYSTEM) {
        const SandeshXMLMessage *sxmsg =
            static_cast<const SandeshXMLMessage *>(vmsgp->msg);
        const pugi::xml_node &parent(sxmsg->GetMessageNode());
        s = LineParser::GetXmlString(parent);
    } else if (!vmsgp->keyword_doc_.empty()) {
        s = std::string(vmsgp->keyword_doc_);
    }
    if (!s.empty()) {
        LineParser::WordListType words = LineParser::ParseDoc(s.begin(),
                s.end());
        LineParser::RemoveStopWords(&words);
        for (LineParser::WordListType::iterator i = words.begin();
                i != words.end(); i++) {
            // tableinsert@{(t2,*i), (t1,header.get_Source())} -> vmsgp->unm
            bool r = MessageIndexTableInsert(
                    g_viz_constants.MESSAGE_TABLE_KEYWORD, header,
                    message_type, vmsgp->unm, *i);
            if (!r)
                DB_LOG(ERROR, "Failed to parse:" << s);
        }
    }

    /*
     * Insert the message types,module_id in the stat table
     * Construct the atttributes,attrib_tags beofore inserting
     * to the StatTableInsert
     */
    if ((stype == SandeshType::SYSLOG) || (stype == SandeshType::SYSTEM)) {
        //Insert only if sandesh type is a SYSTEM LOG or SYSLOG
        //Insert into the FieldNames stats table entries for Messagetype and Module ID
        int ttl = GetTtl(GLOBAL_TTL);
        FieldNamesTableInsert(g_viz_constants.COLLECTOR_GLOBAL_TABLE,
            ":Messagetype", message_type, header.get_Timestamp(), ttl);
        FieldNamesTableInsert(g_viz_constants.COLLECTOR_GLOBAL_TABLE,
            ":ModuleId", header.get_Module(), header.get_Timestamp(), ttl);
    }
}

/*
 * This function takes field name and field value as arguments and inserts
 * into the FieldNames stats table
 */
void DbHandler::FieldNamesTableInsert(const std::string& table_prefix, 
    const std::string& field_name, const std::string& field_val, 
    uint64_t timestamp, int ttl) {
    /*
     * Insert the message types in the stat table
     * Construct the atttributes,attrib_tags before inserting
     * to the StatTableInsert
     */
    DbHandler::TagMap tmap;
    DbHandler::AttribMap amap;
    DbHandler::Var pv;
    DbHandler::AttribMap attribs;
    std::string table_name(table_prefix);
    table_name.append(field_name);
    pv = table_name;
    tmap.insert(make_pair("name", make_pair(pv, amap)));
    attribs.insert(make_pair(string("name"), pv));
    string sattrname("fields.value");
    pv = string(field_val);
    attribs.insert(make_pair(sattrname,pv));

    //pv = string(header.get_Source());
    // Put the name of the collector, not the message source.
    // Using the message source will make queries slower
    pv = string(col_name_);
    tmap.insert(make_pair("Source",make_pair(pv,amap))); 
    attribs.insert(make_pair(string("Source"),pv));

    StatTableInsertTtl(timestamp, "FieldNames","fields",tmap,attribs, ttl);

}

void DbHandler::GetRuleMap(RuleMap& rulemap) {
}

/*
 * insert an entry into an ObjectTrace table
 * key is T2
 * column is
 *  name: <key>:T1 (value in timestamp)
 *  value: uuid (of the corresponding global message)
 */
void DbHandler::ObjectTableInsert(const std::string &table, const std::string &objectkey_str,
        uint64_t &timestamp, const boost::uuids::uuid& unm, const VizMsg *vmsgp) {
    uint32_t T2(timestamp >> g_viz_constants.RowTimeInBits);
    uint32_t T1(timestamp & g_viz_constants.RowTimeInMask);

      {
        uint8_t partition_no = 0;
        std::auto_ptr<GenDb::ColList> col_list(new GenDb::ColList);
        col_list->cfname_ = g_viz_constants.OBJECT_TABLE;
        GenDb::DbDataValueVec& rowkey = col_list->rowkey_;
        rowkey.reserve(3);
        rowkey.push_back(T2);
        rowkey.push_back(partition_no);
        rowkey.push_back(table);
        
        GenDb::DbDataValueVec *col_name(new GenDb::DbDataValueVec());
        col_name->reserve(2);
        col_name->push_back(objectkey_str);
        col_name->push_back(T1);

        GenDb::DbDataValueVec *col_value(new GenDb::DbDataValueVec(1, unm));
        const std::string &message_type(vmsgp->msg->GetMessageType());
        int ttl;
        if (message_type == "VncApiConfigLog") {
            ttl = GetTtl(CONFIGAUDIT_TTL);
        } else {
            ttl = GetTtl(GLOBAL_TTL);
        }
        GenDb::NewCol *col(new GenDb::NewCol(col_name, col_value, ttl));
        GenDb::NewColVec& columns = col_list->columns_;
        columns.reserve(1);
        columns.push_back(col);
        if (!dbif_->Db_AddColumn(col_list)) {
            DB_LOG(ERROR, "Addition of " << objectkey_str <<
                    ", message UUID " << unm << " into table " << table <<
                    " FAILED");
            return;
        }
      }

      {
        std::auto_ptr<GenDb::ColList> col_list(new GenDb::ColList);
        col_list->cfname_ = g_viz_constants.OBJECT_VALUE_TABLE;
        GenDb::DbDataValueVec& rowkey = col_list->rowkey_;
        rowkey.reserve(2);
        rowkey.push_back(T2);
        rowkey.push_back(table);
        GenDb::DbDataValueVec *col_name(new GenDb::DbDataValueVec(1, T1));
        GenDb::DbDataValueVec *col_value(new GenDb::DbDataValueVec(1, objectkey_str));
        GenDb::NewCol *col(new GenDb::NewCol(col_name, col_value));
        GenDb::NewColVec& columns = col_list->columns_;
        columns.reserve(1);
        columns.push_back(col);
        if (!dbif_->Db_AddColumn(col_list)) {
            DB_LOG(ERROR, "Addition of " << objectkey_str <<
                    ", message UUID " << unm << " " << table << " into table "
                    << g_viz_constants.OBJECT_VALUE_TABLE << " FAILED");
            return;
        }

        /*
         * Inserting into the stat table
         */
        const SandeshHeader &header(vmsgp->msg->GetHeader());
        const std::string &message_type(vmsgp->msg->GetMessageType());
        //Insert into the FieldNames stats table entries for Messagetype and Module ID
        int ttl;
        if (message_type == "VncApiConfigLog") {
            ttl = GetTtl(CONFIGAUDIT_TTL);
        } else {
            ttl = GetTtl(GLOBAL_TTL);
        }
        FieldNamesTableInsert(table, ":Objecttype", objectkey_str, timestamp, ttl);
        FieldNamesTableInsert(table, ":Messagetype", message_type, timestamp, ttl);
        FieldNamesTableInsert(table, ":ModuleId", header.get_Module(),
            timestamp, ttl);
    }
}

bool DbHandler::StatTableWrite(uint32_t t2,
        const std::string& statName, const std::string& statAttr,
        const std::pair<std::string,DbHandler::Var>& ptag,
        const std::pair<std::string,DbHandler::Var>& stag,
        uint32_t t1, const boost::uuids::uuid& unm,
        const std::string& jsonline, int ttl) {

    uint8_t part = 0;
    string cfname;
    const DbHandler::Var& pv = ptag.second;
    const DbHandler::Var& sv = stag.second;
    GenDb::DbDataValue pg,sg;

    bool bad_suffix = false;
    switch (pv.type) {
        case DbHandler::STRING : {
                pg = pv.str;
                if (sv.type==DbHandler::STRING) {
                    cfname = g_viz_constants.STATS_TABLE_BY_STR_STR_TAG;
                    sg = sv.str;
                } else if (sv.type==DbHandler::UINT64) {
                    cfname = g_viz_constants.STATS_TABLE_BY_STR_U64_TAG;
                    sg = sv.num;
                } else if (sv.type==DbHandler::INVALID) {
                    cfname = g_viz_constants.STATS_TABLE_BY_STR_TAG;
                } else {
                    bad_suffix = true;
                }
            }
            break;
        case DbHandler::UINT64 : {
                pg = pv.num;
                if (sv.type==DbHandler::STRING) {
                    cfname = g_viz_constants.STATS_TABLE_BY_U64_STR_TAG;
                    sg = sv.str;
                } else if (sv.type==DbHandler::UINT64) {
                    cfname = g_viz_constants.STATS_TABLE_BY_U64_U64_TAG;
                    sg = sv.num;
                } else if (sv.type==DbHandler::INVALID) {
                    cfname = g_viz_constants.STATS_TABLE_BY_U64_TAG;
                } else {
                    bad_suffix = true;
                }
            }
            break;
        case DbHandler::DOUBLE : {
                pg = pv.dbl;
                if (sv.type==DbHandler::INVALID) {
                    cfname = g_viz_constants.STATS_TABLE_BY_DBL_TAG;
                } else {
                    bad_suffix = true;
                }
            }
            break;
        default:
            tbb::mutex::scoped_lock lock(smutex_);
            stable_stats_.Update(statName + ":" + statAttr, true, true);
            DB_LOG(ERROR, "Bad Prefix Tag " << statName <<
                    ", " << statAttr <<  " tag " << ptag.first <<
                    ":" << stag.first << " jsonline " << jsonline);
            return false;
    }
    if (bad_suffix) {
        tbb::mutex::scoped_lock lock(smutex_);
        stable_stats_.Update(statName + ":" + statAttr, true, true);
        DB_LOG(ERROR, "Bad Suffix Tag " << statName <<
                ", " << statAttr <<  " tag " << ptag.first <<
                ":" << stag.first << " jsonline " << jsonline);
        return false;
    }
    std::auto_ptr<GenDb::ColList> col_list(new GenDb::ColList);
    col_list->cfname_ = cfname;
    
    GenDb::DbDataValueVec& rowkey = col_list->rowkey_;
    if (sv.type==DbHandler::INVALID) {
        rowkey.reserve(5);
        rowkey.push_back(t2);
        rowkey.push_back(part);
        rowkey.push_back(statName);
        rowkey.push_back(statAttr);
        rowkey.push_back(ptag.first);
    } else {
        rowkey.reserve(6);
        rowkey.push_back(t2);
        rowkey.push_back(part);
        rowkey.push_back(statName);
        rowkey.push_back(statAttr);
        rowkey.push_back(ptag.first);
        rowkey.push_back(stag.first);
    }

    GenDb::NewColVec& columns = col_list->columns_;

    GenDb::DbDataValueVec *col_name(new GenDb::DbDataValueVec);
    if (sv.type==DbHandler::INVALID) {
        col_name->reserve(3);
        col_name->push_back(pg);
        col_name->push_back(t1);
        col_name->push_back(unm);
    } else {
        col_name->reserve(4);
        col_name->push_back(pg);
        col_name->push_back(sg);
        col_name->push_back(t1);
        col_name->push_back(unm);
    }

    GenDb::DbDataValueVec *col_value(new GenDb::DbDataValueVec(1, jsonline));
    GenDb::NewCol *col(new GenDb::NewCol(col_name, col_value, ttl));
    columns.push_back(col);

    if (!dbif_->Db_AddColumn(col_list)) {
        DB_LOG(ERROR, "Addition of " << statName <<
                ", " << statAttr <<  " tag " << ptag.first <<
                ":" << stag.first << " into table " <<
                cfname <<" FAILED");
        tbb::mutex::scoped_lock lock(smutex_);
        stable_stats_.Update(statName + ":" + statAttr, true, true);
        return false;
    } else {
        tbb::mutex::scoped_lock lock(smutex_);
        stable_stats_.Update(statName + ":" + statAttr, true, false);
        return true;
    }
}

// This returns a list of select terms to use for full aggregation
// for the given row
std::vector<std::string>
DbHandler::StatTableSelectStr(
        const std::string& statName, const std::string& statAttr,
        const AttribMap & attribs) {
    std::vector<std::string> aggstr;
    aggstr.push_back(string("COUNT(") + statAttr + string(")"));
    for (AttribMap::const_iterator it = attribs.begin();
            it != attribs.end(); it++) {
        switch (it->second.type) {
            case STRING: {
                    aggstr.push_back(statAttr + "." + it->first);
                }
                break;
            case UINT64: {
                    aggstr.push_back(string("SUM(") + statAttr + "." + it->first + string(")"));
                    aggstr.push_back(string("MAX(") + statAttr + "." + it->first + string(")"));
                    aggstr.push_back(string("MIN(") + statAttr + "." + it->first + string(")"));
                }
                break;
            case DOUBLE: {
                    aggstr.push_back(string("SUM(") + statAttr + "." + it->first + string(")"));
                    aggstr.push_back(string("MAX(") + statAttr + "." + it->first + string(")"));
                    aggstr.push_back(string("MIN(") + statAttr + "." + it->first + string(")"));
                }
                break;                
            default:
                continue;
        }
    }
    return aggstr;
}
void
DbHandler::StatTableInsert(uint64_t ts, 
        const std::string& statName,
        const std::string& statAttr,
        const TagMap & attribs_tag,
        const AttribMap & attribs) {
    int ttl = GetTtl(STATSDATA_TTL);
    StatTableInsertTtl(ts, statName, statAttr, attribs_tag, attribs, ttl);
}

// This function writes Stats samples to the DB.
void
DbHandler::StatTableInsertTtl(uint64_t ts, 
        const std::string& statName,
        const std::string& statAttr,
        const TagMap & attribs_tag,
        const AttribMap & attribs, int ttl) {

    uint64_t temp_u64 = ts;
    uint32_t temp_u32 = temp_u64 >> g_viz_constants.RowTimeInBits;
    boost::uuids::uuid unm;
    if (statName.compare("FieldNames") != 0) {
         unm = umn_gen_();
    }

    // This is very primitive JSON encoding.
    // Should replace with rapidJson at some point.

    // Encoding of all attribs

    rapidjson::Document dd;
    dd.SetObject();

    AttribMap attribs_buf;
    for (AttribMap::const_iterator it = attribs.begin();
            it != attribs.end(); it++) {
        switch (it->second.type) {
            case STRING: {
                    rapidjson::Value val(rapidjson::kStringType);
                    std::string nm = it->first + std::string("|s");
                    pair<AttribMap::iterator,bool> rt = 
                        attribs_buf.insert(make_pair(nm, it->second));
                    val.SetString(it->second.str.c_str());
                    dd.AddMember(rt.first->first.c_str(), val, dd.GetAllocator());
                    string field_name = it->first;
                     if (field_name.compare("fields.value") == 0) {
                         if (statName.compare("FieldNames") == 0) {
                             //Make uuid a fn of the field.values
                             boost::uuids::name_generator gen(DbHandler::seed_uuid);
                             unm = gen(it->second.str.c_str());
                         }
                     }
                }
                break;
            case UINT64: {
                    rapidjson::Value val(rapidjson::kNumberType);
                    std::string nm = it->first + std::string("|n");
                    pair<AttribMap::iterator,bool> rt = 
                        attribs_buf.insert(make_pair(nm, it->second));
                    val.SetUint64(it->second.num);
                    dd.AddMember(rt.first->first.c_str(), val, dd.GetAllocator());
                }
                break;
            case DOUBLE: {
                    rapidjson::Value val(rapidjson::kNumberType);
                    std::string nm = it->first + std::string("|d");
                    pair<AttribMap::iterator,bool> rt = 
                        attribs_buf.insert(make_pair(nm, it->second));
                    val.SetDouble(it->second.dbl);
                    dd.AddMember(rt.first->first.c_str(), val, dd.GetAllocator());
                }
                break;                
            default:
                continue;
        }
    }

    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> writer(sb);
    dd.Accept(writer);
    string jsonline(sb.GetString());

    uint32_t t1;
    if ( statName.compare("FieldNames") != 0) {
	t1 = (uint32_t)(temp_u64& g_viz_constants.RowTimeInMask);
    } else {
	t1 = 0;
    }

    for (TagMap::const_iterator it = attribs_tag.begin();
            it != attribs_tag.end(); it++) {

        pair<string,DbHandler::Var> ptag;
        ptag.first = it->first;
        ptag.second = it->second.first;
        if (it->second.second.empty()) {
            pair<string,DbHandler::Var> stag;
            StatTableWrite(temp_u32, statName, statAttr,
                                ptag, stag, t1, unm, jsonline, ttl);
        } else {
            for (AttribMap::const_iterator jt = it->second.second.begin();
                    jt != it->second.second.end(); jt++) {
                StatTableWrite(temp_u32, statName, statAttr,
                                    ptag, *jt, t1, unm, jsonline, ttl);
            }
        }

    }

}

typedef boost::array<GenDb::DbDataValue,
    FlowRecordFields::FLOWREC_MAX> FlowValueArray;

static const std::vector<FlowRecordFields::type> FlowRecordTableColumns =
    boost::assign::list_of
    (FlowRecordFields::FLOWREC_VROUTER)
    (FlowRecordFields::FLOWREC_DIRECTION_ING)
    (FlowRecordFields::FLOWREC_SOURCEVN)
    (FlowRecordFields::FLOWREC_SOURCEIP)
    (FlowRecordFields::FLOWREC_DESTVN)
    (FlowRecordFields::FLOWREC_DESTIP)
    (FlowRecordFields::FLOWREC_PROTOCOL)
    (FlowRecordFields::FLOWREC_SPORT)
    (FlowRecordFields::FLOWREC_DPORT)
    (FlowRecordFields::FLOWREC_TOS)
    (FlowRecordFields::FLOWREC_TCP_FLAGS)
    (FlowRecordFields::FLOWREC_VM)
    (FlowRecordFields::FLOWREC_INPUT_INTERFACE)
    (FlowRecordFields::FLOWREC_OUTPUT_INTERFACE)
    (FlowRecordFields::FLOWREC_MPLS_LABEL)
    (FlowRecordFields::FLOWREC_REVERSE_UUID)
    (FlowRecordFields::FLOWREC_SETUP_TIME)
    (FlowRecordFields::FLOWREC_TEARDOWN_TIME)
    (FlowRecordFields::FLOWREC_MIN_INTERARRIVAL)
    (FlowRecordFields::FLOWREC_MAX_INTERARRIVAL)
    (FlowRecordFields::FLOWREC_MEAN_INTERARRIVAL)
    (FlowRecordFields::FLOWREC_STDDEV_INTERARRIVAL)
    (FlowRecordFields::FLOWREC_BYTES)
    (FlowRecordFields::FLOWREC_PACKETS)
    (FlowRecordFields::FLOWREC_DATA_SAMPLE)
    (FlowRecordFields::FLOWREC_ACTION)
    (FlowRecordFields::FLOWREC_SG_RULE_UUID)
    (FlowRecordFields::FLOWREC_NW_ACE_UUID)
    (FlowRecordFields::FLOWREC_VROUTER_IP)
    (FlowRecordFields::FLOWREC_OTHER_VROUTER_IP)
    (FlowRecordFields::FLOWREC_UNDERLAY_PROTO)
    (FlowRecordFields::FLOWREC_UNDERLAY_SPORT);

boost::uuids::uuid DbHandler::seed_uuid = StringToUuid(std::string("ffffffff-ffff-ffff-ffff-ffffffffffff"));

static void PopulateFlowRecordTableColumns(
    const std::vector<FlowRecordFields::type> &frvt,
    FlowValueArray &fvalues, GenDb::NewColVec& columns, const DbHandler::TtlMap& ttl_map) {
    int ttl = DbHandler::GetTtlFromMap(ttl_map, DbHandler::FLOWDATA_TTL);
    columns.reserve(frvt.size());
    for (std::vector<FlowRecordFields::type>::const_iterator it = frvt.begin();
         it != frvt.end(); it++) {
        GenDb::DbDataValue &db_value(fvalues[(*it)]);
        if (db_value.which() != GenDb::DB_VALUE_BLANK) {
            GenDb::NewCol *col(new GenDb::NewCol(
                g_viz_constants.FlowRecordNames[(*it)], db_value, ttl));
            columns.push_back(col);
        }
    }
}

// FLOW_UUID
static void PopulateFlowRecordTableRowKey(
    FlowValueArray &fvalues, GenDb::DbDataValueVec &rkey) {
    rkey.reserve(1);
    GenDb::DbDataValue &flowu(fvalues[FlowRecordFields::FLOWREC_FLOWUUID]);
    assert(flowu.which() != GenDb::DB_VALUE_BLANK);
    rkey.push_back(flowu);
}

static bool PopulateFlowRecordTable(FlowValueArray &fvalues,
    GenDb::GenDbIf *dbif, const DbHandler::TtlMap& ttl_map) {
    std::auto_ptr<GenDb::ColList> colList(new GenDb::ColList);
    colList->cfname_ = g_viz_constants.FLOW_TABLE;
    PopulateFlowRecordTableRowKey(fvalues, colList->rowkey_);
    PopulateFlowRecordTableColumns(FlowRecordTableColumns, fvalues,
        colList->columns_, ttl_map);
    return dbif->Db_AddColumn(colList);
}

static const std::vector<FlowRecordFields::type> FlowIndexTableColumnValues =
    boost::assign::list_of
    (FlowRecordFields::FLOWREC_DIFF_BYTES)
    (FlowRecordFields::FLOWREC_DIFF_PACKETS)
    (FlowRecordFields::FLOWREC_SHORT_FLOW)
    (FlowRecordFields::FLOWREC_FLOWUUID)
    (FlowRecordFields::FLOWREC_VROUTER)
    (FlowRecordFields::FLOWREC_SOURCEVN)
    (FlowRecordFields::FLOWREC_DESTVN)
    (FlowRecordFields::FLOWREC_SOURCEIP)
    (FlowRecordFields::FLOWREC_DESTIP)
    (FlowRecordFields::FLOWREC_PROTOCOL)
    (FlowRecordFields::FLOWREC_SPORT)
    (FlowRecordFields::FLOWREC_DPORT)
    (FlowRecordFields::FLOWREC_JSON);

enum FlowIndexTableType {
    FLOW_INDEX_TABLE_MIN,
    FLOW_INDEX_TABLE_SVN_SIP = FLOW_INDEX_TABLE_MIN,
    FLOW_INDEX_TABLE_DVN_DIP,
    FLOW_INDEX_TABLE_PROTOCOL_SPORT,
    FLOW_INDEX_TABLE_PROTOCOL_DPORT,
    FLOW_INDEX_TABLE_VROUTER,
    FLOW_INDEX_TABLE_MAX_PLUS_1,
};

static const std::string& FlowIndexTable2String(FlowIndexTableType ttype) {
    switch (ttype) {
    case FLOW_INDEX_TABLE_SVN_SIP:
        return g_viz_constants.FLOW_TABLE_SVN_SIP;
    case FLOW_INDEX_TABLE_DVN_DIP:
        return g_viz_constants.FLOW_TABLE_DVN_DIP;
    case FLOW_INDEX_TABLE_PROTOCOL_SPORT:
        return g_viz_constants.FLOW_TABLE_PROT_SP;
    case FLOW_INDEX_TABLE_PROTOCOL_DPORT:
        return g_viz_constants.FLOW_TABLE_PROT_DP;
    case FLOW_INDEX_TABLE_VROUTER:
        return g_viz_constants.FLOW_TABLE_VROUTER;
    default:
        return g_viz_constants.FLOW_TABLE_INVALID;
    }
}
 
static void PopulateFlowIndexTableColumnValues(
    const std::vector<FlowRecordFields::type> &frvt,
    FlowValueArray &fvalues, GenDb::DbDataValueVec &cvalues) {
    cvalues.reserve(frvt.size());
    for (std::vector<FlowRecordFields::type>::const_iterator it = frvt.begin();
         it != frvt.end(); it++) {
        GenDb::DbDataValue &db_value(fvalues[(*it)]);
        if (db_value.which() != GenDb::DB_VALUE_BLANK) {
            cvalues.push_back(db_value);
        }
    }
}

// T2, Partition No, Direction
static void PopulateFlowIndexTableRowKey(
    FlowValueArray &fvalues, uint32_t &T2, uint8_t &partition_no,
    GenDb::DbDataValueVec &rkey) {
    rkey.reserve(3);
    rkey.push_back(T2);
    rkey.push_back(partition_no);
    rkey.push_back(fvalues[FlowRecordFields::FLOWREC_DIRECTION_ING]);
}

// SVN/DVN/Protocol, SIP/DIP/SPORT/DPORT, T1, FLOW_UUID
static void PopulateFlowIndexTableColumnNames(FlowIndexTableType ftype,
    FlowValueArray &fvalues, uint32_t &T1,
    GenDb::DbDataValueVec *cnames) {
    cnames->reserve(4);
    switch(ftype) {
    case FLOW_INDEX_TABLE_SVN_SIP:
        cnames->push_back(fvalues[FlowRecordFields::FLOWREC_SOURCEVN]);
        cnames->push_back(fvalues[FlowRecordFields::FLOWREC_SOURCEIP]);
        break;
    case FLOW_INDEX_TABLE_DVN_DIP:
        cnames->push_back(fvalues[FlowRecordFields::FLOWREC_DESTVN]);
        cnames->push_back(fvalues[FlowRecordFields::FLOWREC_DESTIP]);
        break;
    case FLOW_INDEX_TABLE_PROTOCOL_SPORT:
        cnames->push_back(fvalues[FlowRecordFields::FLOWREC_PROTOCOL]);
        cnames->push_back(fvalues[FlowRecordFields::FLOWREC_SPORT]);
        break;
    case FLOW_INDEX_TABLE_PROTOCOL_DPORT:
        cnames->push_back(fvalues[FlowRecordFields::FLOWREC_PROTOCOL]);
        cnames->push_back(fvalues[FlowRecordFields::FLOWREC_DPORT]);
        break;
    case FLOW_INDEX_TABLE_VROUTER:
        cnames->push_back(fvalues[FlowRecordFields::FLOWREC_VROUTER]);
        break;
    default:
        VIZD_ASSERT(0);
        break;
    }
    cnames->push_back(T1);
    cnames->push_back(fvalues[FlowRecordFields::FLOWREC_FLOWUUID]);
}

static void PopulateFlowIndexTableColumns(FlowIndexTableType ftype,
    FlowValueArray &fvalues, uint32_t &T1,
    GenDb::NewColVec &columns, const GenDb::DbDataValueVec &cvalues,
    const DbHandler::TtlMap& ttl_map) {
    int ttl = DbHandler::GetTtlFromMap(ttl_map, DbHandler::FLOWDATA_TTL);

    GenDb::DbDataValueVec *names(new GenDb::DbDataValueVec);
    PopulateFlowIndexTableColumnNames(ftype, fvalues, T1, names);
    GenDb::DbDataValueVec *values(new GenDb::DbDataValueVec(cvalues));
    GenDb::NewCol *col(new GenDb::NewCol(names, values, ttl));
    columns.reserve(1);
    columns.push_back(col);
}

static bool PopulateFlowIndexTables(FlowValueArray &fvalues, 
    uint32_t &T2, uint32_t &T1, uint8_t partition_no,
    GenDb::GenDbIf *dbif, const DbHandler::TtlMap& ttl_map) {
    // Populate row key and column values (same for all flow index
    // tables)
    GenDb::DbDataValueVec rkey;
    PopulateFlowIndexTableRowKey(fvalues, T2, partition_no, rkey);
    GenDb::DbDataValueVec cvalues;
    PopulateFlowIndexTableColumnValues(FlowIndexTableColumnValues, fvalues,
        cvalues);
    // Populate the Flow Index Tables
    for (int tid = FLOW_INDEX_TABLE_MIN;
         tid < FLOW_INDEX_TABLE_MAX_PLUS_1; ++tid) {
        FlowIndexTableType fitt(static_cast<FlowIndexTableType>(tid));
        std::auto_ptr<GenDb::ColList> colList(new GenDb::ColList);
        colList->cfname_ = FlowIndexTable2String(fitt);
        colList->rowkey_ = rkey;
        PopulateFlowIndexTableColumns(fitt, fvalues, T1, colList->columns_,
            cvalues, ttl_map);
        if (!dbif->Db_AddColumn(colList)) {
            LOG(ERROR, "Populating " << FlowIndexTable2String(fitt) <<
                " FAILED");
        }
    }
    return true;
}

template <typename T>
bool FlowDataIpv4ObjectWalker<T>::for_each(pugi::xml_node& node) {
    std::string col_name(node.name());
    FlowTypeMap::const_iterator it = flow_msg2type_map.find(col_name);
    if (it != flow_msg2type_map.end()) {
        // Extract the values and populate the value array
        const FlowTypeInfo &ftinfo(it->second);
        switch (ftinfo.get<1>()) {
        case GenDb::DbDataType::Unsigned8Type:
            {
                int8_t val;
                stringToInteger(node.child_value(), val);
                values_[ftinfo.get<0>()] = static_cast<uint8_t>(val);
                break;
            }
        case GenDb::DbDataType::Unsigned16Type:
            {
                int16_t val;
                stringToInteger(node.child_value(), val);
                values_[ftinfo.get<0>()] = static_cast<uint16_t>(val);
                break;
            }
        case GenDb::DbDataType::Unsigned32Type:
            {
                int32_t val;
                stringToInteger(node.child_value(), val);
                values_[ftinfo.get<0>()] = static_cast<uint32_t>(val);
                break;
            }
        case GenDb::DbDataType::Unsigned64Type:
            {
                int64_t val;
                stringToInteger(node.child_value(), val);
                values_[ftinfo.get<0>()] = static_cast<uint64_t>(val);
                break;
            }
        case GenDb::DbDataType::DoubleType:
            {
                double val;
                stringToInteger(node.child_value(), val);
                values_[ftinfo.get<0>()] = val;
                break;
            }
        case GenDb::DbDataType::LexicalUUIDType:
        case GenDb::DbDataType::TimeUUIDType:
            {
                std::stringstream ss;
                ss << node.child_value();
                boost::uuids::uuid u;
                ss >> u;
                if (ss.fail()) {
                    LOG(ERROR, "FlowRecordTable: " << col_name << ": (" << 
                        node.child_value() << ") INVALID");
                }     
                values_[ftinfo.get<0>()] = u;
                break;
            }
        case GenDb::DbDataType::AsciiType:
        case GenDb::DbDataType::UTF8Type:
            {
                std::string val = node.child_value();
                TXMLProtocol::unescapeXMLControlChars(val);
                values_[ftinfo.get<0>()] = val;
                break;
            }
        default:
            VIZD_ASSERT(0);
            break;
        }
    }
    return true;
}

/*
 * process the flow message and insert into appropriate tables
 */
bool DbHandler::FlowTableInsert(const pugi::xml_node &parent,
    const SandeshHeader& header) {
    // Traverse and populate the flow entry values
    FlowValueArray flow_entry_values;
    FlowDataIpv4ObjectWalker<FlowValueArray> flow_msg_walker(flow_entry_values);
    pugi::xml_node &mnode = const_cast<pugi::xml_node &>(parent);
    if (!mnode.traverse(flow_msg_walker)) {
        VIZD_ASSERT(0);
    }
    // Populate FLOWREC_VROUTER from SandeshHeader source
    flow_entry_values[FlowRecordFields::FLOWREC_VROUTER] = header.get_Source();
    // Populate FLOWREC_JSON to empty string
    flow_entry_values[FlowRecordFields::FLOWREC_JSON] = std::string();
    // Populate FLOWREC_SHORT_FLOW based on setup_time and teardown_time
    GenDb::DbDataValue &setup_time(
        flow_entry_values[FlowRecordFields::FLOWREC_SETUP_TIME]);
    GenDb::DbDataValue &teardown_time(
        flow_entry_values[FlowRecordFields::FLOWREC_TEARDOWN_TIME]);
    if (setup_time.which() != GenDb::DB_VALUE_BLANK &&
        teardown_time.which() != GenDb::DB_VALUE_BLANK) {
        flow_entry_values[FlowRecordFields::FLOWREC_SHORT_FLOW] = static_cast<uint8_t>(1);
    } else {
        flow_entry_values[FlowRecordFields::FLOWREC_SHORT_FLOW] = static_cast<uint8_t>(0);
    }
    // Calculate T1 and T2 values from timestamp
    uint64_t timestamp(header.get_Timestamp());
    uint32_t T2(timestamp >> g_viz_constants.RowTimeInBits);
    uint32_t T1(timestamp & g_viz_constants.RowTimeInMask);
    // Parittion no
    uint8_t partition_no = 0;
    // Populate Flow Record Table
    if (!PopulateFlowRecordTable(flow_entry_values, dbif_.get(), ttl_map_)) {
        DB_LOG(ERROR, "Populating FlowRecordTable FAILED");
    }
    // Populate Flow Index Tables only if FLOWREC_DIFF_BYTES and
    GenDb::DbDataValue &diff_bytes(
        flow_entry_values[FlowRecordFields::FLOWREC_DIFF_BYTES]);
    GenDb::DbDataValue &diff_packets(
        flow_entry_values[FlowRecordFields::FLOWREC_DIFF_PACKETS]);
    // FLOWREC_DIFF_PACKETS are present
    if (diff_bytes.which() != GenDb::DB_VALUE_BLANK &&
        diff_packets.which() != GenDb::DB_VALUE_BLANK) {
       if (!PopulateFlowIndexTables(flow_entry_values, T2, T1, partition_no,
                dbif_.get(), ttl_map_)) {
           DB_LOG(ERROR, "Populating FlowIndexTables FAILED");
       }
    }
    return true;
}

bool DbHandler::UnderlayFlowSampleInsert(const UFlowData& flow_data,
                                         uint64_t timestamp) {
    const std::vector<UFlowSample>& flow = flow_data.get_flow();
    for (std::vector<UFlowSample>::const_iterator it = flow.begin();
         it != flow.end(); ++it) {
        // Add all attributes
        DbHandler::AttribMap amap;
        DbHandler::Var name(flow_data.get_name());
        amap.insert(std::make_pair("name", name));
        DbHandler::Var pifindex = it->get_pifindex();
        amap.insert(std::make_pair("flow.pifindex", pifindex));
        DbHandler::Var sip = it->get_sip();
        amap.insert(std::make_pair("flow.sip", sip));
        DbHandler::Var dip = it->get_dip();
        amap.insert(std::make_pair("flow.dip", dip));
        DbHandler::Var sport = static_cast<uint64_t>(it->get_sport());
        amap.insert(std::make_pair("flow.sport", sport));
        DbHandler::Var dport = static_cast<uint64_t>(it->get_dport());
        amap.insert(std::make_pair("flow.dport", dport));
        DbHandler::Var protocol = static_cast<uint64_t>(it->get_protocol());
        amap.insert(std::make_pair("flow.protocol", protocol));
        DbHandler::Var ft = it->get_flowtype();
        amap.insert(std::make_pair("flow.flowtype", ft));
        
        DbHandler::TagMap tmap;
        // Add tag -> name:.pifindex
        DbHandler::AttribMap amap_name_pifindex;
        amap_name_pifindex.insert(std::make_pair("flow.pifindex", pifindex));
        tmap.insert(std::make_pair("name", std::make_pair(name,
                amap_name_pifindex)));
        // Add tag -> .sip
        DbHandler::AttribMap amap_sip;
        tmap.insert(std::make_pair("flow.sip", std::make_pair(sip, amap_sip)));
        // Add tag -> .dip
        DbHandler::AttribMap amap_dip;
        tmap.insert(std::make_pair("flow.dip", std::make_pair(dip, amap_dip)));
        // Add tag -> .protocol:.sport
        DbHandler::AttribMap amap_protocol_sport;
        amap_protocol_sport.insert(std::make_pair("flow.sport", sport));
        tmap.insert(std::make_pair("flow.protocol",
                std::make_pair(protocol, amap_protocol_sport)));
        // Add tag -> .protocol:.dport
        DbHandler::AttribMap amap_protocol_dport;
        amap_protocol_dport.insert(std::make_pair("flow.dport", dport));
        tmap.insert(std::make_pair("flow.protocol",
                std::make_pair(protocol, amap_protocol_dport)));
        StatTableInsert(timestamp, "UFlowData", "flow", tmap, amap);
    }
    return true;
}

DbHandlerInitializer::DbHandlerInitializer(EventManager *evm,
    const std::string &db_name, int db_task_instance,
    const std::string &timer_task_name,
    DbHandlerInitializer::InitializeDoneCb callback,
    const std::vector<std::string> &cassandra_ips,
    const std::vector<int> &cassandra_ports, const DbHandler::TtlMap& ttl_map,
    const std::string& cassandra_user, const std::string& cassandra_password) :
    db_name_(db_name),
    db_task_instance_(db_task_instance),
    db_handler_(new DbHandler(evm,
        boost::bind(&DbHandlerInitializer::ScheduleInit, this),
        cassandra_ips, cassandra_ports, db_name, ttl_map,
        cassandra_user, cassandra_password)),
    callback_(callback),
    db_init_timer_(TimerManager::CreateTimer(*evm->io_service(),
        db_name + " Db Init Timer",
        TaskScheduler::GetInstance()->GetTaskId(timer_task_name))) {
}

DbHandlerInitializer::DbHandlerInitializer(EventManager *evm,
    const std::string &db_name, int db_task_instance,
    const std::string &timer_task_name,
    DbHandlerInitializer::InitializeDoneCb callback, DbHandler *db_handler) :
    db_name_(db_name),
    db_task_instance_(db_task_instance),
    db_handler_(db_handler),
    callback_(callback),
    db_init_timer_(TimerManager::CreateTimer(*evm->io_service(),
        db_name + " Db Init Timer",
        TaskScheduler::GetInstance()->GetTaskId(timer_task_name))) {
}

DbHandlerInitializer::~DbHandlerInitializer() {
}

bool DbHandlerInitializer::Initialize() {
    boost::system::error_code ec;
    if (!db_handler_->Init(true, db_task_instance_)) {
        // Update connection info
        boost::asio::ip::address db_addr(boost::asio::ip::address::from_string(
            db_handler_->GetHost(), ec));
        boost::asio::ip::tcp::endpoint db_endpoint(db_addr, db_handler_->GetPort());
        ConnectionState::GetInstance()->Update(ConnectionType::DATABASE,
            db_name_, ConnectionStatus::DOWN, db_endpoint, std::string());
        LOG(DEBUG, db_name_ << ": Db Initialization FAILED");
        ScheduleInit();
        return false;
    }
    // Update connection info
    boost::asio::ip::address db_addr(boost::asio::ip::address::from_string(
        db_handler_->GetHost(), ec));
    boost::asio::ip::tcp::endpoint db_endpoint(db_addr, db_handler_->GetPort());
    ConnectionState::GetInstance()->Update(ConnectionType::DATABASE,
        db_name_, ConnectionStatus::UP, db_endpoint, std::string());

    if (callback_) {
       callback_();
    }

    LOG(DEBUG, db_name_ << ": Db Initialization DONE");
    return true;
}

DbHandler* DbHandlerInitializer::GetDbHandler() const {
    return db_handler_.get();
}

void DbHandlerInitializer::Shutdown() {
    TimerManager::DeleteTimer(db_init_timer_);
    db_init_timer_ = NULL;
    db_handler_->UnInit(true);
}

bool DbHandlerInitializer::InitTimerExpired() {
    // Start the timer again if initialization is not done
    bool done = Initialize();
    return !done;
}

void DbHandlerInitializer::InitTimerErrorHandler(string error_name,
    string error_message) {
    LOG(ERROR, db_name_ << ": " << error_name << " " << error_message);
}

void DbHandlerInitializer::StartInitTimer() {
    db_init_timer_->Start(kInitRetryInterval,
        boost::bind(&DbHandlerInitializer::InitTimerExpired, this),
        boost::bind(&DbHandlerInitializer::InitTimerErrorHandler, this,
                    _1, _2));
}

void DbHandlerInitializer::ScheduleInit() {
    db_handler_->UnInitUnlocked(db_task_instance_);
    StartInitTimer();
}
