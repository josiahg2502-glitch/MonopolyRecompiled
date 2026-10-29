#include "netplay.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>

#include "video_codec.h"
#include "wire.h"

namespace netplay {
namespace {

using Clock = std::chrono::steady_clock;

enum class Msg : uint8_t {
  kHello = 1,       // c->h: u32 version, u8 wanted slot (0 = any), name
  kWelcome = 2,     // h->c: u8 slot
  kReject = 3,      // h->c: reason
  kRoster = 4,      // h->c: per slot u8 connected, u8 len, name
  kInput = 5,       // c->h: Gamepad (12 bytes LE)
  kVideo = 6,       // h->c: u32 width, u32 height, u8 keyframe, Annex B
  kAudio = 7,       // h->c: int16 stereo 48 kHz
  kKeyframeRequest = 8,
};

constexpr size_t kMaxQueuedBytes = 2u << 20;  // per client; beyond this video is dropped until a keyframe
constexpr uint32_t kStreamFps = 30;
constexpr uint32_t kStreamBitrate = 4'000'000;
constexpr auto kInputResend = std::chrono::milliseconds(100);

using namespace wire;

std::vector<uint8_t> Pack(Msg type, const void* payload, size_t size) { return wire::Pack(uint8_t(type), payload, size); }

bool ReadMessage(SOCKET s, Msg* type, std::vector<uint8_t>* payload) {
  uint8_t t;
  if (!wire::ReadMessage(s, &t, payload)) return false;
  *type = Msg(t);
  return true;
}

std::vector<uint8_t> PackRoster(const Roster& r) {
  std::vector<uint8_t> out;
  for (int i = 0; i < kMaxPlayers; ++i) {
    out.push_back(r.connected[i]);
    out.push_back(uint8_t(r.names[i].size()));
    out.insert(out.end(), r.names[i].begin(), r.names[i].end());
  }
  return out;
}

bool UnpackRoster(const std::vector<uint8_t>& in, Roster* r) {
  size_t p = 0;
  for (int i = 0; i < kMaxPlayers; ++i) {
    if (p + 2 > in.size()) return false;
    r->connected[i] = in[p] != 0;
    size_t len = in[p + 1];
    p += 2;
    if (p + len > in.size()) return false;
    r->names[i].assign(reinterpret_cast<const char*>(in.data() + p), len);
    p += len;
  }
  return true;
}

// ---------------------------------------------------------------------------------------------
// Host

class HostImpl final : public Host {
 public:
  struct Peer {
    SOCKET socket = INVALID_SOCKET;
    int slot = 0;
    std::thread reader, writer;
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<std::vector<uint8_t>> queue;
    size_t queued_bytes = 0;
    bool closing = false;
    bool waiting_for_keyframe = true;
    std::atomic<bool> ready{false};  // writer thread exists
    std::atomic<bool> dead{false};
  };

  ~HostImpl() override {
    stopping_ = true;
    closesocket(listener_);
    if (acceptor_.joinable()) acceptor_.join();
    {
      std::lock_guard lock(encode_mutex_);
      encode_cv_.notify_all();
    }
    if (encoder_thread_.joinable()) encoder_thread_.join();
    std::vector<std::shared_ptr<Peer>> peers;
    {
      std::lock_guard lock(peers_mutex_);  // not held while joining: readers broadcast under it
      peers.swap(peers_);
    }
    for (auto& peer : peers) StopPeer(*peer);
  }

  bool Start(uint16_t port, std::string host_name, std::string* error) {
    host_name_ = Sanitize(std::move(host_name));
    roster_.connected[0] = true;
    roster_.names[0] = host_name_;
    listener_ = Listen(port, error);
    if (listener_ == INVALID_SOCKET) return false;
    acceptor_ = std::thread([this] { AcceptLoop(); });
    encoder_thread_ = std::thread([this] { EncodeLoop(); });
    return true;
  }

  bool GetInput(int slot, Gamepad* out) override {
    std::lock_guard lock(state_mutex_);
    if (slot <= 0 || slot >= kMaxPlayers || !roster_.connected[slot]) return false;
    *out = pads_[slot];
    return true;
  }

  void SubmitVideo(const uint8_t* bgra, uint32_t width, uint32_t height, uint32_t pitch) override {
    if (!active_peers_) return;
    auto now = Clock::now();
    if (now < next_frame_time_) return;
    next_frame_time_ = std::max(next_frame_time_ + std::chrono::microseconds(1'000'000 / kStreamFps),
                                now - std::chrono::milliseconds(100));
    width &= ~1u, height &= ~1u;  // the encoder needs even dimensions
    std::unique_lock lock(encode_mutex_, std::try_to_lock);
    if (!lock.owns_lock() || pending_ready_) return;  // encoder busy: drop this frame
    pending_.width = width;
    pending_.height = height;
    pending_.bgra.resize(size_t(width) * height * 4);
    for (uint32_t y = 0; y < height; ++y)
      memcpy(pending_.bgra.data() + size_t(y) * width * 4, bgra + size_t(y) * pitch, size_t(width) * 4);
    pending_ready_ = true;
    encode_cv_.notify_one();
  }

