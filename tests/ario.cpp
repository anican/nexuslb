#include <unistd.h>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <random>
#include <unordered_map>

#include "ario/memory.h"
#include "ario/rdma.h"
#include "ario/utils.h"

using namespace ario;

constexpr size_t kRdmaBufPoolBits = __builtin_ctzl(4L << 30);
constexpr size_t kRdmaBufBlockBits = __builtin_ctzl(4 << 20);

struct RpcMessage {
  size_t seqnum;
  char msg[1000];
};

class TestHandler : public RdmaEventHandler {
 public:
  void OnRemoteMemoryRegionReceived(RdmaQueuePair *conn, uint64_t addr,
                                    size_t size) override {
    fprintf(stderr, "got memory region: addr=0x%016lx, size=%lu\n", addr, size);
  }

  void OnRecv(RdmaQueuePair *conn, OwnedMemoryBlock buf) override {
    auto view = buf.AsMessageView();
    auto *req = reinterpret_cast<RpcMessage *>(view.bytes());
    if (print_message_) {
      fprintf(stderr,
              "Recv message. view.bytes_length()=%u. seqnum=%lu msg=\"%s\"\n",
              view.bytes_length(), req->seqnum, req->msg);
    }
    if (reply_allocator_) {
      auto reply_buf = reply_allocator_->Allocate();
      auto reply_view = reply_buf.AsMessageView();
      auto *reply = reinterpret_cast<RpcMessage *>(reply_view.bytes());
      reply->seqnum = req->seqnum;
      snprintf(reply->msg, sizeof(reply->msg),
               "THIS IS A REPLY FROM THE SERVER. SEQNUM=%lu", req->seqnum);
      conn->AsyncSend(std::move(buf));
    }
  }

  void OnSent(RdmaQueuePair *conn, OwnedMemoryBlock buf) override {}

  void OnError(RdmaQueuePair *conn, RdmaError error) override {
    fprintf(stderr, "TestHandler::OnError. error=%d\n",
            static_cast<int>(error));
  }

  void SetPrintMessage(bool print_message) { print_message_ = print_message; }

  void SetReplyAllocator(MemoryBlockAllocator *reply_allocator) {
    reply_allocator_ = reply_allocator;
  }

 private:
  bool print_message_ = true;
  MemoryBlockAllocator *reply_allocator_ = nullptr;
};

class TestServerHandler : public TestHandler {
 public:
  void OnConnected(RdmaQueuePair *conn) override {
    fprintf(stderr, "New RDMA connection.\n");
  }

  void OnRdmaReadComplete(RdmaQueuePair *conn, WorkRequestID wrid,
                          OwnedMemoryBlock buf) override {}
};

class TestClientHandler : public TestHandler {
 public:
  void OnConnected(RdmaQueuePair *conn) override {
    if (conn_ != nullptr) die("TestHandler::OnConnected: conn_ != nullptr");
    conn_ = conn;
    cv_.notify_all();
  }

  void OnRemoteMemoryRegionReceived(RdmaQueuePair *conn, uint64_t addr,
                                    size_t size) override {
    TestHandler::OnRemoteMemoryRegionReceived(conn, addr, size);
    if (got_memory_region_) die("Already got memory region");
    got_memory_region_ = true;
    cv_.notify_all();
  }

  void OnRdmaReadComplete(RdmaQueuePair *conn, WorkRequestID wrid,
                          OwnedMemoryBlock buf) override {
    if (data_.has_value())
      die("TestHandler::OnRdmaReadComplete: data_.has_value()");
    data_ = std::move(buf);
    cv_.notify_all();
  }

  RdmaQueuePair *WaitConnection() {
    if (conn_) return conn_;
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [this] { return conn_ != nullptr; });
    return conn_;
  }

  void WaitMemoryRegion() {
    if (got_memory_region_) return;
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [this] { return got_memory_region_; });
  }

  OwnedMemoryBlock WaitRead() {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [this] { return data_.has_value(); });
    auto bytes = std::move(*data_);
    data_ = std::nullopt;
    return bytes;
  }

 private:
  std::mutex mutex_;
  std::condition_variable cv_;
  std::optional<OwnedMemoryBlock> data_;
  bool got_memory_region_ = false;
  RdmaQueuePair *conn_ = nullptr;
};

