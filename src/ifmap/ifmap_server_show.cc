/*
 * Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
 */

#include <sandesh/sandesh_types.h>
#include <sandesh/sandesh.h>
#include <sandesh/request_pipeline.h>

#include <boost/bind.hpp>
#include <boost/assign/list_of.hpp>
#include "base/logging.h"
#include "db/db.h"
#include "db/db_graph.h"
#include "db/db_graph_vertex.h"
#include "db/db_table_partition.h"
#include "base/bitset.h"

#include "ifmap/ifmap_client.h"
#include "ifmap/ifmap_exporter.h"
#include "ifmap/ifmap_link.h"
#include "ifmap/ifmap_link_table.h"
#include "ifmap/ifmap_log.h"
#include "ifmap/ifmap_node.h"
#include "ifmap/ifmap_object.h"
#include "ifmap/ifmap_origin.h"
#include "ifmap/ifmap_sandesh_context.h"
#include "ifmap/ifmap_server.h"
#include "ifmap/ifmap_server_show_types.h"
#include "ifmap/ifmap_server_show_internal_types.h"
#include "ifmap/ifmap_log_types.h"
#include "ifmap/ifmap_table.h"
#include "ifmap/ifmap_update.h"
#include "ifmap/ifmap_uuid_mapper.h"

#include <pugixml/pugixml.hpp>

using namespace boost::assign;
using namespace std;
using namespace pugi;

class IFMapNodeCopier {
public:
    IFMapNodeCopier(IFMapNodeShowInfo *dest, DBEntryBase *src,
                    IFMapServer *server) {
        // Get the name of the node
        IFMapNode *src_node = static_cast<IFMapNode *>(src);
        dest->node_name = src_node->ToString();

        IFMapNodeState *state = server->exporter()->NodeStateLookup(src_node);

        if (state) {
            // Get the interests and advertised from state
            dest->interests = state->interest().ToNumberedString();
            dest->advertised = state->advertised().ToNumberedString();
        } else {
            dest->dbentryflags.append("No state, ");
        }

        if (src_node->IsDeleted()) {
            dest->dbentryflags.append("Deleted, ");
        }
        if (src_node->is_onlist()) {
            dest->dbentryflags.append("OnList, ");
        }
        if (src_node->IsOnRemoveQ()) {
            dest->dbentryflags.append("OnRemoveQ");
        }

        dest->obj_info.reserve(src_node->list_.size());
        for (IFMapNode::ObjectList::const_iterator iter = 
             src_node->list_.begin(); iter != src_node->list_.end(); ++iter) {
            const IFMapObject *src_obj = iter.operator->();

            IFMapObjectShowInfo dest_obj;
            dest_obj.sequence_number = src_obj->sequence_number();
            dest_obj.origin = src_obj->origin().ToString();
            dest_obj.data = GetIFMapObjectData(src_obj);

            dest->obj_info.push_back(dest_obj);
        }
        if (src_node->IsVertexValid()) {
            DBGraph *graph = server->graph();
            int neighbor_count = 0;
            for (DBGraphVertex::adjacency_iterator iter =
                src_node->begin(graph); iter != src_node->end(graph); ++iter) {
                ++neighbor_count;
            }
            dest->neighbors.reserve(neighbor_count);
            for (DBGraphVertex::adjacency_iterator iter =
                src_node->begin(graph); iter != src_node->end(graph); ++iter) {
                IFMapNode *adj = static_cast<IFMapNode *>(iter.operator->());
                dest->neighbors.push_back(adj->ToString());
            }
        }
        dest->last_modified = src_node->last_change_at_str();
    }

    string GetIFMapObjectData(const IFMapObject *src_obj) {
        pugi::xml_document xdoc;
        pugi::xml_node xnode = xdoc.append_child("iq");
        src_obj->EncodeUpdate(&xnode);

        ostringstream oss;
        xnode.print(oss);
        string objdata = oss.str();
        return objdata;
    }
};

const string kShowIterSeparator = "||";

// almost everything in this class is static since we dont really want to
// intantiate this class
class ShowIFMapTable {
public:
    static const uint32_t kMaxElementsPerRound = 50;

    struct ShowData : public RequestPipeline::InstData {
        vector<IFMapNodeShowInfo> send_buffer;
        string next_table_name;
        string last_node_name;
    };

    static RequestPipeline::InstData *AllocBuffer(int stage) {
        return static_cast<RequestPipeline::InstData *>(new ShowData);
    }

    static bool BufferStageCommon(const IFMapTableShowReq *request,
                                  RequestPipeline::InstData *data,
                                  const string &next_table_name,
                                  const string &last_node_name);
    static bool BufferStage(const Sandesh *sr,
                            const RequestPipeline::PipeSpec ps, int stage,
                            int instNum, RequestPipeline::InstData *data);
    static bool BufferStageIterate(const Sandesh *sr,
                                   const RequestPipeline::PipeSpec ps, int stage,
                                   int instNum, RequestPipeline::InstData *data);
    static void SendStageCommon(const IFMapTableShowReq *request,
                                const RequestPipeline::PipeSpec ps,
                                IFMapTableShowResp *response);
    static bool SendStage(const Sandesh *sr, const RequestPipeline::PipeSpec ps,
                          int stage, int instNum,
                          RequestPipeline::InstData *data);
    static bool SendStageIterate(const Sandesh *sr,
                                 const RequestPipeline::PipeSpec ps, int stage,
                                 int instNum, RequestPipeline::InstData *data);
    static bool TableToBuffer(const IFMapTableShowReq *request,
                              IFMapTable *table, IFMapServer *server,
                              const string &last_node_name,
                              ShowData *show_data);
    static bool BufferAllTables(const IFMapTableShowReq *req,
                                RequestPipeline::InstData *data,
                                const string &next_table_name,
                                const string &last_node_name);
    static bool BufferOneTable(const IFMapTableShowReq *request,
                               RequestPipeline::InstData *data,
                               const string &last_node_name);
    static bool ConvertReqIterateToReq(
        const IFMapTableShowReqIterate *req_iterate,
        IFMapTableShowReq *req, string *next_table_name,
        string *last_node_name);
};

// Return true if the buffer is full.
bool ShowIFMapTable::TableToBuffer(const IFMapTableShowReq *request,
        IFMapTable *table, IFMapServer *server, const string &last_node_name,
        ShowData *show_data) {

    DBEntryBase *src = NULL;
    if (last_node_name.length()) {
        // If the last_node_name is set, it was the last node printed in the
        // previous round. Search for the node 'after' last_node_name and start
        // this round with it. If there is no next node, we are done with this
        // table.
        IFMapNode *last_node = table->FindNextNode(last_node_name);
        if (last_node) {
            src = last_node;
        } else {
            return false;
        }
    }

    bool buffer_full = false;
    string search_string = request->get_search_string();
    DBTablePartBase *partition = table->GetTablePartition(0);
    if (!src) {
        src = partition->GetFirst();
    }
    for (; src != NULL; src = partition->GetNext(src)) {
        IFMapNode *src_node = static_cast<IFMapNode *>(src);
        if (!search_string.empty() &&
            (src_node->ToString().find(search_string) == string::npos)) {
            continue;
        }
        IFMapNodeShowInfo dest;
        IFMapNodeCopier copyNode(&dest, src, server);
        show_data->send_buffer.push_back(dest);
        // If we have picked up enough nodes for this round...
        if (show_data->send_buffer.size() == kMaxElementsPerRound) {
            // Save the values needed for the next round. When we come
            // back, we will use the 'names' to lookup the elements since
            // the 'names' are the keys in the respective tables.
            show_data->next_table_name = table->name();
            show_data->last_node_name = src_node->name();
            buffer_full = true;
            break;
        }
    }

    return buffer_full;
}

