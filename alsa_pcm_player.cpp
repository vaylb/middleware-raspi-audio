#include <iostream>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <alsa/asoundlib.h>
#include <pthread.h>
#include <netinet/in.h>  
#include <netdb.h>  
#include <sys/socket.h>  
#include <sys/wait.h>  
#include <arpa/inet.h> 
#include <sys/time.h> 
#include <net/if.h>
#include <sys/ioctl.h>
#include "NativeBuffer.h"
extern "C"
{
    #include <libavcodec/avcodec.h>
    #include <libavformat/avformat.h>
    #include <libswscale/swscale.h>
}
//#define AVCODEC_MAX_AUDIO_FRAME_SIZE  32768 //192000 [131072/4096 = 32]
using namespace std;

struct WAV_HEADER
{
    char rld[4]; //riff 标志符号
    int rLen; 
    char wld[4]; //格式类型（wave）
    char fld[4]; //"fmt"

    int fLen; //sizeof(wave format matex)
    
    short wFormatTag; //编码格式
    short wChannels; //声道数
    int nSamplesPersec ; //采样频率
    int nAvgBitsPerSample;//WAVE文件采样大小
    short wBlockAlign; //块对齐
    short wBitsPerSample; //WAVE文件采样大小
    
    char dld[4]; //”data“
    int wSampleLength; //音频数据的大小

} wav_header;

struct sockaddr_in recvAddr; 
NativeBuffer* mDataPool; //holds data that are not decoded
NativeBuffer* mPcmPool; //holds pcm data that are decoded from mDataPool
//int mAudioChannels= 0, mAudioFrequency = 0, mAudioFramebits = 0;
int mAudioChannels= 2, mAudioFrequency = 44100, mAudioFramebits = 16;
enum AudioType
{
	TYPE_AAC = 1,
	TYPE_PCM = 2
};
volatile enum AudioType mKAudioType;
volatile bool mExitFlag;
volatile bool mPlayBackFlag; 

pthread_mutex_t mut_full = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t cond_full = PTHREAD_COND_INITIALIZER;
pthread_mutex_t mut_drain = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t cond_drain = PTHREAD_COND_INITIALIZER;

void init();
int read_buffer(void *opaque, uint8_t *buf, int buf_size);
void print_wav_info(FILE *fp);
int set_pcm_play(FILE *fp);
int socket_read(int fd, char *buffer,int length);
void* device_scan_listen_thread(void*);
void* device_scan_join_up_thread(void*);
void* audio_receive_thread(void* data);
void* audio_play_thread(void*);
void* audio_decode_thread(void*);
int get_mac(char* mac);


int main(int argc,char *argv[])
{
    init();

	pthread_t id;
    int i,ret;
    ret=pthread_create(&id,NULL,device_scan_listen_thread,NULL); // 成功返回0，错误返回错误编号
    if(ret!=0) {
        printf ("Create pthread error!\n");
        exit (1);
    }

    /*
    *使一个线程等待另一个线程结束。
    *代码中如果没有pthread_join主线程会很快结束从而使整个进程结束，从而使创建的线程没有机会开始执行就结束了。
    *加入pthread_join后，主线程会一直等待直到等待的线程结束自己才结束，使创建的线程有机会执行
    */
    pthread_join(id,NULL);
    return 0;
}

// buffer init
void init(){
    size_t buffer_size = 1024*4*32;
    mDataPool = new NativeBuffer(buffer_size);
    buffer_size = AVCODEC_MAX_AUDIO_FRAME_SIZE;
    mPcmPool = new NativeBuffer(buffer_size);

    avcodec_register_all();/*注册所有的编码解码器*/
    av_register_all();
    mPlayBackFlag = false;
}

