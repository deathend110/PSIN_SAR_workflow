#include "live_rtsp_server.hpp"
#include <iostream>
#include "pipeline/base/ring_queue.hpp"

extern ringQueue *rQueue;

// H264LiveServerMediaSubsession 实现：
H264LiveServerMediaSubsession *H264LiveServerMediaSubsession::createNew(
    UsageEnvironment &env,
    Boolean reuseFirstSource,
    std::function<void()> onRTSPClientEnter,
    std::function<void()> onRTSPClientExit,
    u_int8_t const* sps,
    unsigned spsSize,
    u_int8_t const* pps,
    unsigned ppsSize)
{
    // OutPacketBuffer::maxSize = 400000;
    return new H264LiveServerMediaSubsession(env, reuseFirstSource, onRTSPClientEnter, onRTSPClientExit, sps, spsSize, pps, ppsSize);
}

 
H264LiveServerMediaSubsession::H264LiveServerMediaSubsession(UsageEnvironment &env,
                                                            Boolean reuseFirstSource,
                                                            std::function<void()> onRTSPClientEnter,
                                                            std::function<void()> onRTSPClientExit,
                                                            u_int8_t const* sps,
                                                            unsigned spsSize,
                                                            u_int8_t const* pps,
                                                            unsigned ppsSize)
    : OnDemandServerMediaSubsession(env, reuseFirstSource),
    onRTSPClientPlay_(onRTSPClientEnter),
    onRTSPClientTeardown_(onRTSPClientExit),
    sps_(sps),
    spsSize_(spsSize),
    pps_(pps),
    ppsSize_(ppsSize)
{
    std::cout << "============H264LiveServerMediaSubsession created!" << std::endl;
    fAuxSDPLine_ = NULL;
    fDoneFlag = 0;
    fDummyRTPSink = NULL;
}
 
H264LiveServerMediaSubsession::~H264LiveServerMediaSubsession()
{
    std::cout << "============H264LiveServerMediaSubsession deleted!" << std::endl;
    delete[] fAuxSDPLine_;
}
 
static void afterPlayingDummy(void *clientData)
{
    std::cout << "============static void afterPlayingDummy" << std::endl;
    H264LiveServerMediaSubsession *subsess = (H264LiveServerMediaSubsession *)clientData;
    subsess->afterPlayingDummy1();
}
 
void H264LiveServerMediaSubsession::afterPlayingDummy1()
{
    envir().taskScheduler().unscheduleDelayedTask(nextTask());
    setDoneFlag();
}
 
static void checkForAuxSDPLine(void *clientData)
{
    std::cout << "============static void checkForAuxSDPLine" << std::endl;
    H264LiveServerMediaSubsession *subsess = (H264LiveServerMediaSubsession *)clientData;
    subsess->checkForAuxSDPLine1();
}



void H264LiveServerMediaSubsession::checkForAuxSDPLine1()
{
    nextTask() = NULL;
 
    char const *dasl = fDummyRTPSink->auxSDPLine();
    // if(dasl == NULL) dasl = "";

    if (fAuxSDPLine_ != NULL)
    {
        setDoneFlag();
    }
    else if (fDummyRTPSink != NULL && dasl != NULL)
    {
        fAuxSDPLine_ = strDup(dasl);
        fDummyRTPSink = NULL;
        setDoneFlag();
    }
    else if (!fDoneFlag)
    {
        // try again after a brief delay:
        int uSecsToDelay = 100000; // 100 ms
        nextTask() = envir().taskScheduler().scheduleDelayedTask(uSecsToDelay,
                                                                 (TaskFunc *)checkForAuxSDPLine, this);
    }
}

char const *H264LiveServerMediaSubsession::getAuxSDPLine(RTPSink *rtpSink, FramedSource *inputSource)
{
    std::cout << "Processing DESCRIBE request..." << std::endl;

    // Generate the H.264-specific SDP line
    // char const* fmtpFmt = "a=fmtp:%d packetization-mode=1;profile-level-id=42e01f;sprop-parameter-sets=%s,%s\r\n";
    // char const* spropParameterSets = "Z0LgC5ZUCg/I,aMljiA=="; // Example SPS and PPS base64-encoded strings
    // unsigned fmtpLineSize = strlen(fmtpFmt) + 3 /* max char len */ + strlen(spropParameterSets) * 2;
    // char* fmtpLine = new char[fmtpLineSize];
    // sprintf(fmtpLine, fmtpFmt, rtpSink->rtpPayloadType(), spropParameterSets, spropParameterSets);
    // fAuxSDPLine_ = strDup(fmtpLine);
    // fDummyRTPSink = NULL;
    // setDoneFlag();

    if (fAuxSDPLine_ != NULL)
    {
        std::cout << "SDP line exists: " << fAuxSDPLine_ << std::endl;
        return fAuxSDPLine_;
    }
 
    if (fDummyRTPSink == NULL)
    {
        fDummyRTPSink = rtpSink;
        fDummyRTPSink->startPlaying(*inputSource, afterPlayingDummy, this);
        checkForAuxSDPLine(this);
    }
    envir().taskScheduler().doEventLoop(&fDoneFlag);

    return fAuxSDPLine_;
}
 
FramedSource *H264LiveServerMediaSubsession::createNewStreamSource(unsigned clientSessionId, unsigned &estBitrate)
{
    estBitrate = 5000000; // kbps, estimate
 
    H264LiveStreamSource *videoSource = H264LiveStreamSource::createNew(envir(), onRTSPClientPlay_, onRTSPClientTeardown_);
    if (videoSource == NULL)
    {
        return NULL;
    }
    return H264VideoStreamFramer::createNew(envir(), videoSource);
    // return H264VideoStreamDiscreteFramer::createNew(envir(), videoSource);
}