bool ShowIFMapTable::BufferOneTable(const IFMapTableShowReq *request,
        RequestPipeline::InstData *data, const string &last_node_name) {
    IFMapSandeshContext *sctx = 
        static_cast<IFMapSandeshContext *>(request->module_context("IFMap"));

    IFMapTable *table = IFMapTable::FindTable(sctx->ifmap_server()->database(),
                                              request->get_table_name());
    if (table) {
        ShowData *show_data = static_cast<ShowData *>(data);
        show_data->send_buffer.reserve(kMaxElementsPerRound);
        TableToBuffer(request, table, sctx->ifmap_server(), last_node_name,
                      show_data);
    } else {
        IFMAP_WARN(IFMapTblNotFound, "Cant show/find table ",
                   request->get_table_name());
    }

    return true;
}

bool ShowIFMapTable::BufferAllTables(const IFMapTableShowReq *request,
        RequestPipeline::InstData *data, const string &next_table_name,
        const string &last_node_name) {
    IFMapSandeshContext *sctx = 
        static_cast<IFMapSandeshContext *>(request->module_context("IFMap"));
    string last_name = last_node_name;

    DB *db = sctx->ifmap_server()->database();
    DB::iterator iter;
    if (next_table_name.empty()) {
        iter = db->lower_bound("__ifmap__.");
    } else {
        iter = db->FindTableIter(next_table_name);
    }

    ShowData *show_data = static_cast<ShowData *>(data);
    show_data->send_buffer.reserve(kMaxElementsPerRound);
    for (; iter != db->end(); ++iter) {
        if (iter->first.find("__ifmap__.") != 0) {
            break;
        }
        IFMapTable *table = static_cast<IFMapTable *>(iter->second);
        bool buffer_full = TableToBuffer(request, table, sctx->ifmap_server(),
                                         last_name, show_data);
        if (buffer_full) {
            break;
        }
        // last_node_name is only relevant for the first iteration.
        last_name.clear();
    }

    return true;
}

// Format of node_info string:
// table_name||search_string||next_table_name||last_node_name
//      table_name/search_string: original input. Could be empty.
//      next_table_name: next table to lookup in
//      last_node_name: name of last node that was printed in the previous round
bool ShowIFMapTable::ConvertReqIterateToReq(
        const IFMapTableShowReqIterate *req_iterate,
        IFMapTableShowReq *req, string *next_table_name,
        string *last_node_name) {
    // First, set the context from the original request since we might return
    // due to parsing errors.
    req->set_context(req_iterate->context());

    string node_info = req_iterate->get_node_info();
    size_t sep_size = kShowIterSeparator.size();

    // table_name
    size_t pos1 = node_info.find(kShowIterSeparator);
    if (pos1 == string::npos) {
        return false;
    }
    string table_name = node_info.substr(0, pos1);

    // search_string
    size_t pos2 = node_info.find(kShowIterSeparator, (pos1 + sep_size));
    if (pos2 == string::npos) {
        return false;
    }
    string search_string = node_info.substr((pos1 + sep_size),
                                            pos2 - (pos1 + sep_size));

    // next_table_name
    size_t pos3 = node_info.find(kShowIterSeparator, (pos2 + sep_size));
    if (pos3 == string::npos) {
        return false;
    }
    *next_table_name = node_info.substr((pos2 + sep_size),
                                        pos3 - (pos2 + sep_size));

    // last_node_name
    *last_node_name = node_info.substr(pos3 + sep_size);

    // Fill up the fields of IFMapTableShowReq appropriately.
    req->set_table_name(table_name);
    req->set_search_string(search_string);
    return true;
}

bool ShowIFMapTable::BufferStageCommon(const IFMapTableShowReq *request,
        RequestPipeline::InstData *data, const string &next_table_name,
        const string &last_node_name) {
    // If table name has not been passed, print all tables
    if (request->get_table_name().length()) {
        return BufferOneTable(request, data, last_node_name);
    } else {
        return BufferAllTables(request, data, next_table_name, last_node_name);
    }
}

bool ShowIFMapTable::BufferStage(const Sandesh *sr,
                                 const RequestPipeline::PipeSpec ps,
                                 int stage, int instNum,
                                 RequestPipeline::InstData *data) {
    const IFMapTableShowReq *request = 
        static_cast<const IFMapTableShowReq *>(ps.snhRequest_.get());
    string next_table_name;
    string last_node_name;
    return BufferStageCommon(request, data, next_table_name, last_node_name);
}

bool ShowIFMapTable::BufferStageIterate(const Sandesh *sr,
                                        const RequestPipeline::PipeSpec ps,
                                        int stage, int instNum,
                                        RequestPipeline::InstData *data) {
    const IFMapTableShowReqIterate *request_iterate = 
        static_cast<const IFMapTableShowReqIterate *>(ps.snhRequest_.get());

    string next_table_name;
    string last_node_name;
    IFMapTableShowReq *request = new IFMapTableShowReq;
    bool success = ConvertReqIterateToReq(request_iterate, request,
                        &next_table_name, &last_node_name);
    if (success) {
        BufferStageCommon(request, data, next_table_name, last_node_name);
    }
    request->Release();
    return true;
}

void ShowIFMapTable::SendStageCommon(const IFMapTableShowReq *request,
                                     const RequestPipeline::PipeSpec ps,
                                     IFMapTableShowResp *response) {
    const RequestPipeline::StageData *prev_stage_data = ps.GetStageData(0);
    const ShowIFMapTable::ShowData &show_data =
        static_cast<const ShowIFMapTable::ShowData &> (prev_stage_data->at(0));

    vector<IFMapNodeShowInfo> dest_buffer;
    dest_buffer = show_data.send_buffer;

    // If we have filled the buffer, set next_batch with all the values we will
    // need in the next round.
    string next_batch;
    if (dest_buffer.size() == kMaxElementsPerRound) {
        next_batch = request->get_table_name() + kShowIterSeparator +
                     request->get_search_string() + kShowIterSeparator +
                     show_data.next_table_name + kShowIterSeparator +
                     show_data.last_node_name;
    }

    response->set_ifmap_db(dest_buffer);
    response->set_next_batch(next_batch);
}

bool ShowIFMapTable::SendStage(const Sandesh *sr,
                               const RequestPipeline::PipeSpec ps,
                               int stage, int instNum,
                               RequestPipeline::InstData *data) {
    const IFMapTableShowReq *request =
        static_cast<const IFMapTableShowReq *>(ps.snhRequest_.get());
    IFMapTableShowResp *response = new IFMapTableShowResp;
    SendStageCommon(request, ps, response);

    response->set_context(request->context());
    response->set_more(false);
    response->Response();
    return true;
}

bool ShowIFMapTable::SendStageIterate(const Sandesh *sr,
                                      const RequestPipeline::PipeSpec ps,
                                      int stage, int instNum,
                                      RequestPipeline::InstData *data) {
    const IFMapTableShowReqIterate *request_iterate =
        static_cast<const IFMapTableShowReqIterate *>(ps.snhRequest_.get());

    string next_table_name;
    string last_node_name;

    IFMapTableShowResp *response = new IFMapTableShowResp;
    IFMapTableShowReq *request = new IFMapTableShowReq;
    bool success = ConvertReqIterateToReq(request_iterate, request,
                        &next_table_name, &last_node_name);
    if (success) {
        SendStageCommon(request, ps, response);
    }

    response->set_context(request->context());
    response->set_more(false);
    response->Response();

    request->Release();
    return true;
}

void IFMapTableShowReq::HandleRequest() const {

    RequestPipeline::StageSpec s0, s1;
    TaskScheduler *scheduler = TaskScheduler::GetInstance();

    // 2 stages - first: gather/read, second: send

    s0.taskId_ = scheduler->GetTaskId("db::DBTable");
    s0.allocFn_ = ShowIFMapTable::AllocBuffer;
    s0.cbFn_ = ShowIFMapTable::BufferStage;
    s0.instances_.push_back(0);

    // control-node ifmap show command task
    s1.taskId_ = scheduler->GetTaskId("cn_ifmap::ShowCommand");
    s1.cbFn_ = ShowIFMapTable::SendStage;
    s1.instances_.push_back(0);

    RequestPipeline::PipeSpec ps(this);
    ps.stages_= list_of(s0)(s1);
    RequestPipeline rp(ps);
}