void* device_scan_listen_thread(void*)
{
    printf("start broadcast listen \n");

    int sockListen;  
    if((sockListen = socket(AF_INET, SOCK_DGRAM, 0)) == -1){  
        printf("socket fail\n");  
        pthread_exit(0);   
    }  
    int set = 1;  
    setsockopt(sockListen, SOL_SOCKET, SO_REUSEADDR, &set, sizeof(int));
    struct sockaddr_in serverAddr; 

    memset(&serverAddr, 0, sizeof(struct sockaddr_in));
    memset(&recvAddr, 0, sizeof(struct sockaddr_in));  

    serverAddr.sin_family = AF_INET;  
    serverAddr.sin_port = htons(40001);  
    serverAddr.sin_addr.s_addr = INADDR_ANY;  
    // 必须绑定，否则无法监听  
    if(bind(sockListen, (struct sockaddr *)&serverAddr, sizeof(struct sockaddr)) == -1){  
        printf("bind fail\n");  
        pthread_exit(0); 
    }  
    int recvbytes;  
    char recvbuf[128];  
    socklen_t addrLen = sizeof(struct sockaddr_in);  
    while(1){
        if((recvbytes = recvfrom(sockListen, recvbuf, 128, 0, (struct sockaddr *)&recvAddr, &addrLen)) != -1){  
            recvbuf[recvbytes] = '\0';  
            printf("receive a broadCast messgse:%s, from %s\n", recvbuf,inet_ntoa(recvAddr.sin_addr));
            if(strcmp(recvbuf,"a")==0) {
                pthread_t id;
                int ret;
                ret=pthread_create(&id,NULL,device_scan_join_up_thread,&recvAddr);
                if(ret!=0) {
                    printf ("Create device_scan_join_up_thread error!\n");
                }
                if(!mPlayBackFlag){
                    mExitFlag = false;
                }
            }else if(strcmp(recvbuf,"b")==0){
            	mKAudioType = TYPE_AAC;
				mAudioChannels = 2;
				mAudioFrequency = 44100;
                pthread_t id;
                int ret = pthread_create(&id,NULL,audio_receive_thread,&recvAddr);
                if(ret!=0) {
                    printf ("Create audio_receive_thread error!\n");
                }  
            }else if(strcmp(recvbuf,"e")==0){
            	mKAudioType = TYPE_PCM;
            	mPcmPool = mDataPool;
            	mAudioChannels = 1;
				mAudioFrequency = 48000;
                pthread_t id;
                int ret = pthread_create(&id,NULL,audio_receive_thread,&recvAddr);
                if(ret!=0) {
                    printf ("Create audio_receive_thread error!\n");
                } 
            } else if(strcmp(recvbuf,"f")==0){
                mExitFlag = true; 
            }
                  
        }else{  
            printf("recvfrom fail\n");  
        }  
    }
    close(sockListen);  
}


void* device_scan_join_up_thread(void* data){
    char mac[17];
    get_mac(mac);
    string mac_str = mac;
    string msg = "{\"name\":\"audio_player_01\",\"type\":0}"; //Fixme
    msg.replace(22,2,mac_str);
    std::cout<<"device info:"<<msg<<std::endl;
    int joinFd;  
    if((joinFd = socket(PF_INET, SOCK_DGRAM, 0)) == -1){  
        printf("socket fail\n");  
        pthread_exit(0);  
    }

    int optval = 1;//这个值一定要设置，否则可能导致sendto()失败  
    setsockopt(joinFd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(int));  
    struct sockaddr_in theirAddr = *(struct sockaddr_in*)data;     
    theirAddr.sin_port = htons(40002);  
    int sendBytes;  
    if((sendBytes = sendto(joinFd, msg.c_str(), msg.length(), 0,  
            (struct sockaddr *)&theirAddr, sizeof(struct sockaddr))) == -1){  
        printf("sendto fail, errno=%d\n", errno);  
        pthread_exit(0);  
    }  
    
    close(joinFd); 
}

