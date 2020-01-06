#pragma once

#include <https/BufferedSocket.h>
#include <https/RunLoop.h>
#include <webrtc/DTLS.h>
#include <webrtc/RTPSender.h>
#include <webrtc/RTPSession.h>
#include <webrtc/SCTPHandler.h>
#include <webrtc/ServerState.h>
#include <webrtc/STUNMessage.h>

#include <memory>
#include <string_view>
#include <vector>

struct MyWebSocketHandler;

struct RTPSocketHandler
    : public std::enable_shared_from_this<RTPSocketHandler> {

    static constexpr size_t kMaxUDPPayloadSize = 1536;

    static constexpr uint32_t TRACK_VIDEO = 1;
    static constexpr uint32_t TRACK_AUDIO = 2;
    static constexpr uint32_t TRACK_DATA  = 4;

    explicit RTPSocketHandler(
            std::shared_ptr<RunLoop> runLoop,
            std::shared_ptr<ServerState> serverState,
            int domain,
            uint16_t port,
            uint32_t trackMask,
            std::shared_ptr<RTPSession> session);

    uint16_t getLocalPort() const;
    std::string getLocalUFrag() const;
    std::string getLocalIPString() const;

    void run();

    void queueDatagram(
            const sockaddr_storage &addr, const void *data, size_t size);

    void queueRTCPDatagram(const void *data, size_t size);
    void queueRTPDatagram(const void *data, size_t size);

    void notifyDTLSConnected();

private:
    struct Datagram {
        explicit Datagram(
                const sockaddr_storage &addr, const void *data, size_t size);

        const void *data() const;
        size_t size() const;

        const sockaddr_storage &remoteAddress() const;

    private:
        std::vector<uint8_t> mData;
        sockaddr_storage mAddr;
    };

    std::shared_ptr<RunLoop> mRunLoop;
    std::shared_ptr<ServerState> mServerState;
    uint16_t mLocalPort;
    uint32_t mTrackMask;
    std::shared_ptr<RTPSession> mSession;

    std::shared_ptr<BufferedSocket> mSocket;
    std::shared_ptr<DTLS> mDTLS;
    std::shared_ptr<SCTPHandler> mSCTPHandler;

    std::deque<std::shared_ptr<Datagram>> mOutQueue;
    bool mSendPending;
    bool mDTLSConnected;

    std::shared_ptr<RTPSender> mRTPSender;

    void onReceive();
    void onDTLSReceive(const uint8_t *data, size_t size);

    void pingRemote(std::shared_ptr<RTPSession> session);

    bool matchesSession(const STUNMessage &msg) const;

    void scheduleDrainOutQueue();
    void drainOutQueue();

    int onSRTPReceive(uint8_t *data, size_t size);
};