RTPSink *H264LiveServerMediaSubsession::createNewRTPSink(Groupsock *rtpGroupsock,
                                                       unsigned char rtpPayloadTypeIfDynamic,
                                                       FramedSource *inputSource)
{
    std::cout << "============H264LiveServerMediaSubsession::createNewRTPSink!" << std::endl;
    return H264VideoRTPSink::createNew(envir(), rtpGroupsock, rtpPayloadTypeIfDynamic, sps_, spsSize_, pps_, ppsSize_);
    // return H264VideoRTPSink::createNew(envir(), rtpGroupsock, rtpPayloadTypeIfDynamic);
}


void H264LiveServerMediaSubsession::startStream(unsigned clientSessionId, void *streamToken, TaskFunc *rtcpRRHandler, void *rtcpRRHandlerClientData, unsigned short &rtpSeqNum, unsigned &rtpTimestamp, ServerRequestAlternativeByteHandler *serverRequestAlternativeByteHandler, void *serverRequestAlternativeByteHandlerClientData)
{
    std::cout << "===========H264LiveServerMediaSubsession::startStream!" << std::endl;
    // 1) Do parent class logic first
    OnDemandServerMediaSubsession::startStream(clientSessionId,
                                               streamToken,
                                               rtcpRRHandler,
                                               rtcpRRHandlerClientData,
                                               rtpSeqNum,
                                               rtpTimestamp,
                                               serverRequestAlternativeByteHandler,
                                               serverRequestAlternativeByteHandlerClientData);
    if(onRTSPClientPlay_) {
        onRTSPClientPlay_();
    }
}

void H264LiveServerMediaSubsession::deleteStream(unsigned clientSessionId, void *&streamToken)
{
    std::cout << "============H264LiveServerMediaSubsession::deleteStream!" << std::endl;
    // 1) Call parent class logic
    OnDemandServerMediaSubsession::deleteStream(clientSessionId, streamToken);
    if(onRTSPClientTeardown_) {
        onRTSPClientTeardown_();
    }
}

// **********************************************************************************
// H264LiveStreamSource 实现：
H264LiveStreamSource *H264LiveStreamSource::createNew(UsageEnvironment &env, std::function<void()> onRTSPClientEnter, std::function<void()> onRTSPClientExit)
{
    return new H264LiveStreamSource(env, onRTSPClientEnter, onRTSPClientExit);
}
 
H264LiveStreamSource::H264LiveStreamSource(UsageEnvironment &env,
    std::function<void()> onRTSPClientEnter, std::function<void()> onRTSPClientExit)
    : FramedSource(env),
    onRTSPClientPlay_(onRTSPClientEnter),
    onRTSPClientTeardown_(onRTSPClientExit)
{
    std::cout << "============H264LiveStreamSource created!" << std::endl;

}
 
H264LiveStreamSource::~H264LiveStreamSource()
{
    std::cout << "============H264LiveStreamSource deleted!" << std::endl;
}
 
unsigned  int H264LiveStreamSource::maxFrameSize() const
{
    return 1400000; // 设置fMaxSize的值
}

void H264LiveStreamSource::StopGettingFrames(void *clientData)
{
    std::cout << "============H264LiveStreamSource::StopGettingFrames!" << std::endl;
    H264LiveStreamSource *source = (H264LiveStreamSource *)clientData;
    source->doStopGettingFrames();
}

void H264LiveStreamSource::doGetNextFrame()
{
    std::cout << "============H264LiveStreamSource::doGetNextFrame!" << std::endl;
    // 还没准备好要数据
    if (!isCurrentlyAwaitingData())
    {
        std::cout << "isCurrentlyAwaitingData" << std::endl;
        // nextTask() = envir().taskScheduler().scheduleDelayedTask(100000,  // 100ms delay
        //     (TaskFunc*)FramedSource::afterGetting, this);
        return;
    }
    rQueue_data e;
    uint32_t timestamp = 0;
    static uint8_t buffer_data[8000000] = {0};
 
    // 从队列中取出数据
    e.buffer = buffer_data;
    e.len = sizeof(buffer_data);
    if (rQueue_wait_de(rQueue, &e) == -1)
    {
        std::cout << "rQueue_wait_de, should not enter this if" << std::endl;
        fFrameSize = 0;  // Indicate no data was read
        fNumTruncatedBytes = 0;
        nextTask() = envir().taskScheduler().scheduleDelayedTask(4000,  // 16ms delay
            (TaskFunc*)H264LiveStreamSource::StopGettingFrames, this);
        return;
    }
//    get_sys_time_ms_result();
    auto len = rQueue_length(rQueue);
    std::cout << std::dec << "=============rQueue_de(left=" << len << ")=" << e.len << ":" << fMaxSize << std::endl;

    if (e.len > fMaxSize)
    {
        std::cout << "e.len > fMaxSize" << std::endl;
        fFrameSize = fMaxSize;
        fNumTruncatedBytes = e.len - fMaxSize;
    }
    else
    {
        fFrameSize = e.len;
    }
    gettimeofday(&fPresentationTime, NULL);
    memcpy(fTo, buffer_data, fFrameSize);
    // 通知live555，数据已经准备好
    FramedSource::afterGetting(this);
}
 
void H264LiveStreamSource::doStopGettingFrames()
{
    std::cout << "H264LiveStreamSource::doStopGettingFrames" << std::endl;
    envir().taskScheduler().unscheduleDelayedTask(nextTask());
}