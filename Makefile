# 请修改为对应的交叉编译器前缀
# 比如 arm-rockchip830-linux-uclibcgnueabihf-gcc
CC = arm-rockchip830-linux-uclibcgnueabihf-gcc

CFLAGS = -Wall -O2 -I./mpp-develop/inc
# 极其重要：链接 MPP 硬件加速库
LDFLAGS = -L./lib -lrockchip_mpp -lpthread

TARGET = jpeg_spi
SRCS = main.c

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

clean:
	rm -f $(TARGET) *.jpg