void* audio_receive_thread(void* data){ 
	int total = 0;
    int mServerFd = socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
    if(mServerFd == -1){
        printf("vaylb-->create socket error:%d",errno);
        pthread_exit(0);
     }

    struct sockaddr_in  mServerAddress = *(struct sockaddr_in*)data;
    mServerAddress.sin_port = htons(40003);
    
    int res = connect(mServerFd,(struct sockaddr*)&mServerAddress, sizeof(mServerAddress));
    if(res == -1){
        printf("vaylb-->connect to server error:%d",errno);
        return 0;
    }

    // read(mServerFd,(void*)&mAudioChannels,sizeof(mAudioChannels));
    // read(mServerFd,(void*)&mAudioFrequency,sizeof(mAudioFrequency));
    // read(mServerFd,(void*)&mAudioFramebits,sizeof(mAudioFramebits));
    // printf("get audio params channels = %d, frequency = %d, perframebits = %d\n",mAudioChannels,mAudioFrequency,mAudioFramebits);
    size_t buffer_size = 4096;
    char * buffer_tmp = (char*)malloc(buffer_size);
    int count = 0;
    while(!mExitFlag && socket_read(mServerFd,buffer_tmp,buffer_size)){
    	
        while(mDataPool->getWriteSpace()<buffer_size) {
        	usleep(2000);
            #if 0
        	pthread_cond_signal(&cond_drain); 
        	pthread_mutex_lock(&mut_full);
        	int timeout_ms = 10;
        	struct timespec abstime;
			struct timeval now;
			gettimeofday(&now, NULL);
			int nsec = now.tv_usec * 1000 + (timeout_ms % 1000) * 1000000;
			abstime.tv_nsec = nsec % 1000000000;
			abstime.tv_sec = now.tv_sec + nsec / 1000000000 + timeout_ms / 1000;
			pthread_cond_timedwait(&cond_full, &mut_full, &abstime);
			//pthread_cond_wait(&cond_full,&mut_full);
			pthread_mutex_unlock(&mut_full);
            #endif
        }
        mDataPool->Write(buffer_tmp,buffer_size);
        // pthread_cond_signal(&cond_drain); 
        memset(buffer_tmp,0,buffer_size);
        count++;
        //printf ("audio_receive_thread count %d\n",count);
        if(count == 8) {
            printf ("Data Pool size %d\n",(int)mDataPool->getReadSpace());
            pthread_t play_thread_id,decode_thread_id;
            int ret = pthread_create(&play_thread_id,NULL,audio_play_thread,NULL);
            if(ret!=0) {
                printf ("Create audio_play_thread error!\n");
            }

            printf ("audio type = %d\n",mKAudioType);
            if(mKAudioType == TYPE_AAC){
            	ret = pthread_create(&decode_thread_id,NULL,audio_decode_thread,NULL);
	            if(ret!=0) {
	                printf ("Create audio_play_thread error!\n");
	        	}
            }
        }
        //printf ("socket receive %d\n",count*4096);
    }
    printf ("--------------socket receive end---------------------\n");
    

    free(buffer_tmp);
    close(mServerFd); 
    pthread_exit(0);
}

