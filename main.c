#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>
#include <linux/spi/spidev.h>
#include <signal.h>
#include <errno.h>

// Rockchip MPP Headers
#include "rk_mpi.h"
#include "mpp_buffer.h"
#include "mpp_frame.h"
#include "mpp_packet.h"

// --- 宏定义区 ---
#define CAM_DEV         "/dev/video11" 
#define WIDTH           640
#define HEIGHT          480
#define BUFFER_COUNT    3

#define SPI_DEV         "/dev/spidev0.0"
#define SPI_SPEED       1000000  // 1MHz，求稳。后续图传稳定了可以往上拉到 10MHz
#define SPI_CHUNK_SIZE  4096     // Linux SPI 默认最大缓冲 4KB

//检查rkipc的进程是否全部被杀死以获得video的控制权
#define CHK_IOCTL(ret, msg) \
    if ((ret) < 0) { \
        perror("❌ " msg " Failed"); \
        return -1; \
    }

// --- 变长通信协议头 (32 Bytes, 保证内存对齐(字对齐)) ---
#pragma pack(push, 1) // 禁用 C 语言结构体自动填充
struct SPI_Packet_Header {
    uint16_t magic;          // 校验字，设为 0x5AA5
    uint32_t jpeg_size;      // 紧随其后的图片总大小
    uint8_t  ai_target_count;// 预留给下一阶段：识别到的目标数量 (目前写 0)
    uint8_t  reserved[25];   // 凑齐 32 字节，方便 ESP32 接收
};
#pragma pack(pop)

// --- 全局变量区 ---
volatile sig_atomic_t keep_running = 1;
int v4l2_fd = -1;
int spi_fd = -1;
int dma_fds[BUFFER_COUNT]; 

MppBuffer mpp_bufs[BUFFER_COUNT] = {NULL, NULL, NULL};
MppCtx ctx = NULL;
MppApi *mpi = NULL;
MppPacket packet = NULL;

// --- 捕捉 Ctrl+C 信号 ---
void sigint_handler(int dummy) {
    printf("\n[Signal] Stopping Edge AI Pipeline...\n");
    keep_running = 0;
}

// ============================================================================
// 模块 A: SPI 初始化与分块发送逻辑
// ============================================================================
int init_spi() {
    spi_fd = open(SPI_DEV, O_RDWR);
    if (spi_fd < 0) {
        perror("Failed to open SPI"); 
        return -1;
    }
    uint8_t mode = SPI_MODE_0;
    uint8_t bits = 8;
    uint32_t speed = SPI_SPEED;
    ioctl(spi_fd, SPI_IOC_WR_MODE, &mode);
    ioctl(spi_fd, SPI_IOC_WR_BITS_PER_WORD, &bits);
    ioctl(spi_fd, SPI_IOC_WR_MAX_SPEED_HZ, &speed);
    printf("[SPI] Initialized at 1MHz.\n");
    return 0;
}

// 核心分块发送引擎
int spi_send_chunked(uint8_t *data, size_t total_len) {
    size_t offset = 0;
    while (offset < total_len) {
        //可将正则改为if
        size_t current_len = (total_len - offset > SPI_CHUNK_SIZE) ? SPI_CHUNK_SIZE : (total_len - offset);
        
        struct spi_ioc_transfer tr = {
            .tx_buf = (unsigned long)(data + offset),
            .rx_buf = 0,
            .len = current_len,
            .speed_hz = SPI_SPEED,
            .bits_per_word = 8,
        };

        if (ioctl(spi_fd, SPI_IOC_MESSAGE(1), &tr) < 0) {
            perror("SPI Chunk Transfer Failed");
            return -1;
        }
        
        offset += current_len;
        
        // 极其重要：给 ESP32 的 DMA 中断处理留出喘息时间
        // 如果连发太快，ESP32 根本来不及把接收到的数据从 DMA 搬到应用层
        usleep(1500); // 延时 1.5 毫秒
    }
    return 0;
}

