#ifndef _LIVE_RTSP_SERVER_H_
#define _LIVE_RTSP_SERVER_H_
 
#include "liveMedia.hh"
#include "BasicUsageEnvironment.hh"
#include <functional>

 
class H264LiveServerMediaSubsession : public OnDemandServerMediaSubsession
{
public:
    static H264LiveServerMediaSubsession *createNew(UsageEnvironment &env,
                                                    Boolean reuseFirstSource,
                                                    std::function<void()> onRTSPClientEnter,
                                                    std::function<void()> onRTSPClientExit,
                                                    u_int8_t const* sps,
                                                    unsigned spsSize,
                                                    u_int8_t const* pps,
                                                    unsigned ppsSize);
    void checkForAuxSDPLine1();
    void afterPlayingDummy1();
 
protected:
    H264LiveServerMediaSubsession(UsageEnvironment &env,
        Boolean reuseFirstSource,
        std::function<void()> onRTSPClientEnter,
        std::function<void()> onRTSPClientExit,
        u_int8_t const* sps,
        unsigned spsSize,
        u_int8_t const* pps,
        unsigned ppsSize);
    virtual ~H264LiveServerMediaSubsession(void);
    void setDoneFlag() { fDoneFlag = ~0; }
 
protected:
    virtual char const *getAuxSDPLine(RTPSink *rtpSink, FramedSource *inputSource);
    virtual FramedSource *createNewStreamSource(unsigned clientSessionId, unsigned &estBitrate);
    virtual RTPSink *createNewRTPSink(Groupsock *rtpGroupsock,
                                        unsigned char rtpPayloadTypeIfDynamic,
                                        FramedSource *inputSource);
    virtual void startStream(unsigned clientSessionId, void* streamToken,
                            TaskFunc* rtcpRRHandler,
                            void* rtcpRRHandlerClientData,
                            unsigned short& rtpSeqNum,
                            unsigned& rtpTimestamp,
                            ServerRequestAlternativeByteHandler* serverRequestAlternativeByteHandler,
                            void* serverRequestAlternativeByteHandlerClientData);
    virtual void deleteStream(unsigned clientSessionId, void*& streamToken);

private:
    char *fAuxSDPLine_;
    EventLoopWatchVariable fDoneFlag;
    RTPSink *fDummyRTPSink;
    u_int8_t const* sps_;
    unsigned spsSize_;
    u_int8_t const* pps_;
    unsigned ppsSize_;
    std::function<void()> onRTSPClientPlay_;
    std::function<void()> onRTSPClientTeardown_;
};
 
// 创建一个自定义的实时码流数据源类
class H264LiveStreamSource : public FramedSource
{
public:
    static H264LiveStreamSource *createNew(UsageEnvironment &env, std::function<void()> onRTSPClientEnter, std::function<void()> onRTSPClientExit);
    unsigned maxFrameSize() const;
    static void StopGettingFrames(void *clientData);
 
protected:
    H264LiveStreamSource(UsageEnvironment &env, std::function<void()> onRTSPClientEnter, std::function<void()> onRTSPClientExit);
    virtual ~H264LiveStreamSource();
 
private:
    virtual void doGetNextFrame() override;
    virtual void doStopGettingFrames();
    std::function<void()> onRTSPClientPlay_;
    std::function<void()> onRTSPClientTeardown_;
};
 
#endif // _LIVE_RTSP_SERVER_H_