void DieUsage(const char *program) {
  printf("usage:\n");
  printf("  %s tcpserver <listen_port>\n", program);
  printf("  %s tcpclient <server_host> <server_port>\n", program);
  printf(
      "  %s server <dev_name> <listen_port> "
      "<print|noprint> <reply|noreply>\n",
      program);
  printf("  %s client <dev_name> <server_host> <server_port>\n", program);
  printf(
      "  %s benchsend <dev_name> <server_host> <server_port> <num_packets> "
      "<logfilename>\n",
      program);
  printf(
      "  %s benchread <dev_name> <server_host> <server_port> "
      "<num_packets> <read_size> <logfilename>\n",
      program);
  std::exit(1);
}

class SimpleTcpConnection {
 public:
  explicit SimpleTcpConnection(TcpSocket &&peer) : peer_(std::move(peer)) {}

  void RecvMessage() {
    MutableBuffer len_buf(&recv_len_, sizeof(recv_len_));
    peer_.AsyncRead(len_buf, [this](int err, size_t) {
      if (err) {
        fprintf(stderr, "AsyncRead header err=%d\n", err);
        delete this;
        return;
      }
      MutableBuffer msg_buf(recv_data_, recv_len_);
      peer_.AsyncRead(msg_buf, [this](int err, size_t len) {
        if (err) {
          fprintf(stderr, "AsyncRead message err=%d\n", err);
          delete this;
          return;
        }
        fprintf(stderr, "got message. len=%lu. msg: %s\n", len, recv_data_);
        RecvMessage();
      });
    });
  }

  void SendMessage(std::vector<uint8_t> &&data,
                   std::function<void(int error)> &&callback) {
    send_data_ = std::move(data);
    send_callback_ = std::move(callback);
    send_len_ = static_cast<uint16_t>(send_data_.size());

    ConstBuffer len_buf(&send_len_, sizeof(send_len_));
    peer_.AsyncWrite(len_buf, [this](int error, size_t) {
      if (error) {
        auto callback = std::move(send_callback_);
        delete this;
        callback(error);
        return;
      }
      ConstBuffer msg_buf(send_data_.data(), send_len_);
      peer_.AsyncWrite(msg_buf, [this](int error, size_t) {
        auto callback = std::move(send_callback_);
        if (error) {
          delete this;
        }
        callback(error);
      });
    });
  }

 private:
  ~SimpleTcpConnection() {
    fprintf(stderr, "SimpleTcpConnection destructor\n");
  }

  TcpSocket peer_;
  uint16_t recv_len_;
  uint8_t recv_data_[1024];
  uint16_t send_len_;
  std::vector<uint8_t> send_data_;
  std::function<void(int error)> send_callback_;
};

void DoAccept(TcpAcceptor &acceptor) {
  acceptor.AsyncAccept([&acceptor](int err, TcpSocket peer) {
    if (err) return;
    auto *conn = new SimpleTcpConnection(std::move(peer));
    conn->RecvMessage();
    DoAccept(acceptor);
  });
}

void TcpServerMain(int argc, char **argv) {
  if (argc != 3) DieUsage(argv[0]);
  uint16_t listen_port = std::stoi(argv[2]);

  EpollExecutor executor;
  TcpAcceptor acceptor(executor);
  acceptor.BindAndListen(listen_port);
  fprintf(stderr, "Listening on port %d\n", listen_port);
  DoAccept(acceptor);
  executor.RunEventLoop();
}

void TcpClientMain(int argc, char **argv) {
  if (argc != 4) DieUsage(argv[0]);
  std::string server_host = argv[2];
  uint16_t server_port = std::stoi(argv[3]);

  EpollExecutor executor;
  TcpSocket socket;
  socket.Connect(executor, server_host, server_port);
  fprintf(stderr, "connected.\n");
  auto conn = new SimpleTcpConnection(std::move(socket));
  std::string msg = "This is a message from the client.";
  std::vector<uint8_t> data(msg.data(), msg.data() + msg.size());
  data.push_back('\0');
  conn->SendMessage(std::move(data), [&executor](int error) {
    if (error) {
      fprintf(stderr, "error=%d\n", error);
    } else {
      fprintf(stderr, "message sent.\n");
    }
    fprintf(stderr, "stopping event loop\n");
    executor.StopEventLoop();
  });
  executor.RunEventLoop();
}