  void SubmitAudio(const int16_t* samples, uint32_t frames) override {
    if (!active_peers_ || !frames) return;
    // shortcut: raw PCM is ~1.5 Mbit/s per client; switch to the MF AAC encoder if upload bandwidth is a problem.
    Broadcast(Pack(Msg::kAudio, samples, size_t(frames) * 2 * sizeof(int16_t)), false);
  }

  Roster GetRoster() override {
    std::lock_guard lock(state_mutex_);
    return roster_;
  }

  int PeerCount() const override { return active_peers_; }

  void SetInfoReply(std::function<std::vector<uint8_t>()> reply) override {
    std::lock_guard lock(state_mutex_);
    info_reply_ = std::move(reply);
  }

 private:
  void AcceptLoop() {
    while (!stopping_) {
      SOCKET s = accept(listener_, nullptr, nullptr);
      if (s == INVALID_SOCKET) continue;  // listener closed during shutdown, or a transient error
      Reap();
      SetNoDelay(s);
      auto peer = std::make_shared<Peer>();
      peer->socket = s;
      std::lock_guard lock(peers_mutex_);
      peers_.push_back(peer);
      peer->reader = std::thread([this, peer] { ReadLoop(peer); });
    }
  }

  void Reap() {
    std::vector<std::shared_ptr<Peer>> dead;
    {
      std::lock_guard lock(peers_mutex_);
      std::erase_if(peers_, [&](auto& peer) { return peer->dead && (dead.push_back(peer), true); });
    }
    for (auto& peer : dead) StopPeer(*peer);
  }

  void StopPeer(Peer& peer) {
    Abort(peer.socket);
    {
      std::lock_guard lock(peer.mutex);
      peer.closing = true;
      peer.cv.notify_all();
    }
    if (peer.reader.joinable()) peer.reader.join();
    if (peer.writer.joinable()) peer.writer.join();
    closesocket(peer.socket);
  }

  void ReadLoop(std::shared_ptr<Peer> peer) {
    Msg type;
    std::vector<uint8_t> payload;
    SetRecvTimeout(peer->socket, 5000);
    if (!ReadMessage(peer->socket, &type, &payload)) {
      peer->dead = true;
      return;
    }
    if (uint8_t(type) == kLobbyHello) {  // a launcher looking for a lobby: the game has already started
      SendMsg(peer->socket, kLobbyReject, kInGameReason.data(), kInGameReason.size());
      peer->dead = true;
      return;
    }
    if (uint8_t(type) == kInfoQuery) {  // someone browsing public games
      std::function<std::vector<uint8_t>()> reply;
      {
        std::lock_guard lock(state_mutex_);
        reply = info_reply_;  // called outside the lock: it reads the roster
      }
      auto bytes = reply ? reply() : std::vector<uint8_t>{};
      if (!bytes.empty()) SendMsg(peer->socket, kInfoReply, bytes.data(), bytes.size());
      peer->dead = true;
      return;
    }
    if (type != Msg::kHello || payload.size() < 5) {
      peer->dead = true;
      return;
    }
    SetRecvTimeout(peer->socket, 0);
    uint32_t version;
    memcpy(&version, payload.data(), 4);
    int wanted = payload[4];  // the seat this player had in the lobby
    std::string name = Sanitize(std::string(payload.begin() + 5, payload.end()));
    std::string reject;
    {
      std::lock_guard lock(state_mutex_);
      if (version != kProtocolVersion) {
        reject = "version mismatch";
      } else {
        if (wanted > 0 && wanted < kMaxPlayers && !roster_.connected[wanted]) peer->slot = wanted;
        for (int i = 1; i < kMaxPlayers && !peer->slot; ++i)
          if (!roster_.connected[i]) peer->slot = i;
        if (!peer->slot) reject = "game is full";
      }
      if (peer->slot) {
        roster_.connected[peer->slot] = true;
        roster_.names[peer->slot] = name;
        pads_[peer->slot] = {};
      }
    }
    if (!reject.empty()) {
      auto msg = Pack(Msg::kReject, reject.data(), reject.size());
      SendAll(peer->socket, msg.data(), msg.size());
      peer->dead = true;
      return;
    }
    uint8_t slot = uint8_t(peer->slot);
    auto welcome = Pack(Msg::kWelcome, &slot, 1);
    if (!SendAll(peer->socket, welcome.data(), welcome.size())) {
      Leave(*peer);
      return;
    }
    peer->writer = std::thread([this, peer] { WriteLoop(*peer); });
    peer->ready = true;
    ++active_peers_;
    force_keyframe_ = true;
    BroadcastRoster();

    while (ReadMessage(peer->socket, &type, &payload)) {
      if (type == Msg::kInput && payload.size() == sizeof(Gamepad)) {
        std::lock_guard lock(state_mutex_);
        memcpy(&pads_[peer->slot], payload.data(), sizeof(Gamepad));
      } else if (type == Msg::kKeyframeRequest) {
        force_keyframe_ = true;
      }
    }
    --active_peers_;
    Leave(*peer);
  }