void IFMapTableShowReqIterate::HandleRequest() const {

    RequestPipeline::StageSpec s0, s1;
    TaskScheduler *scheduler = TaskScheduler::GetInstance();

    // 2 stages - first: gather/read, second: send

    s0.taskId_ = scheduler->GetTaskId("db::DBTable");
    s0.allocFn_ = ShowIFMapTable::AllocBuffer;
    s0.cbFn_ = ShowIFMapTable::BufferStageIterate;
    s0.instances_.push_back(0);

    // control-node ifmap show command task
    s1.taskId_ = scheduler->GetTaskId("cn_ifmap::ShowCommand");
    s1.cbFn_ = ShowIFMapTable::SendStageIterate;
    s1.instances_.push_back(0);

    RequestPipeline::PipeSpec ps(this);
    ps.stages_= list_of(s0)(s1);
    RequestPipeline rp(ps);
}

// Code to display link-table entries

class ShowIFMapLinkTable {
public:
    static const uint32_t kMaxElementsPerRound = 50;

    struct ShowData : public RequestPipeline::InstData {
        vector<IFMapLinkShowInfo> send_buffer;
        uint32_t table_size;
        string last_link_name;
    };

    static RequestPipeline::InstData *AllocBuffer(int stage) {
        return static_cast<RequestPipeline::InstData *>(new ShowData);
    }

    static bool SkipLink(DBEntryBase *src, const string &search_string);
    static void CopyNode(IFMapLinkShowInfo *dest, DBEntryBase *src,
                         IFMapServer *server);
    static bool BufferStageCommon(const IFMapLinkTableShowReq *request,
                                  RequestPipeline::InstData *data,
                                  const string &last_link_name);
    static bool BufferStage(const Sandesh *sr,
                            const RequestPipeline::PipeSpec ps, int stage,
                            int instNum, RequestPipeline::InstData *data);
    static bool BufferStageIterate(const Sandesh *sr,
                                   const RequestPipeline::PipeSpec ps,
                                   int stage, int instNum,
                                   RequestPipeline::InstData *data);
    static void SendStageCommon(const IFMapLinkTableShowReq *request,
                                const RequestPipeline::PipeSpec ps,
                                IFMapLinkTableShowResp *response);
    static bool SendStage(const Sandesh *sr, const RequestPipeline::PipeSpec ps,
                          int stage, int instNum,
                          RequestPipeline::InstData *data);
    static bool SendStageIterate(const Sandesh *sr,
                                 const RequestPipeline::PipeSpec ps, int stage,
                                 int instNum, RequestPipeline::InstData *data);
    static bool ConvertReqIterateToReq(
        const IFMapLinkTableShowReqIterate *req_iterate,
        IFMapLinkTableShowReq *req, string *last_link_name);
};

bool ShowIFMapLinkTable::SkipLink(DBEntryBase *src,
                                  const string &search_string) {
    IFMapLink *link = static_cast<IFMapLink *>(src);
    IFMapNode *left = link->left();
    IFMapNode *right = link->right();
    if (search_string.empty()) {
        return false;
    }
    // If we do not find the search string in the names of either of the 2 ends,
    // skip the link
    if ((!left || (left->ToString().find(search_string) == string::npos)) &&
        (!right || (right->ToString().find(search_string) == string::npos))) {
        return true;
    }
    return false;
}

void ShowIFMapLinkTable::CopyNode(IFMapLinkShowInfo *dest, DBEntryBase *src,
                                  IFMapServer *server) {
    IFMapLink *src_link = static_cast<IFMapLink *>(src);

    dest->metadata = src_link->metadata();
    if (src_link->left()) {
        dest->left = src_link->left()->ToString();
    }
    if (src_link->right()) {
        dest->right = src_link->right()->ToString();
    }

    // Get the interests and advertised from state
    IFMapLinkState *state = server->exporter()->LinkStateLookup(src_link);
    if (state) {
        dest->interests = state->interest().ToNumberedString();
        dest->advertised = state->advertised().ToNumberedString();
    } else {
        dest->dbentryflags.append("No state, ");
    }

    if (src_link->IsDeleted()) {
        dest->dbentryflags.append("Deleted, ");
    }
    if (src_link->is_onlist()) {
        dest->dbentryflags.append("OnList");
    }
    if (src_link->IsOnRemoveQ()) {
        dest->dbentryflags.append("OnRemoveQ");
    }
    dest->last_modified = src_link->last_change_at_str();

    for (std::vector<IFMapLink::LinkOriginInfo>::const_iterator iter = 
         src_link->origin_info_.begin(); iter != src_link->origin_info_.end();
         ++iter) {
        const IFMapLink::LinkOriginInfo *origin_info = iter.operator->();
        IFMapLinkOriginShowInfo dest_origin;
        dest_origin.sequence_number = origin_info->sequence_number;
        dest_origin.origin = origin_info->origin.ToString();
        dest->origins.push_back(dest_origin);
    }
}

// Format of link_info string:
// search_string||last_link_name
//      search_string: original input. Could be empty.
//      last_link_name: name of last link that was printed in the previous round
bool ShowIFMapLinkTable::ConvertReqIterateToReq(
        const IFMapLinkTableShowReqIterate *req_iterate,
        IFMapLinkTableShowReq *req, string *last_link_name) {
    // First, set the context from the original request since we might return
    // due to parsing errors.
    req->set_context(req_iterate->context());

    string link_info = req_iterate->get_link_info();
    size_t sep_size = kShowIterSeparator.size();

    // search_string
    size_t pos1 = link_info.find(kShowIterSeparator);
    if (pos1 == string::npos) {
        return false;
    }
    string search_string = link_info.substr(0, pos1);

    // last_link_name
    *last_link_name = link_info.substr(pos1 + sep_size);

    // Fill up the fields of IFMapLinkTableShowReq appropriately.
    req->set_search_string(search_string);
    return true;
}

bool ShowIFMapLinkTable::BufferStageCommon(const IFMapLinkTableShowReq *request,
                                           RequestPipeline::InstData *data,
                                           const string &last_link_name) {
    bool buffer_full = false;
    IFMapSandeshContext *sctx =
        static_cast<IFMapSandeshContext *>(request->module_context("IFMap"));

    IFMapLinkTable *table =  static_cast<IFMapLinkTable *>(
        sctx->ifmap_server()->database()->FindTable("__ifmap_metadata__.0"));
    if (table) {
        ShowData *show_data = static_cast<ShowData *>(data);
        show_data->send_buffer.reserve(kMaxElementsPerRound);
        show_data->table_size = table->Size();

        DBEntryBase *src = NULL;
        DBTablePartBase *partition = table->GetTablePartition(0);
        if (last_link_name.length()) {
            src = table->FindNextLink(last_link_name);
        } else {
            src = partition->GetFirst();
        }
        for (; src != NULL; src = partition->GetNext(src)) {
            IFMapLink *src_link = static_cast<IFMapLink *>(src);
            if (SkipLink(src, request->get_search_string())) {
                continue;
            }
            IFMapLinkShowInfo dest;
            CopyNode(&dest, src, sctx->ifmap_server());
            show_data->send_buffer.push_back(dest);
            // If we have picked up enough links for this round...
            if (show_data->send_buffer.size() == kMaxElementsPerRound) {
                show_data->last_link_name = src_link->link_name();
                buffer_full = true;
                break;
            }
        }
    } else {
        IFMAP_WARN(IFMapTblNotFound, "Cant show/find ", "link table");
    }

    return buffer_full;
}

bool ShowIFMapLinkTable::BufferStage(const Sandesh *sr,
                                     const RequestPipeline::PipeSpec ps,
                                     int stage, int instNum,
                                     RequestPipeline::InstData *data) {
    const IFMapLinkTableShowReq *request =
        static_cast<const IFMapLinkTableShowReq *>(ps.snhRequest_.get());
    string last_link_name;
    BufferStageCommon(request, data, last_link_name);
    return true;
}