void FillMemoryPool(std::vector<uint8_t> &memory_pool) {
  auto *mem = memory_pool.data();
  auto pid = getpid();
  char timebuf[100];
  std::time_t t = std::time(nullptr);
  std::strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S %Z",
                std::localtime(&t));
  char buf[100];
  int len = snprintf(buf, sizeof(buf), "MESSAGE FROM PID %d. CREATED AT %s.",
                     pid, timebuf);
  memcpy(mem + 4, buf, len);
  *reinterpret_cast<uint32_t *>(mem) = static_cast<uint32_t>(len);
  fprintf(stderr, "FillMemoryPool: mem[0]=%d. mem[4]=\"%s\"\n", len, buf);

  std::mt19937 gen(123);
  std::uniform_int_distribution<> distrib(0, 255);
  size_t offset = 42 << 20;
  size_t rand_len = 1 << 20;
  uint64_t sum = 0;
  for (size_t i = 0; i < rand_len; ++i) {
    auto x = distrib(gen);
    mem[offset + i] = x;
    sum += x;
  }
  fprintf(stderr, "FillMemoryPool: mem[%lu:%lu].sum()=%lu\n", offset,
          offset + rand_len, sum);
}

void ServerMain(int argc, char **argv) {
  if (argc != 6) DieUsage(argv[0]);
  std::string dev_name = argv[2];
  uint16_t listen_port = std::stoul(argv[3]);

  auto test = std::make_unique<TestServerHandler>();
  MemoryBlockAllocator buf(kRdmaBufPoolBits, kRdmaBufBlockBits);
  for (size_t i = 4; i < 6; ++i) {
    std::string option = argv[i];
    if (option == "print") {
      test->SetPrintMessage(true);
    } else if (option == "noprint") {
      test->SetPrintMessage(false);
    } else if (option == "reply") {
      test->SetReplyAllocator(&buf);
    } else if (option == "noreply") {
      test->SetReplyAllocator(nullptr);
    } else {
      fprintf(stderr, "Unknown option: %s\n", argv[i]);
      DieUsage(argv[0]);
    }
  }

  std::vector<uint8_t> memory_pool(100 << 20);
  FillMemoryPool(memory_pool);

  RdmaManager manager(dev_name, test.get(), &buf);
  manager.ExposeMemory(memory_pool.data(), memory_pool.size());
  manager.ListenTcp(listen_port);
  // std::thread event_loop_thread(&RdmaManager::RunEventLoop, &manager);
  manager.RunEventLoop();
  manager.StopEventLoop();
  // event_loop_thread.join();
}

void ClientMain(int argc, char **argv) {
  if (argc != 5) DieUsage(argv[0]);
  std::string dev_name = argv[2];
  std::string server_host = argv[3];
  uint16_t server_port = std::stoi(argv[4]);

  auto test = std::make_unique<TestClientHandler>();
  MemoryBlockAllocator buf(kRdmaBufPoolBits, kRdmaBufBlockBits);
  RdmaManager manager(dev_name, test.get(), &buf);
  MemoryBlockAllocator read_buf(kRdmaBufPoolBits, kRdmaBufBlockBits);
  manager.RegisterLocalMemory(&read_buf);
  manager.ConnectTcp(server_host, server_port);
  std::thread event_loop_thread(&RdmaManager::RunEventLoop, &manager);

  auto *conn = test->WaitConnection();
  fprintf(stderr, "ClientMain: connected.\n");
  test->WaitMemoryRegion();

  conn->AsyncRead(read_buf.Allocate(), 0, 1024);
  auto read1_data = test->WaitRead();
  if (read1_data.empty()) die("read_data.empty()");
  auto read1_view = read1_data.AsMessageView();
  auto msg_len = *reinterpret_cast<uint32_t *>(read1_view.bytes());
  std::string msg(read1_view.bytes() + 4, read1_view.bytes() + 4 + msg_len);
  fprintf(stderr,
          "ClientMain: Read(mem[0:1024]). read1_view.bytes_length()=%u. "
          "msg_len=%u. msg: %s\n",
          read1_view.bytes_length(), msg_len, msg.c_str());

  size_t offset = 42 << 20;
  size_t rand_len = 1 << 20;
  conn->AsyncRead(read_buf.Allocate(), offset, rand_len);
  auto read2_data = test->WaitRead();
  auto read2_view = read2_data.AsMessageView();
  uint64_t sum = 0;
  for (size_t i = 0; i < read2_view.bytes_length(); ++i) {
    sum += read2_view.bytes()[i];
  }
  fprintf(stderr, "ClientMain: mem[%lu:%lu].sum()=%lu\n", offset,
          offset + rand_len, sum);

  auto send_buf = buf.Allocate();
  auto send_view = send_buf.AsMessageView();
  auto *req = reinterpret_cast<RpcMessage *>(send_view.bytes());
  req->seqnum = 2333;
  strcpy(req->msg, "THIS IS A MESSAGE FROM THE CLIENT.");
  send_view.set_bytes_length(sizeof(*req));
  conn->AsyncSend(std::move(send_buf));
  fprintf(stderr, "ClientMain: AsyncSend.\n");

  manager.StopEventLoop();
  fprintf(stderr, "ClientMain: Joining event loop.\n");
  event_loop_thread.join();
  fprintf(stderr, "ClientMain: event loop joined.\n");
}