  void Leave(Peer& peer) {
    {
      std::lock_guard lock(state_mutex_);
      roster_.connected[peer.slot] = false;
      roster_.names[peer.slot].clear();
      pads_[peer.slot] = {};
    }
    {
      std::lock_guard lock(peer.mutex);
      peer.closing = true;
      peer.cv.notify_all();
    }
    BroadcastRoster();
    peer.dead = true;  // last: Reap may join this thread once it is set
  }

  void WriteLoop(Peer& peer) {
    for (;;) {
      std::vector<uint8_t> msg;
      {
        std::unique_lock lock(peer.mutex);
        peer.cv.wait(lock, [&] { return peer.closing || !peer.queue.empty(); });
        if (peer.closing) return;
        msg = std::move(peer.queue.front());
        peer.queue.pop_front();
        peer.queued_bytes -= msg.size();
      }
      if (!SendAll(peer.socket, msg.data(), msg.size())) {
        shutdown(peer.socket, SD_BOTH);  // wakes the reader, which cleans up
        return;
      }
    }
  }

  // Video frames are droppable; control messages always go out.
  void Broadcast(const std::vector<uint8_t>& msg, bool control, bool keyframe = false) {
    std::lock_guard lock(peers_mutex_);
    for (auto& peer : peers_) {
      if (!peer->ready || peer->dead) continue;
      std::lock_guard peer_lock(peer->mutex);
      if (!control) {
        bool is_video = msg[4] == uint8_t(Msg::kVideo);
        if (is_video && peer->waiting_for_keyframe && !keyframe) continue;
        if (peer->queued_bytes + msg.size() > kMaxQueuedBytes) {
          if (is_video) {
            peer->waiting_for_keyframe = true;
            force_keyframe_ = true;
          }
          continue;
        }
        if (is_video && keyframe) peer->waiting_for_keyframe = false;
      }
      peer->queue.push_back(msg);
      peer->queued_bytes += msg.size();
      peer->cv.notify_one();
    }
  }

  void BroadcastRoster() {
    Roster r = GetRoster();
    auto bytes = PackRoster(r);
    Broadcast(Pack(Msg::kRoster, bytes.data(), bytes.size()), true);
  }

  void EncodeLoop() {
    bool mf = StartMediaFoundation();
    std::unique_ptr<VideoEncoder> encoder;
    uint32_t enc_w = 0, enc_h = 0;
    Frame frame;
    std::vector<uint8_t> bits;
    while (!stopping_) {
      {
        std::unique_lock lock(encode_mutex_);
        encode_cv_.wait(lock, [&] { return stopping_ || pending_ready_; });
        if (stopping_) break;
        std::swap(frame, pending_);
        pending_ready_ = false;
      }
      if (!mf) continue;
      if (!encoder || enc_w != frame.width || enc_h != frame.height) {
        encoder = VideoEncoder::Create(frame.width, frame.height, kStreamFps, kStreamBitrate);
        enc_w = frame.width, enc_h = frame.height;
        if (!encoder) continue;
      }
      bool keyframe = false;
      bool force = force_keyframe_.exchange(false);
      if (!encoder->Encode(frame.bgra.data(), frame.width * 4, force, &bits, &keyframe) || bits.empty()) continue;
      std::vector<uint8_t> payload(9 + bits.size());
      memcpy(payload.data(), &frame.width, 4);
      memcpy(payload.data() + 4, &frame.height, 4);
      payload[8] = keyframe;
      memcpy(payload.data() + 9, bits.data(), bits.size());
      Broadcast(Pack(Msg::kVideo, payload.data(), payload.size()), false, keyframe);
    }
    encoder.reset();
  }

  SOCKET listener_ = INVALID_SOCKET;
  std::string host_name_;
  std::atomic<bool> stopping_{false};
  std::thread acceptor_, encoder_thread_;

  std::mutex peers_mutex_;
  std::vector<std::shared_ptr<Peer>> peers_;
  std::atomic<int> active_peers_{0};

  std::mutex state_mutex_;
  Roster roster_;
  Gamepad pads_[kMaxPlayers];
  std::function<std::vector<uint8_t>()> info_reply_;

