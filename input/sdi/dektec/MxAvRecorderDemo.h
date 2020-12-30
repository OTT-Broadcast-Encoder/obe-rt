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

        // Operations
    public:
        inline bool IsOpen() const { MxAutoCritSec L(m_pLock);  return (m_pFile!=NULL); }
        inline __int64  Size() const { MxAutoCritSec L(m_pLock); return m_Size; }
        inline AvType Type() const { MxAutoCritSec L(m_pLock); return m_Type; }
        inline int  Index() const { MxAutoCritSec L(m_pLock); return m_Index; }

        // Open new file for stream
        bool  Open(const char*  Filename)
        {
            // Lock the stream
            MxAutoCritSec  Lock(m_pLock);

            if (m_pFile != NULL)
                Close();

            // Create file
            m_pFile = ::fopen(Filename, "wb");
            m_Size = 0;
            return (m_pFile != NULL);
        }
        // Close file
        void Close()
        {
            // Lock the stream
            MxAutoCritSec  Lock(m_pLock);

            if (m_pFile != NULL)
            {
                ::fclose(m_pFile); m_pFile = NULL;
                m_Size = 0;
            }
        }
        // Write data to file
        bool  Write(const unsigned char*  pBuf, int  NumToWrite)
        {
            // Lock the stream
            MxAutoCritSec  Lock(m_pLock);

            if (!IsOpen())
                return false;
            
            ::fwrite(pBuf, 1, NumToWrite, m_pFile);
            // Update size
            m_Size += NumToWrite;
            return true;
        }
    protected:
        bool  Init()
        {
            MX_ASSERT(m_pLock == NULL);
            m_pLock = new MxCritSec;
            return m_pLock->Init();
        }
        void  Cleanup()
        {
            // Close file
            Close();

            // Free lock
            if (m_pLock != NULL)
            {
                m_pLock->Close();
                delete m_pLock; m_pLock = NULL;
            }
        }

        // Data / Attributes
    public:
        // Video properties
        int  m_Height, m_Width; // Video height and width in pixels

        // Audio properties
        int  m_SampleSize;      // Audio sample size (in # bits)
        int  m_NumChannels;     // Number of audio channels stroed in stream
    protected:
        AvType  m_Type;         // Type of stream
        int  m_Index;           // Stream index. // NOTE: video uses index -1 and for
                                // audio the index indicates the audio channel/service
        FILE*  m_pFile;         // Record file
        __int64  m_Size;        // #bytes recorded

        IMxCritSec* m_pLock;    // Lock protecting internal state variables

        // Constructor / Destructor
    public:
        AvStream(AvType  T, int  Index=-1) 
            : m_Type(T),
              m_Index(Index), 
              m_Width(0), m_Height(0), 
              m_SampleSize(0), m_NumChannels(0),
              m_pFile(NULL), 
              m_Size(0),
              m_pLock(NULL)
        { 
            Init(); 
        }
        ~AvStream() { Cleanup(); }
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


    // Operations
public:
    virtual bool  Init(DtDevice&  TheCard);
    virtual bool  Detect(DtDevice &TheCard);
    virtual bool  Start();
    virtual void  Stop();

    void *userContext;

    void *userPriv;

	void ParseCommandLine(int argc, char* argv[]);

    // Command-line option getters
    int  CARD_TYPE() const { return m_OptCardType; }
    int  CARD_NO() const { return m_OptCardNo; }
    //int  IN_PORT() const { return m_OptInPort; }
    int  IN_VIDSTD() const { return m_OptInVidStd; }
    AudioMode  AUDMODE() const { return (AudioMode)m_OptAudMode; }
    
protected:
    bool  PrepCard(DtDevice &TheCard);
    static void  OnNewFrame(DtMxData* pData, void* pOpaque);
    void  OnNewFrame(DtMxData* pData);

    static void  RecordLoopEntry(void*  pContext);
    void  RecordLoop();

    // Get a free buffer from the buffer list. A free buffer is a buffer who's stream 
    // pointer has not been set
    inline AvStreamBuf*  GetFreeBuf(std::list<AvStreamBuf>&  BufList) const
    {
        AvStreamBuf*  pFreeBuf = NULL;
        std::list<AvStreamBuf>::iterator  it=BufList.begin();
        for (; it!=BufList.end() && pFreeBuf==NULL; it++)
        {
            m_pRecLock->Lock();
            if (it->m_pStream == NULL)
                pFreeBuf = &(*it);
            m_pRecLock->Unlock();
        }
        return pFreeBuf;
    }

    // Data / Attributes
public:
protected:
    // Command-line options
    int  m_OptCardType;       // -t: Card type (e.g. 2152=DTA-2152)
    int  m_OptCardNo;         // -n: Card number (0=first, 1=second, ... , etc)
    int  m_OptInPort;         // -p: Input port 
    int  m_OptInVidStd;       // -v: input video standard
    int  m_OptAudMode;        // -a: audio mode 

    // Recording data
    IMxThread*  m_pRecThread;   // Recording thread
    volatile  bool  m_Exit;     // Exit record loop
    IMxEvent*  m_pRecEvent;     // Data added to record queue event
    IMxCritSec*  m_pRecLock;    // State lock to protect record data

    AvStreamBufPtrList  m_RecQueue;  // Queue with buffers to record
    AvStreamBufList  m_VidBuffers;  // List with free video buffers
    AvStreamBufList  m_AudBuffers;  // List with free audio buffers

    AvStreamPtrVec  m_VidStreams;  // List with state per video stream
    AvStreamPtrVec  m_AudStreams;  // List with state per audio streams
    
    // Constructor / Destructor
public:
    MxAvRecorderDemo();
    virtual ~MxAvRecorderDemo();
};

#endif // #ifndef  __MXAVRECORDERDEMO_H
