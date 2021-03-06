#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <linux/videodev2.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <sys/time.h>
#include <linux/fb.h>
#include <signal.h>

#define __STDC_CONSTANT_MACROS
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/rational.h>

#include "video.h"
#include "encoder.h"
#include "VOD_network_packet.h"
/* ------------------------------------------------------------ */
#define NET_PORT        5000
#define NET_HOST        "140.125.33.214"
#define NET_BUFFER_SIZE 1028
/* ------------------------------------------------------------ */
#define SYS_STATUS_INIT	1
#define SYS_STATUS_WORK	2
#define SYS_STATUS_RELEASE	4


#define SYS_CAPABILITY_VIDEO_Tx	1
#define SYS_CAPABILITY_VIDEO_Rx	2
#define SYS_CAPABILITY_AUDIO_Tx	4
#define SYS_CAPABILITY_AUDIO_Rx	8

struct system_information
{
	int status ;
	int capability;
	struct webcam_info cam;
	int left ;
	int top ;
};
/* ------------------------------------------------------------ */
int FB ;
unsigned char *FB_ptr =NULL;
int FB_scerrn_size = 0;
struct fb_var_screeninfo var_info ;
struct fb_fix_screeninfo fix_info ;

unsigned char *RGB565_buffer =NULL;

struct system_information sys_info ;
int Rx_thread ;
int Tx_thread ;
/* ------------------------------------------------------------ */
/* Frame Buffer initial
 *
 */
int FB_init()
{
	FB = open("/dev/fb0", O_RDWR);
	if(!FB) {
		fprintf(stderr, "open /dev/fb0 error\n");
		return 0;
	}

	/* get frame buffer information  */
//	if(ioctl(FB, FBIOGET_FSCREENINFO, &fix_info)) {
//		fprintf(stderr, "FBIOGET_FSCREENINFO  error\n");
//		return 0;
//	}

	if(ioctl(FB, FBIOGET_VSCREENINFO, &var_info)) {
		fprintf(stderr, "FBIOGET_VSCREENINFO  error\n");
		return 0;
	}

	FB_scerrn_size = (var_info.xres * var_info.yres * var_info.bits_per_pixel) / 8;
	printf("frame buffer : %d %d, %dbpp\n",var_info.xres, var_info.yres, var_info.bits_per_pixel);
	printf("screensize : %d\n", FB_scerrn_size);

	FB_ptr =(unsigned char*)mmap(0, FB_scerrn_size, PROT_READ | PROT_WRITE, MAP_SHARED, FB, 0);
	if(FB_ptr == NULL){
		fprintf(stderr, "mmap error\n");
		return 0;
	}
	return FB;
}
/* ------------------------------------------------------------ */
/* Buffering data to frame buffer for display
 * 
 */
void FB_display(unsigned char *RGB565_data_ptr,int top, int left, int width, int height)
{
	/* Direct display in localhost FrameBuffer*/
	int x , y ;
	int index_monitor_1 = 0;
	int index_monitor_2 =0;
	int index_monitor_3 =0;
	int index_monitor_4 =0;
	int index_frame = 0;
/*
	for(y = 0 ; y < height ; y++) {
		for(x = 0 ; x < width ; x++) {
			index_monitor_1 = (y * var_info.xres + x) *2;
			index_frame = (y * sys_info.cam.width + x) *2;
			*(FB_ptr + index_monitor_1) = *(RGB565_buffer + index_frame);
			*(FB_ptr + index_monitor_1 +1) = *(RGB565_buffer +index_frame +1);
		}
	}
*/

	for(y = 0 ; y < height ; y++) {
		for(x = 0 ; x < width ; x++) {
			index_monitor_1 = (y*2 * var_info.xres + x*2) *2;
			index_monitor_2 = (y*2 * var_info.xres + (x*2+1)) *2;
			index_monitor_3 = ((y*2+1) * var_info.xres + x*2) *2;
			index_monitor_4 = ((y*2+1) * var_info.xres + (x*2+1)) *2;
			index_frame = (y * sys_info.cam.width + x) *2;

			*(FB_ptr + index_monitor_1) = *(RGB565_buffer + index_frame);
			*(FB_ptr + index_monitor_1 +1) = *(RGB565_buffer +index_frame +1);

			*(FB_ptr + index_monitor_2) = *(RGB565_buffer + index_frame);
			*(FB_ptr + index_monitor_2 +1) = *(RGB565_buffer +index_frame +1);


			*(FB_ptr + index_monitor_3) = *(RGB565_buffer + index_frame);
			*(FB_ptr + index_monitor_3 +1) = *(RGB565_buffer +index_frame +1);


			*(FB_ptr + index_monitor_4) = *(RGB565_buffer + index_frame);
			*(FB_ptr + index_monitor_4 +1) = *(RGB565_buffer +index_frame +1);
		}
	}

}
/* ------------------------------------------------------------ */
struct vbuffer * wait_webcam_data()
{
	fd_set fds;
	struct timeval tv;
	struct vbuffer *webcam_buf;
	int r;