bool ShowIFMapLinkTable::BufferStageIterate(const Sandesh *sr,
                                            const RequestPipeline::PipeSpec ps,
                                            int stage, int instNum,
                                            RequestPipeline::InstData *data) {
    const IFMapLinkTableShowReqIterate *request_iterate =
        static_cast<const IFMapLinkTableShowReqIterate *>(ps.snhRequest_.get());

    IFMapLinkTableShowReq *request = new IFMapLinkTableShowReq;
    string last_link_name;
    bool success = ConvertReqIterateToReq(request_iterate, request,
                                          &last_link_name);
    if (success) {
        BufferStageCommon(request, data, last_link_name);
    }
    request->Release();
    return true;
}

void ShowIFMapLinkTable::SendStageCommon(const IFMapLinkTableShowReq *request,
                                         const RequestPipeline::PipeSpec ps,
                                         IFMapLinkTableShowResp *response) {
    const RequestPipeline::StageData *prev_stage_data = ps.GetStageData(0);
    const ShowIFMapLinkTable::ShowData &show_data = 
        static_cast<const ShowIFMapLinkTable::ShowData &>
                                                       (prev_stage_data->at(0));

    vector<IFMapLinkShowInfo> dest_buffer;
    dest_buffer = show_data.send_buffer;

    // If we have filled the buffer, set next_batch with all the values we will
    // need in the next round.
    string next_batch;
    if (dest_buffer.size() == kMaxElementsPerRound) {
        next_batch = request->get_search_string() + kShowIterSeparator +
                     show_data.last_link_name;
    }

    response->set_table_size(show_data.table_size);
    response->set_ifmap_db(dest_buffer);
    response->set_next_batch(next_batch);
}

bool ShowIFMapLinkTable::SendStage(const Sandesh *sr,
                                   const RequestPipeline::PipeSpec ps,
                                   int stage, int instNum,
                                   RequestPipeline::InstData *data) {
    const IFMapLinkTableShowReq *request = 
        static_cast<const IFMapLinkTableShowReq *>(ps.snhRequest_.get());
    IFMapLinkTableShowResp *response = new IFMapLinkTableShowResp;
    SendStageCommon(request, ps, response);

    response->set_context(request->context());
    response->set_more(false);
    response->Response();
    return true;
}

bool ShowIFMapLinkTable::SendStageIterate(const Sandesh *sr,
                                          const RequestPipeline::PipeSpec ps,
                                          int stage, int instNum,
                                          RequestPipeline::InstData *data) {
    const IFMapLinkTableShowReqIterate *request_iterate =
        static_cast<const IFMapLinkTableShowReqIterate *>(ps.snhRequest_.get());

    IFMapLinkTableShowResp *response = new IFMapLinkTableShowResp;
    IFMapLinkTableShowReq *request = new IFMapLinkTableShowReq;
    string last_link_name;
    bool success = ConvertReqIterateToReq(request_iterate, request,
                                          &last_link_name);
    if (success) {
        SendStageCommon(request, ps, response);
    }

    response->set_context(request->context());
    response->set_more(false);
    response->Response();

    request->Release();
    return true;
}

void IFMapLinkTableShowReq::HandleRequest() const {

    RequestPipeline::StageSpec s0, s1;
    TaskScheduler *scheduler = TaskScheduler::GetInstance();

    // 2 stages - first: gather/read, second: send

    s0.taskId_ = scheduler->GetTaskId("db::DBTable");
    s0.allocFn_ = ShowIFMapLinkTable::AllocBuffer;
    s0.cbFn_ = ShowIFMapLinkTable::BufferStage;
    s0.instances_.push_back(0);

    // control-node ifmap show command task
    s1.taskId_ = scheduler->GetTaskId("cn_ifmap::ShowCommand");
    s1.cbFn_ = ShowIFMapLinkTable::SendStage;
    s1.instances_.push_back(0);

    RequestPipeline::PipeSpec ps(this);
    ps.stages_= list_of(s0)(s1);
    RequestPipeline rp(ps);
}

void IFMapLinkTableShowReqIterate::HandleRequest() const {

    RequestPipeline::StageSpec s0, s1;
    TaskScheduler *scheduler = TaskScheduler::GetInstance();

    // 2 stages - first: gather/read, second: send

    s0.taskId_ = scheduler->GetTaskId("db::DBTable");
    s0.allocFn_ = ShowIFMapLinkTable::AllocBuffer;
    s0.cbFn_ = ShowIFMapLinkTable::BufferStageIterate;
    s0.instances_.push_back(0);

    // control-node ifmap show command task
    s1.taskId_ = scheduler->GetTaskId("cn_ifmap::ShowCommand");
    s1.cbFn_ = ShowIFMapLinkTable::SendStageIterate;
    s1.instances_.push_back(0);

    RequestPipeline::PipeSpec ps(this);
    ps.stages_= list_of(s0)(s1);
    RequestPipeline rp(ps);
}

static bool IFMapNodeShowReqHandleRequest(const Sandesh *sr,
                                          const RequestPipeline::PipeSpec ps,
                                          int stage, int instNum,
                                          RequestPipeline::InstData *data) {
    const IFMapNodeShowReq *request =
        static_cast<const IFMapNodeShowReq *>(ps.snhRequest_.get());
    IFMapSandeshContext *sctx = 
        static_cast<IFMapSandeshContext *>(request->module_context("IFMap"));

    IFMapNodeShowResp *response = new IFMapNodeShowResp();

    string fq_node_name = request->get_fq_node_name();
    // EG: "virtual-network:my:virtual:network" i.e. type:name
    size_t type_length = fq_node_name.find(":");
    if (type_length != string::npos) {
        string node_type = fq_node_name.substr(0, type_length);
        // +1 to go to the next character after ':'
        string node_name = fq_node_name.substr(type_length + 1);

        DB *db = sctx->ifmap_server()->database();
        IFMapTable *table = IFMapTable::FindTable(db, node_type);
        if (table) {
            IFMapNode *src = table->FindNode(node_name);
            if (src) {
                IFMapNodeShowInfo dest;
                IFMapNodeCopier copyNode(&dest, src, sctx->ifmap_server());
                response->set_node_info(dest);
            } else {
                IFMAP_WARN(IFMapIdentifierNotFound, "Cant find identifier",
                           node_name);
            }
        } else {
            IFMAP_WARN(IFMapTblNotFound, "Cant show/find table with node-type",
                       node_type);
        }
    }

    response->set_context(request->context());
    response->set_more(false);
    response->Response();

    // Return 'true' so that we are not called again
    return true;
}

void IFMapNodeShowReq::HandleRequest() const {

    RequestPipeline::StageSpec s0;
    TaskScheduler *scheduler = TaskScheduler::GetInstance();

    s0.taskId_ = scheduler->GetTaskId("db::DBTable");
    s0.cbFn_ = IFMapNodeShowReqHandleRequest;
    s0.instances_.push_back(0);

    RequestPipeline::PipeSpec ps(this);
    ps.stages_= list_of(s0);
    RequestPipeline rp(ps);
}

class ShowIFMapPerClientNodes {
public:
    static const int kMaxElementsPerRound = 50;

    struct ShowData : public RequestPipeline::InstData {
        vector<IFMapPerClientNodesShowInfo> send_buffer;
    };

    static RequestPipeline::InstData *AllocBuffer(int stage) {
        return static_cast<RequestPipeline::InstData *>(new ShowData);
    }

    struct TrackerData : public RequestPipeline::InstData {
        // init as 1 indicates we need to init 'first' to begin() since there
        // is no way to initialize an iterator here.
        TrackerData() : init(1) { }
        int init;
        vector<IFMapPerClientNodesShowInfo>::const_iterator first;
    };

    static RequestPipeline::InstData *AllocTracker(int stage) {
        return static_cast<RequestPipeline::InstData *>(new TrackerData);
    }

    static bool CopyNode(IFMapPerClientNodesShowInfo *dest, IFMapNode *src,
                         IFMapServer *server, int client_index);
    static bool BufferStage(const Sandesh *sr,
                            const RequestPipeline::PipeSpec ps, int stage,
                            int instNum, RequestPipeline::InstData *data);
    static bool SendStage(const Sandesh *sr, const RequestPipeline::PipeSpec ps,
                          int stage, int instNum,
                          RequestPipeline::InstData *data);
};

