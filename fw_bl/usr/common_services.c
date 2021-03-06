/*
 * Software License Agreement (MIT License)
 *
 * Copyright (c) 2017, DUKELEC, Inc.
 * All rights reserved.
 *
 * Author: Duke Fong <duke@dukelec.com>
 */

#include "app_main.h"

static char cpu_id[25];
static char info_str[100];

static cdn_sock_t sock1 = { .port = 1, .ns = &dft_ns };
static cdn_sock_t sock5 = { .port = 5, .ns = &dft_ns };
//static cdn_sock_t sock6 = { .port = 6, .ns = &dft_ns };
static cdn_sock_t sock8 = { .port = 8, .ns = &dft_ns };


static void get_uid(char *buf)
{
    const char tlb[] = "0123456789abcdef";
    int i;

    for (i = 0; i < 12; i++) {
        uint8_t val = *((char *)UID_BASE + i);
        buf[i * 2 + 0] = tlb[val & 0xf];
        buf[i * 2 + 1] = tlb[val >> 4];
    }
    buf[24] = '\0';
}

void init_info_str(void)
{
    // M: model; S: serial string; HW: hardware version; SW: software version
    get_uid(cpu_id);
    sprintf(info_str, "M: cdbus bridge; S: %s; SW: %s", cpu_id, SW_VER);
    d_info("info: %s\n", info_str);
}


// device info
static void p1_service_routine(void)
{
    cdn_pkt_t *pkt = cdn_sock_recvfrom(&sock1);
    if (!pkt)
        return;

    if (pkt->len == 1 && pkt->dat[0] == 0) {
        pkt->dat[0] = 0x80;
        strcpy((char *)pkt->dat + 1, info_str);
        pkt->len = strlen(info_str) + 1;
        pkt->dst = pkt->src;
        cdn_sock_sendto(&sock1, pkt);
        return;
    }
    d_debug("p1 ser: ignore\n");
    list_put(&dft_ns.free_pkts, &pkt->node);
}


// flash memory manipulation
static void p8_service_routine(void)
{
    // erase: 0x2f, addr_32, len_32  | return [0x80] on success
    // read:  0x00, addr_32, len_8   | return [0x80, data]
    // write: 0x20, addr_32 + [data] | return [0x80] on success

    cdn_pkt_t *pkt = cdn_sock_recvfrom(&sock8);
    if (!pkt)
        return;

    if (pkt->dat[0] == 0x2f && pkt->len == 9) {
        uint8_t ret;
        uint32_t err_page = 0;
        FLASH_EraseInitTypeDef f;
        uint32_t addr = *(uint32_t *)(pkt->dat + 1);
        uint32_t len = *(uint32_t *)(pkt->dat + 5);

        f.TypeErase = FLASH_TYPEERASE_PAGES;
        f.PageAddress = addr;
        f.NbPages = (len + FLASH_PAGE_SIZE - 1) / FLASH_PAGE_SIZE;

        ret = HAL_FLASH_Unlock();
        if (ret == HAL_OK)
            ret = HAL_FLASHEx_Erase(&f, &err_page);
        ret |= HAL_FLASH_Lock();

        d_debug("nvm erase: %08x +%08x, %08x, ret: %d\n", addr, len, err_page, ret);
        pkt->len = 1;
        pkt->dat[0] = ret == HAL_OK ? 0x80 : 0x81;

    } else if (pkt->dat[0] == 0x00 && pkt->len == 6) {
        uint32_t *src_dat = (uint32_t *) *(uint32_t *)(pkt->dat + 1);
        uint8_t len = min(pkt->dat[5], CDN_MAX_DAT - 1);
        memcpy(pkt->dat + 1, src_dat, len);
        d_debug("nvm read: %08x %d\n", src_dat, len);
        pkt->dat[0] = 0x80;
        pkt->len = len + 1;

    } else if (pkt->dat[0] == 0x20 && pkt->len > 5) {
        uint8_t ret;
        uint32_t *dst_dat = (uint32_t *) *(uint32_t *)(pkt->dat + 1);
        uint8_t len = pkt->len - 5;
        uint8_t cnt = (len + 3) / 4;
        uint32_t *src_dat = (uint32_t *)(pkt->dat + 5);
        uint8_t i;

        ret = HAL_FLASH_Unlock();
        for (i = 0; ret == HAL_OK && i < cnt; i++)
            ret = HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, (uint32_t)(dst_dat + i), *(src_dat + i));
        ret |= HAL_FLASH_Lock();

        d_debug("nvm write: %08x %d(%d), ret: %d\n", dst_dat, pkt->len - 5, cnt, ret);
        pkt->len = 1;
        pkt->dat[0] = ret == HAL_OK ? 0x80 : 0x81;

    } else {
        list_put(&dft_ns.free_pkts, &pkt->node);
        d_warn("nvm: wrong cmd, len: %d\n", pkt->len);
        return;
    }

    pkt->dst = pkt->src;
    cdn_sock_sendto(&sock8, pkt);
    return;
}


