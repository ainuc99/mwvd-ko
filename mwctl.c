// SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause
/*
 * mwctl — mwvd 用户态控制工具（加载/卸载镜像、查询状态）
 *
 * Copyright (c) 2026 LU WEIFENG (wave.lu@gmail.com)
 * 版权所有 (c) 2026 陆伟峰
 */
#define _GNU_SOURCE // O_DIRECT 是 GNU 扩展
#include <fcntl.h>
#include <sys/ioctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>

#include "mwvd.h"

#define VD_IOCTL_DEV "/dev/mwvd-control"

static void usage(const char *progname)
{
  fprintf(stderr, "Usage: %s <option> [args]\n", progname);
  fprintf(stderr, "\t -c img_file     : auto-allocate mwvd device and attach image file\n");
  fprintf(stderr, "\t -d mwvd_dev     : detach image and remove device (e.g. /dev/mwvd0)\n");
  fprintf(stderr, "\t -l              : list all mwvd devices and their status\n");
  fprintf(stderr, "\t -H              : show firmware-declared hibernate (S4) partition\n");
  exit(EXIT_FAILURE);
}

/* 打开设备节点；udev 异步创建设备节点，最多等待 wait_ms 毫秒（步长 50ms） */
static int open_wait(const char *path, int flags, int wait_ms)
{
  int fd, waited = 0;

  for (;;)
  {
    fd = open(path, flags);
    if (fd >= 0 || errno != ENOENT || waited >= wait_ms)
      return fd;
    usleep(50000);
    waited += 50;
  }
}

static const char *state_name(unsigned int state)
{
  switch (state)
  {
  case 0: return "unbound";
  case 1: return "bound";
  case 2: return "rundown";
  case 3: return "deleting";
  default: return "unknown";
  }
}