bool ShowIFMapPerClientNodes::CopyNode(IFMapPerClientNodesShowInfo *dest,
                                       IFMapNode *src, IFMapServer *server,
                                       int client_index) {
    IFMapNodeState *state = server->exporter()->NodeStateLookup(src);

    if (state && state->interest().test(client_index)) {
        dest->node_name = src->ToString();
        if (state->advertised().test(client_index)) {
            dest->sent = "Yes";
        } else {
            dest->sent = "No";
        }
        if (server->exporter()->ClientHasConfigTracker(client_index)) {
            if (server->exporter()->ClientConfigTrackerHasState(client_index,
                                                                state)) {
                dest->tracked = "Yes";
            } else {
                dest->tracked = "No";
            }
        } else {
            dest->tracked = "No tracker";
        }
        return true;
    } else {
        return false;
    }
}

bool ShowIFMapPerClientNodes::BufferStage(const Sandesh *sr,
        const RequestPipeline::PipeSpec ps, int stage,
        int instNum, RequestPipeline::InstData *data) {
    const IFMapPerClientNodesShowReq *request =
        static_cast<const IFMapPerClientNodesShowReq *>(ps.snhRequest_.get());
    IFMapSandeshContext *sctx =
        static_cast<IFMapSandeshContext *>(request->module_context("IFMap"));
    IFMapServer *ifmap_server = sctx->ifmap_server();

    string client_index_or_name = request->get_client_index_or_name();
    if (client_index_or_name.empty()) {
        return true;
    }

    // The user gives us either a name or an index. If the input is not a
    // number, find the client's index using its name. If we cant find it,
    // we cant process this request. If we have the index, continue processing.
    int client_index;
    if (!stringToInteger(client_index_or_name, client_index)) {
        if (!ifmap_server->ClientNameToIndex(client_index_or_name,
                                             &client_index)) {
            return true;
        }
    }

    string search_string = request->get_search_string();
    ShowData *show_data = static_cast<ShowData *>(data);
    DB *db = ifmap_server->database();
    for (DB::iterator iter = db->lower_bound("__ifmap__.");
         iter != db->end(); ++iter) {
        if (iter->first.find("__ifmap__.") != 0) {
            break;
        }
        IFMapTable *table = static_cast<IFMapTable *>(iter->second);

        for (int i = 0; i < IFMapTable::kPartitionCount; ++i) {
            DBTablePartBase *partition = table->GetTablePartition(i);
            DBEntryBase *src = partition->GetFirst();
            while (src) {
                IFMapNode *src_node = static_cast<IFMapNode *>(src);
                src = partition->GetNext(src);

                // If the search string is not part of the node's name, skip it.
                if (!search_string.empty() &&
                   (src_node->ToString().find(search_string) == string::npos)) {
                    continue;
                }
                IFMapPerClientNodesShowInfo dest;
                bool send = CopyNode(&dest, src_node, ifmap_server,
                                     client_index);
                if (send) {
                    show_data->send_buffer.push_back(dest);
                }
            }
        }
    }

    return true;
}

// Can be called multiple times i.e. approx total/kMaxElementsPerRound
bool ShowIFMapPerClientNodes::SendStage(const Sandesh *sr,
                                        const RequestPipeline::PipeSpec ps,
                                        int stage, int instNum,
                                        RequestPipeline::InstData *data) {
    const RequestPipeline::StageData *prev_stage_data = ps.GetStageData(0);
    const ShowIFMapPerClientNodes::ShowData &show_data = 
        static_cast<const ShowIFMapPerClientNodes::ShowData &> 
        (prev_stage_data->at(0));
    // Data for this stage
    TrackerData *tracker_data = static_cast<TrackerData *>(data);

    vector<IFMapPerClientNodesShowInfo> dest_buffer;
    vector<IFMapPerClientNodesShowInfo>::const_iterator first, last;
    bool more = false;

    if (tracker_data->init) {
        first = show_data.send_buffer.begin();
        tracker_data->init = 0;
    } else {
        first = tracker_data->first;
    }
    int rem_num = show_data.send_buffer.end() - first;
    int send_num = (rem_num < kMaxElementsPerRound) ? rem_num :
                                                      kMaxElementsPerRound;
    last = first + send_num;
    copy(first, last, back_inserter(dest_buffer));
    // Decide if we want to be called again.
    if ((rem_num - send_num) > 0) {
        more = true;
    } else {
        more = false;
    }
    const IFMapPerClientNodesShowReq *request = 
        static_cast<const IFMapPerClientNodesShowReq *>(ps.snhRequest_.get());
    IFMapPerClientNodesShowResp *response = new IFMapPerClientNodesShowResp();
    response->set_node_db(dest_buffer);
    response->set_context(request->context());
    response->set_more(more);
    response->Response();
    tracker_data->first = first + send_num;

    // Return 'false' to be called again
    return (!more);
}

void IFMapPerClientNodesShowReq::HandleRequest() const {

    RequestPipeline::StageSpec s0, s1;
    TaskScheduler *scheduler = TaskScheduler::GetInstance();

    // 2 stages - first: gather/read, second: send

    s0.taskId_ = scheduler->GetTaskId("db::DBTable");
    s0.allocFn_ = ShowIFMapPerClientNodes::AllocBuffer;
    s0.cbFn_ = ShowIFMapPerClientNodes::BufferStage;
    s0.instances_.push_back(0);

    // control-node ifmap show command task
    s1.taskId_ = scheduler->GetTaskId("cn_ifmap::ShowCommand");
    s1.allocFn_ = ShowIFMapPerClientNodes::AllocTracker;
    s1.cbFn_ = ShowIFMapPerClientNodes::SendStage;
    s1.instances_.push_back(0);

    RequestPipeline::PipeSpec ps(this);
    ps.stages_= list_of(s0)(s1);
    RequestPipeline rp(ps);
}

class ShowIFMapPerClientLinkTable {
public:
    static const int kMaxElementsPerRound = 50;

    struct ShowData : public RequestPipeline::InstData {
        vector<IFMapPerClientLinksShowInfo> send_buffer;
    };

    static RequestPipeline::InstData *AllocBuffer(int stage) {
        return static_cast<RequestPipeline::InstData *>(new ShowData);
    }

    struct TrackerData : public RequestPipeline::InstData {
        // init as 1 indicates we need to init 'first' to begin() since there
        // is no way to initialize an iterator here.
        TrackerData() : init(1) { }
        int init;
        vector<IFMapPerClientLinksShowInfo>::const_iterator first;
    };

    static RequestPipeline::InstData *AllocTracker(int stage) {
        return static_cast<RequestPipeline::InstData *>(new TrackerData);
    }

    static bool CopyNode(IFMapPerClientLinksShowInfo *dest, IFMapLink *src,
                         IFMapServer *server, int client_index);
    static bool BufferStage(const Sandesh *sr,
                            const RequestPipeline::PipeSpec ps, int stage,
                            int instNum, RequestPipeline::InstData *data);
    static bool SendStage(const Sandesh *sr, const RequestPipeline::PipeSpec ps,
                          int stage, int instNum,
                          RequestPipeline::InstData *data);
};

bool ShowIFMapPerClientLinkTable::CopyNode(IFMapPerClientLinksShowInfo *dest,
        IFMapLink *src, IFMapServer *server, int client_index) {
    IFMapLinkState *state = server->exporter()->LinkStateLookup(src);

    if (state && state->interest().test(client_index)) {
        dest->metadata = src->metadata();
        dest->left = src->left()->ToString();
        dest->right = src->right()->ToString();
        if (state->advertised().test(client_index)) {
            dest->sent = "Yes";
        } else {
            dest->sent = "No";
        }
        if (server->exporter()->ClientHasConfigTracker(client_index)) {
            if (server->exporter()->ClientConfigTrackerHasState(client_index,
                                                                state)) {
                dest->tracked = "Yes";
            } else {
                dest->tracked = "No";
            }
        } else {
            dest->tracked = "No tracker";
        }
        return true;
    } else {
        return false;
    }
}

