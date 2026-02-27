#pragma once

#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>

#include <stdio.h>
#include <unistd.h>

#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

class Gyro{
public:
    int fd = -1;
    Gyro(){
        struct sockaddr_in addr;
        fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            perror("socket");
            return;
        }
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        addr.sin_port = htons(12345);
        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            perror("connect");
            close(fd);
            fd = -1;
            return;
        }
    }
    ~Gyro(){
        if (bGyroConnect())
        close(fd);
    }
    bool bGyroConnect(){
        return fd > 0;
    }
    void update(float x,float y){
        if (fd < 0) return;
        float temp[2];
        temp[0] = x;
        temp[1] = y;
        write(fd,temp, sizeof(temp));
    }
};