class BenchHandler : public TestClientHandler {
 public:
  void SetAllocator(MemoryBlockAllocator &allocator) {
    allocator_ = &allocator;
  }

  void OnSent(RdmaQueuePair *conn, OwnedMemoryBlock buf) override {
    --cnt_flying_;
    ++cnt_sent_;
    if (cnt_sent_ < num_packets_) {
      SendMore();
    }
  }

  void OnRecv(RdmaQueuePair *conn, OwnedMemoryBlock buf) override {
    auto now = Clock::now();
    auto view = buf.AsMessageView();
    auto *reply = reinterpret_cast<RpcMessage *>(view.bytes());
    rpc_recv_time_[reply->seqnum] = now;

    ++cnt_recv_;
    if (cnt_recv_ == num_packets_) {
      finish_time_ = now;
      cv_.notify_all();
    }
  }

  void BenchSend(size_t num_packets, RdmaQueuePair *conn) {
    num_packets_ = num_packets;
    conn_ = conn;
    cnt_sent_ = 0;
    cnt_send_ = 0;
    cnt_recv_ = 0;
    start_time_ = Clock::now();
    last_report_time_ = start_time_;
    rpc_send_time_.reset(new TimePoint[num_packets]);
    rpc_recv_time_.reset(new TimePoint[num_packets]);

    SendMore();
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [this] { return cnt_recv_.load() == num_packets_; });
    ReportProgress(true);
  }

  void WaitMemoryRegion() {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [this] { return remote_memory_size_ != 0; });
  }

  void OnRemoteMemoryRegionReceived(RdmaQueuePair *conn, uint64_t addr,
                                    size_t size) override {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      remote_memory_size_ = size;
      conn_ = conn;
    }
    cv_.notify_all();
  }

  void OnRdmaReadComplete(RdmaQueuePair *conn, WorkRequestID wrid,
                          OwnedMemoryBlock buf) override {
    auto now = Clock::now();
    auto idx = wrid_to_idx_[wrid];
    rpc_recv_time_[idx] = now;

    ++cnt_recv_;
    if (cnt_recv_ == num_packets_) {
      finish_time_ = now;
      cv_.notify_all();
    }
    ReadOneMore();
  }

  void BenchRead(size_t num_packets, size_t read_size) {
    num_packets_ = num_packets;
    read_size_ = read_size;
    cnt_sent_ = 0;
    cnt_recv_ = 0;
    start_time_ = Clock::now();
    last_report_time_ = start_time_;
    rpc_send_time_.reset(new TimePoint[num_packets]);
    rpc_recv_time_.reset(new TimePoint[num_packets]);
    wrid_to_idx_.clear();
    wrid_to_idx_.reserve(num_packets);
    distrib_ = std::uniform_int_distribution<size_t>(
        0, remote_memory_size_ - read_size_ - 1);

    constexpr size_t kMaxFlying = 100;
    for (size_t i = 0; i < kMaxFlying; ++i) {
      ReadOneMore();
    }
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [this] { return cnt_recv_.load() == num_packets_; });
    ReportProgress(true);
  }

  void SaveAnalysis(const char *filename) {
    FILE *f = nullptr;
    if (filename) {
      f = fopen(filename, "w");
      if (!f) {
        die("Cannot open file to write: " + std::string(filename));
      }
    }

    if (f) {
      fprintf(f, "%lu\n", sizeof(RpcMessage));
    }
    std::vector<int64_t> rtt;
    rtt.reserve(num_packets_);
    for (size_t i = 0; i < num_packets_; ++i) {
      auto send_time_ns = rpc_recv_time_[i].time_since_epoch().count();
      auto rtt_ns = (rpc_recv_time_[i] - rpc_send_time_[i]).count();
      rtt.push_back(rtt_ns);
      if (f) {
        fprintf(f, "%ld %ld\n", send_time_ns, rtt_ns);
      }
    }

    printf("num_packets: %lu\n", num_packets_);
    if (read_size_) {
      printf("mode: READ\n");
      printf("remote_memory_size: %lu\n", remote_memory_size_);
      printf("read_size: %lu\n", read_size_);
    } else {
      printf("mode: SEND/RECV\n");
      printf("msg_size: %lu\n", sizeof(RpcMessage));
    }

    double elapse_s = (finish_time_ - start_time_).count() / 1e9;
    auto bandwidth_gbps =
        sizeof(RpcMessage) * num_packets_ * 8 / 1e9 / elapse_s;
    printf("avg bandwidth: %.3f Gbps\n", bandwidth_gbps);
    double pps = num_packets_ / elapse_s;
    printf("avg rate: %.3f kpps\n", pps / 1e3);

    std::sort(rtt.begin(), rtt.end());
    auto pp = [&rtt, n = num_packets_](double p) {
      auto idx = static_cast<size_t>(std::floor(n * p / 100.));
      printf("p%-5.2f: %-4.0f us\n", p, rtt[idx] / 1e3);
    };
    pp(50);
    pp(75);
    pp(90);
    pp(95);
    pp(99);
    pp(99.5);
    pp(99.9);
    pp(99.95);
    pp(99.99);
  }

 private:
  void SendMore() {
    constexpr size_t kMaxFlying = 10;

    auto last_send = cnt_send_;
    while (cnt_flying_ < kMaxFlying && cnt_send_ < num_packets_) {
      auto send_buf = allocator_->Allocate();
      auto send_view = send_buf.AsMessageView();
      auto *req = reinterpret_cast<RpcMessage *>(send_view.bytes());
      req->seqnum = cnt_send_;
      snprintf(req->msg, sizeof(req->msg), "THIS IS REQUEST SEQNUM=%lu",
               req->seqnum);
      send_view.set_bytes_length(sizeof(*req));
      auto now = Clock::now();
      conn_->AsyncSend(std::move(send_buf));
      rpc_send_time_[req->seqnum] = now;
      ++cnt_flying_;
      ++cnt_send_;
    }
    if (last_send != cnt_send_) {
      ReportProgress(false);
    }
  }

  void ReadOneMore() {
    if (cnt_sent_ == num_packets_) {
      return;
    }
    size_t idx = cnt_sent_;
    auto offset = distrib_(gen_);
    auto wrid = conn_->AsyncRead(allocator_->Allocate(), offset, read_size_);
    auto now = Clock::now();
    wrid_to_idx_[wrid] = idx;
    rpc_send_time_[idx] = now;
    ++cnt_sent_;
    ReportProgress(false);
  }

  void ReportProgress(bool force) {
    auto now = Clock::now();
    auto last_second = std::chrono::duration_cast<std::chrono::seconds>(
                           last_report_time_ - start_time_)
                           .count();
    auto now_second =
        std::chrono::duration_cast<std::chrono::seconds>(now - start_time_)
            .count();
    if (now_second == last_second && !force) {
      return;
    }
    auto nanos = (now - start_time_).count();
    auto seconds = nanos / 1e9;
    auto cnt_sent = cnt_sent_.load();
    fprintf(stderr, "[%3lu%%] Sent %lu/%lu requests in %.6fs. (avg %.3f rps)\n",
            cnt_sent * 100 / num_packets_, cnt_sent, num_packets_, seconds,
            cnt_sent / seconds);
    last_report_time_ = now;
  }

  using Clock = std::chrono::high_resolution_clock;
  using TimePoint = std::chrono::time_point<Clock, std::chrono::nanoseconds>;

  MemoryBlockAllocator *allocator_;
  std::mutex mutex_;
  std::condition_variable cv_;
  RdmaQueuePair *conn_ = nullptr;
  size_t num_packets_ = 0;
  size_t cnt_send_ = 0;
  size_t remote_memory_size_ = 0;
  size_t read_size_ = 0;
  std::atomic<size_t> cnt_flying_;
  std::atomic<size_t> cnt_sent_;
  std::atomic<size_t> cnt_recv_;
  TimePoint start_time_;
  TimePoint finish_time_;
  TimePoint last_report_time_;
  std::unique_ptr<TimePoint[]> rpc_send_time_;
  std::unique_ptr<TimePoint[]> rpc_recv_time_;
  std::unordered_map<WorkRequestID, size_t> wrid_to_idx_;
  std::mt19937 gen_{0xabcdabcd987LL};
  std::uniform_int_distribution<size_t> distrib_;
};