bool ShowIFMapPerClientLinkTable::BufferStage(const Sandesh *sr,
        const RequestPipeline::PipeSpec ps, int stage, int instNum,
        RequestPipeline::InstData *data) {
    const IFMapPerClientLinksShowReq *request = 
        static_cast<const IFMapPerClientLinksShowReq *>(ps.snhRequest_.get());
    IFMapSandeshContext *sctx = 
        static_cast<IFMapSandeshContext *>(request->module_context("IFMap"));

    string client_index_or_name = request->get_client_index_or_name();
    if (client_index_or_name.empty()) {
        return true;
    }

    // The user gives us either a name or an index. If the input is not a
    // number, find the client's index using its name. If we cant find it,
    // we cant process this request. If we have the index, continue processing.
    int client_index;
    if (!stringToInteger(client_index_or_name, client_index)) {
        if (!sctx->ifmap_server()->ClientNameToIndex(client_index_or_name,
                                                     &client_index)) {
            return true;
        }
    }

    string search_string = request->get_search_string();
    IFMapLinkTable *table =  static_cast<IFMapLinkTable *>(
        sctx->ifmap_server()->database()->FindTable("__ifmap_metadata__.0"));
    if (table) {
        ShowData *show_data = static_cast<ShowData *>(data);
        show_data->send_buffer.reserve(table->Size());

        DBTablePartBase *partition = table->GetTablePartition(0);
        DBEntryBase *src = partition->GetFirst();
        while (src) {
            IFMapLink *src_link = static_cast<IFMapLink *>(src);
            src = partition->GetNext(src);

            // If the search string is not part of any of the fields, skip it.
            if (!search_string.empty() &&
                (src_link->metadata().find(search_string) == string::npos) &&
                (src_link->left()->ToString().find(search_string) ==
                                                              string::npos) &&
                (src_link->right()->ToString().find(search_string) ==
                                                              string::npos)) {
                continue;
            }
            IFMapPerClientLinksShowInfo dest;
            bool send = CopyNode(&dest, src_link, sctx->ifmap_server(),
                                 client_index);
            if (send) {
                show_data->send_buffer.push_back(dest);
            }
        }
    } else {
        IFMAP_WARN(IFMapTblNotFound, "Cant show/find ", "link table");
    }

    return true;
}

// Can be called multiple times i.e. approx total/kMaxElementsPerRound
bool ShowIFMapPerClientLinkTable::SendStage(const Sandesh *sr,
        const RequestPipeline::PipeSpec ps, int stage, int instNum,
        RequestPipeline::InstData *data) {
    const RequestPipeline::StageData *prev_stage_data = ps.GetStageData(0);
    const ShowIFMapPerClientLinkTable::ShowData &show_data = 
        static_cast<const ShowIFMapPerClientLinkTable::ShowData &>
            (prev_stage_data->at(0));

    // Data for this stage
    TrackerData *tracker_data = static_cast<TrackerData *>(data);

    vector<IFMapPerClientLinksShowInfo> dest_buffer;
    vector<IFMapPerClientLinksShowInfo>::const_iterator first, last;
    bool more = false;

    if (tracker_data->init) {
        first = show_data.send_buffer.begin();
        tracker_data->init = 0;
    } else {
        first = tracker_data->first;
    }
    int rem_num = show_data.send_buffer.end() - first;
    int send_num = (rem_num < kMaxElementsPerRound) ? rem_num :
                                                      kMaxElementsPerRound;
    last = first + send_num;
    copy(first, last, back_inserter(dest_buffer));
    // Decide if we want to be called again.
    if ((rem_num - send_num) > 0) {
        more = true;
    } else {
        more = false;
    }
    const IFMapPerClientLinksShowReq *request = 
        static_cast<const IFMapPerClientLinksShowReq *>(ps.snhRequest_.get());
    IFMapPerClientLinksShowResp *response = new IFMapPerClientLinksShowResp();
    response->set_link_db(dest_buffer);
    response->set_context(request->context());
    response->set_more(more);
    response->Response();
    tracker_data->first = first + send_num;

    // Return 'false' to be called again
    return (!more);
}

void IFMapPerClientLinksShowReq::HandleRequest() const {

    RequestPipeline::StageSpec s0, s1;
    TaskScheduler *scheduler = TaskScheduler::GetInstance();

    // 2 stages - first: gather/read, second: send

    s0.taskId_ = scheduler->GetTaskId("db::DBTable");
    s0.allocFn_ = ShowIFMapPerClientLinkTable::AllocBuffer;
    s0.cbFn_ = ShowIFMapPerClientLinkTable::BufferStage;
    s0.instances_.push_back(0);

    // control-node ifmap show command task
    s1.taskId_ = scheduler->GetTaskId("cn_ifmap::ShowCommand");
    s1.allocFn_ = ShowIFMapPerClientLinkTable::AllocTracker;
    s1.cbFn_ = ShowIFMapPerClientLinkTable::SendStage;
    s1.instances_.push_back(0);

    RequestPipeline::PipeSpec ps(this);
    ps.stages_= list_of(s0)(s1);
    RequestPipeline rp(ps);
}

class ShowIFMapUuidToNodeMapping {
public:
    static const int kMaxElementsPerRound = 50;

    struct ShowData : public RequestPipeline::InstData {
        vector<IFMapUuidToNodeMappingEntry> send_buffer;
    };

    static RequestPipeline::InstData *AllocBuffer(int stage) {
        return static_cast<RequestPipeline::InstData *>(new ShowData);
    }

    struct TrackerData : public RequestPipeline::InstData {
        // init as 1 indicates we need to init 'first' to begin() since there is
        // no way to initialize an iterator here.
        TrackerData() : init(1) { }
        int init;
        vector<IFMapUuidToNodeMappingEntry>::const_iterator first;
    };

    static RequestPipeline::InstData *AllocTracker(int stage) {
        return static_cast<RequestPipeline::InstData *>(new TrackerData);
    }

    static bool BufferStage(const Sandesh *sr,
                            const RequestPipeline::PipeSpec ps, int stage,
                            int instNum, RequestPipeline::InstData *data);
    static bool SendStage(const Sandesh *sr, const RequestPipeline::PipeSpec ps,
                          int stage, int instNum,
                          RequestPipeline::InstData *data);
};

bool ShowIFMapUuidToNodeMapping::BufferStage(const Sandesh *sr,
                                             const RequestPipeline::PipeSpec ps,
                                             int stage, int instNum,
                                             RequestPipeline::InstData *data) {
    const IFMapUuidToNodeMappingReq *request = 
        static_cast<const IFMapUuidToNodeMappingReq *>(ps.snhRequest_.get());
    IFMapSandeshContext *sctx = 
        static_cast<IFMapSandeshContext *>(request->module_context("IFMap"));
    ShowData *show_data = static_cast<ShowData *>(data);

    IFMapVmUuidMapper *mapper = sctx->ifmap_server()->vm_uuid_mapper();
    IFMapUuidMapper &uuid_mapper = mapper->uuid_mapper_;

    show_data->send_buffer.reserve(uuid_mapper.Size());
    for (IFMapUuidMapper::UuidNodeMap::const_iterator iter = 
                                        uuid_mapper.uuid_node_map_.begin();
         iter != uuid_mapper.uuid_node_map_.end(); ++iter) {
        IFMapUuidToNodeMappingEntry dest;
        dest.set_uuid(iter->first);
        IFMapNode *node = static_cast<IFMapNode *>(iter->second);
        dest.set_node_name(node->ToString());
        show_data->send_buffer.push_back(dest);
    }

    return true;
}

