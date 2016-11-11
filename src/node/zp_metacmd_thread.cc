#include "zp_metacmd_thread.h"

#include <glog/logging.h>
#include <google/protobuf/text_format.h>
#include "zp_data_server.h"

#include "zp_command.h"
#include "zp_admin.h"

extern ZPDataServer* zp_data_server;

ZPMetacmdThread::ZPMetacmdThread()
  :is_working_(false) {
    cli_ = new pink::PbCli();
    cli_->set_connect_timeout(1500);
  }

void ZPMetacmdThread::MetacmdTaskSchedule() {
  if (is_working_)
    return;
  is_working_ = true;
  StartIfNeed();
  Schedule(&DoMetaUpdateTask, static_cast<void*>(this));
}

ZPMetacmdThread::~ZPMetacmdThread() {
  Stop();
  delete cli_;
  LOG(INFO) << "ZPMetacmd thread " << thread_id() << " exit!!!";
}

void ZPMetacmdThread::MetaUpdateTask() {
  int64_t receive_epoch = 0;
  if (!zp_data_server->ShouldPullMeta()) {
    return;
  }

  if (FetchMetaInfo(receive_epoch)) {
    // When we fetch OK, we will FinishPullMeta
    zp_data_server->FinishPullMeta(receive_epoch);
    is_working_ = false;
  } else {
    // Sleep and try again
    sleep(kMetacmdInterval);
    zp_data_server->AddMetacmdTask();
  }
}

pink::Status ZPMetacmdThread::Send() {
  ZPMeta::MetaCmd request;

  DLOG(INFO) << "MetacmdThread send pull to MetaServer(" << zp_data_server->meta_ip() << ":"
    << zp_data_server->meta_port() + kMetaPortShiftCmd
    << ") with local("<< zp_data_server->local_ip() << ":" << zp_data_server->local_port() << ")";
  request.set_type(ZPMeta::MetaCmd_Type::MetaCmd_Type_PULL);
  return cli_->Send(&request);
}

pink::Status ZPMetacmdThread::Recv(int64_t &receive_epoch) {
  pink::Status result;
  ZPMeta::MetaCmdResponse response;
  std::string meta_ip = zp_data_server->meta_ip();
  int meta_port = zp_data_server->meta_port() + kMetaPortShiftCmd;
  result = cli_->Recv(&response); 
  if (result.ok()) {
    DLOG(INFO) << "succ MetacmdThread recv from MetaServer(" << meta_ip << ":" << meta_port;
    std::string text_format;
    google::protobuf::TextFormat::PrintToString(response, &text_format);
    DLOG(INFO) << "Receive from meta(" << meta_ip << ":" << meta_port << "), size: " << response.pull().info().size() << " Response:[" << text_format << "]";

    switch (response.type()) {
      case ZPMeta::MetaCmdResponse_Type::MetaCmdResponse_Type_PULL: {
        if (response.status().code() != ZPMeta::StatusCode::kOk) {
          LOG(WARNING) << "MetacmdThread receive Pull from (" << meta_ip << ":" << meta_port << ") failed! caz: "
            << response.status().msg();
          return pink::Status::IOError(response.status().msg());
        }

        receive_epoch = response.pull().version();
        ZPMeta::MetaCmdResponse_Pull pull = response.pull();

        DLOG(INFO) << "receive Pull message, will handle " << pull.info_size() << " Partitions.";
        for (int i = 0; i < pull.info_size(); i++) {
          const ZPMeta::Partitions& partition = pull.info(i);
          DLOG(INFO) << " - handle Partition " << partition.id() << 
            ": master is " << partition.master().ip() << ":" << partition.master().port();

          Node master_node(partition.master().ip(), partition.master().port());
          if (master_node.empty()) {
            // No master patitions, simply ignore
            continue;
          }
          std::vector<Node> slave_nodes;
          for (int j = 0; j < partition.slaves_size(); j++) {
            slave_nodes.push_back(Node(partition.slaves(j).ip(), partition.slaves(j).port()));
          }

          bool result = zp_data_server->UpdateOrAddPartition(partition.id(), master_node, slave_nodes);
          if (!result) {
            LOG(WARNING) << "Failed to AddPartition " << partition.id() <<
              ", partition master is " << partition.master().ip() << ":" << partition.master().port() ;
          }
        }
        // Print partitioin info
        zp_data_server->DumpPartitions();
        break;
      }
      default:
        break;
    }
  }
  return result;
}

bool ZPMetacmdThread::FetchMetaInfo(int64_t &receive_epoch) {
  pink::Status s;
  std::string meta_ip = zp_data_server->meta_ip();
  int meta_port = zp_data_server->meta_port() + kMetaPortShiftCmd;
  // No more PickMeta, which should be done by ping thread
  assert(!zp_data_server->meta_ip().empty() && zp_data_server->meta_port() != 0);
  DLOG(INFO) << "MetacmdThread will connect ("<< meta_ip << ":" << meta_port << ")";
  s = cli_->Connect(meta_ip, meta_port);
  if (s.ok()) {
    DLOG(INFO) << "Metacmd connect (" << meta_ip << ":" << meta_port << ") ok!";
    cli_->set_send_timeout(1000);
    cli_->set_recv_timeout(1000);

    s = Send();
    if (!s.ok()) {
      LOG(WARNING) << "Metacmd send to (" << meta_ip << ":" << meta_port << ") failed! caz:" << s.ToString();
      cli_->Close();
      return false;
    }
    DLOG(INFO) << "Metacmd send to (" << meta_ip << ":" << meta_port << ") ok";

    s = Recv(receive_epoch);
    if (!s.ok()) {
      LOG(WARNING) << "Metacmd recv from (" << meta_ip << ":" << meta_port << ") failed! caz:" << s.ToString();
      cli_->Close();
      return false;
    }
    DLOG(INFO) << "Metacmd recv from (" << meta_ip << ":" << meta_port << ") ok";
    cli_->Close();
    return true;
  } else {
    LOG(WARNING) << "Metacmd connect (" << meta_ip << ":" << meta_port << ") failed! caz:" << s.ToString();
    return false;
  }
}