// ============================================================================
// 模块 B: V4L2 与 MPP 初始化 (与之前一致，略微折叠)
// ============================================================================
int init_v4l2_camera() {
    v4l2_fd = open(CAM_DEV, O_RDWR, 0);
    if (v4l2_fd < 0) 
        return -1;

    struct v4l2_format fmt; 
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    fmt.fmt.pix_mp.width = WIDTH; 
    fmt.fmt.pix_mp.height = HEIGHT;
    fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
    ioctl(v4l2_fd, VIDIOC_S_FMT, &fmt);

    // 检查！如果这里报 EBUSY，说明 rkipc 没杀干净
    CHK_IOCTL(ioctl(v4l2_fd, VIDIOC_S_FMT, &fmt), "V4L2: S_FMT");
    // 这时候打印出来的才是真理
    uint8_t real_planes = fmt.fmt.pix_mp.num_planes;
    printf("🚀 [DEBUG] Kernel actually negotiated planes: %d\n", real_planes);

    struct v4l2_requestbuffers req; 
    memset(&req, 0, sizeof(req));
    req.count = BUFFER_COUNT; 
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; 
    req.memory = V4L2_MEMORY_MMAP;
    ioctl(v4l2_fd, VIDIOC_REQBUFS, &req);

    for (int i = 0; i < BUFFER_COUNT; ++i) {
        struct v4l2_buffer buf; 
        struct v4l2_plane planes[1];
        memset(&buf, 0, sizeof(buf)); 
        memset(planes, 0, sizeof(planes));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; 
        buf.memory = V4L2_MEMORY_MMAP; 
        buf.index = i; 
        buf.length = 1; 
        buf.m.planes = planes;
        ioctl(v4l2_fd, VIDIOC_QUERYBUF, &buf);

        struct v4l2_exportbuffer expbuf; 
        memset(&expbuf, 0, sizeof(expbuf));
        expbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; 
        expbuf.index = i; 
        expbuf.plane = 0;
        ioctl(v4l2_fd, VIDIOC_EXPBUF, &expbuf);
        dma_fds[i] = expbuf.fd;
        ioctl(v4l2_fd, VIDIOC_QBUF, &buf);
    }
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    ioctl(v4l2_fd, VIDIOC_STREAMON, &type);
    printf("[V4L2] Camera initialized.\n"); return 0;
}

int init_mpp_encoder() {
    mpp_create(&ctx, &mpi);
    mpp_init(ctx, MPP_CTX_ENC, MPP_VIDEO_CodingMJPEG);
    MppEncCfg cfg; 
    mpp_enc_cfg_init(&cfg);
    mpp_enc_cfg_set_s32(cfg, "prep:width", WIDTH); 
    mpp_enc_cfg_set_s32(cfg, "prep:height", HEIGHT);
    mpp_enc_cfg_set_s32(cfg, "prep:hor_stride", WIDTH); 
    mpp_enc_cfg_set_s32(cfg, "prep:ver_stride", HEIGHT);
    mpp_enc_cfg_set_s32(cfg, "prep:format", MPP_FMT_YUV420SP); 
    mpp_enc_cfg_set_s32(cfg, "jpeg:q_factor", 80); 
    mpi->control(ctx, MPP_ENC_SET_CFG, cfg);
    mpp_enc_cfg_deinit(cfg);
    printf("[MPP] Encoder initialized.\n"); return 0;
}