int main(int argc, char *argv[])
{
  if (argc < 2)
    usage(argv[0]);

  /* ---------- -l：列出所有 mwvd 设备状态 ---------- */
  if (strcmp(argv[1], "-l") == 0)
  {
    struct dirent **list;
    int n, i;

    n = scandir("/dev", &list, NULL, NULL);
    if (n < 0)
    {
      perror("scandir /dev");
      return EXIT_FAILURE;
    }

    printf("%-16s %-9s %-14s %s\n", "DEV", "STATE", "SIZE", "IMAGE");
    for (i = 0; i < n; i++)
    {
      int dnum, fd;
      char devpath[64];
      struct mwvd_status st;

      if (sscanf(list[i]->d_name, "mwvd%d", &dnum) != 1)
      {
        free(list[i]);
        continue;
      }
      /* 严格校验设备名：必须恰好是 "mwvd"+纯数字。
       * sscanf("mwvd%d") 会把分区节点 mwvd0p1/mwvd0p2 也匹配成 dnum=0，
       * 导致 -l 对同一条挂载重复打印同一整盘记录（一条挂载输出多条）。 */
      {
        char chk[32];
        snprintf(chk, sizeof(chk), "mwvd%d", dnum);
        if (strcmp(chk, list[i]->d_name) != 0)
        {
          free(list[i]);
          continue;
        }
      }
      free(list[i]);

      snprintf(devpath, sizeof(devpath), "/dev/mwvd%d", dnum);
      fd = open(devpath, O_RDONLY);
      if (fd < 0)
        continue;

      if (ioctl(fd, MWVD_GET_STATUS, &st) == 0)
        printf("%-16s %-9s %-14llu %s\n", devpath, state_name(st.state),
               (unsigned long long)st.file_size,
               st.file_name[0] ? st.file_name : "-");
      close(fd);
    }
    free(list);
    return EXIT_SUCCESS;
  }

  /* ---------- -H：固件声明的休眠（S4）存储分区 ---------- */
  if (strcmp(argv[1], "-H") == 0)
  {
    struct mwvd_hibernate hib;
    int cfd, ret;

    cfd = open(VD_IOCTL_DEV, O_RDONLY);
    if (cfd < 0)
    {
      perror("open " VD_IOCTL_DEV);
      return EXIT_FAILURE;
    }

    memset(&hib, 0, sizeof(hib));
    ret = ioctl(cfd, MWVD_CTL_GET_HIBERNATE, &hib);
    close(cfd);
    if (ret < 0)
    {
      perror("ioctl MWVD_CTL_GET_HIBERNATE");
      return EXIT_FAILURE;
    }

    if (!hib.valid)
    {
      printf("hibernate partition: not declared by firmware (mw.config hibernate unset)\n");
      return EXIT_SUCCESS;
    }

    printf("hibernate partition: %s (usage=%u)\n", hib.part_uuid, hib.usage);
    if (hib.resolved)
      printf("  device: %s\n", hib.device_path);
    else
      printf("  device: not present (partition not visible)\n");
    printf("  \u6ce8\u610f\uff1a\u56fa\u4ef6\u53ea\u58f0\u660e\u610f\u56fe\uff1b\u662f\u5426\u4e3a\u5408\u6cd5 swap\u3001\u5bb9\u91cf\u662f\u5426\u8db3\u591f\u9700\u90e8\u7f72\u5de5\u5177\u81ea\u5df1\u6838\u5b9e\n");
    return EXIT_SUCCESS;
  }

  /* ---------- -c：自动分配设备并关联镜像 ---------- */
  if (strcmp(argv[1], "-c") == 0)
  {
    const char *img = argv[2];
    char devpath[64];
    int cfd, dnum, dfd, ffd, ret;

    if (argc < 3)
      usage(argv[0]);

    cfd = open(VD_IOCTL_DEV, O_RDWR);
    if (cfd < 0)
    {
      perror("open " VD_IOCTL_DEV);
      return EXIT_FAILURE;
    }

    /* 优先复用空闲设备，无空闲则自动新建（与固件自动加载路径一致） */
    dnum = ioctl(cfd, MWVD_CTL_GET_FREE, -1);
    if (dnum < 0)
    {
      perror("ioctl MWVD_CTL_GET_FREE");
      close(cfd);
      return EXIT_FAILURE;
    }
    close(cfd);

    snprintf(devpath, sizeof(devpath), "/dev/mwvd%d", dnum);
    dfd = open_wait(devpath, O_RDWR, 2000);
    if (dfd < 0)
    {
      fprintf(stderr, "open %s: %s\n", devpath, strerror(errno));
      return EXIT_FAILURE;
    }

    ffd = open(img, O_RDWR);
    if (ffd < 0)
    {
      perror("open image");
      close(dfd);
      return EXIT_FAILURE;
    }

    ret = ioctl(dfd, MWVD_SET_FD, ffd);
    if (ret < 0)
    {
      perror("ioctl MWVD_SET_FD");
      close(ffd);
      close(dfd);
      return EXIT_FAILURE;
    }

    printf("%s: attached image %s\n", devpath, img);
    close(ffd);
    close(dfd);
    return EXIT_SUCCESS;
  }

  /* ---------- -d：解绑并注销设备（按需释放设备号） ---------- */
  if (strcmp(argv[1], "-d") == 0)
  {
    const char *devpath = argv[2];
    int dfd, cfd, dnum, ret;

    if (argc < 3)
      usage(argv[0]);

    dfd = open(devpath, O_RDWR);
    if (dfd < 0)
    {
      perror("open device");
      return EXIT_FAILURE;
    }

    ret = ioctl(dfd, MWVD_CLR_FD, 0);
    if (ret < 0)
    {
      perror("ioctl MWVD_CLR_FD");
      close(dfd);
      return EXIT_FAILURE;
    }
    close(dfd);
    printf("%s: detached\n", devpath);

    /* 注销设备释放设备号；若仍有打开者（如分区挂载中），内核返回 -EBUSY，设备保留 */
    if (sscanf(devpath, "/dev/mwvd%d", &dnum) == 1)
    {
      cfd = open(VD_IOCTL_DEV, O_RDWR);
      if (cfd >= 0)
      {
        ret = ioctl(cfd, MWVD_CTL_REMOVE, dnum);
        if (ret < 0)
          perror("ioctl MWVD_CTL_REMOVE (device kept; in use?)");
        else
          printf("mwvd%d: device removed\n", dnum);
        close(cfd);
      }
    }
    return EXIT_SUCCESS;
  }

  usage(argv[0]);
  return EXIT_FAILURE;
}
