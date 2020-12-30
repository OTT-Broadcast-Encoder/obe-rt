#ifndef  __MXAVRECORDERDEMO_H
#define  __MXAVRECORDERDEMO_H

#include "MxExamplesCommon.h"
#include <list>

class MxAvRecorderDemo : public MxDemoMatrixBase
{
    // Constants / Types
public:
    enum  AudioMode
    {
        AUDMODE_CHANNEL_32B,    // Record per channel as 32-bit samples
        AUDMODE_SERVICE_16B,    // Record as service with interleaved 16-bit samples
        AUDMODE_SERVICE_24B,    // Record as service with interleaved 24-bit samples
        AUDMODE_SERVICE_32B     // Record as service with interleaved 32-bit samples
    };

    //.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.- class AvStream -.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.
    // Object maintaning the state information for a AV stream
    class  AvStream
    {
        // Types / Constants
    public:
        enum  AvType {
            VIDEO_STREAM,
            AUDIO_STREAM
        };

    protected:

        // Constructor / Destructor
    public:
        AvStream(AvType  T, int  Index=-1) 
        {
        }
        ~AvStream() { }
    private:
        AvStream(const AvStream&);  // No copy constructor allowed
    };
    typedef std::vector<AvStream*>  AvStreamPtrVec;


    //.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.- class AvStreamBuf -.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-
    // Object describing a data buffer for a stream
    struct AvStreamBuf
    {
        // Operations:
    public:
        bool  Alloc(int  Size) 
        {
            if (m_pBuf != NULL)
                return false;
            m_pBuf = new unsigned char [Size];
            m_Size = Size;
            m_NumValid = 0;
            return true;
        }
        void Free()
        {
            if (m_pBuf != NULL)
            {
                delete [] m_pBuf; m_pBuf = NULL;
            }
        }
        void  Clear()
        {
            m_pStream = NULL;
            m_NumValid = 0;
        }

        // Data / Attributes
    public:
        AvStream*  m_pStream; // Stream the data belong to

        unsigned char*  m_pBuf; // Pointer to buffer 
        int  m_Size;            // Size of the buffer
        int  m_NumValid;        // # of valid bytes in buffer

        __int64  m_Frame;       // Frame number

        // Constructor / Destructor
    public:
        AvStreamBuf() : m_pStream(NULL), m_pBuf(NULL), m_Size(0), m_NumValid(0) {}
    };
    typedef std::list<AvStreamBuf>  AvStreamBufList;
    typedef std::list<AvStreamBuf*>  AvStreamBufPtrList;

public:
    virtual bool  Init(DtDevice&  TheCard);
    virtual bool  Detect(DtDevice &TheCard);
    virtual bool  Start();
    virtual void  Stop();

    void *userContext;

    void *userPriv;

protected:
    bool  PrepCard(DtDevice &TheCard);
    static void  OnNewFrame(DtMxData* pData, void* pOpaque);
    void  OnNewFrame(DtMxData* pData);

    // Get a free buffer from the buffer list. A free buffer is a buffer who's stream 
    // pointer has not been set
    inline AvStreamBuf*  GetFreeBuf(std::list<AvStreamBuf>&  BufList) const
    {
        AvStreamBuf*  pFreeBuf = NULL;
        std::list<AvStreamBuf>::iterator  it=BufList.begin();
        for (; it!=BufList.end() && pFreeBuf==NULL; it++)
        {
            if (it->m_pStream == NULL)
                pFreeBuf = &(*it);
        }
        return pFreeBuf;
    }

protected:
    AvStreamBufPtrList  m_RecQueue;  // Queue with buffers to record

    AvStreamBufList  m_VidBuffers;  // List with free video buffers
    AvStreamBufList  m_AudBuffers;  // List with free audio buffers

    AvStreamPtrVec  m_VidStreams;  // List with state per video stream
    AvStreamPtrVec  m_AudStreams;  // List with state per audio streams
    
public:
    MxAvRecorderDemo();
    virtual ~MxAvRecorderDemo();
};

#endif // #ifndef  __MXAVRECORDERDEMO_H