void* audio_play_thread(void*)
{
    printf("audio_play_thread start\n");
    mPlayBackFlag = true;
    int rc;
    int ret;
    int size;
    snd_pcm_t* handle; //PCI设备句柄
    snd_pcm_hw_params_t* params;//硬件信息和PCM流配置
    unsigned int val;
    int dir=0;
    snd_pcm_uframes_t frames;
    char *buffer;
    int channels = mAudioChannels;
    int frequency = mAudioFrequency;
    int bit = mAudioFramebits;

    rc=snd_pcm_open(&handle, "default", SND_PCM_STREAM_PLAYBACK, 0);
    if(rc<0)
    {
        perror("\nopen PCM device failed:");
        exit(1);
    }

    snd_pcm_hw_params_alloca(&params); //分配params结构体
    if(rc<0)
    {
        perror("\nsnd_pcm_hw_params_alloca:");
        exit(1);
    }

    rc=snd_pcm_hw_params_any(handle, params);//初始化params
    if(rc<0)
    {
        perror("\nsnd_pcm_hw_params_any:");
        exit(1);
    }

    rc=snd_pcm_hw_params_set_access(handle, params, SND_PCM_ACCESS_RW_INTERLEAVED); //初始化访问权限
    if(rc<0)
    {
        perror("\nsed_pcm_hw_set_access:");
        exit(1);
    }

    //采样位数
    switch(bit/8)
    {
    case 1:snd_pcm_hw_params_set_format(handle, params, SND_PCM_FORMAT_U8);
            break ;
    case 2:snd_pcm_hw_params_set_format(handle, params, SND_PCM_FORMAT_S16_LE);
            break ;
    case 3:snd_pcm_hw_params_set_format(handle, params, SND_PCM_FORMAT_S24_LE);
            break ;

    }

    rc=snd_pcm_hw_params_set_channels(handle, params, channels); //设置声道,1表示单声道，2表示立体声
    if(rc<0)
    {
        perror("\nsnd_pcm_hw_params_set_channels:");
        exit(1);
    }

    val = frequency;
    rc=snd_pcm_hw_params_set_rate_near(handle, params, &val, &dir); //设置频率
    if(rc<0)
    {
        perror("\nsnd_pcm_hw_params_set_rate_near:");
        exit(1);
    }

    rc = snd_pcm_hw_params(handle, params);
    if(rc<0)
    {
	    perror("\nsnd_pcm_hw_params: ");
	    exit(1);
    }

    rc=snd_pcm_hw_params_get_period_size(params, &frames, &dir); /*获取周期长度*/
    if(rc<0)
    {
        perror("\nsnd_pcm_hw_params_get_period_size:");
        exit(1);
    }

    if(channels == 1) size = frames * 2;
    else size = frames * 4; /*4 代表数据快长度*/
    printf("snd_pcm buffer size %d\n",size);
    buffer =(char*)malloc(size);
	//fseek(fp,58,SEEK_SET); //定位歌曲到数据区
    int count = 0;
	while (!mExitFlag)
    {
        memset(buffer,0,sizeof(buffer));
        // ret = fread(buffer, 1, size, fp);
        while(mPcmPool->getReadSpace()<size){
        	//printf("no data can read %d, goto wait\n",count);
        	usleep(1000);
            #if 0
        	pthread_cond_signal(&cond_full); 
        	pthread_mutex_lock(&mut_drain);
        	int timeout_ms = 10;
        	struct timespec abstime;
			struct timeval now;
			gettimeofday(&now, NULL);
			int nsec = now.tv_usec * 1000 + (timeout_ms % 1000) * 1000000;
			abstime.tv_nsec = nsec % 1000000000;
			abstime.tv_sec = now.tv_sec + nsec / 1000000000 + timeout_ms / 1000;
			pthread_cond_timedwait(&cond_drain, &mut_drain, &abstime);
			//pthread_cond_wait(&cond_drain,&mut_drain);
			pthread_mutex_unlock(&mut_drain);
			printf("audio_play_thread restart, can read space %d\n",(int)mDataPool->getReadSpace());
            #endif
        }
        count++;
        ret = mPcmPool->Read(buffer,size);
        if(ret == 0)
        {
            printf("歌曲写入结束\n");
            break;
        }
        else if (ret != size)
        {
        	 printf("ret != size\n");
        }
        //printf ("audio_play_thread count %d\n",count);
        //pthread_cond_signal(&cond_full); 
        //printf("consume 4096, capacity %d\n",(mDataPool->getReadSpace())/4096);
        // 写音频数据到PCM设备 
    	while(ret = snd_pcm_writei(handle, buffer, frames)<0)
       	{
            usleep(4000); 
            if (ret == -EPIPE)
            {
              	/* EPIPE means underrun */
              	printf("underrun occurred\n");
              	//完成硬件参数设置，使设备准备好 
              	snd_pcm_prepare(handle);
            }
            else if (ret < 0)
            {
                printf("error from writei: %s\n",snd_strerror(ret));
            }
        }
	}

	printf("------------------audio_play_thread end--------------------\n");

    // snd_pcm_drain(handle);
    snd_pcm_drop(handle);
    snd_pcm_close(handle);
    free(buffer);
    mPlayBackFlag = false;
    pthread_exit(0);
    return 0;
}