// Can be called multiple times i.e. approx total/kMaxElementsPerRound
bool ShowIFMapUuidToNodeMapping::SendStage(const Sandesh *sr,
                                           const RequestPipeline::PipeSpec ps,
                                           int stage, int instNum,
                                           RequestPipeline::InstData *data) {
    const RequestPipeline::StageData *prev_stage_data = ps.GetStageData(0);
    const ShowIFMapUuidToNodeMapping::ShowData &show_data = 
        static_cast<const ShowIFMapUuidToNodeMapping::ShowData &> 
        (prev_stage_data->at(0));
    // Data for this stage
    TrackerData *tracker_data = static_cast<TrackerData *>(data);

    vector<IFMapUuidToNodeMappingEntry> dest_buffer;
    vector<IFMapUuidToNodeMappingEntry>::const_iterator first, last;
    bool more = false;

    if (tracker_data->init) {
        first = show_data.send_buffer.begin();
        tracker_data->init = 0;
    } else {
        first = tracker_data->first;
    }
    int rem_num = show_data.send_buffer.end() - first;
    int send_num = (rem_num < kMaxElementsPerRound) ? rem_num :
                                                      kMaxElementsPerRound;
    last = first + send_num;
    copy(first, last, back_inserter(dest_buffer));
    // Decide if we want to be called again.
    if ((rem_num - send_num) > 0) {
        more = true;
    } else {
        more = false;
    }
    const IFMapUuidToNodeMappingReq *request = 
        static_cast<const IFMapUuidToNodeMappingReq *>(ps.snhRequest_.get());
    IFMapUuidToNodeMappingResp *response = new IFMapUuidToNodeMappingResp();
    response->set_map_count(dest_buffer.size());
    response->set_uuid_to_node_map(dest_buffer);
    response->set_context(request->context());
    response->set_more(more);
    response->Response();
    tracker_data->first = first + send_num;

    // Return 'false' to be called again
    return (!more);
}

void IFMapUuidToNodeMappingReq::HandleRequest() const {

    RequestPipeline::StageSpec s0, s1;
    TaskScheduler *scheduler = TaskScheduler::GetInstance();

    // 2 stages - first: gather/read, second: send

    s0.taskId_ = scheduler->GetTaskId("db::DBTable");
    s0.allocFn_ = ShowIFMapUuidToNodeMapping::AllocBuffer;
    s0.cbFn_ = ShowIFMapUuidToNodeMapping::BufferStage;
    s0.instances_.push_back(0);

    // control-node ifmap show command task
    s1.taskId_ = scheduler->GetTaskId("cn_ifmap::ShowCommand");
    s1.allocFn_ = ShowIFMapUuidToNodeMapping::AllocTracker;
    s1.cbFn_ = ShowIFMapUuidToNodeMapping::SendStage;
    s1.instances_.push_back(0);

    RequestPipeline::PipeSpec ps(this);
    ps.stages_= list_of(s0)(s1);
    RequestPipeline rp(ps);
}

class ShowIFMapNodeToUuidMapping {
public:
    static const int kMaxElementsPerRound = 50;

    struct ShowData : public RequestPipeline::InstData {
        vector<IFMapNodeToUuidMappingEntry> send_buffer;
    };

    static RequestPipeline::InstData *AllocBuffer(int stage) {
        return static_cast<RequestPipeline::InstData *>(new ShowData);
    }

    struct TrackerData : public RequestPipeline::InstData {
        // init as 1 indicates we need to init 'first' to begin() since there is
        // no way to initialize an iterator here.
        TrackerData() : init(1) { }
        int init;
        vector<IFMapNodeToUuidMappingEntry>::const_iterator first;
    };

    static RequestPipeline::InstData *AllocTracker(int stage) {
        return static_cast<RequestPipeline::InstData *>(new TrackerData);
    }

    static bool BufferStage(const Sandesh *sr,
                            const RequestPipeline::PipeSpec ps, int stage,
                            int instNum, RequestPipeline::InstData *data);
    static bool SendStage(const Sandesh *sr, const RequestPipeline::PipeSpec ps,
                          int stage, int instNum,
                          RequestPipeline::InstData *data);
};

bool ShowIFMapNodeToUuidMapping::BufferStage(const Sandesh *sr,
                                             const RequestPipeline::PipeSpec ps,
                                             int stage, int instNum,
                                             RequestPipeline::InstData *data) {
    const IFMapNodeToUuidMappingReq *request = 
        static_cast<const IFMapNodeToUuidMappingReq *>(ps.snhRequest_.get());
    IFMapSandeshContext *sctx = 
        static_cast<IFMapSandeshContext *>(request->module_context("IFMap"));
    ShowData *show_data = static_cast<ShowData *>(data);

    IFMapVmUuidMapper *mapper = sctx->ifmap_server()->vm_uuid_mapper();

    show_data->send_buffer.reserve(mapper->NodeUuidMapCount());
    for (IFMapVmUuidMapper::NodeUuidMap::const_iterator iter = 
                                        mapper->node_uuid_map_.begin();
         iter != mapper->node_uuid_map_.end(); ++iter) {
        IFMapNodeToUuidMappingEntry dest;
        IFMapNode *node = static_cast<IFMapNode *>(iter->first);
        dest.set_node_name(node->ToString());
        dest.set_uuid(iter->second);
        show_data->send_buffer.push_back(dest);
    }

    return true;
}

// Can be called multiple times i.e. approx total/kMaxElementsPerRound
bool ShowIFMapNodeToUuidMapping::SendStage(const Sandesh *sr,
                                           const RequestPipeline::PipeSpec ps,
                                           int stage, int instNum,
                                           RequestPipeline::InstData *data) {
    const RequestPipeline::StageData *prev_stage_data = ps.GetStageData(0);
    const ShowIFMapNodeToUuidMapping::ShowData &show_data = 
        static_cast<const ShowIFMapNodeToUuidMapping::ShowData &> 
        (prev_stage_data->at(0));
    // Data for this stage
    TrackerData *tracker_data = static_cast<TrackerData *>(data);

    vector<IFMapNodeToUuidMappingEntry> dest_buffer;
    vector<IFMapNodeToUuidMappingEntry>::const_iterator first, last;
    bool more = false;

    if (tracker_data->init) {
        first = show_data.send_buffer.begin();
        tracker_data->init = 0;
    } else {
        first = tracker_data->first;
    }
    int rem_num = show_data.send_buffer.end() - first;
    int send_num = (rem_num < kMaxElementsPerRound) ? rem_num :
                                                      kMaxElementsPerRound;
    last = first + send_num;
    copy(first, last, back_inserter(dest_buffer));
    // Decide if we want to be called again.
    if ((rem_num - send_num) > 0) {
        more = true;
    } else {
        more = false;
    }
    const IFMapNodeToUuidMappingReq *request = 
        static_cast<const IFMapNodeToUuidMappingReq *>(ps.snhRequest_.get());
    IFMapNodeToUuidMappingResp *response = new IFMapNodeToUuidMappingResp();
    response->set_map_count(dest_buffer.size());
    response->set_node_to_uuid_map(dest_buffer);
    response->set_context(request->context());
    response->set_more(more);
    response->Response();
    tracker_data->first = first + send_num;

    // Return 'false' to be called again
    return (!more);
}

void IFMapNodeToUuidMappingReq::HandleRequest() const {

    RequestPipeline::StageSpec s0, s1;
    TaskScheduler *scheduler = TaskScheduler::GetInstance();

    // 2 stages - first: gather/read, second: send

    s0.taskId_ = scheduler->GetTaskId("db::DBTable");
    s0.allocFn_ = ShowIFMapNodeToUuidMapping::AllocBuffer;
    s0.cbFn_ = ShowIFMapNodeToUuidMapping::BufferStage;
    s0.instances_.push_back(0);

    // control-node ifmap show command task
    s1.taskId_ = scheduler->GetTaskId("cn_ifmap::ShowCommand");
    s1.allocFn_ = ShowIFMapNodeToUuidMapping::AllocTracker;
    s1.cbFn_ = ShowIFMapNodeToUuidMapping::SendStage;
    s1.instances_.push_back(0);

    RequestPipeline::PipeSpec ps(this);
    ps.stages_= list_of(s0)(s1);
    RequestPipeline rp(ps);
}