// csa manipulation
static void p5_service_routine(void)
{
    // read:  0x00, offset_16, len_8   | return [0x80, data]
    // write: 0x20, offset_16 + [data] | return [0x80] on success

    cdn_pkt_t *pkt = cdn_sock_recvfrom(&sock5);
    if (!pkt)
        return;

    if (pkt->dat[0] == 0x00 && pkt->len == 4) {
        uint16_t offset = *(uint16_t *)(pkt->dat + 1);
        uint8_t len = min(pkt->dat[3], CDN_MAX_DAT - 1);
        memcpy(pkt->dat + 1, ((void *) &csa) + offset, len);
        d_debug("csa read: %04x %d\n", offset, len);
        pkt->dat[0] = 0x80;
        pkt->len = len + 1;

    } else if (pkt->dat[0] == 0x20 && pkt->len > 3) {
        uint16_t offset = *(uint16_t *)(pkt->dat + 1);
        uint8_t len = pkt->len - 3;
        uint8_t *src_dat = pkt->dat + 3;
        uint32_t flags;

        for (int i = 0; i < regr_wa_num; i++) {
            regr_t *regr = regr_wa + i;
            uint16_t start = clip(offset, regr->offset, regr->offset + regr->size);
            uint16_t end = clip(offset + len, regr->offset, regr->offset + regr->size);
            if (start == end) {
                //d_debug("csa i%d: [%x, %x), [%x, %x) -> [%x, %x)\n",
                //        i, regr->offset, regr->offset + regr->size,
                //        offset, offset + len,
                //        start, end);
                continue;
            }

            //d_debug("csa @ %p, %p <- %p, len %d, dat[0]: %x\n",
            //        &csa, ((void *) &csa) + start, src_dat + (start - offset), end - start,
            //        *(src_dat + (start - offset)));

            local_irq_save(flags);
            memcpy(((void *) &csa) + start, src_dat + (start - offset), end - start);
            local_irq_restore(flags);
        }

        d_debug("csa write: %04x %d\n", offset, len);
        pkt->len = 1;
        pkt->dat[0] = 0x80;

    } else {
        list_put(&dft_ns.free_pkts, &pkt->node);
        d_warn("csa: wrong cmd, len: %d\n", pkt->len);
        return;
    }

    pkt->dst = pkt->src;
    cdn_sock_sendto(&sock5, pkt);
    return;
}

#if 0
// qxchg
static void p6_service_routine(void)
{
    cdn_pkt_t *pkt = cdn_sock_recvfrom(&sock6);
    if (!pkt)
        return;

    if (pkt->dat[0] == 0x20 && pkt->len >= 1) {
        uint8_t *src_dat = pkt->dat + 1;
        uint8_t *dst_dat = pkt->dat + 1;
        uint32_t flags;

        local_irq_save(flags);
        for (int i = 0; i < 10; i++) {
            regr_t *regr = csa.qxchg_set + i;
            if (!regr->size)
                break;
            uint16_t lim_size = min(pkt->len - (src_dat - pkt->dat), regr->size);
            if (!lim_size)
                break;
            memcpy(((void *) &csa) + regr->offset, src_dat, lim_size);
            src_dat += lim_size;
        }
        for (int i = 0; i < 10; i++) {
            regr_t *regr = csa.qxchg_ret + i;
            if (!regr->size)
                break;
            memcpy(dst_dat, ((void *) &csa) + regr->offset, regr->size);
            dst_dat += regr->size;
        }
        local_irq_restore(flags);

        pkt->len = dst_dat - pkt->dat;
        pkt->dat[0] = 0x80;
        d_debug("dio: i %d, o %d\n", src_dat - pkt->dat, pkt->len);

    } else if (pkt->dat[0] == 0x00 && pkt->len == 1) {
            uint8_t *dst_dat = pkt->dat + 1;
            uint32_t flags;

            local_irq_save(flags);
            for (int i = 0; i < 10; i++) {
                regr_t *regr = csa.qxchg_ro + i;
                if (!regr->size)
                    break;
                memcpy(dst_dat, ((void *) &csa) + regr->offset, regr->size);
                dst_dat += regr->size;
            }
            local_irq_restore(flags);

    } else {
        list_put(&dft_ns.free_pkts, &pkt->node);
        d_warn("dio: wrong cmd, len: %d\n", pkt->len);
        return;
    }

    pkt->dst = pkt->src;
    cdn_sock_sendto(&sock6, pkt);
    return;
}
#endif


void common_service_init(void)
{
    cdn_sock_bind(&sock1);
    cdn_sock_bind(&sock5);
    //cdn_sock_bind(&sock6);
    cdn_sock_bind(&sock8);
    init_info_str();
}

void common_service_routine(void)
{
    if (csa.do_reboot)
        NVIC_SystemReset();
    p1_service_routine();
    p5_service_routine();
    //p6_service_routine();
    p8_service_routine();
}