// ============================================================================
// 核心：图传总调度流水线
// ============================================================================
int main() {
    signal(SIGINT, sigint_handler);

    // 1. 初始化所有硬件模块
    if (init_spi() < 0 || init_v4l2_camera() < 0 || init_mpp_encoder() < 0) {
        goto CLEANUP;
    }

    // 🌟 2. 【核心大手术】：把 DMA 地契一次性交给 MPP，绝不放进死循环！
    for (int i = 0; i < BUFFER_COUNT; i++) {
        MppBufferInfo info = { 
            .type = MPP_BUFFER_TYPE_EXT_DMA, 
            .fd = dma_fds[i], 
            .size = WIDTH * HEIGHT * 3 / 2  // NV12 的物理大小
        };
        mpp_buffer_import(&mpp_bufs[i], &info);
    }
    printf("[IOMMU] DMA Buffers Imported to MPP Successfully.\n");

    // 3. 准备运行环境
    struct v4l2_buffer buf;
    struct v4l2_plane planes[1];
    fd_set fds;
    struct timeval tv;

    struct SPI_Packet_Header header;
    memset(&header, 0, sizeof(header));
    header.magic = 0x5AA5; // 填入我们的魔法校验字

    int frame_count = 0;
    printf("--- Edge AI Pipeline Running! Press Ctrl+C to stop. ---\n");

    // 4. 进入零拷贝物理流水线
    while (keep_running) {
        FD_ZERO(&fds); 
        FD_SET(v4l2_fd, &fds);
        tv.tv_sec = 2; 
        tv.tv_usec = 0;

        printf("\n--- Waiting for Camera (select) ---\n");
	    int s_ret = select(v4l2_fd + 1, &fds, NULL, NULL, &tv);
        if (s_ret == 0) {
            // 加上这句话，你就能看到 ISP 是不是脑死亡了
            printf("⏳ Timeout: No frame from Camera! (ISP is sleeping)\n");
            continue;
        } else if (s_ret < 0) {
            perror("❌ Select Error");
            break;
        }
        printf("\nleaving select\n");

        // --- 抓取图像 ---
        memset(&buf, 0, sizeof(buf)); memset(planes, 0, sizeof(planes));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; 
        buf.memory = V4L2_MEMORY_MMAP; 
        buf.length = 1; 
        buf.m.planes = planes;
        printf("start frame entering queue");       
        if (ioctl(v4l2_fd, VIDIOC_DQBUF, &buf) < 0) {
            perror("❌ DQBUF Failed");
            usleep(100000); // 🌟 刹车片：休眠 0.1 秒，防止 CPU 100%
            continue;
        }

        printf("[V4L2] DQBUF Success. bytesused = %d\n", buf.m.planes[0].bytesused);
                
        // 🛑 空帧防御
        if (buf.m.planes[0].bytesused == 0) {
            if (ioctl(v4l2_fd, VIDIOC_QBUF, &buf) < 0) perror("❌ QBUF Failed on Empty Frame");
            continue;
        }

        // --- MPP 硬件编码 ---
        MppFrame frame = NULL; 
        mpp_frame_init(&frame);
        mpp_frame_set_width(frame, WIDTH); 
        mpp_frame_set_height(frame, HEIGHT);
        mpp_frame_set_hor_stride(frame, WIDTH); 
        mpp_frame_set_ver_stride(frame, HEIGHT);
        mpp_frame_set_fmt(frame, MPP_FMT_YUV420SP); 
        mpp_frame_set_buffer(frame, mpp_bufs[buf.index]);
        mpp_frame_set_pts(frame, buf.timestamp.tv_sec * 1000000 + buf.timestamp.tv_usec);
        
        // 只有当捕捉到 Ctrl+C 准备退出循环，给硬件塞最后一帧时，才设为 1。正常图传绝对设为 0 或者干脆别写这一行！
        mpp_frame_set_eos(frame, 0); 
        
        //使用同步接口！让 MPP 底层去处理硬件中断和睡眠等待
        MPP_RET enc_ret = mpi->encode_put_frame(ctx, frame);
        
        if (enc_ret != MPP_OK || packet == NULL) {
            printf("❌ MPP Encode Failed! ret = %d\n", enc_ret);
        } else {
            // 成功拿到集装箱！
            uint8_t *jpeg_data = (uint8_t *)mpp_packet_get_pos(packet);
            size_t jpeg_size = mpp_packet_get_length(packet);

            if (jpeg_size > 0) {
                // 发送车头
                header.jpeg_size = (uint32_t)jpeg_size;
                header.ai_target_count = 0; 
                struct spi_ioc_transfer tr_header = {
                    .tx_buf = (unsigned long)&header,
                    .len = sizeof(header), 
                    .speed_hz = SPI_SPEED,
                    .bits_per_word = 8,
                };
                ioctl(spi_fd, SPI_IOC_MESSAGE(1), &tr_header);
                usleep(2000); 

                // 发送车厢
                spi_send_chunked(jpeg_data, jpeg_size);
                printf("✅ [Transmitted] Frame %d | JPEG Size: %zu Bytes\n", frame_count++, jpeg_size);
            }
            
            // 卸货完毕，归还集装箱
            mpp_packet_deinit(&packet);
        }
        
        // 🌟 必须检查 QBUF！一旦失败，必须立刻终止程序，因为队列已经雪崩了
        if (ioctl(v4l2_fd, VIDIOC_QBUF, &buf) < 0) {
            perror("❌ FATAL: QBUF Failed in loop!");
            break; 
        }
    }//end of while
CLEANUP:
    printf("\n--- Shutting down gracefully ---\n");
    
    printf("1. Releasing MPP Buffers...\n");
    for (int i = 0; i < BUFFER_COUNT; i++) {
        if (mpp_bufs[i]) mpp_buffer_put(mpp_bufs[i]);
    }

    // 🌟 极其关键：必须先销毁 MPP 硬件，让它松开对内存的锁定！
    printf("2. Destroying MPP Context...\n");
    if (ctx) mpp_destroy(ctx); 

    // 🌟 然后再去关摄像头
    printf("3. Stopping V4L2 Stream...\n");
    if (v4l2_fd >= 0) {
        enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        ioctl(v4l2_fd, VIDIOC_STREAMOFF, &type);
        close(v4l2_fd);
    }
    
    printf("4. Closing SPI...\n");
    if (spi_fd >= 0) close(spi_fd);
    
    printf("--- Pipeline Closed ---\n");
    return 0;
}