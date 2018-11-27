#include "oneflow/core/comm_network/ibverbs/ibverbs_comm_network.h"
#include "oneflow/core/control/ctrl_client.h"

#if defined(WITH_RDMA) && defined(PLATFORM_POSIX)

namespace oneflow {

namespace {

std::string GenTokensMsgKey(int64_t machine_id) {
  return "IBVerbsTokensMsg/" + std::to_string(machine_id);
}

std::string GenConnInfoKey(int64_t src_machine_id, int64_t dst_machine_id) {
  return "IBVerbsConnInfo/" + std::to_string(src_machine_id) + "/" + std::to_string(dst_machine_id);
}

}  // namespace

IBVerbsCommNet::~IBVerbsCommNet() {
  while (poll_exit_flag_.test_and_set() == true) {}
  poll_thread_.join();
  for (IBVerbsQP* qp : qp_vec_) {
    if (qp) { delete qp; }
  }
  PCHECK(ibv_destroy_cq(cq_) == 0);
  PCHECK(ibv_dealloc_pd(pd_) == 0);
  PCHECK(ibv_close_device(context_) == 0);
}

void IBVerbsCommNet::RegisterMemoryDone() {
  int64_t this_machine_id = Global<MachineCtx>::Get()->this_machine_id();
  IBVerbsTokensMsg this_tokens_msg;
  for (IBVerbsMemDesc* mem_desc : mem_descs()) {
    this_tokens_msg.mutable_token2mem_desc()->insert(
        {reinterpret_cast<uint64_t>(mem_desc), mem_desc->ToProto()});
  }
  Global<CtrlClient>::Get()->PushKV(GenTokensMsgKey(this_machine_id), this_tokens_msg);
  for (int64_t peer_id : peer_machine_id()) {
    IBVerbsTokensMsg peer_tokens_msg;
    Global<CtrlClient>::Get()->PullKV(GenTokensMsgKey(peer_id), &peer_tokens_msg);
    for (const auto& pair : peer_tokens_msg.token2mem_desc()) {
      CHECK(token2mem_desc_.at(peer_id)
                .emplace(reinterpret_cast<void*>(pair.first), pair.second)
                .second);
    }
  }
  OF_BARRIER();
  Global<CtrlClient>::Get()->ClearKV(GenTokensMsgKey(this_machine_id));
}

void IBVerbsCommNet::SendActorMsg(int64_t dst_machine_id, const ActorMsg& msg) {
  qp_vec_.at(dst_machine_id)->PostSendRequest(msg);
}

IBVerbsCommNet::IBVerbsCommNet(const Plan& plan)
    : CommNetIf(plan),
      token2mem_desc_(Global<JobDesc>::Get()->TotalMachineNum()),
      poll_exit_flag_(ATOMIC_FLAG_INIT) {
  const auto& ibv_conf = Global<JobDesc>::Get()->ibverbs_conf();
  int32_t device_num;
  ibv_device** device_list = ibv_get_device_list(&device_num);
  PCHECK(device_list);
  int32_t device_index = 0;
  ibv_device* device = device_list[device_index];
  while ((ibv_conf.device_name() != "") && (device_index < device_num)) {
    if (std::string(ibv_get_device_name(device)) == ibv_conf.device_name()) { break; }
    device = device_list[++device_index];
  }
  CHECK_LT(device_index, device_num);
  context_ = ibv_open_device(device);
  PCHECK(context_);
  ibv_free_device_list(device_list);
  pd_ = ibv_alloc_pd(context_);
  PCHECK(pd_);
  ibv_device_attr device_attr;
  PCHECK(ibv_query_device(context_, &device_attr) == 0);
  cq_ = ibv_create_cq(context_, device_attr.max_cqe, nullptr, nullptr, 0);
  PCHECK(cq_);
  ibv_port_attr port_attr;
  PCHECK(ibv_query_port(context_, 1, &port_attr) == 0);  // TODO(shiyuan): reuse port_num(1)?
  // TODO(shiyuan): GID is the global address when sending packets between different subnets/
  ibv_gid gid;
  PCHECK(ibv_query_gid(context_, 1, 0, &gid) == 0);
  int64_t this_machine_id = Global<MachineCtx>::Get()->this_machine_id();
  qp_vec_.assign(Global<JobDesc>::Get()->TotalMachineNum(), nullptr);
  for (int64_t peer_id : peer_machine_id()) {
    IBVerbsQP* cur_qp = new IBVerbsQP(context_, pd_, cq_);
    qp_vec_.at(peer_id) = cur_qp;
    IBVerbsConnectionInfo conn_info;
    conn_info.set_local_id(port_attr.lid);
    conn_info.set_qp_num(cur_qp->qp_num());
    conn_info.set_subnet_prefix(gid.global.subnet_prefix);
    conn_info.set_interface_id(gid.global.interface_id);
    Global<CtrlClient>::Get()->PushKV(GenConnInfoKey(this_machine_id, peer_id), conn_info);
  }
  for (int64_t peer_id : peer_machine_id()) {
    IBVerbsConnectionInfo conn_info;
    Global<CtrlClient>::Get()->PullKV(GenConnInfoKey(peer_id, this_machine_id), &conn_info);
    qp_vec_.at(peer_id)->Connect(conn_info);
  }
  OF_BARRIER();
  for (int64_t peer_id : peer_machine_id()) {
    qp_vec_.at(peer_id)->PostAllRecvRequest();
    Global<CtrlClient>::Get()->ClearKV(GenConnInfoKey(this_machine_id, peer_id));
  }
  OF_BARRIER();
  poll_thread_ = std::thread(&IBVerbsCommNet::PollCQ, this);
  // TODO(shiyuan): PingPeers
  OF_BARRIER();
}

void IBVerbsCommNet::DoRead(void* read_id, int64_t src_machine_id, void* src_token,
                            void* dst_token) {
  qp_vec_.at(src_machine_id)
      ->PostReadRequest(token2mem_desc_.at(src_machine_id).at(src_token),
                        *static_cast<const IBVerbsMemDesc*>(dst_token), read_id);
}

void IBVerbsCommNet::PollCQ() {
  std::vector<ibv_wc> wc_vec(max_poll_wc_num_);
  while (poll_exit_flag_.test_and_set() == false) {
    poll_exit_flag_.clear();
    int32_t found_wc_num = ibv_poll_cq(cq_, max_poll_wc_num_, wc_vec.data());
    CHECK_GE(found_wc_num, 0);
    FOR_RANGE(int32_t, i, 0, found_wc_num) {
      const ibv_wc& wc = wc_vec.at(i);
      CHECK_EQ(wc.status, IBV_WC_SUCCESS) << wc.opcode;
      WorkRequestId* wr_id = reinterpret_cast<WorkRequestId*>(wc.wr_id);
      IBVerbsQP* qp = wr_id->qp;
      switch (wc.opcode) {
        case IBV_WC_RDMA_READ: {
          qp->ReadDone(wr_id);
          break;
        }
        case IBV_WC_SEND: {
          qp->SendDone(wr_id);
          break;
        }
        case IBV_WC_RECV: {
          qp->RecvDone(wr_id);
          break;
        }
        default: UNIMPLEMENTED();
      }
    }
  }
}

const int32_t IBVerbsCommNet::max_poll_wc_num_ = 32;

}  // namespace oneflow

#endif  // WITH_RDMA && PLATFORM_POSIX