void* audio_decode_thread(void*)
{
	printf("------------------audio_decode_thread run --------------------\n");
    AVFormatContext *pFormatCtx;
    AVCodec *aCodec;
    AVCodecContext * aCodecCtx= NULL;
    pFormatCtx = avformat_alloc_context();

    //init AVIOContext for use of read from memory
    unsigned char *aviobuffer=(unsigned char *)av_malloc(4096);  
    AVIOContext *avio =avio_alloc_context(aviobuffer, 4096,0,NULL,read_buffer,NULL,NULL);  
    pFormatCtx->pb=avio;

    int  len;
    int out_size=AVCODEC_MAX_AUDIO_FRAME_SIZE;
    uint8_t * outbuf;
    int error = 0;
    int audioStream = -1;
    AVPacket packet;
    uint8_t *pktdata;
    int pktsize;

    error = avformat_open_input(&pFormatCtx, NULL, NULL, NULL);

    if(error !=0)
    {
        printf("Couldn't open imput stream, error :%d\n",error);
        return 0;  
    }

    error = avformat_find_stream_info(pFormatCtx,NULL);

    if( error <0)
    {
        printf("Couldn't find stream information error :%d\n",error);
        return 0;
    }

    //dump_format(pFormatCtx, 0, inputfilename, 0);

    for(int i=0; i< pFormatCtx->nb_streams; i++)
    {

        if(pFormatCtx->streams[i]->codec->codec_type == AVMEDIA_TYPE_AUDIO)
        {
            printf("set audio stream to %d\n",i);
            audioStream = i;
        }
    }
    if(audioStream == -1)
    {
        printf("Didn't find a audio stream\n");
    }else printf("find audio stream %d\n",audioStream);

    aCodecCtx=pFormatCtx->streams[audioStream]->codec;

    aCodec = avcodec_find_decoder(aCodecCtx->codec_id);
    if(!aCodec) 
    {
        fprintf(stderr, "Unsupported codec!\n");
        exit(1);
    }
    // Open adio codec
    if(avcodec_open2(aCodecCtx, aCodec,NULL)<0)
    {
        printf("Could not open codec\n");
        return 0;
    }

    outbuf = (uint8_t*)malloc(AVCODEC_MAX_AUDIO_FRAME_SIZE);

    while(!mExitFlag && av_read_frame(pFormatCtx, &packet) >= 0) 
    {
        if(packet.stream_index == audioStream)
        {
            pktdata = packet.data;
            pktsize = packet.size;
            while(pktsize > 0)
            {
                out_size = AVCODEC_MAX_AUDIO_FRAME_SIZE;   
                len = avcodec_decode_audio3(aCodecCtx,(short *)outbuf, &out_size,&packet); 
                if (len < 0) 
                {
                    printf("error len\n");
                    break;
                }
                if (out_size > 0)
                {
                    while(mPcmPool->getWriteSpace()<out_size) {
                        usleep(2000);
                    }
                    //printf("write %d to pool\n",out_size);
                    mPcmPool->Write((char*)outbuf,out_size);
                }
                pktsize -= len;
                pktdata += len;
            }
        } 
        // Free the packet that was allocated by av_read_frame
        av_free_packet(&packet);
    }

    free(outbuf);
    avcodec_close(aCodecCtx);
    av_free(aCodecCtx);
    pthread_exit(0);
}

//Callback  
int read_buffer(void *opaque, uint8_t *buf, int buf_size){
    while(mDataPool->getReadSpace()<buf_size){
        usleep(500);
    }
    return mDataPool->Read((char*)buf,buf_size);
}

void print_wav_info(FILE *fp){
    int nread;
    nread=fread(&wav_header,1,sizeof(wav_header),fp);
    printf("nread=%d\n",nread);
    printf("*************     wav file info    ***************\n");
    printf("文件大小rLen：%d\n",wav_header.rLen);
    printf("声道数：%d\n",wav_header.wChannels);
    printf("采样频率：%d\n",wav_header.nSamplesPersec);
    printf("采样位数：%d\n",wav_header.wBitsPerSample);
    printf("BloakAlign：%d\n",wav_header.wBlockAlign);
    printf("wSampleLength=%d\n",wav_header.wSampleLength);
    printf("************* start playback music ***************\n");
}

int socket_read(int fd, char *buffer,int length) 
{ 
    int i = length;
    int ret = 0;
    while(i > 0 && (ret = read(fd,buffer + (length - i),i)) > 0)
    {
           i -= ret;
    }
    if(i!=0) printf("---------------- socket_read size =%d failed----------------\n",length);
    return (i == 0)?length:0;
}

//获取地址
//返回MAC地址字符串
//返回：0=成功，-1=失败
int get_mac(char* mac)
{
    struct ifreq tmp;
    int sock_mac;
    char mac_addr[30];
    sock_mac = socket(AF_INET, SOCK_STREAM, 0);
    if( sock_mac == -1)
    {
        perror("create socket fail\n");
        return -1;
    }
    memset(&tmp,0,sizeof(tmp));
    strncpy(tmp.ifr_name,"eth0",sizeof(tmp.ifr_name)-1 );
    if( (ioctl( sock_mac, SIOCGIFHWADDR, &tmp)) < 0 )
    {
        printf("mac ioctl error\n");
        return -1;
    }
    sprintf(mac_addr, "%02x:%02x:%02x:%02x:%02x:%02x",
            (unsigned char)tmp.ifr_hwaddr.sa_data[0],
            (unsigned char)tmp.ifr_hwaddr.sa_data[1],
            (unsigned char)tmp.ifr_hwaddr.sa_data[2],
            (unsigned char)tmp.ifr_hwaddr.sa_data[3],
            (unsigned char)tmp.ifr_hwaddr.sa_data[4],
            (unsigned char)tmp.ifr_hwaddr.sa_data[5]
            );
    close(sock_mac);
    memcpy(mac,mac_addr,strlen(mac_addr));
    return 0;
}