void BenchSendMain(int argc, char **argv) {
  if (argc != 7) DieUsage(argv[0]);
  std::string dev_name = argv[2];
  std::string server_host = argv[3];
  uint16_t server_port = std::stoi(argv[4]);
  size_t num_packets = std::stoul(argv[5]);
  std::string logfilename = argv[6];

  auto handler = std::make_unique<BenchHandler>();
  MemoryBlockAllocator buf(kRdmaBufPoolBits, kRdmaBufBlockBits);
  RdmaManager manager(dev_name, handler.get(), &buf);
  handler->SetAllocator(buf);
  manager.ConnectTcp(server_host, server_port);
  std::thread event_loop_thread(&RdmaManager::RunEventLoop, &manager);

  auto *conn = handler->WaitConnection();
  fprintf(stderr, "BenchSendMain: connected.\n");

  fprintf(stderr, "sleep 1 second\n");
  std::this_thread::sleep_for(std::chrono::milliseconds(1000));
  fprintf(stderr, "start bench\n");
  handler->BenchSend(num_packets, conn);
  handler->SaveAnalysis(logfilename.c_str());

  manager.StopEventLoop();
  event_loop_thread.join();
}

void BenchReadMain(int argc, char **argv) {
  if (argc != 8) DieUsage(argv[0]);
  std::string dev_name = argv[2];
  std::string server_host = argv[3];
  uint16_t server_port = std::stoi(argv[4]);
  size_t num_packets = std::stoul(argv[5]);
  size_t read_size = std::stoul(argv[6]);
  std::string logfilename = argv[7];

  auto handler = std::make_unique<BenchHandler>();
  MemoryBlockAllocator buf(kRdmaBufPoolBits, kRdmaBufBlockBits);
  RdmaManager manager(dev_name, handler.get(), &buf);
  handler->SetAllocator(buf);
  manager.ConnectTcp(server_host, server_port);
  std::thread event_loop_thread(&RdmaManager::RunEventLoop, &manager);

  handler->WaitMemoryRegion();
  fprintf(stderr, "BenchReadMain: got memory region.\n");

  fprintf(stderr, "sleep 1 second\n");
  std::this_thread::sleep_for(std::chrono::milliseconds(1000));
  fprintf(stderr, "start bench\n");
  handler->BenchRead(num_packets, read_size);
  handler->SaveAnalysis(logfilename.c_str());

  manager.StopEventLoop();
  event_loop_thread.join();
}

int main(int argc, char **argv) {
  if (argc < 2) DieUsage(argv[0]);
  if (std::string("server") == argv[1]) {
    ServerMain(argc, argv);
  } else if (std::string("client") == argv[1]) {
    ClientMain(argc, argv);
  } else if (std::string("benchsend") == argv[1]) {
    BenchSendMain(argc, argv);
  } else if (std::string("benchread") == argv[1]) {
    BenchReadMain(argc, argv);
  } else if (std::string("tcpserver") == argv[1]) {
    TcpServerMain(argc, argv);
  } else if (std::string("tcpclient") == argv[1]) {
    TcpClientMain(argc, argv);
  } else {
    DieUsage(argv[0]);
  }
}