  std::mutex encode_mutex_;
  std::condition_variable encode_cv_;
  Frame pending_;
  bool pending_ready_ = false;
  std::atomic<bool> force_keyframe_{true};
  Clock::time_point next_frame_time_{};
};

// ---------------------------------------------------------------------------------------------
// Client

class ClientImpl final : public Client {
 public:
  ~ClientImpl() override {
    stopping_ = true;
    Abort(socket_);
    if (reader_.joinable()) reader_.join();
    closesocket(socket_);
  }

  bool Connect(const std::string& address, uint16_t port, std::string name, int wanted_slot, Callbacks callbacks,
               std::string* error) {
    callbacks_ = std::move(callbacks);
    socket_ = wire::Connect(address, port, error);
    if (socket_ == INVALID_SOCKET) return false;

    name = Sanitize(std::move(name));
    std::vector<uint8_t> hello(5 + name.size());
    memcpy(hello.data(), &kProtocolVersion, 4);
    hello[4] = uint8_t(wanted_slot);
    memcpy(hello.data() + 5, name.data(), name.size());
    auto msg = Pack(Msg::kHello, hello.data(), hello.size());
    if (!SendAll(socket_, msg.data(), msg.size())) return *error = "connection lost", false;

    SetRecvTimeout(socket_, 5000);
    Msg type;
    std::vector<uint8_t> payload;
    if (!ReadMessage(socket_, &type, &payload)) return *error = "no answer from host", false;
    if (type == Msg::kReject) return *error = "host refused: " + std::string(payload.begin(), payload.end()), false;
    if (type != Msg::kWelcome || payload.size() != 1 || !payload[0] || payload[0] >= kMaxPlayers)
      return *error = "bad answer from host", false;
    slot_ = payload[0];
    SetRecvTimeout(socket_, 0);
    reader_ = std::thread([this] { ReadLoop(); });
    return true;
  }

  int slot() const override { return slot_; }

  void SendInput(const Gamepad& pad) override {
    auto now = Clock::now();
    std::lock_guard lock(send_mutex_);
    if (pad == last_pad_ && now - last_send_ < kInputResend) return;
    last_pad_ = pad;
    last_send_ = now;
    auto msg = Pack(Msg::kInput, &pad, sizeof(pad));
    SendAll(socket_, msg.data(), msg.size());  // a failure surfaces as a disconnect in the reader
  }

 private:
  void ReadLoop() {
    bool mf = StartMediaFoundation();
    auto decoder = mf ? VideoDecoder::Create() : nullptr;
    Frame frame;
    Msg type;
    std::vector<uint8_t> payload;
    std::string reason = "host closed the game";
    while (ReadMessage(socket_, &type, &payload)) {
      switch (type) {
        case Msg::kVideo: {
          if (payload.size() < 9 || !decoder) break;
          uint32_t w, h;
          memcpy(&w, payload.data(), 4);
          memcpy(&h, payload.data() + 4, 4);
          if (decoder->Decode(payload.data() + 9, payload.size() - 9, w, h, &frame)) {
            if (callbacks_.on_video) callbacks_.on_video(frame);
          }
          break;
        }
        case Msg::kAudio:
          if (callbacks_.on_audio && payload.size() >= 4)
            callbacks_.on_audio(reinterpret_cast<const int16_t*>(payload.data()), uint32_t(payload.size() / 4));
          break;
        case Msg::kRoster: {
          Roster r;
          if (UnpackRoster(payload, &r) && callbacks_.on_roster) callbacks_.on_roster(r);
          break;
        }
        default:
          break;
      }
    }
    if (stopping_) return;
    if (callbacks_.on_disconnect) callbacks_.on_disconnect(reason);
  }

  SOCKET socket_ = INVALID_SOCKET;
  int slot_ = 0;
  Callbacks callbacks_;
  std::thread reader_;
  std::atomic<bool> stopping_{false};
  std::mutex send_mutex_;
  Gamepad last_pad_;
  Clock::time_point last_send_{};
};

}  // namespace

std::unique_ptr<Host> Host::Start(uint16_t port, std::string host_name, std::string* error) {
  if (!StartWinsock()) return *error = "WSAStartup failed", nullptr;
  auto host = std::make_unique<HostImpl>();
  return host->Start(port, std::move(host_name), error) ? std::move(host) : nullptr;
}

std::unique_ptr<Client> Client::Connect(const std::string& address, uint16_t port, std::string name, int wanted_slot,
                                        Callbacks callbacks, std::string* error) {
  if (!StartWinsock()) return *error = "WSAStartup failed", nullptr;
  auto client = std::make_unique<ClientImpl>();
  return client->Connect(address, port, std::move(name), wanted_slot, std::move(callbacks), error)
             ? std::move(client)
             : nullptr;
}

}  // namespace netplay