	/* add in select set */
	FD_ZERO(&fds);
	FD_SET(sys_info.cam.handle, &fds);

	/* Timeout. */
	tv.tv_sec = 2;
	tv.tv_usec = 0;

	r = select(sys_info.cam.handle + 1, &fds, NULL, NULL, &tv);

	if (r == -1) {
		fprintf(stderr, "select");
		return NULL;
	}
	if (r == 0) {
		fprintf(stderr, "select timeout\n");
		return NULL;
	}
	webcam_buf = webcam_read_frame(sys_info.cam.handle);

	return webcam_buf;
}
/* ------------------------------------------------------------ */
static int Rx_socket_init(int *Rx_socket, struct sockaddr_in *Rx_addr)
{
	/* create socket */
	if((*Rx_socket = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
		fprintf(stderr, "Socket Failed\n");
		return 0;
	}

	/* resize Buffer size */
	int exBufferSize = 1024 * 1024 * 10 ;
	if(setsockopt(*Rx_socket ,SOL_SOCKET ,SO_RCVBUF , (char*)&exBufferSize  ,sizeof(int)) == -1) {
		fprintf(stderr, "setsockopt Failed\n");
		return 0;
	}


	/* Bind */
	Rx_addr->sin_family = AF_INET;
	Rx_addr->sin_port = htons(NET_PORT);
	Rx_addr->sin_addr.s_addr = INADDR_ANY;
	if(bind(*Rx_socket, (struct sockaddr *)Rx_addr, sizeof(struct sockaddr_in))== -1) {
		fprintf(stderr, "bind error\n");
		return 0;
	}
	return 1;
}
/* ------------------------------------------------------------ */
static void Rx_loop(void * Rx_arg)
{
	int Rx_socket;
	struct sockaddr_in Rx_addr;
	struct VOD_DataPacket_struct Rx_Buffer ;

	/* Socket init */
	if(Rx_socket_init(&Rx_socket, &Rx_addr) ==0) {
		fprintf(stderr, "Rx Socket init \n");
	}
	printf("Rx socket init finish\n");

	/* receive data */
	int recv_len ; 
	AVPacket packet ;
	int remain_size =0;
	unsigned char *RGB_buffer =NULL;	/* feedback pointer */
	int ID ;
	int count =0;

	while(sys_info.status == SYS_STATUS_WORK) {
		recv_len = recv(Rx_socket, (char*)&Rx_Buffer, sizeof(Rx_Buffer) , 0) ;
		if(recv_len == -1) {
			fprintf(stderr ,"Stream data recv() error\n");
			break ;
		}
		else {
			switch(Rx_Buffer.DataType) {
			case VOD_PACKET_TYPE_FRAME_HEADER :	/* AVPacket as header */
				memcpy(&packet, &Rx_Buffer.header , sizeof(AVPacket));
				packet.data = (unsigned char *)malloc(sizeof(char) * packet.size);
				remain_size = packet.size;
				break ;

			case VOD_PACKET_TYPE_FRAME_DATA :	/* AVPacket.data */
				ID = Rx_Buffer.data.ID ;
				memcpy(packet.data + ID * 1024, &Rx_Buffer.data.data[0] , Rx_Buffer.data.size);

				remain_size -= Rx_Buffer.data.size;
				 /* receive finish */
				if(remain_size <= 0) {	/* receive finish */
					/* Decode frame */
					video_decoder(packet.data, packet.size, &RGB_buffer);

					/* Display in Frame buffer*/
					RGB24_to_RGB565(RGB_buffer, RGB565_buffer, sys_info.cam.width, sys_info.cam.height);
					FB_display(RGB565_buffer, 0, 0, sys_info.cam.width, sys_info.cam.height);
					av_free_packet(&packet);
				}
				count ++;
				break ;
			}
		}
	}
	close(Rx_socket);
}
/* ------------------------------------------------------------ */
/* Video Transmit loop
 *
 *	only transmit Video data , fetch data from webcam , and ecoder that as YUV420 frame .
 */
static void Tx_loop(void * Rx_arg)
{
        unsigned int count = 0;


	/* init socket data*/
	int Tx_socket ;
	struct sockaddr_in Tx_addr;
	if((Tx_socket = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
		printf("Socket Faile\n");
		exit(-1);
	}
	Tx_addr.sin_family = AF_INET;
	Tx_addr.sin_port = htons(NET_PORT);
	Tx_addr.sin_addr.s_addr=inet_addr(NET_HOST);

	/* Webcam init */
	sys_info.cam.handle = webcam_open();

	webcam_show_info(sys_info.cam.handle);
	webcam_init(sys_info.cam.width, sys_info.cam.height, sys_info.cam.handle);
	webcam_set_framerate(sys_info.cam.handle, 20);
	webcam_start_capturing(sys_info.cam.handle);


	struct timeval t_start,t_end;
	double uTime =0.0;
	int total_size=0;
	struct vbuffer *webcam_buf;

	/* start video capture and transmit */
	gettimeofday(&t_start, NULL);
	while (sys_info.status == SYS_STATUS_WORK) {
		webcam_buf = wait_webcam_data();
		if (webcam_buf !=NULL) {
			count++;
			struct AVPacket *pkt_ptr = video_encoder(webcam_buf->start);


			/* ------------------------------------- */
			/* direct display in localhost */
			unsigned char *RGB_buffer =NULL;
			video_decoder(pkt_ptr->data, pkt_ptr->size, &RGB_buffer);
			RGB24_to_RGB565(RGB_buffer, RGB565_buffer, sys_info.cam.width, sys_info.cam.height);
			FB_display(RGB565_buffer, 0, 0, sys_info.cam.width, sys_info.cam.height);

			total_size +=pkt_ptr->size;

			/* network transmit */
			struct VOD_DataPacket_struct Tx_Buffer;

			/* Tx header */
			Tx_Buffer.DataType = VOD_PACKET_TYPE_FRAME_HEADER;
			memcpy(&Tx_Buffer.header, pkt_ptr, sizeof(AVPacket));
			sendto(Tx_socket, (char *)&Tx_Buffer, sizeof(Tx_Buffer), 0, (struct sockaddr *)&Tx_addr, sizeof(struct sockaddr_in));

			/* Tx data */
			int slice =0;
			int offset =1024 ;
			int remain_size = pkt_ptr->size ;

			while(remain_size > 0) {
				Tx_Buffer.DataType = VOD_PACKET_TYPE_FRAME_DATA;
				Tx_Buffer.data.ID = slice ;
				if (remain_size < 1024) {	/* verify transmit data size*/
					Tx_Buffer.data.size = remain_size ;
				}
				else {
					Tx_Buffer.data.size = 1024 ;
				}
				memcpy(&Tx_Buffer.data.data[0], pkt_ptr->data + slice * 1024, sizeof(char) * Tx_Buffer.data.size);
				sendto(Tx_socket, (char *)&Tx_Buffer, sizeof(Tx_Buffer), 0, (struct sockaddr *)&Tx_addr, sizeof(struct sockaddr_in));
				slice ++ ;
				remain_size -= 1024 ;
			}
		}
		else {
			printf("webcam_read_frame error\n");
		}
	}
	gettimeofday(&t_end, NULL);
	uTime = (t_end.tv_sec -t_start.tv_sec)*1000000.0 +(t_end.tv_usec -t_start.tv_usec);
	printf("Total size : %d bit , frame count : %d\n", total_size, count);
	printf("Time :%lf us\n", uTime);
	printf("kb/s %lf\n",total_size/ (uTime/1000.0));

	webcam_stop_capturing(sys_info.cam.handle);
	webcam_release(sys_info.cam.handle);
	close(sys_info.cam.handle);
}
/* ------------------------------------------------------------ */
void SIGINT_release(int arg)
{
	printf("System Relase ... \n");
	sys_info.status = SYS_STATUS_RELEASE;	
}
/* ------------------------------------------------------------ */
int main()
{
	//sys_info.capability = SYS_CAPABILITY_VIDEO_Tx | SYS_CAPABILITY_VIDEO_Rx | SYS_CAPABILITY_AUDIO_Tx | SYS_CAPABILITY_AUDIO_Rx;
	sys_info.capability = SYS_CAPABILITY_VIDEO_Tx;
	sys_info.status = SYS_STATUS_INIT;
	sys_info.cam.width = 320;
	sys_info.cam.height = 240;
//	sys_info.cam.pixel_fmt = V4L2_PIX_FMT_YUV420;
	sys_info.cam.pixel_fmt = V4L2_PIX_FMT_YUYV;
//	sys_info.cam.pixel_fmt = V4L2_PIX_FMT_MJPEG;

	/* alloc RGB565 buffer for frame buffer data store */
	RGB565_buffer = (unsigned char *)malloc(sys_info.cam.width * sys_info.cam.height *2);

	/* step Codec register  */
	avcodec_register_all();
	av_register_all();
	video_encoder_init(sys_info.cam.width, sys_info.cam.height, sys_info.cam.pixel_fmt);
	video_decoder_init(sys_info.cam.width, sys_info.cam.height, sys_info.cam.pixel_fmt);
	printf("Codec init finish\n");

	/* step Frame buffer initial*/
	if(FB_init() == 0) {
		fprintf(stderr, "Frame Buffer init error\n");
	}


	/* Create Video thread */
	if(sys_info.capability & SYS_CAPABILITY_VIDEO_Rx) 
		pthread_create(&Rx_thread, NULL, &Rx_loop, NULL);
	if(sys_info.capability & SYS_CAPABILITY_VIDEO_Tx) 
		pthread_create(&Tx_thread, NULL, &Tx_loop, NULL);


	/* Signale SIGINT */
	sys_info.status = SYS_STATUS_WORK;
	signal (SIGINT, SIGINT_release);


	/* Voice thread */

	/* *******************************************************
	 * Main thread for SIP server communication and HW process
	 *
	 *
	 * *******************************************************/

	pause();
	/* release */
	if(sys_info.capability & SYS_CAPABILITY_VIDEO_Tx) 
		pthread_join(Tx_thread,NULL);
	if(sys_info.capability & SYS_CAPABILITY_VIDEO_Rx) 
		pthread_join(Rx_thread,NULL);

	munmap(FB_ptr, FB_scerrn_size);
	close(FB);
	free(RGB565_buffer);

	video_encoder_release();
	video_decoder_release();
	printf("finish\n");
	return 0;
}