class ShowIFMapPendingVmReg {
public:
    static const int kMaxElementsPerRound = 50;

    struct ShowData : public RequestPipeline::InstData {
        vector<IFMapPendingVmRegEntry> send_buffer;
    };

    static RequestPipeline::InstData *AllocBuffer(int stage) {
        return static_cast<RequestPipeline::InstData *>(new ShowData);
    }

    struct TrackerData : public RequestPipeline::InstData {
        // init as 1 indicates we need to init 'first' to begin() since there is
        // no way to initialize an iterator here.
        TrackerData() : init(1) { }
        int init;
        vector<IFMapPendingVmRegEntry>::const_iterator first;
    };

    static RequestPipeline::InstData *AllocTracker(int stage) {
        return static_cast<RequestPipeline::InstData *>(new TrackerData);
    }

    static bool BufferStage(const Sandesh *sr,
                            const RequestPipeline::PipeSpec ps, int stage,
                            int instNum, RequestPipeline::InstData *data);
    static bool SendStage(const Sandesh *sr, const RequestPipeline::PipeSpec ps,
                          int stage, int instNum,
                          RequestPipeline::InstData *data);
};

bool ShowIFMapPendingVmReg::BufferStage(const Sandesh *sr,
                                        const RequestPipeline::PipeSpec ps,
                                        int stage, int instNum,
                                        RequestPipeline::InstData *data) {
    const IFMapPendingVmRegReq *request = 
        static_cast<const IFMapPendingVmRegReq *>(ps.snhRequest_.get());
    IFMapSandeshContext *sctx = 
        static_cast<IFMapSandeshContext *>(request->module_context("IFMap"));
    ShowData *show_data = static_cast<ShowData *>(data);

    IFMapVmUuidMapper *mapper = sctx->ifmap_server()->vm_uuid_mapper();

    show_data->send_buffer.reserve(mapper->PendingVmRegCount());
    for (IFMapVmUuidMapper::PendingVmRegMap::const_iterator iter = 
                                        mapper->pending_vmreg_map_.begin();
         iter != mapper->pending_vmreg_map_.end(); ++iter) {
        IFMapPendingVmRegEntry dest;
        dest.set_vm_uuid(iter->first);
        dest.set_vr_name(iter->second);
        show_data->send_buffer.push_back(dest);
    }

    return true;
}

// Can be called multiple times i.e. approx total/kMaxElementsPerRound
bool ShowIFMapPendingVmReg::SendStage(const Sandesh *sr,
                                      const RequestPipeline::PipeSpec ps,
                                      int stage, int instNum,
                                      RequestPipeline::InstData *data) {
    const RequestPipeline::StageData *prev_stage_data = ps.GetStageData(0);
    const ShowIFMapPendingVmReg::ShowData &show_data = 
        static_cast<const ShowIFMapPendingVmReg::ShowData &> 
        (prev_stage_data->at(0));
    // Data for this stage
    TrackerData *tracker_data = static_cast<TrackerData *>(data);

    vector<IFMapPendingVmRegEntry> dest_buffer;
    vector<IFMapPendingVmRegEntry>::const_iterator first, last;
    bool more = false;

    if (tracker_data->init) {
        first = show_data.send_buffer.begin();
        tracker_data->init = 0;
    } else {
        first = tracker_data->first;
    }
    int rem_num = show_data.send_buffer.end() - first;
    int send_num = (rem_num < kMaxElementsPerRound) ? rem_num :
                                                      kMaxElementsPerRound;
    last = first + send_num;
    copy(first, last, back_inserter(dest_buffer));
    // Decide if we want to be called again.
    if ((rem_num - send_num) > 0) {
        more = true;
    } else {
        more = false;
    }
    const IFMapPendingVmRegReq *request = 
        static_cast<const IFMapPendingVmRegReq *>(ps.snhRequest_.get());
    IFMapPendingVmRegResp *response = new IFMapPendingVmRegResp();
    response->set_map_count(dest_buffer.size());
    response->set_vm_reg_map(dest_buffer);
    response->set_context(request->context());
    response->set_more(more);
    response->Response();
    tracker_data->first = first + send_num;

    // Return 'false' to be called again
    return (!more);
}

void IFMapPendingVmRegReq::HandleRequest() const {

    RequestPipeline::StageSpec s0, s1;
    TaskScheduler *scheduler = TaskScheduler::GetInstance();

    // 2 stages - first: gather/read, second: send

    s0.taskId_ = scheduler->GetTaskId("db::DBTable");
    s0.allocFn_ = ShowIFMapPendingVmReg::AllocBuffer;
    s0.cbFn_ = ShowIFMapPendingVmReg::BufferStage;
    s0.instances_.push_back(0);

    // control-node ifmap show command task
    s1.taskId_ = scheduler->GetTaskId("cn_ifmap::ShowCommand");
    s1.allocFn_ = ShowIFMapPendingVmReg::AllocTracker;
    s1.cbFn_ = ShowIFMapPendingVmReg::SendStage;
    s1.instances_.push_back(0);

    RequestPipeline::PipeSpec ps(this);
    ps.stages_= list_of(s0)(s1);
    RequestPipeline rp(ps);
}

static bool IFMapServerClientShowReqHandleRequest(const Sandesh *sr,
                const RequestPipeline::PipeSpec ps, int stage, int instNum,
                RequestPipeline::InstData *data) {
    const IFMapServerClientShowReq *request =
        static_cast<const IFMapServerClientShowReq *>(ps.snhRequest_.get());
    IFMapSandeshContext *sctx =
        static_cast<IFMapSandeshContext *>(request->module_context("IFMap"));

    IFMapServerClientShowResp *response = new IFMapServerClientShowResp();

    IFMapServerShowClientMap name_list;
    sctx->ifmap_server()->FillClientMap(&name_list);
    IFMapServerShowIndexMap index_list;
    sctx->ifmap_server()->FillIndexMap(&index_list);

    response->set_name_list(name_list);
    response->set_index_list(index_list);
    response->set_context(request->context());
    response->set_more(false);
    response->Response();

    // Return 'true' so that we are not called again
    return true;
}

void IFMapServerClientShowReq::HandleRequest() const {

    RequestPipeline::StageSpec s0;
    TaskScheduler *scheduler = TaskScheduler::GetInstance();

    s0.taskId_ = scheduler->GetTaskId("db::DBTable");
    s0.cbFn_ = IFMapServerClientShowReqHandleRequest;
    s0.instances_.push_back(0);

    RequestPipeline::PipeSpec ps(this);
    ps.stages_= boost::assign::list_of(s0);
    RequestPipeline rp(ps);
}

static bool IFMapNodeTableListShowReqHandleRequest(const Sandesh *sr,
                const RequestPipeline::PipeSpec ps, int stage, int instNum,
                RequestPipeline::InstData *data) {
    const IFMapNodeTableListShowReq *request =
        static_cast<const IFMapNodeTableListShowReq *>(ps.snhRequest_.get());
    IFMapSandeshContext *sctx =
        static_cast<IFMapSandeshContext *>(request->module_context("IFMap"));

    vector<IFMapNodeTableListShowEntry> dest_buffer;
    IFMapTable::FillNodeTableList(sctx->ifmap_server()->database(),
                                  &dest_buffer);

    IFMapNodeTableListShowResp *response = new IFMapNodeTableListShowResp();
    response->set_table_list(dest_buffer);
    response->set_context(request->context());
    response->set_more(false);
    response->Response();

    // Return 'true' so that we are not called again
    return true;
}

void IFMapNodeTableListShowReq::HandleRequest() const {

    RequestPipeline::StageSpec s0;
    TaskScheduler *scheduler = TaskScheduler::GetInstance();

    s0.taskId_ = scheduler->GetTaskId("db::DBTable");
    s0.cbFn_ = IFMapNodeTableListShowReqHandleRequest;
    s0.instances_.push_back(0);

    RequestPipeline::PipeSpec ps(this);
    ps.stages_= boost::assign::list_of(s0);
    RequestPipeline rp(ps);
}

